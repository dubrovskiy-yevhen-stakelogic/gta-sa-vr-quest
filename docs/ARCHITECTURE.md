# GTA San Andreas VR Quest architecture

This source kit builds a personal VR modification of the official Google Play
ARM64 release of GTA San Andreas 2.11.311. It does not redistribute an APK,
Rockstar game data, sound banks, extracted native libraries, or signing keys.

## Build and injection boundary

The builder validates the package name, version, Google Play signer, and the
SHA-256 of `libGame.so` before modifying a copy of the supplied split set. It
then:

1. builds `libsavr.so` for ARM64;
2. builds the minimal `SavrApplication` Java loader;
3. places the loader in the first unused `classesN.dex` slot;
4. updates the copied Android manifest with the VR application and activity
   declarations required by Horizon OS;
5. adds `libsavr.so` and the verified Khronos OpenXR loader only to the ARM64
   package;
6. aligns and signs every output APK with the player's persistent personal key.

The original `libGame.so` must remain byte-identical. Build and install scripts
fail closed when the supported retail fingerprint or expected package layout is
not present.

## Runtime structure

`SavrApplication` loads `libsavr.so` before the game activity starts. The native
layer waits until `libGame.so` is present, resolves exported engine symbols, and
installs retail-version-guarded hooks. Optional hooks remain disabled when their
symbol, instruction, or ownership checks do not match.

The game thread records the two eye views through the mobile RenderWare path.
The OpenXR thread owns the headset session, swapchains, controller state, VR
hands, and composition layers. Shared state between the two paths uses bounded
buffers, atomics, mutexes, and explicit fallback behavior rather than changing
the original game binary on disk.

Quest controllers are translated to the game's existing mobile gamepad input.
The same input path drives the in-headset VR menus for weapon and holster
calibration, driving, locomotion, HUD, graphics, appearance, and cheats.

## Player-owned runtime data

The installer deploys only verified payload trees:

```text
/sdcard/savr/data_main/assets
/sdcard/Android/data/com.rockstargames.gtasa/files/audio
/sdcard/Android/data/com.rockstargames.gtasa/files/vrhands
```

VR menu choices and calibration values are separate small `vr_*.ini` files in
the application's external `files` directory. `RESET_VR_SETTINGS` removes only
the exact supported settings list, leaving saves, game data, audio, hand assets,
performance captures, and installed APKs intact.

## Verification boundary

The tooling verifies source inputs, hashes, the build, signatures, manifests,
payload transfer, and on-device file placement. Those checks do not establish
headset rendering or gameplay behavior. Runtime claims require a fresh manual
test started by the player after installation.
