// core/transport_weave.cpp — see transport_weave.h.
//
// Reference for the protocol: runtime test_apps/weave/weave_client_vk_android
// (the Android present-owner) and docs/specs/extensions/XR_DXR_weave.md
// §5b / §5b-sync. Differences from that app: GLES on the caller side
// (AHardwareBuffer <-> EGLImage instead of Vulkan imports), and the XR calls
// live on a dedicated Looper-owning thread instead of the render thread, and
// the service connection is brokered (an adopted fd) because the host process
// has no package visibility to the runtime.

#include "transport_weave.h"

#define EGL_EGLEXT_PROTOTYPES 0
#define GL_GLEXT_PROTOTYPES 0
#include <chrono>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <android/looper.h>
#include <dlfcn.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <vulkan/vulkan.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/XR_DXR_weave.h>
#include <openxr/XR_DXR_display_info.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cmath>
#include <algorithm>
#include <sys/system_properties.h>

#define TLOG(prio, ...) __android_log_print(prio, "dxr-weave", __VA_ARGS__)
#define LOGI(...) TLOG(ANDROID_LOG_INFO, __VA_ARGS__)
#define LOGW(...) TLOG(ANDROID_LOG_WARN, __VA_ARGS__)
#define LOGE(...) TLOG(ANDROID_LOG_ERROR, __VA_ARGS__)

namespace dxr::core {

constexpr int kSubmitWaitMs = 500;

namespace {

// ---- EGL / GLES extension entry points (resolved once, on the render thread) ----
struct EglExt {
	PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC getNativeClientBuffer = nullptr;
	PFNEGLCREATEIMAGEKHRPROC createImage = nullptr;
	PFNEGLDESTROYIMAGEKHRPROC destroyImage = nullptr;
	PFNEGLCREATESYNCKHRPROC createSync = nullptr;
	PFNEGLDESTROYSYNCKHRPROC destroySync = nullptr;
	PFNEGLWAITSYNCKHRPROC waitSync = nullptr;
	PFNEGLDUPNATIVEFENCEFDANDROIDPROC dupNativeFenceFd = nullptr;
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageTargetTexture2D = nullptr;
	bool have_fences = false;

	bool load()
	{
		getNativeClientBuffer =
		    (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress("eglGetNativeClientBufferANDROID");
		createImage = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
		destroyImage = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
		createSync = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
		destroySync = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
		waitSync = (PFNEGLWAITSYNCKHRPROC)eglGetProcAddress("eglWaitSyncKHR");
		dupNativeFenceFd = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress("eglDupNativeFenceFDANDROID");
		imageTargetTexture2D =
		    (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
		have_fences = createSync && destroySync && waitSync && dupNativeFenceFd;
		return getNativeClientBuffer && createImage && destroyImage && imageTargetTexture2D;
	}
};

// One AHardwareBuffer seen by GL as a texture + framebuffer.
struct AhbTarget {
	AHardwareBuffer *ahb = nullptr;
	EGLImageKHR image = EGL_NO_IMAGE_KHR;
	GLuint tex = 0, fbo = 0;
	uint32_t w = 0, h = 0;
	bool owned = false;  // true = we allocated it; false = runtime-owned (we hold a reference)

	void destroy(const EglExt &x)
	{
		if (fbo) glDeleteFramebuffers(1, &fbo);
		if (tex) glDeleteTextures(1, &tex);
		if (image != EGL_NO_IMAGE_KHR) x.destroyImage(eglGetCurrentDisplay(), image);
		if (ahb) AHardwareBuffer_release(ahb);  // both cases hold exactly one reference
		*this = AhbTarget{};
	}

	// Drop the GL side only (context replaced); the buffer reference is kept and
	// rewrap() rebuilds the image / texture / framebuffer on the new context.
	void drop_gl(const EglExt &x)
	{
		if (fbo) glDeleteFramebuffers(1, &fbo);
		if (tex) glDeleteTextures(1, &tex);
		if (image != EGL_NO_IMAGE_KHR) x.destroyImage(eglGetCurrentDisplay(), image);
		fbo = tex = 0;
		image = EGL_NO_IMAGE_KHR;
	}
	bool rewrap(const EglExt &x)
	{
		AHardwareBuffer *b = ahb;
		const bool own = owned;
		return b ? wrap(x, b, own) : false;
	}

	bool wrap(const EglExt &x, AHardwareBuffer *buf, bool we_own)
	{
		AHardwareBuffer_Desc d{};
		AHardwareBuffer_describe(buf, &d);
		ahb = buf;
		owned = we_own;
		w = d.width;
		h = d.height;
		EGLClientBuffer cb = x.getNativeClientBuffer(buf);
		const EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
		image = x.createImage(eglGetCurrentDisplay(), EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, cb, attrs);
		if (image == EGL_NO_IMAGE_KHR) {
			LOGE("eglCreateImageKHR(AHardwareBuffer %ux%u) failed: 0x%x", w, h, eglGetError());
			return false;
		}
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);
		x.imageTargetTexture2D(GL_TEXTURE_2D, (GLeglImageOES)image);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glGenFramebuffers(1, &fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
		const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if (st != GL_FRAMEBUFFER_COMPLETE) {
			LOGE("AHardwareBuffer FBO incomplete: 0x%x", st);
			return false;
		}
		return true;
	}
};

// Single-sample scratch target used to resolve a multisampled eye before the
// (scaling) squeeze blit — GLES forbids scaling blits out of an MSAA source.
struct Resolve {
	GLuint rb = 0, fbo = 0;
	int w = 0, h = 0;
	void destroy()
	{
		if (fbo) glDeleteFramebuffers(1, &fbo);
		if (rb) glDeleteRenderbuffers(1, &rb);
		*this = Resolve{};
	}
	bool ensure(int nw, int nh)
	{
		if (fbo && w == nw && h == nh) return true;
		destroy();
		w = nw;
		h = nh;
		glGenRenderbuffers(1, &rb);
		glBindRenderbuffer(GL_RENDERBUFFER, rb);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
		glGenFramebuffers(1, &fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
		return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
	}
};

// Saves and restores the bits of GL state present() disturbs.
struct GlStateGuard {
	GLint read_fbo = 0, draw_fbo = 0, rb = 0, tex = 0, active = 0;
	GLboolean scissor = GL_FALSE, discard = GL_FALSE;
	GlStateGuard()
	{
		glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
		glGetIntegerv(GL_RENDERBUFFER_BINDING, &rb);
		glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
		glActiveTexture(GL_TEXTURE0);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
		scissor = glIsEnabled(GL_SCISSOR_TEST);
		discard = glIsEnabled(GL_RASTERIZER_DISCARD);
		glDisable(GL_SCISSOR_TEST);       // blits obey the scissor
		glDisable(GL_RASTERIZER_DISCARD); // ... and rasterizer discard
	}
	~GlStateGuard()
	{
		glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_fbo);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw_fbo);
		glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)rb);
		glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
		glActiveTexture((GLenum)active);
		if (scissor) glEnable(GL_SCISSOR_TEST);
		if (discard) glEnable(GL_RASTERIZER_DISCARD);
	}
};

constexpr int kLooperIdentCmd = 1;

// OpenXR entry points, resolved through whichever xrGetInstanceProcAddr we got:
// the runtime's own (direct load, the brokered default) or the Khronos loader's.
// The core never links an OpenXR library, so neither path is a link-time choice.
struct XrFns {
	PFN_xrGetInstanceProcAddr gipa = nullptr;
	PFN_xrEnumerateInstanceExtensionProperties enumerateInstanceExtensionProperties = nullptr;
	PFN_xrCreateInstance createInstance = nullptr;
	PFN_xrDestroyInstance destroyInstance = nullptr;
	PFN_xrGetSystem getSystem = nullptr;
	PFN_xrGetSystemProperties getSystemProperties = nullptr;
	PFN_xrCreateSession createSession = nullptr;
	PFN_xrDestroySession destroySession = nullptr;

	template <class T> bool get(XrInstance inst, const char *name, T *out)
	{
		*out = nullptr;
		return gipa(inst, name, reinterpret_cast<PFN_xrVoidFunction *>(out)) == XR_SUCCESS && *out != nullptr;
	}
	// The pre-instance set (valid with XR_NULL_HANDLE).
	bool load_global()
	{
		return get(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &enumerateInstanceExtensionProperties) &&
		       get(XR_NULL_HANDLE, "xrCreateInstance", &createInstance);
	}
	bool load_instance(XrInstance inst)
	{
		return get(inst, "xrDestroyInstance", &destroyInstance) && get(inst, "xrGetSystem", &getSystem) &&
		       get(inst, "xrGetSystemProperties", &getSystemProperties) &&
		       get(inst, "xrCreateSession", &createSession) && get(inst, "xrDestroySession", &destroySession);
	}
};

} // namespace

// =============================================================================
// Shared state between the render thread and the session thread.
// =============================================================================
struct WeaveTransport::Impl {
	WeaveConfig cfg;
	std::string lib_path, app_name;  // owned copies of cfg's strings

	// ---- session thread ----
	std::thread thread;
	int cmd_fd = -1;  // eventfd registered on the session thread's ALooper

	// ---- XR objects (session thread only) ----
	XrFns xr;
	void *runtime_lib = nullptr;  // direct load: never dlclose'd (the runtime may own threads)
	XrInstance instance = XR_NULL_HANDLE;
	XrSystemId system = XR_NULL_SYSTEM_ID;
	XrSession session = XR_NULL_HANDLE;
	VkInstance vk_instance = VK_NULL_HANDLE;
	VkPhysicalDevice vk_phys = VK_NULL_HANDLE;
	VkDevice vk_device = VK_NULL_HANDLE;
	uint32_t vk_family = 0;
	PFN_xrWeaveBindWindow2DXR bind2 = nullptr;
	PFN_xrWeaveSubmitDXR submit = nullptr;
	uint32_t weave_version = 0;

	// ---- mailbox (guarded by mu) ----
	mutable std::mutex mu;
	std::condition_variable cv;
	std::atomic<WeaveState> st{WeaveState::Stopped};
	std::string error;
	DisplayInfo display;
	EyeSample eyes;
	bool quit = false;

	bool geom_dirty = false;
	int32_t gx = 0, gy = 0, gdisp = -1;
	uint32_t gw = 0, gh = 0;

	struct Job {
		bool pending = false, done = false;
		AHardwareBuffer *input = nullptr;
		uint32_t w = 0, h = 0;
		int acquire_fd = -1;
		// results
		XrResult result = XR_ERROR_RUNTIME_FAILURE;
		AHardwareBuffer *woven = nullptr;  // non-null on first submit / resize
		int release_fd = -1;
	} job;
	std::atomic<bool> stalled{false};  // a submit outlived kSubmitWait; job still in flight

	// ---- render thread GL state ----
	EglExt egl;
	bool egl_ok = false;
	AhbTarget input, output;
	Resolve resolve;

	// -------------------------------------------------------------------------
	void fail(const char *fmt, ...) __attribute__((format(printf, 2, 3)))
	{
		char buf[512];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(buf, sizeof buf, fmt, ap);
		va_end(ap);
		LOGE("%s", buf);
		std::lock_guard<std::mutex> lk(mu);
		error = buf;
		st = WeaveState::Failed;
		cv.notify_all();
	}

	void wake() const
	{
		const uint64_t one = 1;
		if (cmd_fd >= 0) (void)!write(cmd_fd, &one, sizeof one);
	}

	// ======================= session thread ===================================
	void thread_main()
	{
		JNIEnv *env = nullptr;
		JavaVMAttachArgs args{JNI_VERSION_1_6, "dxr-weave-session", nullptr};
		const bool attached = cfg.vm && cfg.vm->AttachCurrentThread(&env, &args) == JNI_OK;

		// The session thread owns a Looper for its whole life (runtime #2490).
		// The command eventfd sits on it, so one ALooper_pollOnce both waits for
		// work and services whatever the runtime or a display processor posts.
		ALooper *looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
		ALooper_addFd(looper, cmd_fd, kLooperIdentCmd, ALOOPER_EVENT_INPUT, nullptr, nullptr);

		if (bring_up()) {
			{
				std::lock_guard<std::mutex> lk(mu);
				st = WeaveState::Ready;
			}
			cv.notify_all();
			LOGI("weave transport ready (XR_DXR_weave v%u, sync_fd %s)", weave_version,
			     use_sync() ? "on" : "off");
		}

		for (;;) {
			const int id = ALooper_pollOnce(-1, nullptr, nullptr, nullptr);
			if (id == kLooperIdentCmd) {
				uint64_t v;
				(void)!read(cmd_fd, &v, sizeof v);
			}
			std::unique_lock<std::mutex> lk(mu);
			if (quit) break;
			if (st != WeaveState::Ready) {
				// Failed: answer any job so the render thread never blocks.
				if (job.pending) {
					job.pending = false;
					job.done = true;
					job.result = XR_ERROR_RUNTIME_FAILURE;
					cv.notify_all();
				}
				continue;
			}
			if (geom_dirty) {
				geom_dirty = false;
				const int32_t x = gx, y = gy, d = gdisp;
				const uint32_t w = gw, h = gh;
				lk.unlock();
				bind_geometry(x, y, w, h, d);
				lk.lock();
			}
			if (job.pending) {
				Job j = job;  // copy inputs; the render thread is blocked waiting
				lk.unlock();
				do_submit(j);
				lk.lock();
				job = j;
				job.pending = false;
				job.done = true;
				cv.notify_all();
			}
		}

		tear_down();
		ALooper_removeFd(looper, cmd_fd);
		if (attached) cfg.vm->DetachCurrentThread();
	}

	bool use_sync() const { return cfg.use_sync_fd && weave_version >= 20; }

	template <class T> bool proc(const char *name, T *out)
	{
		return xr.get(instance, name, out);
	}

	/*
	 * Get an xrGetInstanceProcAddr and hand the runtime the brokered service
	 * connection. Two shapes (docs/track4-core.md §3.6):
	 *
	 *  DIRECT (cfg.runtime_lib_path set — the brokered default): dlopen the
	 *  runtime client library by absolute path out of the runtime APK's
	 *  nativeLibraryDir (the broker sends it), negotiate with it the way the
	 *  Khronos loader does, and adopt the fd through the runtime's exported
	 *  ipc_client_connection_adopt_fd(). No loader, no ContentProvider query,
	 *  no createPackageContext, no package visibility.
	 *
	 *  LOADER (runtime_lib_path null): dlopen the Khronos loader shipped with
	 *  the mod. Needs the host to have <queries> for the runtime. The adopt
	 *  symbol is not reachable (the loader dlopens the runtime RTLD_LOCAL), so
	 *  the fd goes through DXR_IPC_FD instead.
	 */
	bool load_runtime()
	{
		if (cfg.runtime_lib_path && cfg.runtime_lib_path[0]) {
			runtime_lib = dlopen(cfg.runtime_lib_path, RTLD_NOW | RTLD_LOCAL);
			if (!runtime_lib) {
				fail("dlopen(%s) failed: %s", cfg.runtime_lib_path, dlerror());
				return false;
			}
			auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(
			    dlsym(runtime_lib, "xrNegotiateLoaderRuntimeInterface"));
			if (!negotiate) {
				fail("%s exports no xrNegotiateLoaderRuntimeInterface", cfg.runtime_lib_path);
				return false;
			}
			XrNegotiateLoaderInfo li{};
			li.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
			li.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
			li.structSize = sizeof(li);
			li.minInterfaceVersion = 1;
			li.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
			li.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
			li.maxApiVersion = XR_CURRENT_API_VERSION;
			XrNegotiateRuntimeRequest rr{};
			rr.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
			rr.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
			rr.structSize = sizeof(rr);
			const XrResult nr = negotiate(&li, &rr);
			if (nr != XR_SUCCESS || !rr.getInstanceProcAddr) {
				fail("runtime negotiation failed: %d", (int)nr);
				return false;
			}
			xr.gipa = rr.getInstanceProcAddr;

			if (cfg.ipc_fd >= 0) {
				using adopt_fn = void (*)(int);
				auto adopt = reinterpret_cast<adopt_fn>(dlsym(runtime_lib, "ipc_client_connection_adopt_fd"));
				if (!adopt) {
					fail("runtime exports no ipc_client_connection_adopt_fd (predates fd adoption)");
					return false;
				}
				adopt(cfg.ipc_fd);  // the runtime dups it; ours is closed after xrCreateInstance
			}
		} else {
			void *loader = dlopen("libopenxr_loader.so", RTLD_NOW | RTLD_LOCAL);
			if (!loader) {
				fail("no runtime path from the broker and no libopenxr_loader.so: %s", dlerror());
				return false;
			}
			xr.gipa = reinterpret_cast<PFN_xrGetInstanceProcAddr>(dlsym(loader, "xrGetInstanceProcAddr"));
			if (!xr.gipa) {
				fail("libopenxr_loader.so exports no xrGetInstanceProcAddr");
				return false;
			}
			if (cfg.ipc_fd >= 0) {
				char buf[16];
				snprintf(buf, sizeof buf, "%d", cfg.ipc_fd);
				setenv("DXR_IPC_FD", buf, 1);  // one bionic libc per process: the runtime sees it
			}
		}
		if (!xr.load_global()) {
			fail("xrEnumerateInstanceExtensionProperties / xrCreateInstance not resolvable");
			return false;
		}
		return true;
	}

	// Our copy of the brokered fd is not needed once xrCreateInstance has run:
	// the runtime takes its own dup when it connects.
	void release_broker_fd()
	{
		if (cfg.ipc_fd >= 0) {
			if (!cfg.runtime_lib_path || !cfg.runtime_lib_path[0]) unsetenv("DXR_IPC_FD");
			close(cfg.ipc_fd);
			cfg.ipc_fd = -1;
		}
	}

	bool bring_up()
	{
		if (!load_runtime()) return false;

		// Loader-init data (XR_KHR_loader_init). Harmless on the direct path —
		// the runtime exposes the entry itself — and required by the loader.
		PFN_xrInitializeLoaderKHR init_loader = nullptr;
		if (xr.get(XR_NULL_HANDLE, "xrInitializeLoaderKHR", &init_loader)) {
			XrLoaderInitInfoAndroidKHR li{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
			li.applicationVM = cfg.vm;
			li.applicationContext = cfg.activity;
			init_loader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR *>(&li));
		}

		// Extension discovery: weave is mandatory, display_info optional.
		uint32_t n = 0;
		xr.enumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
		std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
		xr.enumerateInstanceExtensionProperties(nullptr, n, &n, props.data());
		bool have_display_info = false;
		for (auto &p : props) {
			if (!strcmp(p.extensionName, XR_DXR_WEAVE_EXTENSION_NAME)) weave_version = p.extensionVersion;
			if (!strcmp(p.extensionName, XR_DXR_DISPLAY_INFO_EXTENSION_NAME)) have_display_info = true;
		}
		if (weave_version == 0) {
			fail("runtime does not advertise XR_DXR_weave (runtime too old, or not the DisplayXR runtime)");
			return false;
		}

		std::vector<const char *> exts = {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
		                                  XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, XR_DXR_WEAVE_EXTENSION_NAME};
		if (have_display_info) exts.push_back(XR_DXR_DISPLAY_INFO_EXTENSION_NAME);

		XrInstanceCreateInfoAndroidKHR ai{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
		ai.applicationVM = cfg.vm;
		ai.applicationActivity = cfg.activity;
		XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
		ci.next = &ai;
		ci.enabledExtensionCount = (uint32_t)exts.size();
		ci.enabledExtensionNames = exts.data();
		strncpy(ci.applicationInfo.applicationName, cfg.app_name, XR_MAX_APPLICATION_NAME_SIZE - 1);
		strncpy(ci.applicationInfo.engineName, "dxr-gles-stereo", XR_MAX_ENGINE_NAME_SIZE - 1);
		ci.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

		XrResult r = XR_ERROR_RUNTIME_UNAVAILABLE;
		for (int attempt = 0; attempt < 5 && r == XR_ERROR_RUNTIME_UNAVAILABLE; ++attempt) {
			r = xr.createInstance(&ci, &instance);
			if (r == XR_ERROR_RUNTIME_UNAVAILABLE) std::this_thread::sleep_for(std::chrono::seconds(1));
		}
		release_broker_fd();  // the runtime consumed (dup'd) it during xrCreateInstance, or never will
		if (r != XR_SUCCESS) {
			fail("xrCreateInstance failed: %d", (int)r);
			return false;
		}
		if (!xr.load_instance(instance)) {
			fail("core OpenXR entry points missing");
			return false;
		}

		XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
		sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
		if (xr.getSystem(instance, &sgi, &system) != XR_SUCCESS) {
			sgi.formFactor = XR_FORM_FACTOR_HANDHELD_DISPLAY;
			if ((r = xr.getSystem(instance, &sgi, &system)) != XR_SUCCESS) {
				fail("xrGetSystem failed: %d", (int)r);
				return false;
			}
		}

		if (have_display_info) {
			XrDisplayInfoDXR di{XR_TYPE_DISPLAY_INFO_DXR};
			XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
			sp.next = &di;
			if (xr.getSystemProperties(instance, system, &sp) == XR_SUCCESS) {
				std::lock_guard<std::mutex> lk(mu);
				display.width_m = di.displaySizeMeters.width;
				display.height_m = di.displaySizeMeters.height;
				display.pixel_w = di.displayPixelWidth;
				display.pixel_h = di.displayPixelHeight;
				display.nominal_viewer[0] = di.nominalViewerPositionInDisplaySpace.x;
				display.nominal_viewer[1] = di.nominalViewerPositionInDisplaySpace.y;
				display.nominal_viewer[2] = di.nominalViewerPositionInDisplaySpace.z;
			}
		}

		if (!create_session()) return false;

		if (!proc("xrWeaveBindWindow2DXR", &bind2) || !proc("xrWeaveSubmitDXR", &submit)) {
			fail("weave entry points missing (runtime predates XR_DXR_weave v7)");
			return false;
		}

		// First bind. Android has no window handle: geometry is the phase anchor.
		int32_t x, y, d;
		uint32_t w, h;
		{
			std::lock_guard<std::mutex> lk(mu);
			x = gx, y = gy, w = gw, h = gh, d = gdisp;
			geom_dirty = false;
		}
		if (w && h) bind_geometry(x, y, w, h, d);
		return true;
	}

	// A present-owner session never begins a frame loop; it only needs to
	// exist. The graphics binding is a private, never-used Vulkan device (the
	// reference present-owner's shape). The GLES binding would avoid it but is
	// unexercised on Android today.
	bool create_session()
	{
		PFN_xrGetVulkanGraphicsRequirements2KHR get_reqs = nullptr;
		PFN_xrCreateVulkanInstanceKHR create_vk_instance = nullptr;
		PFN_xrGetVulkanGraphicsDevice2KHR get_phys = nullptr;
		PFN_xrCreateVulkanDeviceKHR create_vk_device = nullptr;
		if (!proc("xrGetVulkanGraphicsRequirements2KHR", &get_reqs) ||
		    !proc("xrCreateVulkanInstanceKHR", &create_vk_instance) ||
		    !proc("xrGetVulkanGraphicsDevice2KHR", &get_phys) || !proc("xrCreateVulkanDeviceKHR", &create_vk_device)) {
			fail("XR_KHR_vulkan_enable2 entry points missing");
			return false;
		}
		XrGraphicsRequirementsVulkanKHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
		get_reqs(instance, system, &reqs);

		VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
		app.pApplicationName = cfg.app_name;
		app.apiVersion = VK_MAKE_VERSION(XR_VERSION_MAJOR(reqs.minApiVersionSupported),
		                                 XR_VERSION_MINOR(reqs.minApiVersionSupported), 0);
		VkInstanceCreateInfo vci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
		vci.pApplicationInfo = &app;
		XrVulkanInstanceCreateInfoKHR xvi{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
		xvi.systemId = system;
		xvi.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
		xvi.vulkanCreateInfo = &vci;
		VkResult vr = VK_SUCCESS;
		if (create_vk_instance(instance, &xvi, &vk_instance, &vr) != XR_SUCCESS || vr != VK_SUCCESS) {
			fail("xrCreateVulkanInstanceKHR failed (vk %d)", (int)vr);
			return false;
		}
		XrVulkanGraphicsDeviceGetInfoKHR dgi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
		dgi.systemId = system;
		dgi.vulkanInstance = vk_instance;
		if (get_phys(instance, &dgi, &vk_phys) != XR_SUCCESS) {
			fail("xrGetVulkanGraphicsDevice2KHR failed");
			return false;
		}
		uint32_t qn = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(vk_phys, &qn, nullptr);
		std::vector<VkQueueFamilyProperties> qf(qn);
		vkGetPhysicalDeviceQueueFamilyProperties(vk_phys, &qn, qf.data());
		for (uint32_t i = 0; i < qn; ++i)
			if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
				vk_family = i;
				break;
			}
		const float prio = 1.0f;
		VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
		qci.queueFamilyIndex = vk_family;
		qci.queueCount = 1;
		qci.pQueuePriorities = &prio;
		VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
		dci.queueCreateInfoCount = 1;
		dci.pQueueCreateInfos = &qci;
		XrVulkanDeviceCreateInfoKHR xdi{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
		xdi.systemId = system;
		xdi.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
		xdi.vulkanPhysicalDevice = vk_phys;
		xdi.vulkanCreateInfo = &dci;
		if (create_vk_device(instance, &xdi, &vk_device, &vr) != XR_SUCCESS || vr != VK_SUCCESS) {
			fail("xrCreateVulkanDeviceKHR failed (vk %d)", (int)vr);
			return false;
		}

		XrGraphicsBindingVulkanKHR gb{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
		gb.instance = vk_instance;
		gb.physicalDevice = vk_phys;
		gb.device = vk_device;
		gb.queueFamilyIndex = vk_family;
		gb.queueIndex = 0;
		XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
		sci.next = &gb;
		sci.systemId = system;
		const XrResult r = xr.createSession(instance, &sci, &session);
		if (r != XR_SUCCESS) {
			fail("xrCreateSession failed: %d", (int)r);
			return false;
		}
		{	// debug.mc3d.eyes_manual=1: MANUAL eye-tracking mode, so the vendor's
			// no-face single view stands down (the game stays 3D with nobody in
			// front of the panel; needs runtime support for weave-only clients).
			char v[PROP_VALUE_MAX] = {0};
			__system_property_get("debug.mc3d.eyes_manual", v);
			if (v[0] == '1') {
				PFN_xrRequestEyeTrackingModeDXR req = nullptr;
				const bool got = xr.gipa(instance, "xrRequestEyeTrackingModeDXR",
				                         reinterpret_cast<PFN_xrVoidFunction *>(&req)) == XR_SUCCESS && req;
				const XrResult rr = got ? req(session, XR_EYE_TRACKING_MODE_MANUAL_DXR) : XR_ERROR_FUNCTION_UNSUPPORTED;
				LOGW("eye-tracking mode MANUAL requested -> %d", (int)rr);
			}
		}
		return true;
	}

	void bind_geometry(int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t d)
	{
		XrWeaveWindowGeometryDXR g{XR_TYPE_WEAVE_WINDOW_GEOMETRY_DXR};
		g.windowOriginOnScreen = {x, y};
		g.clientSize = {(int32_t)w, (int32_t)h};
		g.displayId = d;
		XrWeaveBindWindowInfoDXR b{XR_TYPE_WEAVE_BIND_WINDOW_INFO_DXR};
		b.next = &g;
		b.windowHandle = nullptr;  // Android: the Surface stays the game's
		const XrResult r = bind2(session, &b);
		LOGI("bind window %d,%d %ux%u display %d -> %d", x, y, w, h, d, (int)r);
	}

	void do_submit(Job &j)
	{
		// Legacy (non-batch) layout: the whole input is one 2x1 SBS atlas whose
		// size equals the rect; the rect is the full window.
		XrWeaveSubmitHandlesDXR kinds{XR_TYPE_WEAVE_SUBMIT_HANDLES_DXR};
		kinds.inputKind = XR_WEAVE_HANDLE_KIND_AHARDWAREBUFFER_DXR;
		kinds.overlayKind = XR_WEAVE_HANDLE_KIND_PLATFORM_DEFAULT_DXR;

		XrWeaveSubmitInfoDXR info{XR_TYPE_WEAVE_SUBMIT_INFO_DXR};
		info.next = &kinds;
		info.inputTexture = j.input;
		info.rect = {{0, 0}, {(int32_t)j.w, (int32_t)j.h}};
		info.firstChunk = XR_TRUE;

		XrWeaveOutputDXR out{XR_TYPE_WEAVE_OUTPUT_DXR};
		XrWeaveSubmitSyncDXR sin{XR_TYPE_WEAVE_SUBMIT_SYNC_DXR};
		XrWeaveOutputSyncDXR sout{XR_TYPE_WEAVE_OUTPUT_SYNC_DXR};
		if (use_sync()) {
			sin.next = info.next;
			sin.acquireFenceFd = j.acquire_fd;  // runtime's on XR_SUCCESS
			info.next = &sin;
			sout.releaseFenceFd = -1;
			out.next = &sout;
		}

		j.result = submit(session, &info, &out);
		j.woven = nullptr;
		j.release_fd = -1;
		if (j.result != XR_SUCCESS) {
			LOGW("xrWeaveSubmitDXR failed: %d", (int)j.result);
			return;  // acquire fd stays the caller's (render thread closes it)
		}
		j.acquire_fd = -1;  // consumed
		j.woven = static_cast<AHardwareBuffer *>(out.weavedTexture);
		j.release_fd = use_sync() ? sout.releaseFenceFd : -1;

		std::lock_guard<std::mutex> lk(mu);
		eyes.valid = out.eyesValid == XR_TRUE;
		eyes.tracking = out.eyesTracking == XR_TRUE;
		eyes.count = out.eyeCount > 8 ? 8 : out.eyeCount;
		for (uint32_t i = 0; i < eyes.count; ++i) {
			eyes.pos[i][0] = out.eyes[i].x;
			eyes.pos[i][1] = out.eyes[i].y;
			eyes.pos[i][2] = out.eyes[i].z;
		}
	}

	void tear_down()
	{
		release_broker_fd();
		if (session != XR_NULL_HANDLE) xr.destroySession(session);
		if (vk_device != VK_NULL_HANDLE) vkDestroyDevice(vk_device, nullptr);
		if (vk_instance != VK_NULL_HANDLE) vkDestroyInstance(vk_instance, nullptr);
		if (instance != XR_NULL_HANDLE) xr.destroyInstance(instance);
		session = XR_NULL_HANDLE;
		vk_device = VK_NULL_HANDLE;
		vk_instance = VK_NULL_HANDLE;
		instance = XR_NULL_HANDLE;
	}

	// ======================= render thread ====================================
	bool ensure_input(uint32_t w, uint32_t h)
	{
		if (input.ahb && input.w == w && input.h == h) return true;
		input.destroy(egl);
		AHardwareBuffer_Desc d{};
		d.width = w;
		d.height = h;
		d.layers = 1;
		d.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
		d.usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
		AHardwareBuffer *buf = nullptr;
		if (AHardwareBuffer_allocate(&d, &buf) != 0 || !buf) {
			LOGE("AHardwareBuffer_allocate %ux%u failed", w, h);
			return false;
		}
		return input.wrap(egl, buf, true);
	}

	// Squeeze one eye (w x h, GL y-up) into its half of the SBS input
	// (AHardwareBuffer rows are top-down, so flip Y).
	// sx/sy: shift of the picture in source pixels (x right, y up in GL terms);
	// the uncovered strip is black.
	void squeeze_eye(GLuint src_fbo, int w, int h, int dst_x0, int dst_x1, float sx = 0.f, float sy = 0.f)
	{
		GLuint src = src_fbo;
		glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
		GLint samples = 0;
		glGetFramebufferParameteriv_safe(src_fbo, &samples);
		// A multisampled source must be resolved 1:1 before the scaling blit.
		// GLES requires matching formats for an MSAA resolve: the scratch
		// target is RGBA8, which matches the usual window config; a 565 or
		// 1010102 window needs the scratch to follow the EGL config (open
		// item, same root cause as the window-twin format bug).
		if (samples > 0 && resolve.ensure(w, h)) {
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve.fbo);
			glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
			src = resolve.fbo;
			glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
		}
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, input.fbo);
		if (sx == 0.f && sy == 0.f) {
			glBlitFramebuffer(0, 0, w, h, dst_x0, h, dst_x1, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
			return;
		}
		// The half is squeezed 2:1 horizontally, so a source shift of sx lands as sx/2.
		const int dx = (int)lroundf(sx * (dst_x1 - dst_x0) / (float)w);
		const int dy = (int)lroundf(sy);  // dest rows are top-down: up in GL = smaller row
		GLboolean had_scissor = glIsEnabled(GL_SCISSOR_TEST);
		GLint box[4];
		glGetIntegerv(GL_SCISSOR_BOX, box);
		glEnable(GL_SCISSOR_TEST);
		glScissor(dst_x0, 0, dst_x1 - dst_x0, h);
		const GLfloat black[4] = {0, 0, 0, 1};
		glClearBufferfv(GL_COLOR, 0, black);
		// Clip the shifted copy to the half by hand: a destination outside the
		// target (negative x) is legal GL but this GPU's blit mangles it.
		const float sxw = (float)w / (float)(dst_x1 - dst_x0);
		const int dX0 = std::max(dst_x0, dst_x0 + dx), dX1 = std::min(dst_x1, dst_x1 + dx);
		const int sX0 = (int)lroundf((dX0 - (dst_x0 + dx)) * sxw), sX1 = (int)lroundf((dX1 - (dst_x0 + dx)) * sxw);
		const int sY0 = std::max(0, -dy), sY1 = std::min(h, h - dy);  // dest rows top-down: row = h - y - dy
		const int dY0 = h - sY0 - dy, dY1 = h - sY1 - dy;
		if (dX1 > dX0 && sY1 > sY0)
			glBlitFramebuffer(sX0, sY0, sX1, sY1, dX0, dY0, dX1, dY1, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		glScissor(box[0], box[1], box[2], box[3]);
		if (!had_scissor) glDisable(GL_SCISSOR_TEST);
	}

	// GL_SAMPLES is a property of the DRAW framebuffer in GLES 3.0, so bind the
	// candidate as draw to query it.
	static void glGetFramebufferParameteriv_safe(GLuint fbo, GLint *samples)
	{
		GLint prev = 0;
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
		glGetIntegerv(GL_SAMPLES, samples);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev);
	}

	int make_acquire_fence()
	{
		if (!egl.have_fences) {
			glFinish();  // v9 contract: writes complete before submit
			return -1;
		}
		EGLDisplay dpy = eglGetCurrentDisplay();
		EGLSyncKHR s = egl.createSync(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
		glFlush();  // the fence fd materialises only once the fence is flushed
		const int fd = s != EGL_NO_SYNC_KHR ? egl.dupNativeFenceFd(dpy, s) : -1;
		if (s != EGL_NO_SYNC_KHR) egl.destroySync(dpy, s);
		if (fd < 0) glFinish();
		return fd;
	}

	void gpu_wait_release(int fd)
	{
		if (fd < 0) return;  // output already complete
		EGLDisplay dpy = eglGetCurrentDisplay();
		const EGLint attrs[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fd, EGL_NONE};
		EGLSyncKHR s = egl.createSync(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, attrs);
		if (s == EGL_NO_SYNC_KHR) {
			close(fd);  // not adopted; fall back to a CPU-side finish
			glFinish();
			return;
		}
		egl.waitSync(dpy, s, 0);  // EGL owns fd now
		egl.destroySync(dpy, s);
	}
};

// =============================================================================
// Public API
// =============================================================================
WeaveTransport::WeaveTransport() : m_(new Impl) {}
WeaveTransport::~WeaveTransport()
{
	stop();
	delete m_;
}

bool WeaveTransport::start(const WeaveConfig &cfg)
{
	if (m_->thread.joinable()) return true;
	m_->cfg = cfg;
	m_->lib_path = cfg.runtime_lib_path ? cfg.runtime_lib_path : "";
	m_->app_name = cfg.app_name ? cfg.app_name : "dxr-gles-stereo";
	m_->cfg.runtime_lib_path = m_->lib_path.empty() ? nullptr : m_->lib_path.c_str();
	m_->cfg.app_name = m_->app_name.c_str();
	m_->gx = cfg.window_x;
	m_->gy = cfg.window_y;
	m_->gw = cfg.window_w;
	m_->gh = cfg.window_h;
	m_->gdisp = cfg.display_id;
	m_->quit = false;
	m_->error.clear();
	m_->cmd_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (m_->cmd_fd < 0) return false;
	m_->st = WeaveState::Starting;
	m_->thread = std::thread([this] { m_->thread_main(); });
	return true;
}

void WeaveTransport::stop()
{
	if (m_->thread.joinable()) {
		{
			std::lock_guard<std::mutex> lk(m_->mu);
			m_->quit = true;
		}
		m_->wake();
		m_->thread.join();
	}
	if (m_->cmd_fd >= 0) close(m_->cmd_fd);
	m_->cmd_fd = -1;
	if (eglGetCurrentContext() != EGL_NO_CONTEXT && m_->egl_ok) {
		m_->input.destroy(m_->egl);
		m_->output.destroy(m_->egl);
		m_->resolve.destroy();
	}
	m_->st = WeaveState::Stopped;
}

void WeaveTransport::on_context_reset()
{
	Impl &m = *m_;
	if (!m.egl_ok) return;
	// The names belong to the dead context; deleting them on the new one is a
	// no-op, and the EGLImages / AHardwareBuffers are released properly.
	m.input.destroy(m.egl);   // ours: reallocated by ensure_input
	m.output.drop_gl(m.egl);  // the runtime's: keep the reference, rewrap on the next present
	m.resolve.destroy();
	LOGW("GL context replaced: weave input dropped, output kept for rewrap on the next present");
}

WeaveState WeaveTransport::state() const { return m_->st.load(); }

const char *WeaveTransport::last_error() const
{
	std::lock_guard<std::mutex> lk(m_->mu);
	return m_->error.c_str();  // stable until the next failure
}

DisplayInfo WeaveTransport::display_info() const
{
	std::lock_guard<std::mutex> lk(m_->mu);
	return m_->display;
}

EyeSample WeaveTransport::last_eyes() const
{
	std::lock_guard<std::mutex> lk(m_->mu);
	return m_->eyes;
}

void WeaveTransport::set_window_geometry(int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t display_id)
{
	{
		std::lock_guard<std::mutex> lk(m_->mu);
		if (m_->gx == x && m_->gy == y && m_->gw == w && m_->gh == h && m_->gdisp == display_id) return;
		m_->gx = x, m_->gy = y, m_->gw = w, m_->gh = h, m_->gdisp = display_id;
		m_->geom_dirty = true;
	}
	m_->wake();
}

bool WeaveTransport::stalled() const { return m_->stalled.load(); }

bool WeaveTransport::present(uint32_t left_fbo, uint32_t right_fbo, int w, int h, EyeSample *eyes_out, const float *shift_px)
{
	Impl &m = *m_;
	if (m.st.load() != WeaveState::Ready || w <= 1 || h <= 0) return false;
	// A submit that outlived its wait still owns the input buffer. Skip frames
	// until it returns; the caller's own picture stays on screen meanwhile.
	if (m.stalled.load()) {
		Impl::Job late;
		{
			std::lock_guard<std::mutex> lk(m.mu);
			if (!m.job.done && m.job.pending) return false;  // still in flight
			// Done, or nothing in flight any more (the slot was reset after the
			// timeout): either way the stall is over. An empty slot must not keep
			// the transport stalled forever.
			late = m.job;
			m.job = Impl::Job{};
		}
		m.stalled = false;
		GlStateGuard late_guard;  // wrap() binds objects the game expects untouched
		if (late.acquire_fd >= 0) close(late.acquire_fd);
		if (late.result == XR_SUCCESS && late.woven) {
			m.output.destroy(m.egl);
			if (!m.output.wrap(m.egl, late.woven, false)) m.output.destroy(m.egl);
		}
		m.gpu_wait_release(late.release_fd);  // input writes must follow the runtime's reads
	}
	if (!m.egl_ok) {
		m.egl_ok = m.egl.load();
		if (!m.egl_ok) {
			m.fail("EGL_ANDROID_get_native_client_buffer / EGL_KHR_image / GL_OES_EGL_image unavailable");
			return false;
		}
	}

	GlStateGuard guard;

	// Full-screen default: the window geometry follows the back buffer size
	// until the host publishes the real on-screen rect.
	{
		std::lock_guard<std::mutex> lk(m.mu);
		if (m.gw == 0 || m.gh == 0) {
			m.gw = (uint32_t)w, m.gh = (uint32_t)h;
			m.geom_dirty = true;
		}
	}

	if (!m.ensure_input((uint32_t)w, (uint32_t)h)) return false;

	// 1. Both eyes into the SBS input. The previous frame's release fence was
	//    GPU-waited before its woven blit, so these writes are ordered after
	//    the runtime's reads of the last input.
	const int half = w / 2;
	m.squeeze_eye(left_fbo, w, h, 0, half, shift_px ? shift_px[0] : 0.f, shift_px ? shift_px[1] : 0.f);
	m.squeeze_eye(right_fbo, w, h, half, w, shift_px ? shift_px[2] : 0.f, shift_px ? shift_px[3] : 0.f);

	// 2. One acquire fence: input written AND the previous woven output read
	//    (the woven buffer is single-buffered; GL executes in order).
	const int acquire = m.make_acquire_fence();

	// 3. Hand the job to the session thread and wait for the submit to return
	//    (with v20 fences it returns without the service's CPU wait).
	Impl::Job result;
	{
		std::unique_lock<std::mutex> lk(m.mu);
		m.job = Impl::Job{};
		m.job.pending = true;
		m.job.input = m.input.ahb;
		m.job.w = (uint32_t)w;
		m.job.h = (uint32_t)h;
		m.job.acquire_fd = acquire;
		m.wake();
		// Never hold the game's render thread hostage to a blocked submit: that
		// freezes the game and, in time, gets it killed as not responding.
		const bool answered = m.cv.wait_for(lk, std::chrono::milliseconds(kSubmitWaitMs), [&] {
			return m.job.done || m.st.load() == WeaveState::Failed;
		});
		if (!answered) {
			m.stalled = true;
			return false;  // the job stays in flight; collected above when it returns
		}
		result = m.job;
		m.job = Impl::Job{};
	}
	if (result.acquire_fd >= 0) close(result.acquire_fd);  // not consumed
	if (result.result != XR_SUCCESS) return false;

	// 4. Adopt a new woven buffer (first submit, or after a resize). The
	//    runtime hands us a reference; AhbTarget::destroy releases it.
	if (result.woven) {
		m.output.destroy(m.egl);
		if (!m.output.wrap(m.egl, result.woven, false)) {
			m.output.destroy(m.egl);
			if (result.release_fd >= 0) close(result.release_fd);
			return false;
		}
	}
	if (!m.output.fbo && m.output.ahb) {  // context replaced since the woven buffer was adopted
		if (!m.output.rewrap(m.egl)) m.output.destroy(m.egl);
		else LOGW("woven output rewrapped on the new GL context");
	}
	if (!m.output.fbo) {
		if (result.release_fd >= 0) close(result.release_fd);
		return false;
	}

	// 5. GPU-wait the weave, then the woven image into the back buffer
	//    (top-down AHardwareBuffer -> y-up window: flip). 1:1, NEAREST: the
	//    interlace pattern is per-pixel and must not be filtered.
	m.gpu_wait_release(result.release_fd);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, m.output.fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	const int ow = (int)m.output.w, oh = (int)m.output.h;
	glBlitFramebuffer(0, 0, ow, oh, 0, h, ow, h - oh, GL_COLOR_BUFFER_BIT, GL_NEAREST);


	if (eyes_out) *eyes_out = last_eyes();
	return true;
}

} // namespace dxr::core
