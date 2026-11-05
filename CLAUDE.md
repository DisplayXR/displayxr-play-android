# CLAUDE.md

Guidance for Claude Code (claude.ai/code) when working in this repository.

## What this repo is

The Android lane of **DisplayXR Play**: stereo mods for native Android games. A mod is a native
library a community launcher preloads into the game's process; it hooks the graphics API, renders
every screen-sized pass a second time for the right eye, and hands the pair to the DisplayXR
runtime over `XR_DXR_weave`, which weaves it for the display and returns the tracked eye positions.
`core/` is game-independent (weave transport, profile interface); `minecraft/` is the first game
(mod plus the helper app that holds the camera grant). The method, with the measurements behind
each rule, is `docs/android-game-porting.md`.

## Rules that hold here

- **Public and vendor-neutral.** No company, device, vendor SDK, service package or person is
  named anywhere in this repo, code and docs alike. Say "the display's plug-in", "the vendor's
  tracking services", "a 2560x1600 tablet". The only product named is the game a mod targets, and
  only to say which game it is for (nominative use). Never add a trademark to a mod's name.
- **No game assets, no game code, no store bypass.** The mod hooks the API inside a process the
  launcher starts; the user owns the game and installs it from its store. Keep it that way.
- **Real stereo only.** Two rendered views. Nothing here reprojects from depth.
- **Convergence is a camera parameter, never a picture shift.** Fix the pass (shader rewrite is
  fine); do not shift the viewport or the output.
- **No binaries in git.** Libraries, apks and mod folders go on the Releases page.

## Build

```sh
cmake -S minecraft/mod -B build -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 \
      -DDXR_OPENXR_INCLUDE=<displayxr-runtime>/src/external/openxr_includes
ninja -C build                      # build/libdisplayxr3d.so + build/manifest.json
cd minecraft/broker && ./gradlew assembleRelease -x lintVitalRelease   # the helper app
```

The mod is built against the launcher's preloader SDK and **the pin in `minecraft/mod/CMakeLists.txt`
must match the launcher version exactly**; a mismatch crashes the game at mod registration. The
helper app targets API 31 on purpose (runtime discovery was validated there), which is why the
Play-store lint gate is skipped.

## Releases

One release per version of the lane (`vX.Y.Z`). Inside it: one zip per game holding the launcher's
`mods/` folder ready to copy (`<game>-mod-<version>.zip`), and the helper app once. A game's mod
has its own version in its `manifest.json`; the release version moves when any part of the lane
changes. A new game is a new zip in the next release, never a new release line.

## Device rules when testing

Helper app on first (camera grant, Turn 3D On), then the launcher's own Launch button. Never start
the game activity by hand and never bring the running game to the front with an activity start:
it creates a second activity in the same process and aborts. Use the task switcher. Pause and
resume are supported; a late 3D-on after a resume is a known open item.

## Where things go

- Bugs and game reports: this repo's issues. Profiles for new games: a PR adding
  `<game>/` with a mod and a `docs/` note; keep `core/` game-independent.
- Program-level planning (milestones, the other systems) is not in this repo.
