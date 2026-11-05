# dxr_gles_core — the shared Android GLES stereo core (static library).
# Today: the XR transport only. The hooks / twins / replay / camera modules
# move here from the prototype's StereoGL.cpp as they are split (see
# docs/track4-core.md).
#
#   add_subdirectory(core)
#   target_link_libraries(<mod> PRIVATE dxr_gles_core)
#
# DXR_OPENXR_INCLUDE: directory containing openxr/openxr.h and the DisplayXR
# extension headers (openxr/XR_DXR_weave.h, openxr/XR_DXR_display_info.h), e.g.
# displayxr-runtime/src/external/openxr_includes.
# No OpenXR library is linked: the transport dlopens the runtime client library
# by the path the broker sends (or a bundled libopenxr_loader.so as a fallback).

cmake_minimum_required(VERSION 3.18)

add_library(dxr_gles_core STATIC transport_weave.cpp)
target_compile_features(dxr_gles_core PUBLIC cxx_std_17)
target_include_directories(dxr_gles_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} PRIVATE ${DXR_OPENXR_INCLUDE})
target_link_libraries(dxr_gles_core PUBLIC EGL GLESv3 android log nativewindow vulkan dl)
