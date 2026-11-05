// Crash logger (debug aid). Minecraft's Breakpad handler swallows native
// crashes (the minidump is uploaded and deleted, no tombstone), so this logs
// the faulting PC/LR and a backtrace as module+offset to logcat first, then
// chains to whatever handler was installed before it (normally Breakpad).
// crash_log_install() is idempotent and re-arms itself if someone replaced it.

#include <android/log.h>
#include <dlfcn.h>
#include <signal.h>
#include <string.h>
#include <ucontext.h>
#include <unwind.h>

#define CLOG(...) __android_log_print(ANDROID_LOG_FATAL, "displayxr3d-crash", __VA_ARGS__)

namespace {

constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE};
struct sigaction g_old[32];

void log_addr(const char *what, int i, void *pc)
{
	Dl_info info = {};
	if (dladdr(pc, &info) && info.dli_fname) {
		const char *base = strrchr(info.dli_fname, '/');
		base = base ? base + 1 : info.dli_fname;
		CLOG("%s #%02d pc %p  %s+0x%lx  (%s)", what, i, pc, base,
		     (unsigned long)((char *)pc - (char *)info.dli_fbase), info.dli_sname ? info.dli_sname : "?");
	} else {
		CLOG("%s #%02d pc %p  ?", what, i, pc);
	}
}

struct BtState {
	int n;
};

_Unwind_Reason_Code bt_cb(_Unwind_Context *ctx, void *arg)
{
	auto *st = static_cast<BtState *>(arg);
	void *pc = reinterpret_cast<void *>(_Unwind_GetIP(ctx));
	if (pc) log_addr("bt", st->n, pc);
	return ++st->n >= 40 ? _URC_END_OF_STACK : _URC_NO_REASON;
}

void handler(int sig, siginfo_t *si, void *uc_)
{
	CLOG("signal %d code %d fault addr %p", sig, si ? si->si_code : 0, si ? si->si_addr : nullptr);
#if defined(__aarch64__)
	if (auto *uc = static_cast<ucontext_t *>(uc_)) {
		log_addr("pc", 0, reinterpret_cast<void *>(uc->uc_mcontext.pc));
		log_addr("lr", 0, reinterpret_cast<void *>(uc->uc_mcontext.regs[30]));
		// The frame-pointer chain from the faulting context (the unwinder below
		// starts in this handler and may not cross the signal frame).
		auto *fp = reinterpret_cast<void **>(uc->uc_mcontext.regs[29]);
		for (int i = 1; fp && i < 32; ++i) {
			void *ret = fp[1];
			if (!ret) break;
			log_addr("fp", i, ret);
			void **next = static_cast<void **>(fp[0]);
			if (next <= fp) break;
			fp = next;
		}
	}
#endif
	BtState st{0};
	_Unwind_Backtrace(bt_cb, &st);
	// Chain to the previous handler (Breakpad), or re-raise with the default.
	const struct sigaction &old = g_old[sig];
	if (old.sa_flags & SA_SIGINFO) {
		if (old.sa_sigaction) {
			old.sa_sigaction(sig, si, uc_);
			return;
		}
	} else if (old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN) {
		old.sa_handler(sig);
		return;
	}
	signal(sig, SIG_DFL);
	raise(sig);
}

} // namespace

void crash_log_install()
{
	for (int sig : kSignals) {
		struct sigaction cur = {};
		sigaction(sig, nullptr, &cur);
		if ((cur.sa_flags & SA_SIGINFO) && cur.sa_sigaction == handler) continue;  // still ours
		struct sigaction sa = {};
		sa.sa_sigaction = handler;
		sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
		sigemptyset(&sa.sa_mask);
		sigaction(sig, &sa, &g_old[sig]);
	}
}
