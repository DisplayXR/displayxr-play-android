// Runs DisplayXR inside Minecraft's process.
//
// Every Minecraft frame is rendered for both eyes (StereoGL). At eglSwapBuffers
// the left eye (the window) and the right eye (its twin) go to the XR_DXR_weave
// transport (core/transport_weave), which writes the woven picture into
// Minecraft's own back buffer. No overlay. If anything is not ready, Minecraft
// just shows its normal flat picture.
//
// LeviLauncher cannot see the DisplayXR runtime, so the small "DisplayXR 3D
// Bridge" app connects to the runtime service and hands us the socket and the
// runtime library path over Binder.
#pragma once

#include <jni.h>

#include <atomic>
#include <string>
#include <vector>

namespace dxr3d {

void bridge_start(JavaVM *vm);          // worker thread, once Minecraft is loaded
void bridge_install_hooks();            // eglSwapBuffers
void bridge_set_enabled(bool enabled);  // the ModMenu switch
std::vector<std::string> bridge_lines();
bool bridge_has_exit_report();          // the game's last close was a crash or kill
bool bridge_3d_on();                    // woven frames are going out
void bridge_post_info(const std::string &text);  // shown on the Bridge app's screen

} // namespace dxr3d
