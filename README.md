# displayxr-play-android

The Android lane of DisplayXR Play: stereo mods for native Android games. Real two-eye rendering,
woven in-process by the DisplayXR runtime through `XR_DXR_weave`, with look-around from the
runtime's tracked eye positions. First game: Minecraft Bedrock. Releases are on this repo's
[Releases](../../releases) page.

## How it works

A mod is a native library loaded into the game's process by a mod loader. It never modifies the
game binary. It hooks the graphics API (about 40 OpenGL ES entry points plus the EGL swap) and:

1. gives every screen-sized render target and framebuffer a **right-eye twin**, seeded from the
   original so the twin inherits everything the game put there once (one-time clears, uploads);
2. **replays each draw** into the twin with the camera moved for the right eye;
3. keeps every **camera-dependent input** consistent with that eye: inverse projection (deferred
   lighting, shadows, fog), previous-frame camera, camera position; shaders that read the inverse
   projection selectively are rewritten at compile time;
4. at swap, hands the side-by-side pair to the DisplayXR runtime over the **weave transport**
   (`core/transport_weave`), gets back the woven image and the tracked eye positions, and
   presents the woven image; the eye positions drive look-around.

The method, with the measurements behind each rule, is in
[`docs/android-game-porting.md`](docs/android-game-porting.md). The split is core versus profile:
`core/` holds what is true for any GLES game, a profile names one game.

## Layout

| Path | What |
|---|---|
| `core/` | Game-independent: the weave transport (`transport_weave.{h,cpp}`), the per-game profile interface (`profile.h`) |
| `minecraft/mod/` | The Minecraft mod: hooks, twins, replay, camera patch, shader rewrite (`src/StereoGL.cpp`, `src/Bridge.cpp`), CMake, mod manifest template |
| `minecraft/broker/` | The helper app ("DisplayXR 3D Bridge"): holds the camera foreground-service grant the runtime needs for head tracking and brokers the connection to the runtime |
| `docs/` | Method and the core-versus-profile split |

## Dependencies

- **LeviLauncher** (the LeviMC community's Android launcher for Minecraft Bedrock), currently 1.6.0. It starts the game with a mod
  loader that preloads native mods declared by a manifest (`"type": "preload-native"`). The mod
  is built against the launcher's preloader SDK; **the SDK must match the launcher build
  exactly** (a mismatch crashes the game at mod registration; pinned in `minecraft/mod/CMakeLists.txt`).
- **DisplayXR runtime for Android** with `XR_DXR_weave`, and the vendor plug-in and tracking
  services underneath it.
- **Minecraft Bedrock**, the exact version the manifest lists, installed with the Play Store
  recorded as installer (the launcher refuses other installers) and the buying account signed in.

## Build

```sh
cmake -S minecraft/mod -B build -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 \
      -DDXR_OPENXR_INCLUDE=<displayxr-runtime>/src/external/openxr_includes
ninja -C build            # → build/libdisplayxr3d.so
```

The helper app is a Gradle project in `minecraft/broker/` (`./gradlew assembleRelease`).

## Install (adb over USB, about ten minutes)

1. Install Minecraft (five split packages) with the Play Store attribution:
   `adb install-multiple -r -i com.android.vending <splits>`.
2. Install LeviLauncher and the helper app. Open the launcher once so it creates
   `Android/media/org.levimc.launcher/minecraft/...`.
3. Push the mod folder `mods/DisplayXR 3D/` (`libdisplayxr3d.so`, `manifest.json`) and enable
   it in `mods/mods_config.json`. Push a world into
   `minecraft/_shared/internal/games/com.mojang/minecraftWorlds/` if needed.
4. The DisplayXR runtime must already be installed on the device.

## Launch, in this order

1. Helper app: allow the camera, tap **Turn 3D On**.
2. LeviLauncher's own **Launch** button.
3. Inside the game: **Play**, then the world. The mod's settings (depth, screen distance) are in
   the game's mod menu under *DisplayXR 3D*.

Never start the game activity by hand (`am start`) and never bring the running game back to
the front that way: the game creates a second activity in the same process and aborts on its
second bootstrap. Use the launcher's button and the system's task switcher.

## Known behavior

- **Background and resume** is supported: the game replaces its GL context when backgrounded;
  the mod detects it at swap, forgets its GL objects, rewraps the runtime's woven buffer and
  retries twin builds for a few frames.
- **Head tracking costs frame rate** on phone-class devices (the CPU face tracker shares the
  cores with the game): roughly 22 fps tracked versus 36 to 45 on the no-face fallback.
- **Without a face** the vendor weaves flat in MANAGED eye-tracking mode. MANUAL mode for
  weave-only clients is a runtime change (DisplayXR/displayxr-runtime#2532); the mod selects it
  with `debug.mc3d.eyes_manual=1` once the runtime has it.
- **Power managers freeze background helpers** on some ROMs: start the helper right before the
  game.
- Two switches exist in the shipped mod: `debug.mc3d.eyes_manual` (above) and
  `debug.mc3d.ssbotwin` (per-eye storage buffers, off: a copy per buffer per frame for a
  cosmetic gain). Investigation instrumentation lives on development branches only.

## Status

Verified on a 2560x1600 tablet (45 fps) and a 1080x2400 phone. Shadows, fog, sky,
reflections, the first-person hand and the HUD are correct in both eyes; look-around tracks;
pause and resume survive.

## Rules

This project ships no game assets and no game code, and bypasses no store: you own the game, the
launcher is a community project, and the mod only hooks the graphics API inside the process the
launcher starts. Game names appear here only to say which game a mod is for. The vendor plug-in
and tracking services under the runtime are the device's; the mod talks to the DisplayXR runtime
only.

## License

Boost Software License 1.0, the same as the DisplayXR runtime (see `LICENSE`).
