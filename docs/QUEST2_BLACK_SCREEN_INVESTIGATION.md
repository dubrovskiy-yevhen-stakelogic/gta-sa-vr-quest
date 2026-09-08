# Quest 2 black world: investigation and diagnostic build

Date: 2026-09-08. Public baseline: `e344d99` (0.2.0).

**Status: regression candidates identified; cause not confirmed on an affected Quest 2.**
The reported sequence is: 0.1.0 works, 0.1.1 and 0.2.0 fail after the opening
cutscene, CJ/bike appear briefly, the world becomes black, menus and movement
continue. No affected-device capture was supplied for this investigation.

## What the release history establishes

| Change | First relevant commit | Why it matters |
|---|---|---|
| Initial public 0.1.0 | `1c1555b` | Comparison baseline; release tags are absent. |
| Colour resolve, default ON | `36195c9` | Separate from WORLD EFFECTS. FXAA OFF still uses a graded copy when colour grading is active. |
| Script-control/camera gate and default eye scale 100 to 110 percent | `1eb18f7` | Affects transitions and increases eye pixel allocations by about 21 percent. Scale defaults reverted in 0.1.2. |
| Fully opaque fade routes to theater | `abdef50` | A persistent fade/gate condition could keep the world out of stereo. |
| First source labelled 0.1.1 | `bab8be7` | Exact player APK still needs identification; later 0.1.1 commits exist. |
| Effects default enabled | `2521c2f` | Initial 0.1.1 already had effects OFF, so effects are not a sufficient explanation. |
| New liveness, eye backing/fence, hidden-area mask and foveation paths | `0edfcce`, 0.1.2 | These cannot alone explain a verified first-0.1.1 regression. |

FXAA itself and the D24 eye-depth change existed in 0.1.0. Do not infer a
Quest 2 driver defect or insufficient GPU performance solely from these reports.
Windows is used for installation; the renderer and game execute on the headset.

Relevant current source: `GraphicsEffects.cpp` / `PublishMobileColorState`,
`VrCamera.cpp` / `ShouldRunStereo`, `Xr.cpp` / `RenderStereoEyeProjection`.
The compositor can deliberately submit a black fallback when it rejects a
stereo pair. A successful xrEndFrame and one layer therefore do not establish
that a world image was submitted.

## Useful test on the existing public release

Keep the same save/location and record a report before changing settings.

1. Turn **Color Grading OFF** in the graphics menu, fully close/reopen the game,
   and test the same cutscene transition. WORLD EFFECTS is a separate setting.
2. If still black, retain Color Grading OFF, set FXAA OFF, close/reopen and test:

   ```text
   adb shell setprop debug.savr.fxaa 0
   adb shell am force-stop com.rockstargames.gtasa
   ```

   Open the game manually. On dry ground, with grading OFF, this uses the plain
   blit instead of both colour-copy shaders. Underwater has a separate tint path.
3. Save a separate report for each outcome. Restore the prior grading/FXAA values
   afterwards. The normal FXAA default is restored with `adb shell setprop debug.savr.fxaa 1`
   followed by a full process restart, unless the player had a different override.

A successful combined bypass identifies a suspect path, not an exact faulty
instruction. The separate grading-only run is needed to narrow it further.

## New diagnostic channel (requires the modified build)

The new source is in both the shared development tree and public source kit.
It has not been pushed/published or installed on a headset by this task.
The original downloaded 0.2.0 binary cannot produce this channel.

Basic capture:

```text
adb shell setprop debug.savr.render_diag 1
adb shell setprop debug.savr.render_diag_pixels 0
adb shell am force-stop com.rockstargames.gtasa
```

Open the game manually and reproduce the issue. Leave the game running and the
headset awake, then run `COLLECT_RENDER_REPORT.bat` immediately. Give each capture
a short label describing settings and whether the world was visible.

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
No real device was contacted. Compilation and collector checks do not establish
Quest 2 rendering acceptance; that requires a report/test from an affected user.
