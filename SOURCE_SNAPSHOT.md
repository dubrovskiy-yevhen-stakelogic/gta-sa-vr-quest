# Source snapshot

## Release candidate sync — 2026-09-01

The working tree includes the complete source of the current Quest runtime plus
the public build/install safety layer. This is a source-only sync; signed APKs,
original game files, audio, signing keys, diagnostics and local build products
are intentionally not part of the repository.

- `native/src/VrCamera.cpp`: `89F1BB767675CE79AF79F09D4744DA95FCF9EA9B27E8E97E4966139F42247D30`
- `native/src/Xr.cpp`: `B3622A2AB4778800F1099AC54E37CF272FA4FDE995A4D25B1AC8F61936D7064E`
- `native/src/Xr.h`: `47F9BB1226C0FC84774FA31A26D4735A9B4E1E774EC4A7004EF16D3D8E57E491`
- `native/src/main.cpp`: `F0AF48FB863C4DB229BBD17FAF1B419B922183FBDCE1FCB7B040E59248C9275F`
- `native/src/Driving.cpp`: `1370ECF0E14BB06D4F9E0F7EE3355FBD9BF8F207F1D4CA9F7DF737717ED2A392`
- `native/src/Basketball.cpp`: `9A5CD65721A08E58A2A942F08DBCEDBB1C5F6314CB8B48F711C765E029E032A1`
- `native/src/Locomotion.cpp`: `FA3618C8B6C5C48D30E178BEFA3C3CB660437A883F5A3B7C7CA8D554E308D602`
- `native/src/HudSettings.cpp`: `B2E9D54DAD9A94EC85BD288433509DCADCC4FD00829DD6299F4874FF0B251771`
- `native/src/PerfTelemetry.cpp`: `42F2BA988C5E9E19CF02CD7B4D0E7E92FFA3FD99BE9DC4AE38F3EB57554A05A8`
- `defaults/quest/vr_hud.ini`: `777632D404B0B13987323B9F7E5C6D1558179C0098C82BAFB2F54B732C416D6A`
- Aircraft HLOD input: `9DBA3B2D040E663D0EA495052CDCF3B0721F4EE560F6509504C13A77BA4CA1E3`
- Full player-config (`SAVR_DEV=OFF`) `BuildOnly` `libsavr.so`:
  `6A3CE1520864DAB773B237EE695270C66289BA042D85410223578485D7E83FFD`

The runtime contains the accepted stereo RenderQueue retirement/backing work,
adaptive fresh-pair wait, absolute 72 Hz deadline pacing, full-resolution menu
and map path, aircraft HLOD/flight-distance controls, graphics/shadow modes,
expanded control remapping, vehicle and holster calibration, physical pickups,
and immersive basketball. The player build keeps the profiler/debug chord and
persistent performance CSV captures out of the binary (`SAVR_DEV=OFF`). The
HUD menu also exposes an opt-out for wrist/dashboard gaze auto-hide, while the
default classic preset and the author's corrected health crop are preserved.

The current performance layer keeps the retail RenderQueue `Finish` fence but
overlaps its driver tail with left-eye command recording, then closes it before
the right eye begins. The compositor uses a bounded adaptive fresh-pair wait:
1.25 ms normally, up to 4.0 ms only for a stable near-72-Hz producer with at
least 1 ms of measured GPU headroom. Both paths are runtime-property gated and
preserve the previous synchronous/repeat fallbacks.

The outer GameThread limiter now uses an absolute monotonic 72-Hz deadline
grid. Normal `nanosleep` overshoot is repaid by the next frame instead of being
accumulated into a permanent ~71.8-Hz producer drift. A real missed deadline
rebases immediately, so the limiter never emits catch-up bursts. The previous
relative limiter remains available through `debug.savr.absolute_pacer=0`.

Controller-yaw MOTION driving is not exposed or executed; legacy saved values
migrate to ordinary stick control. Basketball AUTO RETURN is an explicit menu
opt-in and defaults to OFF, while the remaining shipped basketball physics and
hand calibration use the author's tested values.

This kit was prepared from the active GTA San Andreas VR Quest source tree on
2026-08-25.

- Mod version: `0.2.0 alpha`
- Active snapshot boundary: native build completed at `2026-08-25 23:39:12`
  (Europe/Kiev)
- Canonical author-source set before public-kit adaptations: 51 files
- Source manifest SHA-256:
  `6861B4187AE1B27D3A8579CBD310E4EBDA024B6A4AE4C6BE3B0E9177007A24A4`
- Reference loader DEX SHA-256:
  `824774163257697C3DF4C2AFA35F534CA0C8B37EFD2C2197252245FA38BB4468`

The manifest is the SHA-256 of an ordinally sorted UTF-8/LF list containing
`relative-path<TAB>lowercase-file-sha256`, with a final LF, for the active
tree's root `.gitignore`, `README.md`, `native/CMakeLists.txt`, all 46 files
under `native/src`, the Java loader, and `tools/build.ps1`. It records the
private active-tree provenance before the public adaptations below; the release
audit treats it as documentation rather than a hash of the adapted kit.

The public-kit adaptation publishes version `0.2.0 alpha`, hardens the build
scripts and `native/CMakeLists.txt`, sanitizes internal-only wording, and adds
reset/support tooling. No compiled OpenXR loader, APK, extracted game binary,
audio, signing key, or local absolute toolchain path is required in the
repository. The original checkout has no Git metadata, so the provenance block
above is a file-manifest snapshot rather than a commit identifier.
