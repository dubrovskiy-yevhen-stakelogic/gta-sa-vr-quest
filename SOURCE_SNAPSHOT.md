# Source snapshot

This kit was prepared from the active GTA San Andreas VR Quest source tree on
2026-08-25.

- Mod version: `0.1.0 alpha`
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

The public-kit adaptation resets the public version to `0.1.0`, changes build
scripts and `native/CMakeLists.txt`, removes an unreachable prototype
weapon-mesh path, replaces stale internal research notes with one public
architecture document, sanitizes internal-only comment wording, and adds
reset/support tooling. No compiled OpenXR loader, APK, extracted game binary, or
local absolute toolchain path is required in the repository. The original
checkout has no Git metadata, so this is a file-manifest snapshot rather than a
commit identifier.
