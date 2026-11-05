#include "Bridge.hpp"
#include "StereoGL.hpp"
#include "transport_weave.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <dlfcn.h>
#include <unistd.h>

#include <pl/memory/Hook.hpp>

#include <chrono>
#include <vector>
#include <cstdlib>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

void crash_log_install();  // CrashLog.cpp (global namespace)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "displayxr3d", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "displayxr3d", __VA_ARGS__)

namespace dxr3d {

namespace {

#include "BridgeDex.inc"

using Clock = std::chrono::steady_clock;
using dxr::core::WeaveState;

constexpr int kRetrySeconds = 5;

std::atomic<bool> g_enabled{true};
std::atomic<bool> g_connected{false};
std::atomic<int> g_protocol{0}, g_runtime_found{0};
std::atomic<uint64_t> g_woven{0}, g_swaps{0}, g_present_fail{0}, g_present_ns{0};
// Worst cases since start: these survive a freeze, for reading afterwards.
std::atomic<int> g_max_gap_ms{0}, g_max_present_ms{0}, g_stalls{0};
void note_max(std::atomic<int> &slot, int v)
{
	if (v > slot.load(std::memory_order_relaxed)) slot.store(v, std::memory_order_relaxed);
}
std::atomic<bool> g_hook_ok{false}, g_dex_ok{false};
std::atomic<int> g_win_w{0}, g_win_h{0};

std::mutex g_mu;  // guards the strings below
std::condition_variable g_cv;
std::string g_problem;  // why 3D is not running
std::string g_info;
bool g_info_dirty = false;

void set_problem(const std::string &s)
{
	std::lock_guard<std::mutex> l(g_mu);
	if (g_problem != s && !s.empty()) LOGW("%s", s.c_str());
	g_problem = s;
}

// ---- the transport ----------------------------------------------------------
// Started by the Java thread (it has the socket); stopped on the render thread
// (stop() frees GL objects). g_xr_running says who may touch it.
dxr::core::WeaveTransport g_xr;
std::atomic<bool> g_xr_running{false};
std::atomic<int64_t> g_next_try_ms{0};
dxr::core::EyeSample g_eyes;  // render thread
std::atomic<bool> g_eyes_valid{false}, g_eyes_tracking{false};
std::atomic<int> g_panel_w{0}, g_panel_h{0};
bool g_woven_this_frame = false;  // render thread

int64_t now_ms()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

void on_swap(EGLDisplay dpy, EGLSurface surf)
{
	GLBypass bypass;  // our own GL calls must not be doubled / tracked
	g_swaps.fetch_add(1, std::memory_order_relaxed);
	{
		static auto last = Clock::now();
		const auto n = Clock::now();
		note_max(g_max_gap_ms, (int)std::chrono::duration_cast<std::chrono::milliseconds>(n - last).count());
		last = n;
	}
	EGLint w = 0, h = 0;
	eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
	eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
	g_win_w = w;
	g_win_h = h;

	g_woven_this_frame = false;
	if (g_xr_running.load()) {
		const WeaveState st = g_xr.state();
		if (st == WeaveState::Failed) {
			set_problem(std::string("DisplayXR: ") + g_xr.last_error());
			g_xr.stop();
			g_xr_running = false;
			g_next_try_ms = now_ms() + kRetrySeconds * 1000;
		} else if (st == WeaveState::Ready) {
			const auto d = g_xr.display_info();
			g_panel_w = (int)d.pixel_w;
			g_panel_h = (int)d.pixel_h;
			if (g_enabled.load() && w > 0 && h > 0) {
				// One eye rendered (a menu)? Both tiles get it: it sits at screen depth.
				const uint32_t right = stereo_frame_was_stereo() ? stereo_window_twin_fbo() : 0;
				const auto t0 = Clock::now();
				g_woven_this_frame = g_xr.present(0, right, w, h, &g_eyes, nullptr);
				const auto took = Clock::now() - t0;
				g_present_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(took).count(),
				                       std::memory_order_relaxed);
				note_max(g_max_present_ms, (int)std::chrono::duration_cast<std::chrono::milliseconds>(took).count());
				static bool was_stalled = false;
				const bool stalled_now = g_xr.stalled();
				if (stalled_now && !was_stalled) g_stalls.fetch_add(1, std::memory_order_relaxed);
				was_stalled = stalled_now;
				if (g_woven_this_frame) {
					g_woven.fetch_add(1, std::memory_order_relaxed);
					g_eyes_valid = g_eyes.valid;
					g_eyes_tracking = g_eyes.tracking;
					// Look-around: tracked eyes (display space, metres) drive the
					// two cameras. Scale so a 64 mm IPD equals the set separation.
					if (g_eyes.tracking && g_eyes.count >= 2) {
						const float k = g_half_ipd.load(std::memory_order_relaxed) / 0.032f;
						for (int i = 0; i < 2; ++i) {
							g_eye_off[i][0].store(k * g_eyes.pos[i][0], std::memory_order_relaxed);
							g_eye_off[i][1].store(k * g_eyes.pos[i][1], std::memory_order_relaxed);
						}
						g_eye_tracked = true;
						static int en = 0;
						if ((en++ % 240) == 0)
							LOGI("look-around: L (%.3f,%.3f,%.3f) R (%.3f,%.3f,%.3f) m -> blocks L %.2f,%.2f R %.2f,%.2f",
							     g_eyes.pos[0][0], g_eyes.pos[0][1], g_eyes.pos[0][2], g_eyes.pos[1][0], g_eyes.pos[1][1],
							     g_eyes.pos[1][2], g_eye_off[0][0].load(), g_eye_off[0][1].load(), g_eye_off[1][0].load(),
							     g_eye_off[1][1].load());
					} else {
						g_eye_tracked = false;
					}
				} else {
					g_present_fail.fetch_add(1, std::memory_order_relaxed);
				}
			}
		}
	}
	const bool ready = g_enabled.load() && stereo_hooks_ok() && g_xr_running.load() &&
	                   g_xr.state() == WeaveState::Ready && !g_xr.stalled();
	stereo_frame_boundary(ready, w, h);  // next frame: both eyes, or plain 2D
}

thread_local bool t_in_swap = false;
EGLBoolean (*o_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;
EGLBoolean (*o_eglSwapBuffersWithDamage)(EGLDisplay, EGLSurface, EGLint *, EGLint) = nullptr;

// A swap on a thread whose current context changed since its last swap means
// the game restarted its renderer (pause/resume, surface loss): every GL object
// the mod and the transport own is gone with the old context.
void check_context()
{
	static thread_local EGLContext last = EGL_NO_CONTEXT;
	const EGLContext cur = eglGetCurrentContext();
	if (last != EGL_NO_CONTEXT && cur != EGL_NO_CONTEXT && cur != last) {
		LOGW("GL context changed on the presenting thread (%p -> %p): resetting", last, cur);
		stereo_context_reset();
		g_xr.on_context_reset();
	}
	last = cur;
}

EGLBoolean h_eglSwapBuffers(EGLDisplay dpy, EGLSurface surf)
{
	check_context();
	::crash_log_install();  // debug: re-arm in front of Breakpad
	if (!t_in_swap) {
		t_in_swap = true;
		on_swap(dpy, surf);
		EGLBoolean r = o_eglSwapBuffers(dpy, surf);
		t_in_swap = false;
		return r;
	}
	return o_eglSwapBuffers(dpy, surf);
}

EGLBoolean h_eglSwapBuffersWithDamage(EGLDisplay dpy, EGLSurface surf, EGLint *rects, EGLint n)
{
	check_context();
	::crash_log_install();  // debug: re-arm in front of Breakpad
	if (!t_in_swap) {
		t_in_swap = true;
		on_swap(dpy, surf);
		// A woven frame changed every pixel, so drop the damage hint.
		EGLBoolean r = o_eglSwapBuffersWithDamage(dpy, surf, g_woven_this_frame ? nullptr : rects,
		                                          g_woven_this_frame ? 0 : n);
		t_in_swap = false;
		return r;
	}
	return o_eglSwapBuffersWithDamage(dpy, surf, rects, n);
}

// ---- Java/Binder side -------------------------------------------------------
JavaVM *g_vm = nullptr;
jclass g_cls = nullptr;
jobject g_app = nullptr;  // global ref: the Context xrCreateInstance needs
jmethodID m_query = nullptr, m_connect = nullptr, m_path = nullptr, m_err = nullptr, m_info = nullptr;
jmethodID m_exit = nullptr, m_mem = nullptr;
std::string g_exit_report, g_mem_line;  // guarded by g_mu
std::atomic<bool> g_has_exit_report{false};

bool clear_exc(JNIEnv *env)
{
	if (env->ExceptionCheck()) {
		env->ExceptionClear();
		return true;
	}
	return false;
}

bool load_dex(JNIEnv *env)
{
	jclass at = env->FindClass("android/app/ActivityThread");
	if (clear_exc(env) || !at) return false;
	jmethodID cur = env->GetStaticMethodID(at, "currentApplication", "()Landroid/app/Application;");
	jobject app = cur ? env->CallStaticObjectMethod(at, cur) : nullptr;
	if (clear_exc(env) || !app) return false;
	jclass ctx_cls = env->FindClass("android/content/Context");
	jmethodID get_loader = env->GetMethodID(ctx_cls, "getClassLoader", "()Ljava/lang/ClassLoader;");
	jobject parent = env->CallObjectMethod(app, get_loader);
	if (clear_exc(env)) return false;

	jobject buf = env->NewDirectByteBuffer((void *)kBridgeDex, sizeof(kBridgeDex));
	jclass imdcl = env->FindClass("dalvik/system/InMemoryDexClassLoader");
	if (clear_exc(env) || !imdcl || !buf) return false;
	jmethodID ctor = env->GetMethodID(imdcl, "<init>", "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
	jobject loader = env->NewObject(imdcl, ctor, buf, parent);
	if (clear_exc(env) || !loader) return false;
	jclass cl_cls = env->FindClass("java/lang/ClassLoader");
	jmethodID load_class = env->GetMethodID(cl_cls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
	jstring name = env->NewStringUTF("com.displayxr.mc3d.BridgeReceiver");
	jclass cls = (jclass)env->CallObjectMethod(loader, load_class, name);
	if (clear_exc(env) || !cls) return false;
	g_cls = (jclass)env->NewGlobalRef(cls);
	jmethodID install = env->GetStaticMethodID(g_cls, "install", "(Landroid/content/Context;)V");
	m_query = env->GetStaticMethodID(g_cls, "queryStatus", "()[I");
	m_connect = env->GetStaticMethodID(g_cls, "connect", "()I");
	m_path = env->GetStaticMethodID(g_cls, "runtimePath", "()Ljava/lang/String;");
	m_err = env->GetStaticMethodID(g_cls, "lastError", "()Ljava/lang/String;");
	m_info = env->GetStaticMethodID(g_cls, "sendInfo", "(Ljava/lang/String;)Z");
	m_exit = env->GetStaticMethodID(g_cls, "lastExitReport", "()Ljava/lang/String;");
	m_mem = env->GetStaticMethodID(g_cls, "memoryLine", "()Ljava/lang/String;");
	if (clear_exc(env) || !install || !m_query || !m_connect || !m_path || !m_err || !m_info || !m_exit || !m_mem)
		return false;
	env->CallStaticVoidMethod(g_cls, install, app);
	if (clear_exc(env)) return false;
	g_app = env->NewGlobalRef(app);
	return true;
}

std::string java_string(JNIEnv *env, jmethodID m)
{
	jstring s = (jstring)env->CallStaticObjectMethod(g_cls, m);
	if (clear_exc(env) || !s) return "";
	const char *c = env->GetStringUTFChars(s, nullptr);
	std::string out = c ? c : "";
	env->ReleaseStringUTFChars(s, c);
	env->DeleteLocalRef(s);
	return out;
}

void try_start_transport(JNIEnv *env)
{
	set_problem("Connecting to DisplayXR...");
	const jint fd = env->CallStaticIntMethod(g_cls, m_connect);
	if (clear_exc(env) || fd < 0) {
		set_problem("Bridge app could not connect to DisplayXR: " + java_string(env, m_err));
		g_next_try_ms = now_ms() + kRetrySeconds * 1000;
		return;
	}
	const std::string path = java_string(env, m_path);
	LOGI("bridge gave socket fd %d, runtime %s", (int)fd, path.c_str());
	dxr::core::WeaveConfig c;
	c.vm = g_vm;
	c.activity = g_app;
	c.ipc_fd = fd;  // the transport owns it now
	c.runtime_lib_path = path.c_str();  // copied by start()
	c.app_name = "Minecraft 3D";
	if (g_xr.start(c)) {
		g_xr_running = true;
		set_problem("");
	} else {
		close(fd);
		set_problem("could not start the DisplayXR thread");
		g_next_try_ms = now_ms() + kRetrySeconds * 1000;
	}
}

void java_thread_main()
{
	JNIEnv *env = nullptr;
	if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
		set_problem("could not attach the bridge thread to Java");
		return;
	}
	for (int i = 0; i < 50 && !g_dex_ok; ++i) {
		g_dex_ok = load_dex(env);
		if (!g_dex_ok) std::this_thread::sleep_for(std::chrono::milliseconds(200));
	}
	if (!g_dex_ok) {
		set_problem("could not load the bridge Java code");
		return;
	}
	{
		std::string report = java_string(env, m_exit);
		clear_exc(env);
		std::lock_guard<std::mutex> l(g_mu);
		g_exit_report = report;
		g_has_exit_report = !report.empty();
	}
	for (;;) {
		{
			std::string mem = java_string(env, m_mem);
			clear_exc(env);
			std::lock_guard<std::mutex> l(g_mu);
			g_mem_line = mem;
		}
		jintArray a = (jintArray)env->CallStaticObjectMethod(g_cls, m_query);
		if (clear_exc(env) || !a) {
			g_connected = false;
		} else {
			jint s[6] = {};
			env->GetIntArrayRegion(a, 0, 6, s);
			env->DeleteLocalRef(a);
			g_connected = true;
			g_protocol = s[1];
			g_runtime_found = s[2];
		}
		if (g_connected.load() && g_enabled.load() && !g_xr_running.load() && g_protocol.load() >= 2 &&
		    g_runtime_found.load() && now_ms() >= g_next_try_ms.load())
			try_start_transport(env);

		std::string info;
		{
			std::unique_lock<std::mutex> l(g_mu);
			if (!g_info_dirty) g_cv.wait_for(l, std::chrono::milliseconds(500));
			if (g_info_dirty) {
				info = g_info;
				g_info_dirty = false;
			}
		}
		if (!info.empty() && g_connected.load()) {
			jstring js = env->NewStringUTF(info.c_str());
			env->CallStaticBooleanMethod(g_cls, m_info, js);
			clear_exc(env);
			env->DeleteLocalRef(js);
		}
	}
}

std::atomic<bool> g_thread_started{false};

} // namespace

void bridge_start(JavaVM *vm)
{
	g_vm = vm;
	if (!g_thread_started.exchange(true)) std::thread(java_thread_main).detach();
}

void bridge_install_hooks()
{
	void *egl = dlopen("libEGL.so", RTLD_NOW);
	if (!egl) return;
	bool ok = false;
	if (void *sym = dlsym(egl, "eglSwapBuffers"))
		ok = pl::memory::hook(sym, (void *)h_eglSwapBuffers, (void **)&o_eglSwapBuffers) == 0;
	void *dmg = dlsym(egl, "eglSwapBuffersWithDamageKHR");
	if (!dmg) dmg = (void *)eglGetProcAddress("eglSwapBuffersWithDamageKHR");
	if (dmg) pl::memory::hook(dmg, (void *)h_eglSwapBuffersWithDamage, (void **)&o_eglSwapBuffersWithDamage);
	g_hook_ok = ok;
	LOGI("eglSwapBuffers hook: %d (with damage: %p)", ok, dmg);
}

void bridge_set_enabled(bool enabled) { g_enabled = enabled; }

bool bridge_has_exit_report() { return g_has_exit_report.load(); }

bool bridge_3d_on() { return g_enabled.load() && g_xr_running.load() && g_xr.state() == WeaveState::Ready; }

void bridge_post_info(const std::string &text)
{
	std::lock_guard<std::mutex> l(g_mu);
	g_info = text;
	g_info_dirty = true;
	g_cv.notify_all();
}

std::vector<std::string> bridge_lines()
{
	static auto last_t = Clock::now();
	static uint64_t last_woven = 0, last_swaps = 0, last_ns = 0;
	static double woven_s = 0, swaps_s = 0, present_ms = 0;
	const auto now = Clock::now();
	const double dt = std::chrono::duration<double>(now - last_t).count();
	if (dt >= 1.0) {
		const uint64_t s = g_woven.load(), w = g_swaps.load(), ns = g_present_ns.load();
		present_ms = s > last_woven ? (ns - last_ns) / 1e6 / (s - last_woven) : 0;
		last_ns = ns;
		woven_s = (s - last_woven) / dt;
		swaps_s = (w - last_swaps) / dt;
		last_woven = s;
		last_swaps = w;
		last_t = now;
	}
	std::vector<std::string> out;
	char b[256];
	std::snprintf(b, sizeof b, "Frame hook %s   Java code %s   frames/s %.0f   window %dx%d  panel %dx%d",
	              g_hook_ok ? "ok" : "FAIL", g_dex_ok ? "ok" : "not loaded", swaps_s, g_win_w.load(), g_win_h.load(),
	              g_panel_w.load(), g_panel_h.load());
	out.emplace_back(b);
	if (!g_connected) {
		out.emplace_back("Bridge app: NOT CONNECTED. Open \"DisplayXR 3D Bridge\" and tap Turn 3D ON.");
	} else if (g_protocol.load() < 2) {
		out.emplace_back("Bridge app is too old: install the new one from the same release.");
	} else if (!g_runtime_found.load()) {
		out.emplace_back("Bridge app cannot find DisplayXR: install the DisplayXR runtime.");
	} else if (!g_xr_running.load()) {
		out.emplace_back("DisplayXR: not running (see the note below), will retry");
	} else if (g_xr.state() != WeaveState::Ready) {
		out.emplace_back("DisplayXR: starting...");
	} else {
		std::snprintf(b, sizeof b, "3D ON%s: %.0f frames/s, frame %.0f ms, of it DisplayXR %.0f ms, failed %llu, eye tracking %s",
		              stereo_is_flat() ? " (menu, one eye)" : "", woven_s, swaps_s > 0 ? 1000.0 / swaps_s : 0.0,
		              present_ms, (unsigned long long)g_present_fail.load(),
		              g_eyes_tracking.load() ? "LOCKED" : (g_eyes_valid.load() ? "fallback" : "no data yet"));
		out.emplace_back(b);
		std::snprintf(b, sizeof b, "Slowest so far: frame gap %d ms, DisplayXR %d ms, DisplayXR stopped answering %d times%s",
		              g_max_gap_ms.load(), g_max_present_ms.load(), g_stalls.load(),
		              g_xr.stalled() ? " (RIGHT NOW, showing 2D)" : "");
		out.emplace_back(b);
	}
	{
		std::lock_guard<std::mutex> l(g_mu);
		if (!g_problem.empty()) out.push_back("Note: " + g_problem.substr(0, 170));
		if (!g_mem_line.empty()) out.push_back(g_mem_line);
		size_t from = 0;
		while (from < g_exit_report.size()) {
			size_t nl = g_exit_report.find('\n', from);
			if (nl == std::string::npos) nl = g_exit_report.size();
			if (nl > from) out.push_back(g_exit_report.substr(from, nl - from));
			from = nl + 1;
		}
	}
	if (!g_enabled) out.emplace_back("3D switched off in the mod menu.");
	return out;
}

} // namespace dxr3d
