# Porting a native Android game to 3D: a generic method

*Status: draft, November 3, 2026. Derived from making Minecraft Bedrock weave and look around on a 2560x1600 tablet, and from the stereo DXVK path for PC games.*

A native Android game gives us no engine source and no stable names. What it always gives us is the graphics API (GLES or Vulkan), the window swap, and a camera that ends up as numbers in uniforms or uniform buffers. A port is generic when every step below works from those, and per-game knowledge is only a fallback profile.

## The pipeline

| Layer | Job | Generic mechanism | Per-game fallback |
|---|---|---|---|
| 1. Entry | get code into the process | mod loader (LeviLauncher for Minecraft); otherwise a GLES/Vulkan layer or a launcher that preloads | loader-specific glue |
| 2. Frame | find the frame and the window | hook `eglSwapBuffers` / `eglSwapBuffersWithDamage` (`vkQueuePresentKHR`); present from whichever thread swaps | none |
| 3. Two eyes | render the second eye | multiview (`GL_OVR_multiview2` / Vulkan multiview) first; per-pass replay second; per-draw doubling last | which passes to keep mono |
| 4. Eye offset | move each eye's camera | clip-space injection in every vertex shader: `gl_Position.xy += eye.xy * (gl_Position.w - conv)` | camera uniform names |
| 5. Camera-dependent inputs | keep everything that depends on the camera consistent with the eye | numeric classification of matrices against the camera (below), patched with the eye's clip-space transform | name list |
| 6. Look-around | follow the viewer's head | the weave returns tracked eye positions every frame; they set `eye.xy` | gain |
| 7. Weave | interlace for the panel | `core/WeaveTransport`, in-process | none |
| 8. Policy | resolution, 2D/3D, convergence | shared library (view-scale resolution, auto-2D, convergence, separation gain) | defaults |

## Layer 4 and 5: one transform, applied everywhere it matters

Moving an eye by `e` (in camera units) with the convergence plane held fixed is, in clip space,

```
E = Shift(e) * P * T(-e) * P^-1
```

where `P` is the frame's projection, `T(-e)` translates the camera, and `Shift` (`clip.xy += s * clip.w`, `s = e * P00 / conv`, `e * P11 / conv`) re-centres the convergence plane. The vertex-shader injection in layer 4 is this transform written per vertex.

Every input that depends on the camera has to see the same eye, or the picture comes apart. Minecraft showed the classes:

| Input | What breaks if left alone | Fix |
|---|---|---|
| forward camera (`VP`, `P`) | no stereo | `E * VP` (or the shader injection) |
| inverse camera / inverse projection (deferred lighting, shadows, SSR rebuild world positions from depth) | shadows and lighting don't follow the eye: they are computed for the unmoved camera | `M * E^-1` |
| previous-frame camera (TAA, motion vectors) | ghosting, smearing | `E * VP_prev`, `M_prev * E^-1` |
| camera world position (view vectors: specular, fog) | highlights fixed to the centre view | `pos += e.x * right + e.y * up`, axes from the view matrix |
| world-space matrices (model, bones, shadow-map projections) | nothing | leave alone |

**Classifying without names.** Once the frame's camera `VP` (and `P`) is known, any uploaded matrix `M` can be tested:
- `M * VP ≈ I` → inverse camera;
- `M * P ≈ I` → inverse projection;
- `M ≈ VP` within a few percent → a previous-frame or jittered camera;
- otherwise → unrelated, leave alone.

The camera itself is found the same way: the perspective test already used (`|m[11]| ≈ 1`, `m[15] ≈ 0`) picks out projections, and the vertex-shader injection removes the need to find it at all for the forward pass. The mod now logs this classification next to the name-based one (`matrix probe:` lines) so we can check it on Minecraft before relying on it.

**Uniform buffers.** Many engines put matrices in UBOs instead of plain uniforms. The same test applies to 16-float aligned runs in `glBufferSubData` / mapped ranges for buffers bound as uniform blocks. That is the main work left for a truly generic GLES port.

## Lessons from Minecraft that every port will hit

1. **Match the loader's SDK exactly.** LeviLauncher 1.6.0 ships preloader `5d108da`; building against tag 0.2.2 crashed the game at mod registration (struct layout changed).
2. **Games present from more than one thread.** Minecraft swaps from a loading thread and a render thread. Anything the hooks share (program tables, the weave job slot) must be thread-safe, or a stalled weave never recovers.
3. **Deferred renderers are the norm now.** Patching only the forward camera gives correct geometry with wrong shadows and lighting. Layer 5 is not optional.
4. **Head tracking needs a foreground camera grant.** The runtime inherits capabilities from its client's binding. When the client is a helper app, the helper must hold a camera-type foreground service started from its UI, or Android denies the tracking service the camera.
5. **Without a face the vendor goes flat.** In MANAGED mode the vendor display processor weaves a single view when no face is tracked. Request MANUAL (now forwarded for weave-only clients, runtime branch `mac/weave-eye-mode`) when the game should stay 3D regardless.
6. **ROM power managers freeze helpers.** Some ROMs freeze a background helper; a foreground service bound to by the game helps, starting the helper just before the game is the workaround.

## What stays per game

A small profile: which passes stay mono (UI, post), default convergence and separation, and names for anything the numeric classification can't decide. The goal is that a new game needs a profile, not new code.

## Addendum, November 3, 2026: what the Minecraft shadows taught us

**Symptom.** With the eye offset in the camera matrices, geometry was correct per eye but cast shadows, fog and the sky sat at the wrong depth: shifted horizontally by a constant amount in each eye.

**Cause.** Minecraft's screen-space passes (directional light and shadows, fog, reflections: 13 shaders) rebuild the view-space position from depth with a hand-written inverse projection that reads only four elements of `u_invProj`:

```
vec4(x * invProj[0].x, y * invProj[1].y, w * invProj[3].z, z * invProj[2].w + w * invProj[3].w)
```

It assumes a symmetric frustum and skips the translation column, which is exactly where a per-eye matrix carries the eye translation and the convergence shift. The forward shaders use the full multiply and were fine. No uniform patch can reach a shader that reads elements selectively.

**Fix.** At `glShaderSource`, rewrite the shortcut into the full multiply `u_invProj * vec4(x, y, z, w)` (regex on the spirv-cross output, checked against `mat4 _n = u_invProj;` copies). The per-eye `u_invProj` then applies, `u_invView` stays the game's (the full inverse yields the game's view space), and shadows, fog and sky are glued. Keep this as a standard step: **dump every shader that mentions an inverse camera matrix and check how it uses it; a selective read is a bug the eye cannot pass through.**

**Rejected: convergence as a picture shift.** Applying the convergence as a horizontal offset of the finished eye images fixes the same symptom without touching shaders, but the black edges move with the head under look-around and the HUD leaves the screen plane. Convergence stays a camera parameter.

**Diagnosis method that worked** (after several that did not): take the buffer the suspect pass writes, divide by albedo where needed, and block-match it against the albedo between the eyes; the feature is correct where its disparity equals the geometry's. Row-wise edge detectors locked onto silhouettes and gave false positives; global block statistics were dominated by wall shading and missed the shadow. Switching every patch off one at a time and re-measuring the same buffer told which uniforms the pass actually reads (here: none of the matrices' translation columns).

**Per-eye state inventory for this engine.** Screen-sized textures (incl. 19x12 / 18x11 tile grids), camera-aligned 3D volumes written by compute (seeded from the left eye per frame), compute dispatches (run twice with per-eye images), storage buffers (optional twins; exposure grids are a cosmetic case). Shadow maps, LUTs and mip pyramids stay shared.

## Addendum, November 3, 2026 (evening): the night sky that doubled in one eye

**Symptom.** At game night, with the viewer left of the panel (right eye's camera offset
negative), the right eye's whole sky was about twice as bright. A vertical line between
a bright and a dark sky moved with the head and sat at the screen centre exactly when
the right eye's offset was zero. Vanilla shows the same line, faint and flickering.

**Diagnosis, in order.** Fake head position via `debug.mc3d.headx` (blocks) to reproduce
without a viewer. Per-texture dumps: the lift first appears in the scene buffer before
tone mapping (program 168's fog composite plus whatever draws on top), the bloom chain
spreads it, the local exposure hides it in the tone-mapped buffer but the bloom add
brings it back. It is a flat, neutral constant added to sky pixels only (clouds and
stars excluded). The volumes (fog, shadow, exposure grid) are identical per eye. The
matrices are clean (the game's far plane is finite, w at the sky is about 4e-4, so no
division by zero). Elimination by `debug.mc3d.invskip=<program>` (leave one program's
inverse matrices unpatched): only program 190, the deferred specular pass (SSR +
image-based reflections from the sky-probe cube array), moves the ratio: 1.90 -> 1.02.

**Cause.** The specular pass runs on every pixel, sky included, using the sky's
degenerate normal. The reflected direction lands on a boundary between two faces of the
sky-probe cube map, sampled at its lowest mip where each face is one colour. The pixel's
x decides which face wins: a vertical line in vanilla; with a per-eye convergence shift
the sign of the shift puts the whole sky of that eye on one face (bright) or the other
(dark). Night only because only then do the two faces differ.

**Fix.** Shader rewrite at `glShaderSource` for shaders that sample `s_SpecularIBLRecords`
and read `s_SceneDepth`: `if (ndcDepth >= 1.0) discard;` right after the depth decode
(`patch_sky_specular`). Specular reflection on the sky has no meaning, and vanilla's
result there is the arbitrary face colour. `debug.mc3d.skyibl=0` disables it;
`debug.mc3d.invskip=190` is the fallback (program ids are stable across runs here).
Status: built, not yet verified on the pad (needs a game night with the head left).

**Also changed on the way.** The inverse-matrix patch is now closed form
(`M' = (M + (e.x p00 M.col0 + e.y p11 M.col1) (x) M.roww) S^-1`, no inversions), which
keeps the z and w rows of an inverse projection bit-exact; `debug.mc3d.invexact=0`
restores the product form. Not the cause of this bug, kept because it is cleaner.
Debug aids added: `headx`/`heady` (fake head), `invskip`, `logmat` (log patched
matrices), `invexact`.

**Lesson for the method.** A step at exactly zero offset in one eye is a sign-sensitive
discontinuity in the game, not a matrix error: look for face selection (cube maps),
integer truncation, or branch conditions that the eye's shift pushes across zero.

**Open after the fix (reported November 3, evening, before verification).** By day,
a reflection or lighting difference travels across house walls in the right eye only as the
head moves. Same pass is the first suspect (image-based specular on rough walls samples the
sky probe at its lowest mip, where a cube-face boundary gives a hard line that the per-eye
direction can cross; the sky discard does not cover walls). Test first: `debug.mc3d.invskip=190`
live by day; if the travelling stops, the specular pass is confirmed and the options are a
bias-free face choice (cap the probe lod one level above the lowest) or computing that pass's
directions from the centre camera. Note: SSR is off on this device (its sampler is a 1x1
dummy), so it is the probe path. Also note for anyone using the watchers: `debug.mc3d.headx`
overrides the tracked head, so look-around is off while a watcher runs; clear it after.

## Correction, November 3, 2026 (late): the real cause of the one-eye night sky

The cube-face explanation above was the mechanism, not the cause. Per-pass snapshots of the
scene buffer (new `debug.mc3d.snap=<program>`) showed both eyes identical through the fog,
atmosphere and translucent passes and diverging only after the specular pass, although that
pass now discards sky pixels. It samples the game's *second* depth-stencil texture (the
terrain pass renders through a framebuffer whose depth is that texture; everything else uses
another). Logging every clear showed that this texture is never cleared per frame: it got its
clear value once at creation and the terrain only ever overwrites the ground, so its sky stays
1.0 for the life of the process. The mod's right-eye twin was created later as uninitialised
storage and never received that one-time clear, so in the right eye the sky depth was garbage,
the specular pass treated sky as geometry, and vanilla's own cube-face line (and its
brightening when the eye shift pushed the tie) showed in that eye only. Same mechanism gave
the daytime "travelling reflection" on walls.

**Fix (generic).** Every new twin starts as a copy of its original, done at the frame boundary
with a framebuffer blit (`seed_pending_twins`; depth-stencil aware; renderbuffers too). Copying
at creation time, inside the pass that is writing the original, stalls this GPU and the weave
never recovers; the end of frame is the safe moment. Verified: night sky ratio 1.06 to 1.12
flat at every head position (was 1.5 to 2.1 with a hard step), daytime wall change equal in
both eyes, shadows still glued to the ground (median 0 px).

**Rule for the method.** A right-eye twin must inherit *everything* the game puts into the
original outside the per-frame draws the mod doubles: one-time clears, CPU uploads
(`glTexSubImage2D`, now mirrored), copies (`glCopyImageSubData`, `glCopyTexSubImage2D`, now
mirrored), blits (mirrored before). Seeding each twin from its original at creation covers the
one-time cases in one stroke. Symptom signature: a static defect in one eye, tile- or
region-shaped, present from the first frame, immune to every matrix switch.

**Still to decide.** The sky discard in the specular pass and running that pass on the centre
camera (`spec_program`) were added while chasing the mechanism; with the seeding fix they may
be unnecessary. A/B pending: `debug.mc3d.specinv=1` (per-eye matrices back on in that pass)
and `debug.mc3d.skyibl=0` (discard off, needs a restart).

**A/B outcome (November 3, evening).** With twins seeded: the specular pass must stay on the
game's own camera (per-eye matrices there still brighten the right eye's sky edge, 36 to 40
versus 22); the sky discard is not needed (ratios 1.05 to 1.08 without it) and is now off by
default (`debug.mc3d.skyibl=1` enables it). Final configuration verified at night with
tracking, head left and head right.

## Second device, November 3, 2026 (late evening): a 1080x2400 phone

Same build, no device-specific change, runs on a 1080x2400 phone (DisplayXR
runtime v2.61.0, the vendor's tracking services): both eyes in the world, HUD at the screen plane,
look-around tracked, froxel and exposure volumes twinned at the phone's tile grid (23x11x32),
26 twins seeded at start. Frame rate 21 to 24 fps with a face tracked, 36 fps on the fallback
(same tracking cost as on the tablet, still an open item).

**Install recipe (adb over USB, ~10 min).**
1. Pull the exact Minecraft the mod targets from a device that has it (`pm path
   com.mojang.minecraftpe`, five split packages) and install them with
   `adb install-multiple -r -i com.android.vending ...`: LeviLauncher refuses a game whose
   installer is not the Play Store, and the `-i` flag records that attribution. The game's own
   Play licence check still runs and needs the buying Google account signed in on the device.
2. Install LeviLauncher (1.6.0) and the helper app. Start the launcher once so it creates
   `Android/media/org.levimc.launcher/minecraft/...`; then push `mods/DisplayXR 3D/`
   (library, manifest), set `mods/mods_config.json` to enable it, and push a world into
   `minecraft/_shared/internal/games/com.mojang/minecraftWorlds/`.
3. Helper first: open it, allow the camera (or `pm grant ... CAMERA`), tap Turn 3D On. Then the
   launcher's own Launch button, then Play and the world in the game's UI.
4. Never bring the running game to the front with an activity start: it creates a second game
   activity in the same process and the game aborts on its second bootstrap. Use the launcher's
   button and stay inside the game. On this phone's ROM, never use the monkey launcher either.

## Pause and resume (phone, November 3, late): the game replaces its GL context

Backgrounding Minecraft and bringing it back restarts its renderer: a new EGL context, every
GL object name reissued, programs relinked. The mod's records (twins, framebuffers, program
tables, the weave transport's input and output images) all pointed at the dead context; the
first present after resume crashed inside the driver's blit, and the launcher showed a black
screen. Fix: the swap hook compares the thread's current EGL context with the one it last
presented on; on a change it forgets every GL object the mod and the transport own
(`stereo_context_reset`, `WeaveTransport::on_context_reset`). The transport keeps its reference
to the runtime's woven buffer and rewraps it on the new context (the runtime hands that buffer
over once; dropping it left every later present failing). Twin framebuffers that fail to build
in the frames right after the restart (attachments not yet complete) are retried for a few
frames instead of being marked mono for good, which otherwise left the right eye identical to
the left after a resume.

**Lesson for the test scripts.** A capture loop that sets a live switch must clear it with the
exact same name; one loop here left seven switches set on the phone (vanilla cameras, storage
twins, per-eye specular...) and cost an hour of contradictory measurements and one driver
crash. Scripts now clear the whole switch list before and after a run, and a fresh capture
starts by listing `getprop | grep mc3d`.

## The vertical line in the right eye (phone and tablet, November 4, 2026, early)

**Symptom.** At night, a faint vertical line in the right eye only, about 2/3 across the screen on
the tablet and 71 % across on the phone; right of it the sky was lighter and clouds showed as dark
silhouettes. Visible by day on dark grays. Backgrounding and resuming the game made it vanish for a
few seconds, then it crept back as the clouds drifted.

**Cause.** The twin seeding (above) copies with `glBlitFramebuffer`, which honours the scissor
test. The game leaves a 1024x1024 scissor box enabled at the frame boundary (logged at seed time:
`scissor ON box 0,0 1024x1024`), so only the first 1024 columns of each twin were seeded; the rest
stayed uninitialised, which is the one-eye night sky again, confined to a band. 1024 px of a
1432-px-wide render target is 71 %; on the tablet it is about 2/3. The band is fixed in pixels
while the clouds move, which is why resume (a fresh seed) hid it briefly.

**Fix.** Disable `GL_SCISSOR_TEST` around the seeding blit and restore it after. Verified on the
phone at night: right-minus-left in the sky flat across the width, line gone by eye.

**Rule for the method.** Any copy the mod makes on the game's behalf runs under the game's raster
state. Blits are clipped by the scissor box; clears too. Save, neutralise and restore the scissor
(and anything else that clips: viewport for draws, color/depth write masks for clears) around
every mod-issued copy.

## Where the probes live

The `debug.mc3d.*` switches named in the diagnosis sections above (`headx`, `invskip`, `skyibl`,
`specinv`, `snap`, the buffer dumps) were investigation instrumentation and are **not** in the
committed mod; they live on a development branch. The committed build keeps
two switches only: `debug.mc3d.eyes_manual` (eye-tracking mode, needs the runtime change that
forwards MANUAL mode for weave-only clients) and `debug.mc3d.ssbotwin` (per-eye storage buffers,
off by default: costs a copy per buffer per frame for a cosmetic gain).
