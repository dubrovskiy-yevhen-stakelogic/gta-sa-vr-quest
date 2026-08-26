#!/usr/bin/env python3
"""Build a personal GTA San Andreas VR package from user-owned inputs.

This tool never downloads or redistributes GTA. It accepts an official
2.11.311 Play split export (or a compatible single APK), validates the retail
engine, injects the source-built VR layer, stages the user's game assets and
the separately supplied audio mod, and signs the result with a personal key.

Device installation is intentionally handled by build-and-install.ps1/.sh so
all local validation can finish before a Quest is modified.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import zipfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Iterable


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BUILD = ROOT / "build"
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

PACKAGE_NAME = "com.rockstargames.gtasa"
VERSION_CODE = "4234641"
VERSION_NAME = "2.11.311"
OFFICIAL_SIGNER_SHA256 = (
    "FF5B7B6A083FE5994E3306B30AE19D311951D019A8DE7C3E6914F0E06D130A13"
)
LIBGAME_SHA256 = (
    "4C6A7445E30B27AFDDA781302E4DB9BAC89C28FC1181B68B1EEF16F84D6A282E"
)
OPENXR_LOADER_SHA256 = (
    "713E3BB8D955254C670ACC1C4899A65CB8C930E97DD9958BF37EA922D72B7A06"
)
APKTOOL_SHA256 = (
    "DBF930B076C6B9BE08D57C449CACEFC3BDD6B71EBD59B3066FC0E1F5B14F9423"
)

APPLICATION_CLASS = "com.savr.SavrApplication"
LAUNCHER_ACTIVITY = "com.rockstargames.gtasa.DownloaderActivity"
GAME_ACTIVITY = "com.rockstargames.gtasa.GameActivity"

LIBGAME_ENTRY = "lib/arm64-v8a/libGame.so"
DATA_SENTINELS = (
    "assets/data/gta.dat",
    "assets/anim/anim.img",
    "assets/texdb/gta3.img",
)
AUDIO_SENTINELS = (
    "CONFIG/BankLkup.dat",
    "SFX/GENRL.osw",
    "STREAMS/CUTSCENE.osw",
)
SUPPORTED_AUDIO_ARCHIVE = "gta-sa-ps2-style-mod-pack_1786856007_737162.7z"
SUPPORTED_AUDIO_PAGE = (
    "https://libertycity.net/files/gta-san-andreas-ios-android/241069-gta-sa-classic-avanced-mod-pack.html"
)
HAND_FILES = (
    "BigHandLeft.uxrh",
    "BigHandRight.uxrh",
    "BigHandsAlbedo.rgba",
)
OFFICIAL_ARM64_LIBS = {
    "lib/arm64-v8a/libapp.so",
    "lib/arm64-v8a/libc++_shared.so",
    "lib/arm64-v8a/libdatastore_shared_counter.so",
    "lib/arm64-v8a/libflutter.so",
    "lib/arm64-v8a/libGame.so",
    "lib/arm64-v8a/libopenal.so",
    "lib/arm64-v8a/libsentry-android.so",
    "lib/arm64-v8a/libsentry.so",
    "lib/arm64-v8a/libVendor_mpg123.so",
    "lib/arm64-v8a/libz.so",
}

STRIP_NAMES = ("stamp-cert-sha256",)
SIGNATURE_SUFFIXES = (".sf", ".rsa", ".dsa", ".ec")
ARCHIVE_SUFFIXES = {".zip", ".apks", ".xapk", ".apkm", ".7z", ".rar"}
MAX_APK_ARCHIVE_FILES = 64
MAX_SINGLE_APK_BYTES = 2_000_000_000
MAX_APK_ARCHIVE_BYTES = 2_500_000_000


class KitError(RuntimeError):
    """Expected, user-facing source-kit failure."""


@dataclass(frozen=True)
class Toolchain:
    sdk: Path
    java_home: Path
    build_tools: Path
    android_jar: Path
    aapt2: Path
    zipalign: Path
    apksigner: Path
    keytool: Path
    java: Path
    apktool: Path


@dataclass(frozen=True)
class InputApk:
    path: Path
    original_name: str
    package: str
    version_code: str
    version_name: str
    split: str | None


@dataclass(frozen=True)
class GamePackage:
    apks: tuple[InputApk, ...]
    base: InputApk
    arm64: InputApk
    data: InputApk
    monolithic: bool
    signer_sha256: str


@dataclass(frozen=True)
class AudioTree:
    root: Path
    entries: tuple[dict, ...]


def say(message: str) -> None:
    print(message, flush=True)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def run(command: Iterable[object], *, failure: str, cwd: Path | None = None) -> str:
    args = [str(value) for value in command]
    result = subprocess.run(
        args,
        cwd=str(cwd) if cwd else None,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    combined = "\n".join(part for part in (result.stdout, result.stderr) if part).strip()
    if result.returncode != 0:
        detail = f"\n{combined}" if combined else ""
        raise KitError(f"{failure} (exit code {result.returncode}).{detail}")
    return combined


def ensure_inside(path: Path, parent: Path) -> None:
    resolved = path.resolve()
    root = parent.resolve()
    if resolved == root:
        raise KitError(f"refusing to replace the build root itself: {resolved}")
    try:
        resolved.relative_to(root)
    except ValueError as error:
        raise KitError(f"refusing to modify path outside build root: {resolved}") from error


def trees_overlap(first: Path, second: Path) -> bool:
    first_resolved = first.resolve()
    second_resolved = second.resolve()
    if first_resolved == second_resolved:
        return True
    try:
        first_resolved.relative_to(second_resolved)
        return True
    except ValueError:
        pass
    try:
        second_resolved.relative_to(first_resolved)
        return True
    except ValueError:
        return False


def clean_directory(path: Path, build_root: Path) -> None:
    ensure_inside(path, build_root)
    if path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True, exist_ok=True)


def safe_member(name: str) -> PurePosixPath:
    normalized = name.replace("\\", "/")
    if "\x00" in normalized or "\r" in normalized or "\n" in normalized:
        raise KitError(f"unsafe archive member: {name!r}")
    member = PurePosixPath(normalized)
    if member.is_absolute() or ".." in member.parts or not member.parts:
        raise KitError(f"unsafe archive member: {name}")
    if any(":" in part for part in member.parts):
        raise KitError(f"unsafe archive member: {name}")
    return member


def clone_zip_info(source: zipfile.ZipInfo) -> zipfile.ZipInfo:
    target = zipfile.ZipInfo(source.filename, date_time=source.date_time)
    target.compress_type = source.compress_type
    target.comment = source.comment
    target.extra = source.extra
    target.internal_attr = source.internal_attr
    target.external_attr = source.external_attr
    target.create_system = source.create_system
    target.flag_bits = source.flag_bits
    return target


def copy_zip_stream(
    source_archive: zipfile.ZipFile,
    source_info: zipfile.ZipInfo,
    destination_archive: zipfile.ZipFile,
    destination_info: zipfile.ZipInfo | None = None,
) -> None:
    info = destination_info or clone_zip_info(source_info)
    with source_archive.open(source_info, "r") as source, destination_archive.open(
        info, "w", force_zip64=True
    ) as destination:
        shutil.copyfileobj(source, destination, length=1024 * 1024)


def resolve_external_archive_tool(archive: Path) -> tuple[str, str]:
    """Return (executable, backend) for non-ZIP archives.

    The public Linux/macOS master supplies a pinned 7zz through
    SAVR_ARCHIVE_TOOL.  Auto-discovery remains useful for low-level developer
    runs and for Windows, whose bundled tar.exe is bsdtar/libarchive.  GNU tar
    is deliberately rejected for 7z/RAR because it cannot read those formats.
    """

    override = os.environ.get("SAVR_ARCHIVE_TOOL")
    candidates: list[str] = []
    if override:
        candidates.append(str(Path(override).expanduser()))
    for name in ("7zz", "7z", "7za", "bsdtar", "tar"):
        candidate = shutil.which(name)
        if candidate and candidate not in candidates:
            candidates.append(candidate)

    for candidate in candidates:
        path = Path(candidate)
        if not path.is_file():
            if override and candidate == candidates[0]:
                raise KitError(f"SAVR_ARCHIVE_TOOL does not exist: {path}")
            continue
        executable_name = path.name.lower()
        if executable_name.startswith(("7zz", "7z", "7za")):
            return str(path), "7z"
        if executable_name.startswith("bsdtar"):
            return str(path), "tar"
        try:
            probe = subprocess.run(
                [str(path), "--version"],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                errors="replace",
                check=False,
            ).stdout
        except OSError:
            continue
        if "bsdtar" in probe.lower() or "libarchive" in probe.lower():
            return str(path), "tar"
        if override and candidate == candidates[0]:
            raise KitError(
                "SAVR_ARCHIVE_TOOL must be 7zz/7z or bsdtar/libarchive; "
                f"unsupported executable: {path}"
            )

    raise KitError(
        f"{archive.suffix} needs 7zz/7z or bsdtar/libarchive. The public "
        "Linux/macOS master installs a verified 7zz automatically; low-level "
        "runs can set SAVR_ARCHIVE_TOOL or select an extracted folder."
    )


def archive_members_external(archive: Path) -> list[str]:
    return [record[0] for record in archive_metadata_external(archive)]


def archive_metadata_external(archive: Path) -> list[tuple[str, int, str]]:
    tool, backend = resolve_external_archive_tool(archive)
    if backend == "7z":
        output = run(
            [tool, "l", "-slt", "-ba", "--", archive],
            failure=f"cannot inspect archive {archive}",
        )
        records: list[tuple[str, int, str]] = []
        current: dict[str, str] = {}

        def finish_record() -> None:
            if not current:
                return
            name = current.get("Path")
            size_text = current.get("Size")
            if name is None or size_text is None or not size_text.isdigit():
                raise KitError(f"cannot parse 7-Zip member metadata in {archive}")
            safe_member(name)
            attributes = current.get("Attributes", "")
            mode = next(
                (
                    token
                    for token in attributes.split()
                    if len(token) == 10 and token[0] in "-dl"
                ),
                "",
            )
            if current.get("Encrypted") == "+":
                kind = "e"
            elif attributes.startswith("D") or mode.startswith("d"):
                kind = "d"
            elif (
                attributes.startswith("L")
                or mode.startswith("l")
                or any("link" in key.casefold() and value for key, value in current.items())
            ):
                kind = "l"
            else:
                kind = "-"
            records.append((name, int(size_text), kind))
            current.clear()

        for line in output.splitlines():
            if not line.strip():
                finish_record()
                continue
            if " = " not in line:
                raise KitError(f"cannot parse 7-Zip listing line: {line!r}")
            key, value = line.split(" = ", 1)
            current[key] = value
        finish_record()
        if not records:
            raise KitError(f"could not read member sizes from {archive}")
        return records

    output = run([tool, "-tvf", archive], failure=f"cannot inspect archive {archive}")
    records: list[tuple[str, int, str]] = []
    for line in output.splitlines():
        fields = line.split(None, 8)
        name = None
        size = None
        kind = None
        # bsdtar: mode links uid gid size month day time/year name
        if (
            len(fields) == 9
            and fields[1].isdigit()
            and fields[4].isdigit()
        ):
            kind, size, name = fields[0][0], int(fields[4]), fields[8]
        else:
            # GNU tar: mode owner/group size date time name
            fields = line.split(None, 5)
            if len(fields) == 6 and fields[2].isdigit():
                kind, size, name = fields[0][0], int(fields[2]), fields[5]
        if name is None or size is None or kind is None:
            continue
        safe_member(name)
        records.append((name, size, kind))
    if not records:
        raise KitError(f"could not read member sizes from {archive}; extract it first")
    return records


def external_member_record(
    member_name: str,
    records: list[tuple[str, int, str]],
) -> tuple[str, int, str]:
    wanted = safe_member(member_name).as_posix().lower()
    matches = [
        record
        for record in records
        if safe_member(record[0]).as_posix().lower() == wanted
    ]
    if len(matches) != 1:
        raise KitError(f"archive must contain exactly one {member_name}; found {len(matches)}")
    return matches[0]


def extract_external_members(archive: Path, destination: Path, members: list[str]) -> None:
    tool, backend = resolve_external_archive_tool(archive)
    for name in members:
        safe_member(name)
    if backend == "7z":
        run(
            [
                tool,
                "x",
                "-y",
                "-aoa",
                "-spd",
                f"-o{destination}",
                "--",
                archive,
                *members,
            ],
            failure=f"cannot extract selected content from {archive}",
        )
    else:
        run(
            [tool, "-xf", archive, "-C", destination, "--", *members],
            failure=f"cannot extract selected content from {archive}",
        )


def copy_limited(source, destination, limit: int, label: str) -> int:
    total = 0
    while True:
        block = source.read(1024 * 1024)
        if not block:
            return total
        total += len(block)
        if total > limit:
            raise KitError(f"{label} exceeds the safe extraction limit of {limit} bytes")
        destination.write(block)


def materialize_apk_inputs(source: Path, work: Path, build_root: Path) -> list[tuple[Path, str]]:
    if not source.exists():
        raise KitError(f"game package path does not exist: {source}")

    candidates: list[tuple[Path, str]] = []
    if source.is_dir():
        candidates = [(path, path.name) for path in sorted(source.rglob("*.apk"))]
    elif source.suffix.lower() == ".apk":
        with zipfile.ZipFile(source) as archive:
            names = {item.filename.lower() for item in archive.infolist()}
        complete_single = LIBGAME_ENTRY.lower() in names and all(
            marker.lower() in names for marker in DATA_SENTINELS
        )
        if complete_single:
            candidates = [(source, source.name)]
        elif source.name.lower() == "base.apk":
            candidates = [(path, path.name) for path in sorted(source.parent.glob("*.apk"))]
        else:
            raise KitError(
                "selected APK is not self-contained. Select base.apk to scan its sibling "
                "Play splits, or select the complete export folder/archive."
            )
    elif source.suffix.lower() in ARCHIVE_SUFFIXES:
        clean_directory(work, build_root)
        if zipfile.is_zipfile(source):
            with zipfile.ZipFile(source) as archive:
                apk_infos = [
                    item
                    for item in archive.infolist()
                    if not item.is_dir() and item.filename.lower().endswith(".apk")
                ]
                if len(apk_infos) > MAX_APK_ARCHIVE_FILES:
                    raise KitError(f"APK archive contains too many APK files: {len(apk_infos)}")
                total_unpacked = sum(info.file_size for info in apk_infos)
                if any(info.file_size > MAX_SINGLE_APK_BYTES for info in apk_infos) or total_unpacked > MAX_APK_ARCHIVE_BYTES:
                    raise KitError("APK archive exceeds the safe extraction size limit")
                for index, info in enumerate(apk_infos):
                    member = safe_member(info.filename)
                    output = work / f"{index:03d}-{member.name}"
                    with archive.open(info) as input_stream, output.open("wb") as output_stream:
                        extracted = copy_limited(
                            input_stream,
                            output_stream,
                            MAX_SINGLE_APK_BYTES,
                            info.filename,
                        )
                    if extracted != info.file_size:
                        raise KitError(f"APK archive size changed while extracting {info.filename}")
                    candidates.append((output, member.name))
        else:
            members = [name for name in archive_members_external(source) if name.lower().endswith(".apk")]
            if len(members) > MAX_APK_ARCHIVE_FILES:
                raise KitError(f"APK archive contains too many APK files: {len(members)}")
            metadata = archive_metadata_external(source)
            sizes: list[int] = []
            for name in members:
                _, size, kind = external_member_record(name, metadata)
                if kind != "-":
                    raise KitError(f"APK archive member is not a regular file: {name}")
                sizes.append(size)
            if any(size > MAX_SINGLE_APK_BYTES for size in sizes) or sum(sizes) > MAX_APK_ARCHIVE_BYTES:
                raise KitError("APK archive exceeds the safe extraction size limit")
            extract_external_members(source, work, members)
            for name in members:
                member = safe_member(name)
                path = work.joinpath(*member.parts)
                candidates.append((path, member.name))
    else:
        raise KitError(
            "game package must be an APK, split-APK folder, or ZIP/APKS/XAPK/APKM/7z archive"
        )

    candidates = [(path.resolve(), name) for path, name in candidates if path.is_file()]
    if not candidates:
        raise KitError(f"no APK files found in {source}")
    return candidates


def parse_badging(path: Path, aapt2: Path) -> tuple[str, str, str, str | None]:
    output = run([aapt2, "dump", "badging", path], failure=f"aapt2 rejected {path.name}")
    package_line = next((line for line in output.splitlines() if line.startswith("package:")), "")
    if not package_line:
        raise KitError(f"cannot read package metadata from {path}")

    def value(name: str) -> str | None:
        match = re.search(rf"\b{re.escape(name)}='([^']*)'", package_line)
        return match.group(1) if match else None

    package = value("name")
    version_code = value("versionCode")
    version_name = value("versionName")
    split = value("split")
    if package is None or version_code is None or version_name is None:
        raise KitError(f"incomplete package metadata in {path}")
    return package, version_code, version_name, split


def zip_name_set(path: Path) -> set[str]:
    with zipfile.ZipFile(path) as archive:
        return {item.filename.lower() for item in archive.infolist() if not item.is_dir()}


def apk_signer_digest(path: Path, tools: Toolchain) -> str:
    output = run(
        [tools.java, "-jar", tools.apksigner, "verify", "--print-certs", path],
        failure=f"APK signature verification failed for {path.name}",
    )
    match = re.search(
        r"Signer #1 certificate SHA-256 digest:\s*([0-9a-fA-F]{64})", output
    )
    if not match:
        raise KitError(f"cannot read signer certificate from {path.name}")
    return match.group(1).upper()


def hash_zip_entry(path: Path, entry_name: str) -> str:
    digest = hashlib.sha256()
    with zipfile.ZipFile(path) as archive:
        try:
            source = archive.open(entry_name)
        except KeyError as error:
            raise KitError(f"{path.name} is missing {entry_name}") from error
        with source:
            for block in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(block)
    return digest.hexdigest().upper()


def classify_game_package(
    source: Path,
    work: Path,
    build_root: Path,
    tools: Toolchain,
    allow_unofficial: bool,
) -> GamePackage:
    materialized = materialize_apk_inputs(source, work, build_root)
    apks: list[InputApk] = []
    for path, original_name in materialized:
        package, version_code, version_name, split = parse_badging(path, tools.aapt2)
        if package != PACKAGE_NAME:
            say(f"    ignoring unrelated APK: {original_name} ({package})")
            continue
        if version_code != VERSION_CODE or (version_name and version_name != VERSION_NAME):
            raise KitError(
                f"unsupported GTA SA build in {original_name}: {version_name} ({version_code}); "
                f"required {VERSION_NAME} ({VERSION_CODE})"
            )
        apks.append(
            InputApk(path, original_name, package, version_code, version_name, split)
        )
    if not apks:
        raise KitError(f"no {PACKAGE_NAME} APKs found")

    split_ids: set[str] = set()
    for apk in apks:
        key = apk.split or "<base>"
        if key in split_ids:
            raise KitError(f"duplicate APK split '{key}' in the selected package")
        split_ids.add(key)

    contents = {apk: zip_name_set(apk.path) for apk in apks}
    base_candidates = [
        apk
        for apk in apks
        if apk.split is None and "androidmanifest.xml" in contents[apk] and "classes.dex" in contents[apk]
    ]
    arm_candidates = [apk for apk in apks if LIBGAME_ENTRY.lower() in contents[apk]]
    data_candidates = [
        apk for apk in apks if all(marker.lower() in contents[apk] for marker in DATA_SENTINELS)
    ]
    if len(base_candidates) != 1:
        raise KitError(f"expected one base APK, found {len(base_candidates)}")
    if len(arm_candidates) != 1:
        available_game_abis = sorted(
            {
                name.split("/")[1]
                for names in contents.values()
                for name in names
                if re.fullmatch(r"lib/[^/]+/libgame\.so", name)
            }
        )
        available_text = ", ".join(available_game_abis) if available_game_abis else "none"
        raise KitError(
            "Quest requires exactly one APK containing lib/arm64-v8a/libGame.so; "
            f"found {len(arm_candidates)} (available game ABIs: {available_text}). "
            "The complete Google Play export must include split_config.arm64_v8a.apk. "
            "Export GTA SA 2.11.311 from a real 64-bit ARM Android phone/tablet, "
            "not an emulator or Windows Android subsystem."
        )
    if len(data_candidates) != 1:
        raise KitError(f"expected one data_main APK with GTA assets, found {len(data_candidates)}")

    base = base_candidates[0]
    arm64 = arm_candidates[0]
    data = data_candidates[0]
    monolithic = base.path == arm64.path == data.path

    signers = {apk_signer_digest(apk.path, tools) for apk in apks}
    if len(signers) != 1:
        raise KitError("the selected APKs are not signed by one certificate")
    signer = next(iter(signers))
    if signer != OFFICIAL_SIGNER_SHA256:
        message = (
            "selected APK certificate does not match the verified Google Play "
            f"GTA SA {VERSION_NAME} retail certificate. "
            f"Found: {signer}; required: {OFFICIAL_SIGNER_SHA256}"
        )
        if not allow_unofficial:
            raise KitError(
                message
                + ". Do not use an APK downloaded from a third-party site or a merged/re-signed APK. "
                "Export every split from your own legally installed Google Play copy and select "
                "the complete export directory. See README.md, 'Exporting your own Google Play APK set'."
            )
        say("WARNING: " + message)

    engine_hash = hash_zip_entry(arm64.path, LIBGAME_ENTRY)
    if engine_hash != LIBGAME_SHA256:
        raise KitError(
            f"unsupported libGame.so: {engine_hash}; required retail 2.11.311 {LIBGAME_SHA256}"
        )

    dex_names = sorted(
        name for name in contents[base] if re.fullmatch(r"classes(?:\d+)?\.dex", name)
    )
    expected_dex = ["classes.dex", "classes2.dex", "classes3.dex"]
    arm_libs = {name for name in contents[arm64] if name.startswith("lib/arm64-v8a/")}
    official_libs_lower = {name.lower() for name in OFFICIAL_ARM64_LIBS}
    if not allow_unofficial:
        if dex_names != expected_dex:
            raise KitError(f"base APK is not clean retail: unexpected DEX set {dex_names}")
        if arm_libs != official_libs_lower:
            extra = sorted(arm_libs - official_libs_lower)
            missing = sorted(official_libs_lower - arm_libs)
            raise KitError(f"arm64 APK is not clean retail; extra={extra}, missing={missing}")

    say(
        f"    GTA SA {VERSION_NAME}: {len(apks)} APK(s), "
        f"{'single APK' if monolithic else 'Play split set'}, retail engine verified"
    )
    return GamePackage(tuple(apks), base, arm64, data, monolithic, signer)


def load_audio_reference() -> dict:
    reference_path = ROOT / "tools" / "audio-reference.json"
    if not reference_path.is_file():
        raise KitError(f"missing audio reference manifest: {reference_path}")
    reference = json.loads(reference_path.read_text(encoding="utf-8"))
    files = reference.get("files")
    if not isinstance(files, list) or len(files) != 59:
        raise KitError("audio reference manifest must contain exactly 59 files")
    lowered = [str(entry["path"]).lower() for entry in files]
    if len(set(lowered)) != len(lowered):
        raise KitError("audio reference manifest contains duplicate paths")
    if sum(int(entry["size"]) for entry in files) != int(reference.get("totalBytes", -1)):
        raise KitError("audio reference manifest byte total is inconsistent")
    return reference


def select_audio_members(
    members: Iterable[str],
    prefix: str,
    reference: dict,
) -> list[tuple[str, dict]]:
    by_lower: dict[str, list[str]] = {}
    for raw_name in members:
        if raw_name.endswith(("/", "\\")):
            continue
        normalized = safe_member(raw_name).as_posix()
        by_lower.setdefault(normalized.lower(), []).append(raw_name)

    selected: list[tuple[str, dict]] = []
    prefix_clean = prefix.rstrip("/")
    for spec in reference["files"]:
        desired = f"{prefix_clean}/{spec['path']}" if prefix_clean else str(spec["path"])
        matches = by_lower.get(desired.lower(), [])
        if len(matches) != 1:
            raise KitError(
                f"audio archive must contain exactly one {desired}; found {len(matches)}"
            )
        selected.append((matches[0], spec))
    return selected


def find_audio_prefix(members: Iterable[str]) -> str:
    original_by_lower: dict[str, str] = {}
    for raw_name in members:
        member = safe_member(raw_name)
        if raw_name.endswith(("/", "\\")):
            continue
        original_by_lower[member.as_posix().lower()] = member.as_posix()

    prefixes: list[str] = []
    first = AUDIO_SENTINELS[0].lower()
    for lower_name, original in original_by_lower.items():
        if not lower_name.endswith("/" + first) and lower_name != first:
            continue
        prefix = original[: -len(AUDIO_SENTINELS[0])].rstrip("/")
        prefix_lower = prefix.lower()
        if all(
            f"{prefix_lower}/{sentinel.lower()}" in original_by_lower
            if prefix_lower
            else sentinel.lower() in original_by_lower
            for sentinel in AUDIO_SENTINELS
        ):
            prefixes.append(prefix)
    unique = sorted(set(prefixes), key=str.lower)
    if len(unique) != 1:
        raise KitError(f"expected one complete audio tree, found {len(unique)}")
    return unique[0]


def find_audio_directory(source: Path) -> Path:
    candidates: list[Path] = []
    directories = [source] + [path for path in source.rglob("*") if path.is_dir()]
    for directory in directories:
        children = {child.name.lower(): child for child in directory.iterdir()} if directory.exists() else {}
        if not {"config", "sfx", "streams"}.issubset(children):
            continue
        required_names = (("config", "banklkup.dat"), ("sfx", "genrl.osw"), ("streams", "cutscene.osw"))
        complete = True
        for folder_key, file_key in required_names:
            files = {child.name.lower(): child for child in children[folder_key].iterdir()}
            if file_key not in files or not files[file_key].is_file():
                complete = False
                break
        if complete:
            candidates.append(directory)
    unique = sorted({path.resolve() for path in candidates})
    if len(unique) != 1:
        raise KitError(f"expected one complete audio folder, found {len(unique)} under {source}")
    return unique[0]


def materialize_audio_source(source: Path, work: Path, build_root: Path) -> Path:
    if not source.exists():
        raise KitError(f"audio source does not exist: {source}")
    if source.is_dir():
        return find_audio_directory(source)
    if source.suffix.lower() not in ARCHIVE_SUFFIXES:
        raise KitError("audio source must be a folder or ZIP/7z/RAR archive")

    clean_directory(work, build_root)
    reference = load_audio_reference()
    destination_root = work / "audio"
    if zipfile.is_zipfile(source):
        with zipfile.ZipFile(source) as archive:
            infos = [item for item in archive.infolist() if not item.is_dir()]
            names = [item.filename for item in infos]
            prefix = find_audio_prefix(names)
            selected = select_audio_members(names, prefix, reference)
            info_by_name = {info.filename: info for info in infos}
            for member_name, spec in selected:
                info = info_by_name[member_name]
                expected_size = int(spec["size"])
                if info.file_size != expected_size:
                    raise KitError(
                        f"audio archive size mismatch before extraction: {spec['path']} "
                        f"({info.file_size} != {expected_size})"
                    )
                destination = destination_root.joinpath(*PurePosixPath(spec["path"]).parts)
                destination.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(info) as input_stream, destination.open("wb") as output_stream:
                    extracted = copy_limited(
                        input_stream,
                        output_stream,
                        expected_size,
                        str(spec["path"]),
                    )
                if extracted != expected_size:
                    raise KitError(f"audio archive truncated while extracting {spec['path']}")
    else:
        names = archive_members_external(source)
        prefix = find_audio_prefix(names)
        selected = select_audio_members(names, prefix, reference)
        metadata = archive_metadata_external(source)
        for member_name, spec in selected:
            expected_size = int(spec["size"])
            _, observed_size, kind = external_member_record(member_name, metadata)
            if kind != "-":
                raise KitError(f"audio archive member is not a regular file: {member_name}")
            if observed_size != expected_size:
                raise KitError(
                    f"audio archive size mismatch before extraction: {spec['path']} "
                    f"({observed_size} != {expected_size})"
                )
        extract_external_members(source, work, [member_name for member_name, _ in selected])
    return find_audio_directory(work)


def validate_audio(source: Path, work: Path, build_root: Path) -> AudioTree:
    try:
        audio_root = materialize_audio_source(source, work, build_root)
    except KitError as error:
        raise KitError(
            f"{error}\nRequired sound archive: "
            f"{SUPPORTED_AUDIO_ARCHIVE}. "
            f"Download it from {SUPPORTED_AUDIO_PAGE} and choose Original plan mod pack "
            "(16 August 2026, 1.41 GB), not CLASSIC ADVANCED v1.0."
        ) from error
    reference = load_audio_reference()
    expected = {entry["path"].lower(): entry for entry in reference["files"]}

    actual: dict[str, Path] = {}
    root_resolved = audio_root.resolve()
    for path in audio_root.rglob("*"):
        if not path.is_file():
            continue
        resolved = path.resolve()
        try:
            resolved.relative_to(root_resolved)
        except ValueError as error:
            raise KitError(f"audio file escapes the selected tree: {path}") from error
        relative = path.relative_to(audio_root).as_posix()
        actual[relative.lower()] = path

    if set(actual) != set(expected):
        missing = sorted(set(expected) - set(actual))
        extra = sorted(set(actual) - set(expected))
        raise KitError(f"audio mod does not match the supported 59-file subset; missing={missing}, extra={extra}")

    verified: list[dict] = []
    for key in sorted(expected):
        spec = expected[key]
        path = actual[key]
        size = path.stat().st_size
        if size != spec["size"]:
            raise KitError(
                f"audio size mismatch: {spec['path']} ({size} != {spec['size']}). "
                "This is not the supported Original plan mod pack. Use "
                f"{SUPPORTED_AUDIO_ARCHIVE}, not CLASSIC ADVANCED v1.0."
            )
        digest = sha256_file(path)
        if digest != spec["sha256"].upper():
            raise KitError(
                f"audio hash mismatch: {spec['path']} ({digest}). "
                "This is not the supported Original plan mod pack. Use "
                f"{SUPPORTED_AUDIO_ARCHIVE}, not CLASSIC ADVANCED v1.0."
            )
        verified.append(
            {
                "path": spec["path"],
                "sourcePath": path.relative_to(audio_root).as_posix(),
                "size": size,
                "sha256": digest,
            }
        )

    total = sum(entry["size"] for entry in verified)
    say(f"    audio mod verified: {len(verified)} files, {total} bytes")
    return AudioTree(audio_root, tuple(verified))


def patch_manifest(text: str) -> str:
    app_match = re.search(r"<application\b([^>]*)>", text)
    if not app_match:
        raise KitError("application tag not found in decoded manifest")
    if re.search(r"\bandroid:name\s*=", app_match.group(1)):
        raise KitError("manifest already declares an Application class; refusing to overwrite it")
    text = text[: app_match.start()] + app_match.group(0).replace(
        "<application", f'<application android:name="{APPLICATION_CLASS}"', 1
    ) + text[app_match.end() :]

    pattern = rf'<activity[^>]*android:name="{re.escape(LAUNCHER_ACTIVITY)}".*?</activity>'
    match = re.search(pattern, text, re.DOTALL)
    if match is None:
        raise KitError(f"launcher activity {LAUNCHER_ACTIVITY} not found")
    block = match.group(0)
    stripped = re.sub(
        r"\s*<intent-filter>(?:(?!</intent-filter>).)*?LAUNCHER.*?</intent-filter>",
        "",
        block,
        flags=re.DOTALL,
    )
    if stripped == block:
        raise KitError(f"no launcher intent-filter on {LAUNCHER_ACTIVITY}")
    text = text.replace(block, stripped, 1)

    body = (
        '\n            <meta-data android:name="com.oculus.vr.focusaware" android:value="true"/>'
        "\n            <intent-filter>"
        '\n                <action android:name="android.intent.action.MAIN"/>'
        '\n                <category android:name="android.intent.category.LAUNCHER"/>'
        '\n                <category android:name="org.khronos.openxr.intent.category.IMMERSIVE_HMD"/>'
        '\n                <category android:name="com.oculus.intent.category.VR"/>'
        "\n            </intent-filter>"
    )
    text, count = re.subn(
        rf'(<activity[^>]*android:name="{re.escape(GAME_ACTIVITY)}"[^>]*?)/>',
        lambda found: f'{found.group(1)} android:exported="true">{body}\n        </activity>',
        text,
        count=1,
    )
    if count != 1:
        raise KitError(f"game activity {GAME_ACTIVITY} not found in the expected layout")

    text = text.replace(
        "    <application",
        '    <uses-feature android:name="android.hardware.vr.headtracking" '
        'android:required="true" android:version="1"/>\n    <application',
        1,
    )
    return text


def create_manifest_stub(base: Path, destination: Path) -> None:
    with zipfile.ZipFile(base) as source, zipfile.ZipFile(destination, "w", allowZip64=True) as output:
        for info in source.infolist():
            lower = info.filename.lower()
            if lower in {"androidmanifest.xml", "resources.arsc"} or lower.startswith("res/"):
                copy_zip_stream(source, info, output)


def build_manifest(base: Path, build: Path, tools: Toolchain) -> bytes:
    decoded = build / "manifest-decoded"
    framework = build / "apktool-framework"
    stub = build / "manifest-source.apk"
    rebuilt = build / "manifest-rebuilt.apk"
    if decoded.exists():
        shutil.rmtree(decoded)
    clean_directory(framework, build)
    stub.unlink(missing_ok=True)
    rebuilt.unlink(missing_ok=True)
    create_manifest_stub(base, stub)

    say("==> decoding and patching AndroidManifest.xml")
    # Never use Apktool's user-global/default framework cache. An older cached
    # framework cannot compile attributes used by the current retail manifest.
    run(
        [tools.java, "-jar", tools.apktool, "if", "-p", framework, tools.android_jar],
        failure="apktool Android framework setup failed",
    )
    run(
        [
            tools.java,
            "-jar",
            tools.apktool,
            "d",
            "-s",
            "--no-assets",
            "-f",
            "-p",
            framework,
            "-o",
            decoded,
            stub,
        ],
        failure="apktool manifest decode failed",
    )
    manifest = decoded / "AndroidManifest.xml"
    manifest.write_text(patch_manifest(manifest.read_text(encoding="utf-8")), encoding="utf-8")
    run(
        [tools.java, "-jar", tools.apktool, "b", "-p", framework, decoded, "-o", rebuilt],
        failure="apktool manifest rebuild failed",
    )
    with zipfile.ZipFile(rebuilt) as archive:
        return archive.read("AndroidManifest.xml")


def is_signature_entry(name: str) -> bool:
    lower = name.replace("\\", "/").strip("/").lower()
    if lower in STRIP_NAMES:
        return True
    if not lower.startswith("meta-inf/"):
        return False
    relative = lower[len("meta-inf/") :]
    if "/" in relative:
        return False
    return relative == "manifest.mf" or relative.endswith(SIGNATURE_SUFFIXES)


def rewrite_apk(
    source: Path,
    destination: Path,
    replacements: dict[str, bytes],
    additions: dict[str, tuple[bytes, int]],
) -> None:
    pending = dict(replacements)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(source) as input_apk, zipfile.ZipFile(
        destination, "w", allowZip64=True
    ) as output_apk:
        existing = {info.filename for info in input_apk.infolist()}
        collision = sorted(set(additions) & existing)
        if collision:
            raise KitError(f"refusing to overwrite existing APK entries: {collision}")
        for info in input_apk.infolist():
            if is_signature_entry(info.filename):
                continue
            if info.filename in pending:
                replacement_info = clone_zip_info(info)
                output_apk.writestr(replacement_info, pending.pop(info.filename))
            else:
                copy_zip_stream(input_apk, info, output_apk)
        for name, (data, compression) in additions.items():
            info = zipfile.ZipInfo(name)
            info.compress_type = compression
            info.external_attr = 0o100644 << 16
            output_apk.writestr(info, data)
    if pending:
        raise KitError(f"{source.name}: entries to replace were not found: {sorted(pending)}")


def next_dex_name(base: Path) -> str:
    with zipfile.ZipFile(base) as archive:
        names = {info.filename for info in archive.infolist()}
    index = 1
    while True:
        name = "classes.dex" if index == 1 else f"classes{index}.dex"
        if name not in names:
            return name
        index += 1


def output_name(apk: InputApk) -> str:
    if apk.split is None:
        return "base.apk"
    safe = re.sub(r"[^A-Za-z0-9_.-]+", "_", apk.split)
    return f"split_{safe}.apk"


def ensure_keystore(path: Path, tools: Toolchain) -> None:
    if path.is_file():
        return
    say("==> generating persistent personal signing key")
    path.parent.mkdir(parents=True, exist_ok=True)
    run(
        [
            tools.keytool,
            "-genkeypair",
            "-keystore",
            path,
            "-alias",
            "savr",
            "-keyalg",
            "RSA",
            "-keysize",
            "3072",
            "-validity",
            "10000",
            "-storepass",
            "savrsavr",
            "-keypass",
            "savrsavr",
            "-dname",
            "CN=GTA SA VR Personal Builder",
        ],
        failure="personal signing-key generation failed",
    )


def align_sign_verify(apk: Path, keystore: Path, tools: Toolchain) -> str:
    aligned = apk.with_suffix(".aligned.apk")
    run(
        [tools.zipalign, "-P", "16", "-f", "4", apk, aligned],
        failure=f"zipalign failed for {apk.name}",
    )
    aligned.replace(apk)
    run(
        [
            tools.java,
            "-jar",
            tools.apksigner,
            "sign",
            "--ks",
            keystore,
            "--ks-key-alias",
            "savr",
            "--ks-pass",
            "pass:savrsavr",
            "--key-pass",
            "pass:savrsavr",
            "--v1-signing-enabled",
            "true",
            "--v2-signing-enabled",
            "true",
            "--v3-signing-enabled",
            "true",
            "--v4-signing-enabled",
            "false",
            apk,
        ],
        failure=f"APK signing failed for {apk.name}",
    )
    run(
        [tools.zipalign, "-c", "-P", "16", "4", apk],
        failure=f"zipalign verification failed for {apk.name}",
    )
    return apk_signer_digest(apk, tools)


def apk_info_map(path: Path) -> dict[str, zipfile.ZipInfo]:
    with zipfile.ZipFile(path) as archive:
        return {info.filename: info for info in archive.infolist() if not info.is_dir()}


def audit_rewrite(
    source: Path,
    output: Path,
    replaced: set[str],
    added: set[str],
) -> None:
    before = apk_info_map(source)
    after = apk_info_map(output)
    for name, old in before.items():
        if is_signature_entry(name) or name in replaced:
            continue
        new = after.get(name)
        if new is None:
            raise KitError(f"APK audit: {output.name} lost {name}")
        if (old.CRC, old.file_size, old.compress_type) != (new.CRC, new.file_size, new.compress_type):
            raise KitError(f"APK audit: unexpected content change in {output.name}:{name}")
    for name in after:
        if is_signature_entry(name) or name in before or name in added:
            continue
        raise KitError(f"APK audit: unexpected added entry in {output.name}: {name}")
    for name in replaced | added:
        if name not in after:
            raise KitError(f"APK audit: expected patched entry missing in {output.name}: {name}")


def assemble_apks(
    package: GamePackage,
    build: Path,
    output: Path,
    keystore: Path,
    tools: Toolchain,
    native_lib: Path,
    loader_dex: Path,
    openxr_loader: Path,
) -> tuple[list[dict], str]:
    for required in (native_lib, loader_dex, openxr_loader, tools.apktool):
        if not required.is_file():
            raise KitError(f"required build input is missing: {required}")
    if sha256_file(tools.apktool) != APKTOOL_SHA256:
        raise KitError("Apktool hash mismatch; required official Apktool 3.0.3 JAR")
    if sha256_file(openxr_loader) != OPENXR_LOADER_SHA256:
        raise KitError("OpenXR loader hash mismatch; required Khronos 1.1.43 arm64 binary")

    manifest = build_manifest(package.base.path, build, tools)
    dex_name = next_dex_name(package.base.path)
    native_bytes = native_lib.read_bytes()
    loader_bytes = openxr_loader.read_bytes()
    dex_bytes = loader_dex.read_bytes()
    default_settings: dict[str, tuple[bytes, int]] = {}
    for name in DEFAULT_SETTING_NAMES:
        source = DEFAULT_SETTINGS_DIR / name
        if not source.is_file():
            raise KitError(f"required shipping default setting is missing: {source}")
        default_settings[f"assets/savr_defaults/{name}"] = (
            source.read_bytes(),
            zipfile.ZIP_DEFLATED,
        )

    clean_directory(output, build)
    patches: dict[Path, tuple[set[str], set[str]]] = {}
    output_records: list[dict] = []

    say("==> assembling personal APK set")
    for apk in package.apks:
        replacements: dict[str, bytes] = {}
        additions: dict[str, tuple[bytes, int]] = {}
        if apk.path == package.base.path:
            replacements["AndroidManifest.xml"] = manifest
            additions[dex_name] = (dex_bytes, zipfile.ZIP_STORED)
            additions.update(default_settings)
        if apk.path == package.arm64.path:
            additions["lib/arm64-v8a/libsavr.so"] = (native_bytes, zipfile.ZIP_DEFLATED)
            additions["lib/arm64-v8a/libopenxr_loader.so"] = (
                loader_bytes,
                zipfile.ZIP_DEFLATED,
            )
        destination = output / output_name(apk)
        rewrite_apk(apk.path, destination, replacements, additions)
        patches[destination] = (set(replacements), set(additions))

    ensure_keystore(keystore, tools)
    say("==> aligning, signing and verifying")
    output_signers: set[str] = set()
    for apk in package.apks:
        destination = output / output_name(apk)
        signer = align_sign_verify(destination, keystore, tools)
        output_signers.add(signer)
        replaced, added = patches[destination]
        audit_rewrite(apk.path, destination, replaced, added)
        output_records.append(
            {
                "file": destination.name,
                "split": apk.split,
                "size": destination.stat().st_size,
                "sha256": sha256_file(destination),
            }
        )
        say(f"    {destination.name}")
    if len(output_signers) != 1:
        raise KitError("output APKs do not share one signing certificate")

    arm_output = output / output_name(package.arm64)
    if hash_zip_entry(arm_output, LIBGAME_ENTRY) != LIBGAME_SHA256:
        raise KitError("post-build audit: libGame.so changed")
    if hash_zip_entry(arm_output, "lib/arm64-v8a/libsavr.so") != sha256_file(native_lib):
        raise KitError("post-build audit: embedded libsavr.so differs from the build")
    if hash_zip_entry(arm_output, "lib/arm64-v8a/libopenxr_loader.so") != OPENXR_LOADER_SHA256:
        raise KitError("post-build audit: embedded OpenXR loader differs from the verified input")
    return output_records, next(iter(output_signers))


def write_tree_metadata(root: Path, entries: list[dict], title: str) -> dict:
    ordered = sorted(entries, key=lambda item: item["path"].lower())
    sums = "".join(f"{item['sha256'].lower()}  {item['path']}\n" for item in ordered)
    (root / "SHA256SUMS").write_text(sums, encoding="utf-8", newline="\n")
    manifest = {
        "name": title,
        "fileCount": len(ordered),
        "totalBytes": sum(item["size"] for item in ordered),
        "files": ordered,
    }
    (root / "manifest.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    return manifest


def extract_hashed(source, destination: Path) -> tuple[int, str]:
    digest = hashlib.sha256()
    size = 0
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("wb") as output:
        while True:
            block = source.read(1024 * 1024)
            if not block:
                break
            output.write(block)
            digest.update(block)
            size += len(block)
    return size, digest.hexdigest().upper()


def stage_payload(
    package: GamePackage,
    audio: AudioTree,
    payload: Path,
    build: Path,
) -> dict:
    staging = payload.with_name(payload.name + ".staging")
    clean_directory(staging, build)

    say("==> extracting user-owned data_main assets")
    data_root = staging / "data_main"
    data_entries: list[dict] = []
    with zipfile.ZipFile(package.data.path) as archive:
        for info in archive.infolist():
            if info.is_dir() or not info.filename.lower().startswith("assets/"):
                continue
            lower = info.filename.lower()
            if package.monolithic and (
                lower.startswith("assets/audio/sfx/")
                or lower.startswith("assets/audio/streams/")
            ):
                continue
            member = safe_member(info.filename)
            relative = member.as_posix()
            destination = data_root.joinpath(*member.parts)
            with archive.open(info) as source:
                size, digest = extract_hashed(source, destination)
            data_entries.append({"path": relative, "size": size, "sha256": digest})
    data_manifest = write_tree_metadata(data_root, data_entries, "GTA SA data_main assets")
    found_data = {entry["path"].lower() for entry in data_entries}
    missing_data = [marker for marker in DATA_SENTINELS if marker.lower() not in found_data]
    if missing_data:
        raise KitError(f"staged data_main is incomplete: {missing_data}")

    say("==> staging verified audio subset")
    audio_root = staging / "audio"
    audio_entries: list[dict] = []
    for entry in audio.entries:
        source = audio.root.joinpath(*PurePosixPath(entry["sourcePath"]).parts)
        destination = audio_root.joinpath(*PurePosixPath(entry["path"]).parts)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        audio_entries.append({key: value for key, value in entry.items() if key != "sourcePath"})
    audio_manifest = write_tree_metadata(audio_root, audio_entries, "GTA SA PS2-style audio subset")

    say("==> staging MIT-licensed VR hand assets")
    hands_source = ROOT / "assets" / "vrhands"
    hands_root = staging / "vrhands"
    hand_entries: list[dict] = []
    for name in HAND_FILES:
        source = hands_source / name
        if not source.is_file():
            raise KitError(f"required VR hand asset is missing: {source}")
        hands_root.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, hands_root / name)
        hand_entries.append(
            {"path": name, "size": source.stat().st_size, "sha256": sha256_file(source)}
        )
    hands_manifest = write_tree_metadata(hands_root, hand_entries, "SAVR VR hands")

    if payload.exists():
        ensure_inside(payload, build)
        shutil.rmtree(payload)
    staging.replace(payload)
    result = {
        "data_main": data_manifest,
        "audio": audio_manifest,
        "vrhands": hands_manifest,
        "totalBytes": (
            data_manifest["totalBytes"]
            + audio_manifest["totalBytes"]
            + hands_manifest["totalBytes"]
        ),
    }
    (payload / "payload-manifest.json").write_text(
        json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    return result


def resolve_tools(args) -> Toolchain:
    sdk_text = args.sdk or os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME")
    java_text = args.java_home or os.environ.get("JAVA_HOME")
    if not sdk_text:
        raise KitError("Android SDK path is required (--sdk or ANDROID_SDK_ROOT)")
    if not java_text:
        raise KitError("JDK path is required (--java-home or JAVA_HOME)")
    sdk = Path(sdk_text).expanduser().resolve()
    java_home = Path(java_text).expanduser().resolve()
    build_tools = sdk / "build-tools" / "35.0.0"
    exe = ".exe" if os.name == "nt" else ""
    tools = Toolchain(
        sdk=sdk,
        java_home=java_home,
        build_tools=build_tools,
        android_jar=sdk / "platforms" / "android-35" / "android.jar",
        aapt2=build_tools / f"aapt2{exe}",
        zipalign=build_tools / f"zipalign{exe}",
        apksigner=build_tools / "lib" / "apksigner.jar",
        keytool=java_home / "bin" / f"keytool{exe}",
        java=java_home / "bin" / f"java{exe}",
        apktool=Path(args.apktool).expanduser().resolve() if args.apktool else ROOT / "tools" / "vendor" / "apktool_3.0.3.jar",
    )
    for required in (
        tools.android_jar,
        tools.aapt2,
        tools.zipalign,
        tools.apksigner,
        tools.keytool,
        tools.java,
    ):
        if not required.exists():
            raise KitError(f"required tool not found: {required}")
    return tools


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-package", required=True, help="original APK, split folder, or archive")
    parser.add_argument("--audio-source", required=True, help="supported sound-mod folder or archive")
    parser.add_argument("--build-dir", default=str(DEFAULT_BUILD))
    parser.add_argument("--output-dir")
    parser.add_argument("--payload-dir")
    parser.add_argument("--sdk")
    parser.add_argument("--java-home")
    parser.add_argument("--apktool")
    parser.add_argument("--native-lib")
    parser.add_argument("--loader-dex")
    parser.add_argument("--openxr-loader")
    parser.add_argument("--keystore")
    parser.add_argument("--validate-only", action="store_true")
    parser.add_argument(
        "--allow-unofficial-source",
        action="store_true",
        help="developer escape hatch; the public wizard does not enable this by default",
    )
    args = parser.parse_args()

    build = Path(args.build_dir).expanduser().resolve()
    source_root = ROOT.resolve()
    if build == source_root or source_root.is_relative_to(build):
        raise KitError(f"build directory must not be the source root or its parent: {build}")

    game_source = Path(args.game_package).expanduser().resolve()
    audio_source = Path(args.audio_source).expanduser().resolve()
    for label, source in (("game package", game_source), ("audio source", audio_source)):
        if not source.exists():
            raise KitError(f"{label} path does not exist: {source}")
        if trees_overlap(source, build):
            raise KitError(f"{label} must be outside the disposable build directory: {source}")

    output = Path(args.output_dir).expanduser().resolve() if args.output_dir else build / "out"
    payload = Path(args.payload_dir).expanduser().resolve() if args.payload_dir else build / "payload"
    ensure_inside(output, build)
    ensure_inside(payload, build)
    payload_staging = payload.with_name(payload.name + ".staging")
    ensure_inside(payload_staging, build)
    if trees_overlap(output, payload) or trees_overlap(output, payload_staging):
        raise KitError("output, payload and payload staging directories must not overlap")

    reserved = (
        build / "input-apks",
        build / "input-audio",
        build / "manifest-decoded",
        build / "apktool-framework",
        build / "manifest-source.apk",
        build / "manifest-rebuilt.apk",
        build / "native",
        build / "loader-classes",
        build / "signing",
    )
    disposable = (("output", output), ("payload", payload), ("payload staging", payload_staging))
    for candidate_name, candidate in disposable:
        for reserved_path in reserved:
            if trees_overlap(candidate, reserved_path):
                raise KitError(f"{candidate_name} directory overlaps reserved build path: {reserved_path}")
    tools = resolve_tools(args)

    native_lib = Path(args.native_lib).expanduser().resolve() if args.native_lib else build / "native" / "libsavr.so"
    loader_dex = Path(args.loader_dex).expanduser().resolve() if args.loader_dex else build / "classes.dex"
    openxr_loader = Path(args.openxr_loader).expanduser().resolve() if args.openxr_loader else None
    keystore = Path(args.keystore).expanduser().resolve() if args.keystore else build / "signing" / "savr.keystore"
    if not args.validate_only and openxr_loader is None:
        raise KitError("--openxr-loader is required for assembly")

    destructive_paths = (
        *(candidate for _, candidate in disposable),
        build / "input-apks",
        build / "input-audio",
        build / "manifest-decoded",
        build / "apktool-framework",
        build / "manifest-source.apk",
        build / "manifest-rebuilt.apk",
        build / "build-manifest.json",
    )
    protected_inputs = [
        ("native library", native_lib),
        ("loader DEX", loader_dex),
        ("Apktool", tools.apktool),
        ("Android framework", tools.android_jar),
        ("aapt2", tools.aapt2),
        ("zipalign", tools.zipalign),
        ("apksigner", tools.apksigner),
        ("keytool", tools.keytool),
        ("Java", tools.java),
        ("keystore", keystore),
    ]
    if openxr_loader is not None:
        protected_inputs.append(("OpenXR loader", openxr_loader))
    for label, protected in protected_inputs:
        if any(trees_overlap(protected, candidate) for candidate in destructive_paths):
            raise KitError(f"{label} must not overlap a disposable build path: {protected}")

    build.mkdir(parents=True, exist_ok=True)

    say("==> validating original GTA SA package")
    package = classify_game_package(
        game_source,
        build / "input-apks",
        build,
        tools,
        args.allow_unofficial_source,
    )
    say("==> validating separate audio mod")
    audio = validate_audio(
        audio_source,
        build / "input-audio",
        build,
    )
    if args.validate_only:
        say("validation complete; no APK or device was modified")
        return 0

    outputs, output_signer = assemble_apks(
        package,
        build,
        output,
        keystore,
        tools,
        native_lib,
        loader_dex,
        openxr_loader,
    )
    payload_result = stage_payload(package, audio, payload, build)
    result = {
        "formatVersion": 1,
        "package": PACKAGE_NAME,
        "versionCode": VERSION_CODE,
        "versionName": VERSION_NAME,
        "sourceSignerSha256": package.signer_sha256,
        "officialSource": package.signer_sha256 == OFFICIAL_SIGNER_SHA256,
        "libGameSha256": LIBGAME_SHA256,
        "outputSignerSha256": output_signer,
        "outputs": outputs,
        "payload": payload_result,
    }
    (build / "build-manifest.json").write_text(
        json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    say(f"\nAPK output: {output}")
    say(f"Quest payload: {payload}")
    say(f"Build manifest: {build / 'build-manifest.json'}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KitError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
