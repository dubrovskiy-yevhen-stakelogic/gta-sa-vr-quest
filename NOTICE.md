# Third-party notices

- **OpenXR headers** (`native/vendor/openxr/`) — Khronos OpenXR SDK headers,
  version 1.1.43, Apache License 2.0. The license is preserved at
  `native/vendor/openxr/LICENSE`.
- **OpenXR Android loader** — the build master downloads the official Khronos
  `org.khronos.openxr:openxr_loader_for_android:1.1.43` AAR from Maven Central,
  verifies it and extracts the ARM64 loader at build time. It is not included
  in this repository and is Apache License 2.0.
- **Apktool** — the build master downloads Apktool 3.0.3 at build time. It is
  not included in this repository and is Apache License 2.0.
- **VR hand assets** (`assets/vrhands/`) — baked from UltimateXR SDK hand
  models, copyright VRMADA, MIT License. Provenance and license are preserved
  beside the assets.

## Acknowledgements

- **gta-reversed / gta-reversed-modern** — the community reverse-engineering of
  the PC version of GTA San Andreas
  (https://github.com/gta-reversed/gta-reversed-modern). We consult it as a
  behavioural reference to understand the original game's logic (physics,
  vehicles, missions, streaming, etc.) when reproducing that behaviour for VR.
  No source code from that project is copied into or distributed with this kit:
  the VR layer targets the retail mobile ARM64 binary and is written
  independently. Credit and thanks to its authors and contributors.

This repository contains no GTA San Andreas APK, Rockstar source code or game
assets, no extracted `libGame.so`, and no sound-mod archive or audio banks.
Users must provide their own lawful inputs locally.

