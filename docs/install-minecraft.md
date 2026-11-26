# Play Minecraft Bedrock in 3D: install guide

This guide sets up Minecraft Bedrock on an Android 3D display so the game renders a view for
each eye and the display shows it in 3D. Everything is done on the device; no computer is needed.
It takes about fifteen minutes, most of it downloads.

## What you need

- An Android device with a 3D display, arm64, Android 9 or later.
- The **DisplayXR runtime for Android** installed, with the display's plug-in and tracking
  services. Open the DisplayXR app once after installing it: its self-test should pass.
- **Minecraft Bedrock** bought on Google Play, on the Google account signed in on the device.
  The mod works with Minecraft 1.26.x.
- **LeviLauncher**, a community launcher that starts Minecraft with mods. Tested with 1.6.0 and
  1.7.1.
- From this repo's [Releases](../../../releases) page, latest release:
  - `displayxr-3d-bridge-<version>.apk`, the helper app that connects the game to DisplayXR and
    holds the camera permission used for eye tracking;
  - `minecraft-bedrock-mod-<version>.zip`, the mod.

## Install

1. **Minecraft.** Install it from Google Play. Open it once if you like; it plays in 2D on its own.
2. **LeviLauncher.** Download `LeviLauncher-<version>-release.apk` from the launcher's GitHub
   Releases page (LiteLDev/LeviLaunchroid) and open it to install. Android asks to allow
   installs from your browser or file manager: allow it. Open LeviLauncher once and accept its
   license. If it offers an update, it is fine to take it.
3. **Helper app.** Download `displayxr-3d-bridge-<version>.apk` and open it to install.
4. **Mod.** Download `minecraft-bedrock-mod-<version>.zip` and extract it. It holds a `mods/`
   folder with `DisplayXR 3D/` and `mods_config.json`. Copy the contents of that `mods/` folder
   into LeviLauncher's mods folder for Minecraft:

   ```
   Android/media/org.levimc.launcher/minecraft/com.mojang.minecraftpe/mods/
   ```

   so that it contains `DisplayXR 3D/libdisplayxr3d.so`, `DisplayXR 3D/manifest.json` and
   `mods_config.json`. Replace the `mods_config.json` already there. LeviLauncher creates the
   folder the first time it sees Minecraft; if it is missing, open LeviLauncher once more.

   Check: in LeviLauncher's home screen, the **Mods** card lists **DisplayXR 3D**.

## Play

Always in this order:

1. Open **DisplayXR 3D Bridge**. Tap **Allow camera (eye tracking)** the first time, then
   **Turn 3D On**. Leave it on while you play.
2. Open **LeviLauncher** and tap its **Launch** button.
3. In the game, tap **Play** and open a world. It shows in 3D.

Depth and screen distance are in the game's mod menu, under **DisplayXR 3D**.

To come back to the game after switching apps, use Android's recent-apps switcher. Don't start
Minecraft from its own icon while LeviLauncher is running it.

## If it stays 2D

- **The mod is not loaded.** LeviLauncher's Mods card does not list DisplayXR 3D: the files are
  in the wrong folder (step 4). Close the game, fix the folder, launch again from LeviLauncher.
- **3D was turned on after the game started.** Close the game, turn 3D on in the helper app, then
  launch from LeviLauncher.
- **The game was started from the Minecraft icon** instead of LeviLauncher: mods only load
  through LeviLauncher.
- **No face is tracked.** Without a face the display can fall back to 2D. Check that the camera
  is allowed in the helper app and that your face is in front of the screen.
- **The helper app was frozen in the background.** Some devices stop background apps to save
  power. Open the helper right before the game, and set its battery usage to unrestricted.

## Known limits

- Eye tracking costs frame rate on phone-class devices: roughly 22 to 26 fps tracked, 36 to 45
  without tracking.
- Turning 3D on after a resume, instead of before launch, can break the 3D view until the game
  is restarted.
