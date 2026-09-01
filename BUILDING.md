# Building and installing

The normal entry points are `BUILD_AND_INSTALL.bat` on Windows and
`bash BUILD_AND_INSTALL.sh` on Linux/macOS. They prepare pinned tools, validate
user-owned inputs, build the native and Java components, assemble a personal
APK set, and optionally install it on a selected Quest.

## Inputs

`-GamePackage` accepts a complete Play split folder, a supported archive, or a
self-contained APK. A valid source must contain:

- one base APK for `com.rockstargames.gtasa` 2.11.311;
- exactly one ARM64 APK containing the expected `libGame.so`;
- a `data_main` split or equivalent APK containing `assets/`;
- any locale/density config splits supplied with that installation;
- the official Google Play certificate on every selected split.

Selecting `base.apk` automatically scans sibling APK files. A lone base APK is
rejected before any device action. The public master requires the original
Google Play certificate on the complete export; third-party repacks, modded
downloads and merged APKs with a changed signer are rejected even if their
version label looks correct. Export the split set from your own Play install.
Step-by-step ADB export commands for Windows, Linux, and macOS are provided in
[README.md](README.md#exporting-your-own-google-play-apk-set).

`-AudioSource`/`--audio-source` accepts a directory or archive. The recommended
input is the exact archive
[`gta-sa-ps2-style-mod-pack_1786856007_737162.7z`](https://libertycity.net/files/gta-san-andreas-ios-android/241069-gta-sa-classic-avanced-mod-pack.html).
On that page choose **Original plan mod pack** dated **16 August 2026** (1.41
GB), not **CLASSIC ADVANCED v1.0**. The validator searches for
the unique subtree containing `CONFIG/BankLkup.dat`, `SFX/GENRL.osw`, and
`STREAMS/CUTSCENE.osw`, then compares all 59 paths, sizes and SHA-256 values
against `tools/audio-reference.json`. Only the verified files under `CONFIG`,
`SFX`, and `STREAMS` are staged; unrelated mod-pack content is never copied.
The official Play split export has only the small `assets/audio/config` subset;
the required `SFX` and `STREAMS` payload is not present in those APKs. Seeing an
`assets/audio` directory in an archive browser is therefore not proof that the
sound input is complete.

The validation pass also builds the aircraft HLOD from the verified
`data_main` split. It extracts `assets/texdb/gta3.img` only into the disposable
build directory and writes `aircraft-hlod-global/aircraft.hlod` there before
native compilation. The generated pack is intentionally ignored by Git: the
public repository contains the generator and renderer, not Rockstar geometry.
The normal Windows and Linux/macOS masters perform this step automatically.

## Pinned build environment

The Windows master can download and verify:

- Eclipse Temurin JDK `21.0.11+10`;
- Android command-line tools `15859902`;
- Android platform `35`, Build Tools `35.0.0`;
- NDK `27.2.12479018`, CMake `3.22.1`;
- Python embeddable `3.12.10`;
- Apktool `3.0.3`;
- Khronos OpenXR Android loader `1.1.43`.

Downloaded archives are SHA-256 checked before extraction. The OpenXR loader
shared object is also checked after extraction. Nothing downloaded by the
master is committed to this repository. Windows downloads show byte progress,
speed, and ETA and stop with an explicit error after 90 seconds without data.
When SDK packages are first required, the interactive master asks the user to
type `ACCEPT` before it supplies the repetitive affirmative responses to
Google's `sdkmanager --licenses`; non-interactive mode never grants that
acceptance.

The Linux/macOS master additionally downloads pinned 7-Zip `26.02` when an
input requires it. Both the official 7-Zip archive and the extracted `7zz`
executable are SHA-256 checked before use. This is the extraction path used for
the recommended `.7z`; GNU tar support for 7-Zip archives is not assumed.

## Manual Windows build

With the dependencies already present:

```powershell
.\tools\build.ps1 `
  -AndroidSdk C:\Android\sdk `
  -JavaHome C:\Java\jdk-21 `
  -OpenXrLoader C:\deps\libopenxr_loader.so `
  -BuildRoot C:\SAVRBuild\manual `
  -PythonExe C:\Python312\python.exe `
  -Apktool C:\deps\apktool_3.0.3.jar `
  -GamePackage D:\Backups\GTA-SA-Play-export `
  -AudioSource D:\Mods\gta-sa-ps2-style-mod-pack_1786856007_737162.7z `
  -Keystore "$env:LOCALAPPDATA\GTASAVRBuilder\signing\savr.keystore" `
  -Package
```

Native-only output is `native/libsavr.so`; the Java loader is `classes.dex`.
During packaging the loader is placed in the first unused `classesN.dex` slot
(currently `classes4.dex`). `libsavr.so` and the verified Khronos loader are
added only to the ARM64 split. `libGame.so` must remain byte-identical.

## Command-line master

```powershell
.\tools\build-and-install.ps1 `
  -GamePackage <apk-or-export> `
  -AudioSource <archive-or-folder> `
  [-WorkDir C:\SAVRBuild] `
  [-Serial QUEST_SERIAL] `
  [-BuildOnly] `
  [-NonInteractive]
```

`-NonInteractive` requires both inputs. `-BuildOnly` performs no ADB mutation.
The low-level `tools/build.ps1` has a developer-only
`-AllowUnofficialSource` escape hatch that weakens provenance checks. It is
intentionally absent from the public master and must not be used for release
verification or player packages.

## Linux/macOS master

Linux x86_64 prerequisites are Bash, Python 3.10 or newer, `curl`, `tar` with
xz support, and `unzip`. An install run also requires working USB access and
appropriate udev rules for an Android device. macOS requires the equivalent
command-line tools and USB access. The master bootstraps its pinned JDK,
Android SDK/NDK/CMake, Apktool, OpenXR loader, and 7-Zip dependencies inside
the managed work directory.

Interactive entry point:

```bash
bash BUILD_AND_INSTALL.sh
```

Non-device BuildOnly example using the distributed sound-mod archive:

```bash
bash BUILD_AND_INSTALL.sh \
  --game-package "$HOME/Backups/GTA-SA-Play-export" \
  --audio-source "$HOME/Downloads/gta-sa-ps2-style-mod-pack_1786856007_737162.7z" \
  --work-dir "$HOME/SAVRBuild" \
  --build-only \
  --non-interactive
```

Every invocation builds in a new
`~/SAVRBuild/runs/<run-id>` directory, or `<work-dir>/runs/<run-id>` when
`--work-dir` is supplied. Tool downloads can be reused, but a stale CMake tree
or output from another invocation cannot become the current run.

The persistent signing key is stored by default at
`$XDG_DATA_HOME/gtasavr-builder/signing/savr.keystore`, or at
`~/.local/share/gtasavr-builder/signing/savr.keystore` when `XDG_DATA_HOME` is
unset. Preserve this file securely. Android updates must be signed with the
same key as the already installed personal build.

Before troubleshooting installation, inspect `adb devices -l`:

- `unauthorized`: accept the USB-debugging prompt inside the headset, then
  reconnect it;
- `offline`: reconnect the Quest and restart ADB if necessary;
- no device or a permissions error on Linux: install/correct Android udev
  rules, reload them (or sign out/in), and reconnect the headset.

The Bash master never starts GTA SA. `--build-only` performs no ADB selection,
stop, install, upload, or other Quest mutation.
For an intentional unattended install, pass `--non-interactive --yes` together.
The master still refuses to automate the destructive uninstall required for a
certificate change. Interactive installs print a complete plan and require the
exact word `INSTALL` before the first Quest mutation.

## Installation transaction

All local validation and signing complete before ADB installation. The master:

1. selects exactly one authorized device (or the requested serial);
2. records the selected model and checks free space;
3. force-stops GTA SA without launching it;
4. installs the complete rebuilt split set;
5. asks explicitly before uninstall if Android reports an incompatible signer;
6. uploads game data, audio and VR hands to staging directories;
7. runs `sha256sum -c` on-device and swaps each staged tree into place;
8. restores the previous tree on an activation failure;
9. verifies the installed APK paths and leaves the package force-stopped.

The runtime destinations are:

```text
/sdcard/savr/data_main/assets
/sdcard/Android/data/com.rockstargames.gtasa/files/audio
/sdcard/Android/data/com.rockstargames.gtasa/files/vrhands
```

## Resetting headset VR settings

Use `RESET_VR_SETTINGS.bat` on Windows or
`bash RESET_VR_SETTINGS.sh` on Linux/macOS after a release changes compiled
calibration defaults, or when a player wants to discard all menu adjustments.
The tool verifies the package and selected Quest, prints the exact target list,
requires the word `RESET`, force-stops the game, removes the files, and verifies
that they are absent. It never launches the game.

Only these persisted settings are removed:

```text
vr_appearance.ini
vr_basketball.ini
vr_calib.ini
vr_calib.ini.tmp
vr_driving.ini
vr_graphics.ini
vr_holsters.ini
vr_hud.ini
vr_locomotion.ini
```

Saves, `audio/`, `vrhands/`, game data, performance CSV files, installed APKs,
and every unlisted file are preserved. Command-line automation requires both
`-Yes -NonInteractive` (PowerShell) or `--yes --non-interactive` (Bash).

Build/sign/install evidence is not runtime evidence. The scripts never invoke
`am start`, `monkey`, or any other application launch command.
