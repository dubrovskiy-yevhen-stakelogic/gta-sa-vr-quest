# GTA San Andreas VR for Quest — Source Kit

Version `0.1.0 alpha`.

This repository contains the source code and build/install tools for the GTA
San Andreas VR Quest mod. It does **not** contain GTA San Andreas, Rockstar
assets, the sound mod, a prebuilt APK, native binaries, or signing keys.

> [!TIP]
> **Join the Flat2VR Discord!** Development updates, player feedback, testing,
> and discussion of the mod take place in the
> [GTA San Andreas VR discussion channel](https://discord.com/channels/747967102895390741/1540234546182750228).
> Join the Flat2VR server first if the channel link does not open for you.

## Requirements

- Your own installed copy of GTA San Andreas from Google Play, version
  `2.11.311`, ARM64. Export the complete Play split set into one directory or
  archive. A single `base.apk` is not sufficient.
- A separate archive or extracted directory containing the supported PS2-style
  sound mod. The recommended input is the exact file
  `gta-sa-ps2-style-mod-pack_1786856007_737162.7z`. The wizard copies only 59
  verified files from `CONFIG`, `SFX`, and `STREAMS`; CLEO scripts, saves,
  models, launch configuration, and all other mod-pack content are ignored.
  In the original Play split set, `assets/audio` contains only the service
  `config` subset. The required `SFX` and `STREAMS` data is not present in the
  APKs, so seeing that directory in an archive browser or simply unpacking the
  original APK is not enough.
- Windows 10/11, Linux x86_64, or macOS. Internet access is required for the
  initial toolchain download. Installing on Quest also requires Developer Mode
  and authorized USB debugging.
- Approximately 15 GB of free disk space on the computer and 6 GB on Quest.

## Exporting your own Google Play APK set

Use an Android phone or tablet on which your legally owned Google Play copy of
GTA San Andreas `2.11.311` is currently installed. Do not download an APK from a
third-party APK site: the build wizard accepts only the original Google Play
certificate and rejects modified, merged, or re-signed packages.

### One-click Windows export

The easiest Windows method does not require installing ADB or typing commands:

1. Enable USB debugging on the phone/tablet containing the Google Play game.
2. Connect it by USB, unlock it, and approve the debugging prompt.
3. Double-click `EXPORT_PLAY_APKS.bat`.
4. Keep the device unlocked until the export completes.
5. Disconnect the phone/tablet, run `BUILD_AND_INSTALL.bat`, and select the
   exported `base.apk`. The builder automatically includes its sibling splits.

The exporter downloads Google's pinned Platform Tools itself, verifies their
SHA-256, checks GTA SA version `2.11.311` (`4234641`), exports every installed
split, verifies the resulting files, and opens the finished folder.

### Manual export

Download and extract Google's official [SDK Platform Tools for
Windows](https://developer.android.com/tools/releases/platform-tools). Enable
Developer Options and USB debugging on the Android device, connect it by USB,
and approve the authorization prompt.

If PowerShell says `adb is not recognized`, do not continue with a bare `adb`
command. Point PowerShell directly at the extracted executable instead (change
the path if you extracted it elsewhere):

```powershell
$adb = (Resolve-Path "$HOME\Downloads\platform-tools\adb.exe").Path
& $adb version
```

Confirm the device and installed game version:

```powershell
& $adb devices
& $adb shell dumpsys package com.rockstargames.gtasa |
  Select-String 'versionName=|versionCode='
```

The expected values are `versionName=2.11.311` and `versionCode=4234641`.

On Windows PowerShell, export every installed split into one directory:

```powershell
$destination = Join-Path $PWD 'GTA-SA-Play-export'
New-Item -ItemType Directory -Force -Path $destination | Out-Null
$apkPaths = @(
  & $adb shell pm path com.rockstargames.gtasa |
    ForEach-Object { ($_ -replace '^package:', '').Trim() } |
    Where-Object { $_ }
)
if ($apkPaths.Count -lt 3) {
  throw 'A complete split APK installation was not found.'
}
foreach ($remotePath in $apkPaths) {
  & $adb pull $remotePath $destination
  if ($LASTEXITCODE -ne 0) { throw "Failed to export $remotePath" }
}
```

On Linux or macOS, use:

```bash
destination="$PWD/GTA-SA-Play-export"
mkdir -p "$destination"
apk_paths="$(adb shell pm path com.rockstargames.gtasa | tr -d '\r' | sed 's/^package://')"
test -n "$apk_paths" || { echo 'GTA San Andreas is not installed'; exit 1; }
for remote_path in $apk_paths; do
  adb pull "$remote_path" "$destination/" || exit 1
done
```

The resulting directory must contain the base APK, the ARM64 split, the
`data_main`/assets split, and any locale or density splits returned by
`pm path`. Do not rename, merge, modify, or re-sign these input files. Select
the complete `GTA-SA-Play-export` directory when the build wizard asks for the
original game package. The wizard performs the final version, ABI, game-library,
split, and official-signer checks before building or accessing Quest.

If the Android device refuses direct `adb pull` access to its installed APKs,
use an on-device split-APK backup/export tool that preserves every installed APK
unchanged, then select the exported `.apks` archive or directory. The same strict
certificate and content checks still apply.

## Quick start on Windows

1. Double-click `BUILD_AND_INSTALL.bat`.
2. Select the APK, archive, or directory containing the complete original-game
   export.
3. Select the sound-mod archive or directory.
4. Connect the Quest and approve USB debugging inside the headset.
5. Review the installation summary.

On the first run, type `ACCEPT` when asked only if you agree to the Google
Android SDK licenses. The wizard then handles sdkmanager's repetitive `y/N`
prompts. Dependency downloads report transferred size, total size, speed, and
ETA every five seconds; a connection that receives no data for 90 seconds fails
with a retry message instead of appearing to hang indefinitely.

The wizard validates both inputs and completes the build on the computer before
changing anything on Quest. It installs the APK set, deploys `data_main`, audio,
and VR hands, verifies the resulting hashes, and leaves the game stopped. It
never launches the game automatically.

The original Play APKs must be signed with a personal key. A first installation
may therefore require removal of an installed copy that uses a different
signature. The wizard backs up accessible saves and settings first and **always
asks for separate confirmation** before removal. Keep the personal signing key
stored in `%LOCALAPPDATA%\GTASAVRBuilder\signing`; it is required for updates.

### Build only, without connecting a Quest

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\build-and-install.ps1 `
  -GamePackage "D:\Backups\GTA-SA-Play-export" `
  -AudioSource "D:\Mods\gta-sa-ps2-style-mod-pack_1786856007_737162.7z" `
  -BuildOnly
```

The output and complete `build-manifest.json` are written to the run directory
under `C:\SAVRBuild`, or under the directory passed through `-WorkDir`.

## Linux and macOS

On Linux x86_64, install Bash, Python 3.10 or newer, `curl`, `tar` with xz
support, and `unzip`. Installing on Quest also requires working USB access and
appropriate Android udev rules. macOS requires the equivalent command-line
tools and USB access. The wizard downloads the pinned JDK 21, Android SDK, and
other build tools when needed.

Run the interactive wizard with:

```bash
bash BUILD_AND_INSTALL.sh
```

The recommended `.7z` is extracted with the automatically downloaded, pinned
7-Zip `26.02`. Both the archive and executable are verified by SHA-256. A
system-wide 7-Zip installation and manual extraction are not required.

To build and validate the output without connecting a Quest:

```bash
bash BUILD_AND_INSTALL.sh \
  --game-package "$HOME/Backups/GTA-SA-Play-export" \
  --audio-source "$HOME/Downloads/gta-sa-ps2-style-mod-pack_1786856007_737162.7z" \
  --work-dir "$HOME/SAVRBuild" \
  --build-only
```

Each run receives a separate `~/SAVRBuild/runs/<run-id>` directory, or
`<work-dir>/runs/<run-id>`, so stale CMake caches are never reused. The
persistent signing key is stored at
`$XDG_DATA_HOME/gtasavr-builder/signing/savr.keystore`, or at
`~/.local/share/gtasavr-builder/signing/savr.keystore` when `XDG_DATA_HOME` is
unset. Do not delete or replace this key; it is required to update an installed
build.

If the Quest appears as `unauthorized`, approve USB debugging inside the headset
and reconnect the cable. For an `offline` device, restart ADB and reconnect it.
A permissions error or missing device on Linux usually means that Android udev
rules must be installed or corrected before signing in again or reconnecting
the Quest.

Run `bash BUILD_AND_INSTALL.sh --help` for all options. Before its first Quest
mutation, the wizard prints the selected device, APK and payload sizes, signing
key path, and complete action list, then requires the word `INSTALL`.
Automation requires both `--non-interactive` and `--yes`; destructive removal
of a package with a foreign signature is never automated. ZIP executable bits
are not required because every internal shell script is invoked through Bash.
After installation, the wizard leaves GTA SA stopped and never launches it.

## Resetting VR settings on Quest

Remove old player overrides after compiled calibration defaults change:

- Windows: double-click `RESET_VR_SETTINGS.bat`.
- Linux/macOS: run `bash RESET_VR_SETTINGS.sh`.

The script selects the connected Quest, prints the exact plan, and asks for the
word `RESET`. It then stops GTA SA, removes only the eight exact VR settings
files listed in [BUILDING.md](BUILDING.md), and verifies the result. Saves,
`audio`, `vrhands`, game data, APKs, and performance CSV files remain intact.
The game is not launched; new compiled defaults take effect after the next
manual start. Version 0.1.0 embeds the author's release-Quest menu,
weapon, HUD, holster, and vehicle calibration as its defaults. The sole quality
override is the eye-buffer resolution, which resets to `100%`.

For automation, use `-Yes -NonInteractive` in PowerShell or
`--yes --non-interactive` in Bash. With multiple devices, specify
`-Serial`/`--serial`.

## Supported original game

The public wizard fails closed and accepts only the verified Google Play ARM64
release:

- package: `com.rockstargames.gtasa`
- version: `2.11.311` (`versionCode 4234641`)
- official signer SHA-256:
  `FF5B7B6A083FE5994E3306B30AE19D311951D019A8DE7C3E6914F0E06D130A13`
- `libGame.so` SHA-256:
  `4C6A7445E30B27AFDDA781302E4DB9BAC89C28FC1181B68B1EEF16F84D6A282E`

Locale and density splits may vary; the wizard identifies them from their
manifests rather than filenames. Directories and `.zip`, `.apks`, `.xapk`,
`.apkm`, `.7z`, and `.rar` archives are supported. On Linux and macOS, supported
`.7z` and `.rar` inputs are handled by the pinned, verified 7-Zip downloaded by
the wizard.

## Source kit contents

- `native/` — the ARM64 OpenXR/VR layer and permitted Khronos headers;
- `loader/` — the minimal Android `Application` loader;
- `tools/` — strict validation, assembly, and safe installation tools;
- `assets/vrhands/` — MIT-licensed UltimateXR-derived hand assets;
- `docs/` — public architecture and project-boundary documentation.

See [BUILDING.md](BUILDING.md) and [NOTICE.md](NOTICE.md) for details.

## Validation boundary

The source kit validates sources, builds, signatures, APK payloads, and file
copying. This does not prove that the game works correctly inside a headset.
After installation, the player starts GTA SA manually and performs the visual
and runtime validation.

GTA, Grand Theft Auto and Rockstar Games are trademarks of their respective
owners. This independent project is not affiliated with or endorsed by
Rockstar Games or Take-Two Interactive.
