# Quest 2 black world: investigation and diagnostic build

Updated: 2026-09-09. Native diagnostic source: local public commit `0ca8766` (0.2.0).

**Status: regression candidates identified; cause not confirmed on an affected Quest 2.**
The developer corrected the release boundary: **0.1.1 works; 0.1.2 is the first
broken release**, with the issue continuing in 0.2.0. CJ/bike appear briefly after
the cutscene, the world becomes black, menus and movement continue. The earlier
0.1.1-failure premise is superseded. An actual Quest 2 report is now available
(see below), but native diagnostics were disabled during that run.

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

## First tests for the corrected 0.1.1 to 0.1.2 boundary

Collect basic native diagnostics with current settings first. Keep Color Grading
at the same value throughout these tests and use the same save/transition.

1. Disable only the new replacement of retail swap with a GL fence:

   ```text
   adb shell setprop debug.savr.rq_swap46_fence 0
   adb shell am force-stop com.rockstargames.gtasa
   ```

   Open manually, reproduce and collect a separately labelled report. This new
   default-ON path only activates after a healthy stereo projection, so its
   timing is a plausible match for a brief visible world followed by black.
   That is a source-based hypothesis, not a cause proven by this archive.
2. Restore `debug.savr.rq_swap46_fence` to its prior value (default 1). In a
   separate run disable only `debug.savr.hidden_area_mask` with value 0 and
   restart. The default-ON depth mask was also added in 0.1.2.
3. If needed, test `debug.savr.rq_eye_set_backing=0` and then
   `debug.savr.rq_state_coalesce=0`, one at a time, with other overrides restored.
   Both new paths default ON and both properties existed in 0.1.2.

All these properties latch at process start. Restore the recorded original
values after testing; defaults are 1 for these four controls. Avoid changing
several at once, which loses attribution. No renderer behavior is changed by
the support-script update itself.

Colour grading, FXAA and D24 existed in working 0.1.1, so they are lower-priority
compatibility tests. The plain shader-bypass comparison on dry ground remains
Color Grading OFF plus `debug.savr.fxaa=0`, followed by a process restart.

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
do not establish a Quest 2 fix; the supplied report establishes only the
observations above. A capture with native diagnostics enabled is still required.
