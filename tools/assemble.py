#!/usr/bin/env python3
"""Assemble a VR build of GTA:SA from the user's own Play split APKs.

The original game is never redistributed and never rebuilt wholesale. Two
surgical changes are made and everything else is copied byte for byte:

  base.apk                     manifest patched; loader DEX and SAVR defaults added
  split_config.arm64_v8a.apk   lib/arm64-v8a/libsavr.so added
  the remaining splits         untouched apart from re-signing

apktool is used only as a manifest compiler: the decoded tree is rebuilt, but
just the compiled AndroidManifest.xml is taken out of the result. The original
resources.arsc and dex files stay exactly as Rockstar shipped them, so a bad
resource rebuild cannot silently reach the output.

All splits are re-signed with one locally generated key and installed together
with `adb install-multiple`, which keeps the split layout Play produced instead
of merging 1.5 GB of assets into a single archive.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLCHAIN = Path("C:/Dev/android-toolchain")
JAVA = TOOLCHAIN / "jdk21/bin/java.exe"
KEYTOOL = TOOLCHAIN / "jdk21/bin/keytool.exe"
BUILD_TOOLS = TOOLCHAIN / "sdk/build-tools/35.0.0"
ADB = TOOLCHAIN / "sdk/platform-tools/adb.exe"
APKTOOL = ROOT / "tools/vendor/apktool_3.0.3.jar"

SPLITS_IN = ROOT / "recon/apk-original"
BUILD = ROOT / "build"
OUT = BUILD / "out"

APPLICATION_CLASS = "com.savr.SavrApplication"
LAUNCHER_ACTIVITY = "com.rockstargames.gtasa.DownloaderActivity"
DEFAULT_SETTINGS_DIR = ROOT / "defaults" / "quest"
DEFAULT_SETTING_NAMES = (
    "vr_driving.ini",
    "vr_appearance.ini",
    "vr_calib.ini",
    "vr_graphics.ini",
    "vr_holsters.ini",
    "vr_hud.ini",
    "vr_locomotion.ini",
)
GAME_ACTIVITY = "com.rockstargames.gtasa.GameActivity"

# Signatures are stripped before re-signing; a stale source stamp refers to a
# key we do not have and only invites installer complaints.
STRIP_PREFIXES = ("META-INF/",)
STRIP_NAMES = ("stamp-cert-sha256",)


def run(cmd: list) -> subprocess.CompletedProcess:
    result = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(
            "FAILED: {}\n{}\n{}".format(" ".join(str(c) for c in cmd), result.stdout, result.stderr)
        )
    return result


# --------------------------------------------------------------------------
# manifest
# --------------------------------------------------------------------------

def patch_manifest(text: str, vr: bool) -> str:
    """Insert the VR layer's hooks into the decoded manifest."""

    # The application tag carries no android:name in the stock build, so this is
    # an insertion rather than a replacement. Our class only loads libsavr.so.
    app_tag = text.split("<application", 1)[1].split(">", 1)[0]
    if "android:name=" in app_tag:
        sys.exit("manifest already declares an Application class - refusing to overwrite it")
    text = text.replace(
        "<application ",
        '<application android:name="{}" '.format(APPLICATION_CLASS),
        1,
    )

    # DownloaderActivity is the stock launcher and it refuses to hand over to the
    # game until Play Core has served every asset pack. On a headset there is no
    # Play Store to serve them, so it can never succeed and has to stop being the
    # entry point: its launcher filters are dropped and GameActivity gets one.
    text = strip_launcher_filters(text)

    body = (
        '\n            <intent-filter>'
        '\n                <action android:name="android.intent.action.MAIN"/>'
        '\n                <category android:name="android.intent.category.LAUNCHER"/>'
    )
    if vr:
        # Horizon OS decides an app is immersive from the categories on the
        # activity it actually launches. Putting them on the stock launcher is
        # useless when that activity is skipped: the shell then lists the game
        # among its 2D panels and the session never leaves the flat world.
        body += (
            '\n                <category android:name="org.khronos.openxr.intent.category.IMMERSIVE_HMD"/>'
            '\n                <category android:name="com.oculus.intent.category.VR"/>'
        )
    body += '\n            </intent-filter>'

    if vr:
        # Activity-level, read off the ActivityInfo: without it Horizon OS
        # backgrounds the app the moment any system overlay appears.
        body = ('\n            <meta-data android:name="com.oculus.vr.focusaware" '
                'android:value="true"/>') + body

    # The stock tag is self-closing, so it has to be reopened to hold all this.
    text, count = re.subn(
        r'(<activity[^>]*android:name="{}"[^>]*?)/>'.format(re.escape(GAME_ACTIVITY)),
        lambda m: '{} android:exported="true">{}\n        </activity>'.format(m.group(1), body),
        text,
        count=1,
    )
    if count != 1:
        sys.exit("game activity {} not found - manifest layout changed".format(GAME_ACTIVITY))

    if vr:
        text = text.replace(
            "    <application",
            '    <uses-feature android:name="android.hardware.vr.headtracking" '
            'android:required="true" android:version="1"/>\n    <application',
            1,
        )
    return text


def strip_launcher_filters(text: str) -> str:
    """Drop the launcher intent-filters from the stock downloader activity."""
    pattern = r'<activity[^>]*android:name="{}".*?</activity>'.format(re.escape(LAUNCHER_ACTIVITY))
    match = re.search(pattern, text, re.DOTALL)
    if match is None:
        sys.exit("launcher activity {} not found - manifest layout changed".format(LAUNCHER_ACTIVITY))

    block = match.group(0)
    stripped = re.sub(r'\s*<intent-filter>(?:(?!</intent-filter>).)*?LAUNCHER.*?</intent-filter>',
                      "", block, flags=re.DOTALL)
    if stripped == block:
        sys.exit("no launcher intent-filter on {} - manifest layout changed".format(LAUNCHER_ACTIVITY))
    return text.replace(block, stripped, 1)


def build_manifest(vr: bool) -> bytes:
    """Decode, patch and recompile the manifest; return the binary AXML."""
    decoded = BUILD / "apk-decoded"
    rebuilt = BUILD / "base-rebuilt.apk"

    print("==> decoding base.apk")
    shutil.rmtree(decoded, ignore_errors=True)
    run([JAVA, "-jar", APKTOOL, "d", "-s", "-f", "-o", decoded, SPLITS_IN / "base.apk"])

    manifest = decoded / "AndroidManifest.xml"
    print("==> patching manifest")
    manifest.write_text(patch_manifest(manifest.read_text(encoding="utf-8"), vr), encoding="utf-8")

    print("==> recompiling manifest")
    rebuilt.unlink(missing_ok=True)
    run([JAVA, "-jar", APKTOOL, "b", decoded, "-o", rebuilt])

    with zipfile.ZipFile(rebuilt) as archive:
        return archive.read("AndroidManifest.xml")


# --------------------------------------------------------------------------
# apk surgery
# --------------------------------------------------------------------------

def rewrite_apk(src: Path, dst: Path, replace: dict, add: dict) -> None:
    """Copy an APK entry by entry, swapping and appending the named files.

    Each entry keeps the compression method it had. That is not cosmetic:
    from API 30 the installer rejects any package whose resources.arsc is
    deflated rather than stored, so blanket re-compression makes the build
    uninstallable with a parse error that never mentions compression.
    """
    pending = dict(replace)
    dst.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, "w") as zout:
        for item in zin.infolist():
            if item.filename.startswith(STRIP_PREFIXES) or item.filename in STRIP_NAMES:
                continue
            data = pending.pop(item.filename, None)
            entry = zipfile.ZipInfo(item.filename, date_time=item.date_time)
            entry.compress_type = item.compress_type
            entry.external_attr = item.external_attr
            zout.writestr(entry, data if data is not None else zin.read(item.filename))
        for name, data in add.items():
            zout.writestr(name, data, zipfile.ZIP_DEFLATED)
    if pending:
        sys.exit("{}: entries to replace not found: {}".format(src.name, sorted(pending)))


def align_and_sign(apk: Path, keystore: Path) -> None:
    aligned = apk.with_suffix(".aligned.apk")
    run([BUILD_TOOLS / "zipalign.exe", "-f", "4", apk, aligned])
    aligned.replace(apk)
    run([
        BUILD_TOOLS / "apksigner.bat", "sign",
        "--ks", keystore, "--ks-pass", "pass:savrsavr", "--key-pass", "pass:savrsavr",
        "--v1-signing-enabled", "true", "--v2-signing-enabled", "true",
        apk,
    ])


def ensure_keystore() -> Path:
    keystore = BUILD / "savr.keystore"
    if keystore.exists():
        return keystore
    print("==> generating signing key")
    keystore.parent.mkdir(parents=True, exist_ok=True)
    run([
        KEYTOOL, "-genkeypair", "-keystore", keystore, "-alias", "savr",
        "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000",
        "-storepass", "savrsavr", "-keypass", "savrsavr", "-dname", "CN=SAVR",
    ])
    return keystore


# --------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--vr", action="store_true",
        help="declare the app immersive (stage 1 onward; without it the build stays a flat 2D app)",
    )
    parser.add_argument("--install", metavar="SERIAL", help="adb install-multiple onto this device")
    args = parser.parse_args()

    lib = BUILD / "native/libsavr.so"
    dex = BUILD / "classes.dex"
    for required in (lib, dex, APKTOOL):
        if not required.exists():
            sys.exit("missing {} - run tools/build.ps1 (Windows) or tools/build.sh first".format(required))

    manifest = build_manifest(args.vr)

    default_settings = {}
    for name in DEFAULT_SETTING_NAMES:
        source = DEFAULT_SETTINGS_DIR / name
        if not source.is_file():
            sys.exit("missing shipping default setting: {}".format(source))
        default_settings["assets/savr_defaults/{}".format(name)] = source.read_bytes()

    shutil.rmtree(OUT, ignore_errors=True)
    OUT.mkdir(parents=True)

    print("==> assembling splits")
    # base.apk ships three dex files, so ours lands as classes4.dex.
    rewrite_apk(
        SPLITS_IN / "base.apk", OUT / "base.apk",
        replace={"AndroidManifest.xml": manifest},
        add={"classes4.dex": dex.read_bytes(), **default_settings},
    )
    # The OpenXR loader rides along with our library rather than being linked
    # statically: it is the piece that talks to Horizon OS, and shipping the
    # exact Khronos build keeps that contract out of our hands.
    loader = ROOT / "native/vendor/openxr/lib/arm64-v8a/libopenxr_loader.so"
    rewrite_apk(
        SPLITS_IN / "split_config.arm64_v8a.apk", OUT / "split_config.arm64_v8a.apk",
        replace={},
        add={
            "lib/arm64-v8a/libsavr.so": lib.read_bytes(),
            "lib/arm64-v8a/libopenxr_loader.so": loader.read_bytes(),
        },
    )
    for name in ("split_config.uk.apk", "split_config.xxhdpi.apk", "split_data_main.apk"):
        rewrite_apk(SPLITS_IN / name, OUT / name, replace={}, add={})

    keystore = ensure_keystore()
    print("==> signing")
    for apk in sorted(OUT.glob("*.apk")):
        align_and_sign(apk, keystore)
        print("    {}".format(apk.name))

    if args.install:
        print("==> installing")
        run([ADB, "-s", args.install, "install-multiple", "-r", *sorted(OUT.glob("*.apk"))])
        print("installed")

    print("\noutput: {}".format(OUT))


if __name__ == "__main__":
    main()
