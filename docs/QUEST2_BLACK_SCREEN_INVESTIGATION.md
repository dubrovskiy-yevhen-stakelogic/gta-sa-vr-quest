# Quest 2 black world: investigation and diagnostic build

Updated: 2026-09-09. Native diagnostic baseline: `0ca8766` (0.2.0);
the surface-registration correction is a subsequent local source change.

**Status: a deterministic surface-size regression is identified and corrected in source; Quest 2 acceptance is pending.**
The developer corrected the release boundary: **0.1.1 works; 0.1.2 is the first
broken release**, with the issue continuing in 0.2.0. CJ/bike appear briefly after
the cutscene, the world becomes black, menus and movement continue. The earlier
0.1.1-failure premise is superseded. The latest Quest 2 report contains native
diagnostics and confirms repeated `no-safe-pair` rejection after the cutscene,
even with the swap-fence path disabled. Its `1440x1008` game surface exposes the
remaining main-pass filter: the old code rejected any height below 1024.

## What the release history establishes

| Change | First relevant commit | Why it matters |
|---|---|---|
| Initial public 0.1.0 | `1c1555b` | Comparison baseline; release tags are absent. |
| Colour resolve, default ON | `36195c9` | Separate from WORLD EFFECTS. FXAA OFF still uses a graded copy when colour grading is active. |
| Script-control/camera gate and default eye scale 100 to 110 percent | `1eb18f7` | Affects transitions and increases eye pixel allocations by about 21 percent. Scale defaults reverted in 0.1.2. |
| Fully opaque fade routes to theater | `abdef50` | A persistent fade/gate condition could keep the world out of stereo. |
| First source labelled 0.1.1 | `bab8be7` | Exact player APK still needs identification; later 0.1.1 commits exist. |
| Effects default enabled | `2521c2f` | Initial 0.1.1 already had effects OFF, so effects are not a sufficient explanation. |
| New liveness, eye backing/fence, hidden-area mask and foveation paths | `0edfcce`, 0.1.2 | Primary regression interval. Mask, backing and fence paths are default ON; native QCOM foveation is default OFF. |
| Flat game surface changes from XR eye dimensions to the 640:448 aspect | `0edfcce`, 0.1.2 | A 1440-wide Quest 2 surface becomes 1008 high; the unchanged `OnRenderScene` height filter rejects it before stereo production. |

FXAA itself and the D24 eye-depth change existed in 0.1.0. Do not infer a
Quest 2 driver defect or insufficient GPU performance solely from these reports.
Windows is used for installation; the renderer and game execute on the headset.

Relevant current source: `GraphicsEffects.cpp` / `PublishMobileColorState`,
`VrCamera.cpp` / `ShouldRunStereo`, `Xr.cpp` / `RenderStereoEyeProjection`.
The compositor can deliberately submit a black fallback when it rejects a
stereo pair. A successful xrEndFrame and one layer therefore do not establish
that a world image was submitted.

## Captured report: 2026-09-08 21:27 UTC

Archive: `SAVR_Render_20260908-212702620_3c3f67d5.zip`, SHA256
`25e104a90d0a48ee72a26be322d25334d1cf79cdd0245c0335b79b13d24c574a`.
The archive is complete and readable; the two earlier pasted texts were truncated
Discord previews, ending 74 ms after a different process launch.

The report identifies Quest 2 / SM8250 / Android 14, with driver version 0819.0.
Active game PID is 4316, starting at 17:26:27.139 local device time. The log continues
to 17:27:02.328 and also contains older unrelated process history.

- Saved `ColorGrading=0`, `HdWeapons=0`, `Effects=1`, `RenderScale=4` (an index).
- The game loads OpenXR, creates its swapchains, reaches FOCUSED, and keeps
  presenting at approximately 72/72 according to its own VrApi records.
- Java flat SurfaceTexture timestamps keep advancing. This does not establish
  that the direct stereo ring contains a valid visible world.
- No game crash, GPU fault, explicit GL allocation error or out-of-memory event
  is captured. The memory snapshot is 456,770 KB total PSS / 198,242 KB graphics;
  late runtime records report 2,679 MB free. Transient unlogged failures remain possible.
- The runtime-broker error is followed by a successful manifest fallback. The
  call-order errors belong to PID 4538 (Guardian), not game PID 4316.
- The missing HD-weapon payload messages and thread-hint warning are not evidence
  that either caused the black world.
- No `debug.savr.render_diag` property and no native `[render.diag]` messages are
  present. Java SAVR messages alone do not mean that detailed diagnostics are ON.
- Android package version 2.11.311 is the retail game version. This report has no
  native build stamp, so the exact mod commit cannot be established from it.

## Diagnostic report: 2026-09-08 22:53 UTC

Archive: `SAVR_Render_20260908-225307292_72450aed.zip`, SHA256
`511c8fb7a612c06672196a6cc46d3f5573ea4c910684d36b0bc48a2a9b21c196`.
The player has now installed the diagnostic build: package update at 18:47:27
device time, native compile stamp Sep 8 2026 18:44:53. PID 13356 contains 81 native
diagnostic lines. Basic logging is ON; pixel reads are OFF.

At 18:53:04.202, the first transition window contains 27 `no-safe-pair` /
`fallback=black` frames. The following two windows each contain 144/144 of the
same rejection: 315 classified black frames in total. The GameThread reports
`stereo=1 fade=0 ped=1 cutscene=0 menu=0 controls=0 grade=0`.
Frame submission succeeds; no eye-copy diagnostic or submitted-stereo outcome
is present. This localizes the observed black fallback before eye copy, colour
resolve and FXAA, with the cutscene/fade gate already cleared.

`seq=-1` is the submitted sequence, not the newest producer sequence.
`no-safe-pair` conflates too few published pairs with a producer generation
that is not ready. The zero `fence_failures` count concerns source-read
retirement fences, not the producer-readiness check. Do not conclude that
the producer has stopped or that all synchronization is healthy from these
fields. Pixel probes cannot help until a candidate reaches the copy stage.

Static audit also found incomplete producer-ready notification coverage.
Retail `RenderQueue::RunThread` dispatches directly during Flush at 0x78d044,
and `ProcessAll_Locked` dispatches directly at 0x78cf20. Both can bypass our
two custom loops, the only current callers of `TryPublishStereoProducerFence`
and `NotifyStereoRetailSwapCompleted`. A swap consumed on a bypass path can
leave a published generation Pending. This is a concrete source-coverage defect;
the report does not establish that this player used that path. A fix must
cover every swap dispatch and preserve exact-generation completion ordering,
not simply mark the newest sequence ready after an unrelated queue service.
The later fence-OFF capture below bypasses that readiness requirement and still
fails. The dispatch-coverage gap remains a separate finding; it does not explain
the deterministic surface rejection now identified for this player.

## Fence-OFF report: 2026-09-08 23:14 UTC

Extracted report: `build/diagnostics/quest2-player-20260908-231427287/`.
Collector 1.1 reports no collection warnings. All 114 native `[render.diag]`
lines belong to the current game process, PID 15399; older unrelated log
history is also retained. Its build stamp is Sep 8 2026 18:44:53, with pixel
readbacks OFF (`logcat-render.txt:1954`).

- At startup, `debug.savr.rq_swap46_fence=0` is recorded at line 1962.
  The A/B override was active in this process, not merely set after launch.
- The game records `game surface 1440x1008` at line 2301.
- At 19:13:57.634, the gameplay gate is `stereo=1 fade=0 ped=1 cutscene=0
  menu=0 controls=0 grade=0` (line 2799).
- At 19:13:57.926, the transition window records 69 `no-safe-pair` black
  frames (lines 2800/2802). Subsequent windows repeatedly record 144/144
  `no-safe-pair` frames with `fallback=black`, `live=0`, `seq=-1`, and
  successful frame submission, beginning at lines 2810/2811.

The player reports that the world remains black. Disabling the fence/readiness
path did not recover the picture. `seq=-1` still describes the submitted
sequence; the source analysis below, rather than that field alone, identifies
why this surface never reaches the stereo producer.

## Identified source cause and correction

In 0.1.1, `main.cpp::OnSurfaceChanged` created the Android game surface with
the XR eye width and height. Commit `0edfcce` changed the flat surface height
to `max(64, round(flatWidth * 448 / 640))`, rounded down to an even number.
For this player's width, `1440 * 448 / 640 = 1008`, matching the runtime log.

`VrCamera.cpp::OnRenderScene` still required both main-pass dimensions to be
at least 1024. Its `h < 1024` early return sent this valid main world pass
through the ordinary mono renderer before stereo raster rendering/publication.
The transition gate could therefore request stereo while the compositor had
no usable stereo pair and deliberately presented black. This is a deterministic
size-policy mismatch, independent of whether the producer fence is enabled.
It does not establish a GPU-performance or Quest 2 driver failure.

The local correction registers the actual flat dimensions through
`vrcam::SetGameSurfaceSize` before the game surface callback. `OnRenderScene`
uses `StereoSurfacePolicy.h::IsStereoMainPassSize` to match those registered
dimensions. The original minimum/maximum validity bounds remain, and the old
1024 heuristic is retained only when dimensions have not yet been registered.
Mismatched reflection/offscreen passes remain outside the stereo path; XR eye
dimensions remain independent of the flat surface.

The diagnostic build now emits `surface-policy=registered-main-size-v1` at
registration and `main-pass accepted ... policy=registered-main-size-v1`
when it accepts the main pass. These markers identify the corrected path in
the running process. Source correction and host checks do not yet establish
that the affected headset displays the world correctly.

## Next player check

1. Publish/push the current public source-kit changes containing the native
   surface correction. Before updating, the player runs `RESTORE_STEREO_SYNC.bat`
   from the original test folder containing that device's property backup.
   This restores the earlier A/B overrides and stops SAVR while the original
   backup folder is still available.
2. The player runs UPDATE and completes the native rebuild/install, then runs
   `ENABLE_RENDER_DIAGNOSTICS.bat` for basic logs, with pixel readbacks OFF.
   Copying support BAT/PowerShell files alone cannot apply this renderer change.
   Keep the same game graphics settings; no additional rendering switches are
   required for this check.
3. Manually launch SAVR and reproduce the same cutscene transition. Wait ten
   seconds after it ends, keep the headset awake, and run
   `COLLECT_RENDER_REPORT.bat` while the game remains open.
4. Attach the complete ZIP manually and report `World visible: YES` or `NO`.
   Check the current PID for the new surface-policy/accepted-main-pass markers,
   stereo submission, and the reported visible result. Old retained markers
   alone cannot verify the newly installed build. Disable diagnostics afterwards.

## New diagnostic channel (requires the modified build)

Native diagnostics are in local public commit `0ca8766`; both source trees
contain them. The new ENABLE/DISABLE workflow and report-status warnings are
local follow-up changes from 2026-09-09. This task has not published or installed
those follow-up changes. A binary built before the native diagnostic commit
cannot produce this channel.

With the updated support scripts, run `ENABLE_RENDER_DIAGNOSTICS.bat` once,
then open the game manually and reproduce. This button enables basic diagnostics,
disables pixel probes and stops only the named game so the next launch reads
the settings. `DISABLE_RENDER_DIAGNOSTICS.bat` turns both channels OFF and stops
the game. Neither button launches or installs anything.

Equivalent basic capture commands:

```text
adb shell setprop debug.savr.render_diag 1
adb shell setprop debug.savr.render_diag_pixels 0
adb shell am force-stop com.rockstargames.gtasa
```

Open the game manually and reproduce the issue. Leave the game running and the
headset awake, then run `COLLECT_RENDER_REPORT.bat` immediately. Give each capture
a short label describing settings and whether the world was visible. Attach
the ZIP manually to the developer: the buttons never upload it.

Basic diagnostics are separate from SAVR_DEV and remain in production builds.
They record build identity, device/GPU/GL versions and limits, configuration
overrides, the GameThread stereo gate each second, and two-second present
windows with rejection counts. Each eye is probed at most once every two seconds
for copy path, colour coefficients, texture validity, FBO status, and GL errors
pending before late overlays can consume them. Normal runs leave this OFF.

For a **second capture only**, enable sparse source/destination pixel reads:

```text
adb shell setprop debug.savr.render_diag_pixels 1
adb shell am force-stop com.rockstargames.gtasa
```

Open manually, reproduce, and collect again. Five RGBA points are sampled in
each source and destination eye before HUD/hand overlays. Readbacks can stall
the GPU, alter timing and even mask a synchronization bug; do not use this run
to compare performance. Basic capture is required first. Pixel-pack and read-FBO
state are restored as required by the [GLES readback API](https://registry.khronos.org/OpenGL-Refpages/es3.0/html/glReadPixels.xhtml).

Disable diagnostics after testing and restart:

```text
adb shell setprop debug.savr.render_diag 0
adb shell setprop debug.savr.render_diag_pixels 0
adb shell am force-stop com.rockstargames.gtasa
```

These commands only configure logging/FXAA and stop the named game process.
They do not install a build or replace game data. For more than one connected
device add `-s SERIAL` immediately after `adb` in each command.

## Reading the report

| Observation | Next investigation |
|---|---|
| `fallback=black`, no-safe-pair/stale/recovery-hold | Producer publication, queue/fence progress and logged gate state. |
| Gate stays non-stereo with fade=2 after cutscene | Retail fade/camera transition and theater frame contents. |
| Complete FBOs, nonzero source points, black destination points | Colour coefficients/copy shader/GL state; compare grading-only and plain-blit runs. |
| Source sample points already black | World render target, fade/mask/depth/queue completion upstream of copy. |
| Source and destination points visible but headset black | Later overlays, projection/layer submission and runtime; probe is before overlays. |
| Invalid texture, incomplete FBO, allocation/GL errors | Dimensions, memory pressure, sharing and the actual GPU/driver version. |

Five black points do not prove the entire image is black. Errors labelled
`pending_through_copy` may originate earlier than the copy; GL errors cannot be
restored after reading. CPU/GPU fields have validity flags; unavailable metrics
are not zero load. `compiled` plus ELF BuildId/hash identify local diagnostics
even when the public version string remains 0.2.0.

## Offline validation and artifacts

Public and shared builds use isolated directories under
`build/quest2-render-diag/`, with `SAVR_DEV=OFF` explicitly configured. Build logs,
ELF identity/hash and the before/after source manifest are retained there.
The public patch includes the two new headers and small instrumentation changes
in Xr, VrCamera and PerfTelemetry; it preserves the existing rendering settings.

The report collector was functionally checked with a fake adb under Windows
PowerShell 5.1 and PowerShell 7, including partial failures, multiple/no-device
refusals, paths with spaces, ZIP contents and a read-only command allowlist.
No real device was contacted by the tools. Compilation and collector checks
do not establish a Quest 2 fix; the supplied reports establish the observations
above. The fence-OFF A/B has now completed without recovery. The separate
surface-policy host suite passed 23 cases, including acceptance of 1440x1008,
rejection of reflection/mismatched dimensions, and the unregistered fallback.
Both shared and public ARM64 production builds passed with `SAVR_DEV=OFF`.
The patch also passed an apply check against the backed-up original public
source. Evidence and binaries are under
`build/diagnostics/quest2-main-surface-fix/` (`validation.json`,
`quest2-main-surface-fix.patch`, `libsavr-public.so`, `libsavr-shared.so`).
No APK was installed and the device was untouched. Runtime acceptance requires
the affected player to test the rebuilt correction and return a report; no
corrected-build Quest 2 capture has yet been received.
