// core/transport_weave.h — in-process XR transport for the GLES stereo core.
//
// Replaces the helper app + overlay transport of the prototype. OpenXR runs
// inside the game process as an XR_DXR_weave PRESENT-OWNER:
//
//   game renders L into its window + R into the window twin
//     -> present(): both eyes squeezed into one SBS AHardwareBuffer
//     -> xrWeaveSubmitDXR (sync_file acquire fence in, release fence out)
//     -> woven AHardwareBuffer back + tracked eyes
//     -> GPU-wait the release fence, blit the woven image into the game's
//        back buffer
//   caller then calls the real eglSwapBuffers. One window, one producer, the
//   game's own pacing; no overlay, no 0.80 opacity clamp, L/R always paired.
//
// Brokered connection (the host launcher has no <queries> for the runtime, so
// this process can neither see the runtime package nor bindService to it):
//   * a small broker app (the old helper, minus its overlay) does the runtime's
//     Java connect, keeps its Client alive, and sends this process
//       - the connected service socket as a ParcelFileDescriptor -> ipc_fd
//       - the runtime APK's nativeLibraryDir + "/libopenxr_displayxr.so"
//         -> runtime_lib_path
//   * the transport dlopens the runtime client library by that path,
//     negotiates with it like the Khronos loader would, adopts the fd, and
//     creates the instance. XR_DXR_weave (and the adopted fd) route the
//     runtime's hybrid router to the IPC/service path, where the weave lives.
//   See docs/track4-core.md §3.6 for why each step is needed and allowed.
//
// Threading:
//   * present() / set_window_geometry() / stop() are called on the game's GL
//     render thread (the thread that calls eglSwapBuffers), with its context
//     current and with the core's GL hooks bypassed (they must not double the
//     transport's own blits).
//   * Every OpenXR call runs on a transport-owned SESSION THREAD that owns and
//     pumps an ALooper. Display-processor init on Android posts to the calling
//     thread's Looper (runtime #2490); the weave DP lives in the service and
//     already gets the service's main Looper, but older runtimes and any
//     in-process fallback need the session thread to have one. Cheap, so
//     always on.
#pragma once

#include <cstdint>

#include <jni.h>

namespace dxr::core {

struct WeaveConfig {
	JavaVM *vm = nullptr;          // the game process's VM
	// Global ref to the hosting Activity (any Context works; it must be
	// non-null: the runtime dereferences XrInstanceCreateInfoAndroidKHR).
	jobject activity = nullptr;
	// Brokered connection. ipc_fd: connected service socket from the broker
	// (ParcelFileDescriptor.detachFd()); ownership passes to the transport,
	// which closes it after xrCreateInstance. -1 = let the runtime connect
	// itself (needs package visibility).
	int ipc_fd = -1;
	// Absolute path of the runtime client library inside the runtime APK
	// (ApplicationInfo.nativeLibraryDir + "/libopenxr_displayxr.so", sent by
	// the broker; it changes on every runtime update, so resend per connect).
	// nullptr = use a bundled Khronos loader (libopenxr_loader.so) instead.
	const char *runtime_lib_path = nullptr;
	const char *app_name = "dxr-gles-stereo";
	// Window geometry used until set_window_geometry() is called. 0 size =
	// "the size passed to present()" at origin 0,0 (a full-screen game).
	int32_t window_x = 0, window_y = 0;
	uint32_t window_w = 0, window_h = 0;
	int32_t display_id = -1;       // -1 = unknown / single display
	bool use_sync_fd = true;       // XR_DXR_weave v20 fences; false = v9 synchronous
};

// Tracked eyes from the last submit, display space, metres (x right, y up,
// z toward the viewer, origin at the panel centre). Use them for the NEXT
// frame's per-eye cameras (separation, convergence, look-around).
struct EyeSample {
	bool valid = false;            // false until the tracker has produced a sample
	bool tracking = false;         // false = fallback position (no live lock), still usable
	uint32_t count = 0;            // 2 for a stereo panel
	float pos[8][3] = {};
};

// Physical panel, from XR_DXR_display_info (zeros if the runtime lacks it).
struct DisplayInfo {
	float width_m = 0, height_m = 0;
	uint32_t pixel_w = 0, pixel_h = 0;
	float nominal_viewer[3] = {0, 0, 0.6f};
};

enum class WeaveState {
	Stopped,
	Starting,  // session thread bringing up instance/session
	Ready,
	Failed,    // see last_error(); present() returns false, game shows mono
};

class WeaveTransport {
public:
	WeaveTransport();
	~WeaveTransport();
	WeaveTransport(const WeaveTransport &) = delete;
	WeaveTransport &operator=(const WeaveTransport &) = delete;

	// Spawns the session thread and starts the XR bring-up asynchronously.
	// Returns false only if the thread could not be started.
	bool start(const WeaveConfig &cfg);

	// Tears down the session (blocks until the session thread exits). Call on
	// the render thread: it also frees the transport's GL objects.
	void stop();
	// The client replaced its GL context: drop the GL side of the input/output
	// buffers (they are rebuilt on the next present). Call with the new context current.
	void on_context_reset();

	WeaveState state() const;
	const char *last_error() const;
	DisplayInfo display_info() const;  // valid once state() == Ready

	// Window client rect on screen (View.getLocationOnScreen + size). The
	// runtime anchors the interlace phase to it, so publish every move/resize.
	void set_window_geometry(int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t display_id = -1);

	// Weave one frame and write the result into the default framebuffer.
	//   left_fbo / right_fbo: read framebuffers holding each eye at w x h
	//   (left is usually 0 = the window itself, right the window twin). May
	//   be multisampled; they are resolved first.
	// Returns true if the back buffer now holds the woven image. On false the
	// back buffer is untouched (still the left eye) and the caller presents
	// mono. Eyes are written when non-null and a submit happened.
	// shift_px (optional): {Lx, Ly, Rx, Ry} pixel offsets applied to each eye's
	// picture in the weave input (convergence as an image shift; edges are
	// cleared to black).
	bool present(uint32_t left_fbo, uint32_t right_fbo, int w, int h, EyeSample *eyes_out,
	             const float *shift_px = nullptr);

	// Last eyes without submitting (e.g. for a frame rendered mono).
	EyeSample last_eyes() const;

	// A submit has been blocked for over 500 ms. present() returns false, without
	// blocking, until it returns.
	bool stalled() const;

private:
	struct Impl;
	Impl *m_;
};

} // namespace dxr::core
