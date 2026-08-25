#!/usr/bin/env bash
# Linux/macOS personal builder and optional Quest installer.
# Original GTA APKs and the audio mod are always supplied by the player.
set -euo pipefail
umask 077

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
PACKAGE_NAME="com.rockstargames.gtasa"
GAME_PACKAGE=""
AUDIO_SOURCE=""
WORK_DIR="${HOME}/SAVRBuild"
ANDROID_SDK=""
JAVA_HOME_ARG=""
SERIAL=""
LOG_PATH=""
BUILD_ONLY=0
NON_INTERACTIVE=0
ASSUME_YES=0
PYTHON_EXE=""
BUILD_ROOT=""
RUN_ID=""
KEY_LOCK=""
KEY_TEMP=""
BUILD_KEYSTORE=""
SIGNING_ROOT=""

JDK_VERSION="21.0.11+10"
ANDROID_TOOLS_VERSION="15859902"
PLATFORM_TOOLS_VERSION="37.0.1"
NDK_VERSION="27.2.12479018"
CMAKE_VERSION="3.22.1"
APKTOOL_VERSION="3.0.3"
SEVENZIP_VERSION="26.02"
APKTOOL_URL="https://github.com/iBotPeaches/Apktool/releases/download/v3.0.3/apktool_3.0.3.jar"
APKTOOL_SHA256="DBF930B076C6B9BE08D57C449CACEFC3BDD6B71EBD59B3066FC0E1F5B14F9423"
OPENXR_AAR_URL="https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android/1.1.43/openxr_loader_for_android-1.1.43.aar"
OPENXR_AAR_SHA256="7E1B36141F9A4F1FA4A7E061936344FD9FCD36BCE6C47EAE2AD09812736167B6"
OPENXR_LOADER_SHA256="713E3BB8D955254C670ACC1C4899A65CB8C930E97DD9958BF37EA922D72B7A06"

ADB=""
ADB_BASE=()
INSTALL_ACTIVE=0
REMOTE_STAGING=()
REMOTE_ROLLBACK_TARGETS=()
REMOTE_ROLLBACK_BACKUPS=()
REMOTE_ROLLBACK_HAD_EXISTING=()

usage() {
  cat <<'EOF'
Usage: tools/build-and-install.sh [options]

  --game-package PATH   Original GTA SA APK, split folder, or APK archive
  --audio-source PATH   Supported sound-mod folder or archive
  --work-dir DIR        Managed workspace (default: ~/SAVRBuild)
  --android-sdk DIR     Android SDK root; discovered/bootstrapped when omitted
  --java-home DIR       JDK 21 root; discovered/bootstrapped when omitted
  --serial SERIAL       Quest USB serial when more than one device is connected
  --log-path FILE       Persistent diagnostic log
  --build-only          Build/verify APKs and payload, but do not touch a Quest
  --non-interactive     Never prompt; missing inputs/components are fatal
  --yes                 Approve the printed Quest install plan without prompting
  -h, --help            Show this help

Host prerequisite: Python 3.10+ must be available as python3 or python.
Install is enabled by default after every local check succeeds. The game is
force-stopped before and after deployment and is never launched by this tool.
EOF
}

die() {
  echo "ERROR: $*" >&2
  exit 1
}

absolute_existing() {
  local value="$1"
  local parent base
  parent="$(dirname "$value")"
  base="$(basename "$value")"
  printf '%s/%s\n' "$(cd "$parent" && pwd -P)" "$base"
}

normalize_prompt_path() {
  local value="$1" first last
  if [ "${#value}" -ge 2 ]; then
    first="${value:0:1}"
    last="${value: -1}"
    if { [ "$first" = '"' ] && [ "$last" = '"' ]; } ||
       { [ "$first" = "'" ] && [ "$last" = "'" ]; }; then
      value="${value:1:${#value}-2}"
    fi
  fi
  case "$value" in
    '~') value="$HOME" ;;
    '~/'*) value="$HOME/${value#\~/}" ;;
  esac
  value="${value//\\ / }"
  printf '%s\n' "$value"
}

canonicalize_directory_target() {
  local target="$1" current component resolved index
  local pending=()
  [ -n "$target" ] || die "directory path must not be empty"
  [[ "$target" != *$'\n'* && "$target" != *$'\r'* ]] ||
    die "directory path must stay on one line"
  case "$target" in
    /*) current="$target" ;;
    *) current="$PWD/$target" ;;
  esac
  while [ ! -e "$current" ]; do
    component="$(basename "$current")"
    [ -n "$component" ] && [ "$component" != "/" ] ||
      die "could not resolve directory target: $target"
    pending+=("$component")
    resolved="$(dirname "$current")"
    [ "$resolved" != "$current" ] || die "could not resolve directory target: $target"
    current="$resolved"
  done
  [ -d "$current" ] || die "directory target has a non-directory ancestor: $current"
  resolved="$(cd "$current" && pwd -P)"
  for ((index=${#pending[@]}-1; index>=0; index--)); do
    component="${pending[index]}"
    case "$component" in
      ''|.) ;;
      ..)
        if [ "$resolved" != "/" ]; then
          resolved="${resolved%/*}"
          [ -n "$resolved" ] || resolved="/"
        fi
        ;;
      *)
        if [ "$resolved" = "/" ]; then resolved="/$component"; else resolved="$resolved/$component"; fi
        ;;
    esac
  done
  printf '%s\n' "$resolved"
}

path_is_within() {
  local candidate="$1" parent="$2"
  case "$candidate/" in
    "$parent/"*) return 0 ;;
    *) return 1 ;;
  esac
}

remove_managed_tree() {
  local target="$1"
  local parent resolved_parent
  path_is_within "$target" "$WORK_DIR" && [ "$target" != "$WORK_DIR" ] ||
    die "refusing to remove a directory outside the managed work directory: $target"
  parent="$(dirname "$target")"
  [ -d "$parent" ] || die "managed parent directory is missing: $parent"
  resolved_parent="$(cd "$parent" && pwd -P)"
  path_is_within "$resolved_parent" "$WORK_DIR" ||
    die "managed parent resolves outside the work directory: $parent -> $resolved_parent"
  rm -rf -- "$target"
}

prepare_managed_directory() {
  local target="$1" resolved
  path_is_within "$target" "$WORK_DIR" && [ "$target" != "$WORK_DIR" ] ||
    die "refusing a managed directory outside the work directory: $target"
  [ ! -L "$target" ] || die "managed directory must not be a symlink: $target"
  if [ -e "$target" ] && [ ! -d "$target" ]; then
    die "managed path exists but is not a directory: $target"
  fi
  mkdir -p -- "$target"
  resolved="$(cd "$target" && pwd -P)"
  [ "$resolved" = "$target" ] ||
    die "managed directory resolves through a symlink: $target -> $resolved"
}

sha256_file() {
  local path="$1"
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$path" | awk '{print toupper($1)}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$path" | awk '{print toupper($1)}'
  else
    die "sha256sum or shasum is required"
  fi
}

download_verified() {
  local name="$1" url="$2" destination="$3" expected="$4"
  local partial observed
  [ ! -L "$destination" ] || die "$name cache path must not be a symlink: $destination"
  mkdir -p "$(dirname "$destination")"
  if [ -f "$destination" ]; then
    observed="$(sha256_file "$destination")"
    [ "$observed" = "$expected" ] ||
      die "$name cache hash mismatch. Expected $expected, got $observed. Delete $destination and retry."
    echo "Reusing verified $name: $destination" >&2
    printf '%s\n' "$destination"
    return 0
  fi
  command -v curl >/dev/null 2>&1 || die "curl is required to download $name"
  partial="${destination}.partial"
  [ ! -L "$partial" ] || die "$name partial path must not be a symlink: $partial"
  echo "Downloading $name..." >&2
  curl -fL --retry 5 --retry-delay 2 -C - -o "$partial" "$url" ||
    die "$name download failed"
  observed="$(sha256_file "$partial")"
  [ "$observed" = "$expected" ] ||
    die "$name hash mismatch. Expected $expected, got $observed. Delete $partial and retry."
  mv "$partial" "$destination"
  printf '%s\n' "$destination"
}

java_home_is_21() {
  local home="$1"
  [ -x "$home/bin/java" ] && [ -x "$home/bin/javac" ] || return 1
  "$home/bin/java" -version 2>&1 | head -n 1 | grep -Eq 'version "21([.]|\")' &&
    "$home/bin/javac" -version 2>&1 | grep -Eq '^javac[[:space:]]+21([.]|$)'
}

configure_host_downloads() {
  local kernel arch
  kernel="$(uname -s)"
  arch="$(uname -m)"
  case "$kernel:$arch" in
    Linux:x86_64|Linux:amd64)
      JDK_ARCHIVE_NAME="OpenJDK21U-jdk_x64_linux_hotspot_21.0.11_10.tar.gz"
      JDK_SHA256="4B2220E232A97997B436CA6AB15CBF70171ECFF52958A46159DFA5A8C44CA4DE"
      ANDROID_ARCHIVE_NAME="commandlinetools-linux-${ANDROID_TOOLS_VERSION}_latest.zip"
      ANDROID_TOOLS_SHA256="4E4C464F145A7512B57D088AC6C278C03C9EEA610886B35A5E0804E74EEDF583"
      PLATFORM_TOOLS_ARCHIVE_NAME="platform-tools_r${PLATFORM_TOOLS_VERSION}-linux.zip"
      PLATFORM_TOOLS_SHA256="D230F13842F60F782A8645F9C813F8F845BF36089EA7289F28C48F17979313F1"
      SEVENZIP_ARCHIVE_NAME="7z2602-linux-x64.tar.xz"
      SEVENZIP_SHA256="41AABA7B1235304AB5AA0624530C67AE829496CD29E875925271EFDCCC28C03E"
      SEVENZIP_BINARY_SHA256="1676A968815B92E865BC0FFEECEE3FA284BA4402BF23DC2BEC2412C4B502E922"
      HOST_TAG="linux-x64"
      ;;
    Darwin:x86_64)
      JDK_ARCHIVE_NAME="OpenJDK21U-jdk_x64_mac_hotspot_21.0.11_10.tar.gz"
      JDK_SHA256="34180EB03E6D207C388CCE3DA668F6CC7CD7508C185C24782FADAC2C9C0E66F9"
      ANDROID_ARCHIVE_NAME="commandlinetools-mac_x86_64-${ANDROID_TOOLS_VERSION}_latest.zip"
      ANDROID_TOOLS_SHA256="C5A6378AB5CF7E0D5701921405115BEFFF13E9FF7417FB588389338F8BD050F3"
      PLATFORM_TOOLS_ARCHIVE_NAME="platform-tools_r${PLATFORM_TOOLS_VERSION}-darwin.zip"
      PLATFORM_TOOLS_SHA256="EE39AD5967E95C2A07F04DBCBDE96B1A0C916BA376096DB5D2F498B7727A5D1D"
      SEVENZIP_ARCHIVE_NAME="7z2602-mac.tar.xz"
      SEVENZIP_SHA256="1CF6760579502F87E591FF5C73A005EC50B3E4D6F507E8B038382D563C3175B9"
      SEVENZIP_BINARY_SHA256="9C56CF3379A0D8544E9244958B96FDC7C17F9CE70F5A160EB2B41F5F3DF96D8C"
      HOST_TAG="mac-x64"
      ;;
    Darwin:arm64|Darwin:aarch64)
      JDK_ARCHIVE_NAME="OpenJDK21U-jdk_aarch64_mac_hotspot_21.0.11_10.tar.gz"
      JDK_SHA256="6EBCF221C9B41507B14C098E93C6EAD6440B8D9BD154F8EC666C4C73ABBDB201"
      ANDROID_ARCHIVE_NAME="commandlinetools-mac_arm64-${ANDROID_TOOLS_VERSION}_latest.zip"
      ANDROID_TOOLS_SHA256="835B62A26162B229B441D1F6D4680383815A270809EB33522C0D480FA5002C4E"
      PLATFORM_TOOLS_ARCHIVE_NAME="platform-tools_r${PLATFORM_TOOLS_VERSION}-darwin.zip"
      PLATFORM_TOOLS_SHA256="EE39AD5967E95C2A07F04DBCBDE96B1A0C916BA376096DB5D2F498B7727A5D1D"
      SEVENZIP_ARCHIVE_NAME="7z2602-mac.tar.xz"
      SEVENZIP_SHA256="1CF6760579502F87E591FF5C73A005EC50B3E4D6F507E8B038382D563C3175B9"
      SEVENZIP_BINARY_SHA256="9C56CF3379A0D8544E9244958B96FDC7C17F9CE70F5A160EB2B41F5F3DF96D8C"
      HOST_TAG="mac-arm64"
      ;;
    *) die "automatic tool bootstrap supports x86_64 Linux and x86_64/arm64 macOS; pass complete --java-home and --android-sdk paths on $kernel/$arch" ;;
  esac
  JDK_URL="https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.11%2B10/${JDK_ARCHIVE_NAME}"
  ANDROID_TOOLS_URL="https://dl.google.com/android/repository/${ANDROID_ARCHIVE_NAME}"
  PLATFORM_TOOLS_URL="https://dl.google.com/android/repository/${PLATFORM_TOOLS_ARCHIVE_NAME}"
  SEVENZIP_URL="https://github.com/ip7z/7zip/releases/download/${SEVENZIP_VERSION}/${SEVENZIP_ARCHIVE_NAME}"
}

archive_requires_7zip() {
  local source="$1" lower
  [ -f "$source" ] || return 1
  lower="$(printf '%s' "$source" | tr '[:upper:]' '[:lower:]')"
  case "$lower" in *.7z|*.rar) return 0 ;; *) return 1 ;; esac
}

ensure_7zip() {
  local archive install_root binary version_output
  configure_host_downloads
  archive="$(download_verified "7-Zip $SEVENZIP_VERSION" "$SEVENZIP_URL" \
    "$WORK_DIR/.downloads/$SEVENZIP_ARCHIVE_NAME" "$SEVENZIP_SHA256")"
  install_root="$WORK_DIR/.tools/7zip-${SEVENZIP_VERSION}-${HOST_TAG}"
  prepare_managed_directory "$install_root"
  binary="$install_root/7zz"
  if [ ! -f "$binary" ] ||
     [ "$(sha256_file "$binary" 2>/dev/null || true)" != "$SEVENZIP_BINARY_SHA256" ]; then
    remove_managed_tree "$install_root"
    prepare_managed_directory "$install_root"
    tar -xJf "$archive" -C "$install_root" || die "7-Zip extraction failed"
  fi
  [ -f "$binary" ] || die "7-Zip archive has an unexpected layout"
  [ ! -L "$binary" ] || die "7-Zip executable must not be a symlink: $binary"
  chmod 700 "$binary" || die "could not make the pinned 7-Zip executable private"
  [ "$(sha256_file "$binary")" = "$SEVENZIP_BINARY_SHA256" ] || die "7-Zip executable hash mismatch"
  version_output="$("$binary" i 2>&1)" || die "pinned 7-Zip failed to run: $version_output"
  printf '%s\n' "$version_output" | grep -Eq '7-Zip.*26[.]02' ||
    die "pinned 7-Zip version validation failed"
  printf '%s\n' "$binary"
}

find_java_home() {
  local archive install_root
  if [ -n "$JAVA_HOME_ARG" ]; then
    java_home_is_21 "$JAVA_HOME_ARG" || die "--java-home must point to a complete JDK 21: $JAVA_HOME_ARG"
    (cd "$JAVA_HOME_ARG" && pwd -P)
    return 0
  fi

  configure_host_downloads
  archive="$(download_verified "Eclipse Temurin JDK $JDK_VERSION" "$JDK_URL" \
    "$WORK_DIR/.downloads/$JDK_ARCHIVE_NAME" "$JDK_SHA256")"
  install_root="$WORK_DIR/.tools/jdk-${JDK_VERSION}-${HOST_TAG}"
  if ! java_home_is_21 "$install_root" && ! java_home_is_21 "$install_root/Contents/Home"; then
    remove_managed_tree "$install_root"
    mkdir -p "$install_root"
    tar -xzf "$archive" -C "$install_root" --strip-components=1 || die "Temurin JDK extraction failed"
  fi
  if java_home_is_21 "$install_root"; then
    printf '%s\n' "$install_root"
  elif java_home_is_21 "$install_root/Contents/Home"; then
    printf '%s\n' "$install_root/Contents/Home"
  else
    die "Temurin JDK extraction did not produce a JDK 21 home"
  fi
}

ensure_android_command_line_tools() {
  local sdk_root="$1" candidate archive extract_root install_root invalid_root
  candidate="$sdk_root/cmdline-tools/$ANDROID_TOOLS_VERSION/bin/sdkmanager"
  if [ -f "$candidate" ]; then
    chmod +x "$candidate" 2>/dev/null || true
    printf '%s\n' "$candidate"
    return 0
  fi
  configure_host_downloads
  command -v unzip >/dev/null 2>&1 || die "unzip is required for Android tools"
  archive="$(download_verified "Android command-line tools $ANDROID_TOOLS_VERSION" \
    "$ANDROID_TOOLS_URL" "$WORK_DIR/.downloads/$ANDROID_ARCHIVE_NAME" "$ANDROID_TOOLS_SHA256")"
  extract_root="$WORK_DIR/.tools/android-command-line-tools-${ANDROID_TOOLS_VERSION}-${HOST_TAG}"
  install_root="$sdk_root/cmdline-tools/$ANDROID_TOOLS_VERSION"
  remove_managed_tree "$extract_root"
  mkdir -p "$extract_root" "$(dirname "$install_root")"
  unzip -q -o "$archive" -d "$extract_root" || die "Android command-line tools extraction failed"
  [ -f "$extract_root/cmdline-tools/bin/sdkmanager" ] || die "Android tools archive has an unexpected layout"
  if [ -e "$install_root" ]; then
    invalid_root="${install_root}.invalid-${STAMP}-$$"
    mv "$install_root" "$invalid_root" || die "could not preserve invalid Android CLI directory: $install_root"
    echo "Preserved invalid Android CLI directory: $invalid_root" >&2
  fi
  cp -R "$extract_root/cmdline-tools" "$install_root"
  candidate="$install_root/bin/sdkmanager"
  [ -f "$candidate" ] || die "Android SDK bootstrap did not create sdkmanager"
  chmod +x "$candidate" 2>/dev/null || true
  printf '%s\n' "$candidate"
}

find_android_sdk() {
  local candidate
  if [ -n "$ANDROID_SDK" ]; then
    mkdir -p "$ANDROID_SDK"
    (cd "$ANDROID_SDK" && pwd -P)
    return 0
  fi
  candidate="$WORK_DIR/.android-sdk"
  mkdir -p "$candidate"
  ensure_android_command_line_tools "$candidate" >/dev/null
  (cd "$candidate" && pwd -P)
}

missing_sdk_packages() {
  local sdk="$1"
  [ -f "$sdk/platforms/android-35/android.jar" ] || echo "platforms;android-35"
  if [ ! -f "$sdk/build-tools/35.0.0/aapt2" ] ||
     [ ! -f "$sdk/build-tools/35.0.0/apksigner" ] ||
     [ ! -f "$sdk/build-tools/35.0.0/d8" ]; then
    echo "build-tools;35.0.0"
  fi
  [ -f "$sdk/ndk/$NDK_VERSION/build/cmake/android.toolchain.cmake" ] || echo "ndk;$NDK_VERSION"
  if [ ! -f "$sdk/cmake/$CMAKE_VERSION/bin/cmake" ] ||
     [ ! -f "$sdk/cmake/$CMAKE_VERSION/bin/ninja" ]; then
    echo "cmake;$CMAKE_VERSION"
  fi
}

ensure_platform_tools() {
  local archive install_root adb version
  configure_host_downloads
  archive="$(download_verified "Android Platform-Tools $PLATFORM_TOOLS_VERSION" \
    "$PLATFORM_TOOLS_URL" "$WORK_DIR/.downloads/$PLATFORM_TOOLS_ARCHIVE_NAME" \
    "$PLATFORM_TOOLS_SHA256")"
  install_root="$WORK_DIR/.tools/platform-tools-${PLATFORM_TOOLS_VERSION}-${HOST_TAG}"
  adb="$install_root/platform-tools/adb"
  if [ ! -f "$adb" ]; then
    remove_managed_tree "$install_root"
    mkdir -p "$install_root"
    unzip -q -o "$archive" -d "$install_root" || die "Android Platform-Tools extraction failed"
  fi
  [ -f "$adb" ] || die "Android Platform-Tools archive has an unexpected layout"
  chmod +x "$install_root/platform-tools"/* 2>/dev/null || true
  version="$("$adb" version 2>&1)" || die "pinned ADB failed to run: $version"
  printf '%s\n' "$version" | grep -Eq 'Version[[:space:]]+37[.]0[.]1-' ||
    die "pinned ADB version validation failed: $version"
  printf '%s\n' "$adb"
}

ensure_sdk_packages() {
  local sdk="$1" sdkmanager answer rc
  local missing=()
  while IFS= read -r line; do
    [ -n "$line" ] && missing+=("$line")
  done < <(missing_sdk_packages "$sdk")
  [ "${#missing[@]}" -gt 0 ] || return 0
  echo "Missing Android SDK components:"
  printf '  %s\n' "${missing[@]}"
  sdkmanager="$(ensure_android_command_line_tools "$sdk")"
  if [ "$NON_INTERACTIVE" -eq 1 ]; then
    [ -f "$sdk/licenses/android-sdk-license" ] ||
      die "SDK licenses have not been accepted; run once without --non-interactive"
  else
    read -r -p "Install the missing SDK components and accept Google licenses? [Y/n] " answer
    case "$answer" in n|N|no|NO) die "required SDK components were not installed" ;; esac
    set +e
    set +o pipefail
    yes | "$sdkmanager" --sdk_root="$sdk" --licenses
    rc="${PIPESTATUS[1]}"
    set -o pipefail
    set -e
    [ "$rc" -eq 0 ] || die "Android SDK license step failed"
  fi
  "$sdkmanager" --sdk_root="$sdk" "${missing[@]}" || die "Android SDK component installation failed"
  missing=()
  while IFS= read -r line; do
    [ -n "$line" ] && missing+=("$line")
  done < <(missing_sdk_packages "$sdk")
  [ "${#missing[@]}" -eq 0 ] || die "Android SDK components remain missing: ${missing[*]}"
}

find_python() {
  local candidate
  for candidate in python3 python python.exe; do
    if command -v "$candidate" >/dev/null 2>&1 &&
       "$candidate" -c 'import sys; raise SystemExit(0 if sys.version_info >= (3, 10) else 1)' >/dev/null 2>&1; then
      command -v "$candidate"
      return 0
    fi
  done
  return 1
}

ensure_apktool() {
  local bundled="$ROOT/tools/vendor/apktool_3.0.3.jar"
  local tool_root="$WORK_DIR/.tools/apktool-$APKTOOL_VERSION"
  if [ -f "$bundled" ]; then
    [ "$(sha256_file "$bundled")" = "$APKTOOL_SHA256" ] || die "bundled apktool hash mismatch"
    printf '%s\n' "$bundled"
    return 0
  fi
  prepare_managed_directory "$tool_root"
  download_verified "apktool $APKTOOL_VERSION" "$APKTOOL_URL" \
    "$tool_root/apktool_3.0.3.jar" "$APKTOOL_SHA256"
}

ensure_openxr_loader() {
  local bundled="$ROOT/native/vendor/openxr/lib/arm64-v8a/libopenxr_loader.so"
  local aar output entry
  if [ -f "$bundled" ]; then
    [ "$(sha256_file "$bundled")" = "$OPENXR_LOADER_SHA256" ] || die "bundled OpenXR loader hash mismatch"
    printf '%s\n' "$bundled"
    return 0
  fi
  aar="$(download_verified "Khronos OpenXR Android loader AAR 1.1.43" "$OPENXR_AAR_URL" \
    "$WORK_DIR/.downloads/openxr_loader_for_android-1.1.43.aar" "$OPENXR_AAR_SHA256")"
  output="$WORK_DIR/.tools/openxr-1.1.43/libopenxr_loader.so"
  prepare_managed_directory "$(dirname "$output")"
  if [ -f "$output" ]; then
    [ "$(sha256_file "$output")" = "$OPENXR_LOADER_SHA256" ] ||
      die "cached OpenXR loader payload hash mismatch. Delete $output and retry."
    printf '%s\n' "$output"
    return 0
  fi
  entry="prefab/modules/openxr_loader/libs/android.arm64-v8a/libopenxr_loader.so"
  mkdir -p "$(dirname "$output")"
  unzip -j -o "$aar" "$entry" -d "$(dirname "$output")" >/dev/null || die "OpenXR loader extraction failed"
  [ -f "$output" ] || die "OpenXR AAR did not contain the arm64 loader"
  [ "$(sha256_file "$output")" = "$OPENXR_LOADER_SHA256" ] || die "OpenXR loader payload hash mismatch"
  printf '%s\n' "$output"
}

verify_local_tree() {
  local tree="$1"
  [ -f "$tree/SHA256SUMS" ] || die "payload checksum manifest is missing: $tree/SHA256SUMS"
  echo "Verifying local payload: $tree"
  if command -v sha256sum >/dev/null 2>&1; then
    (cd "$tree" && sha256sum -c SHA256SUMS >/dev/null)
  else
    (cd "$tree" && shasum -a 256 -c SHA256SUMS >/dev/null)
  fi
}

verify_build_manifest() {
  local manifest="$1"
  "$PYTHON_EXE" - "$BUILD_ROOT" "$manifest" <<'PY'
import hashlib, json, pathlib, sys

root = pathlib.Path(sys.argv[1]).resolve()
manifest_path = pathlib.Path(sys.argv[2]).resolve()
data = json.loads(manifest_path.read_text(encoding="utf-8"))

expected = {
    "formatVersion": 1,
    "package": "com.rockstargames.gtasa",
    "versionCode": "4234641",
    "versionName": "2.11.311",
    "sourceSignerSha256": "FF5B7B6A083FE5994E3306B30AE19D311951D019A8DE7C3E6914F0E06D130A13",
    "libGameSha256": "4C6A7445E30B27AFDDA781302E4DB9BAC89C28FC1181B68B1EEF16F84D6A282E",
}
for key, value in expected.items():
    if data.get(key) != value:
        raise SystemExit(f"build manifest mismatch: {key}")
if data.get("officialSource") is not True or not data.get("outputSignerSha256"):
    raise SystemExit("build manifest does not describe an official, signed source")

def inside(parent: pathlib.Path, child: pathlib.Path) -> pathlib.Path:
    resolved = child.resolve()
    try:
        resolved.relative_to(parent.resolve())
    except ValueError as error:
        raise SystemExit(f"manifest path escapes managed root: {resolved}") from error
    return resolved

def digest(path: pathlib.Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest().upper()

outputs = data.get("outputs") or []
if not outputs:
    raise SystemExit("build manifest contains no APK outputs")
for record in outputs:
    path = inside(root / "out", root / "out" / record["file"])
    if not path.is_file() or path.stat().st_size != int(record["size"]):
        raise SystemExit(f"built APK size/path mismatch: {path}")
    if digest(path) != record["sha256"].upper():
        raise SystemExit(f"built APK hash mismatch: {path}")

for tree_name in ("data_main", "audio", "vrhands"):
    tree = root / "payload" / tree_name
    section = data.get("payload", {}).get(tree_name)
    if not section or not (tree / "SHA256SUMS").is_file() or not (tree / "manifest.json").is_file():
        raise SystemExit(f"payload metadata is incomplete: {tree_name}")
    records = section.get("files") or []
    if len(records) != int(section.get("fileCount", -1)):
        raise SystemExit(f"payload file count mismatch: {tree_name}")
    total = 0
    for record in records:
        path = inside(tree, tree / record["path"])
        if not path.is_file() or path.stat().st_size != int(record["size"]):
            raise SystemExit(f"payload size/path mismatch: {path}")
        if digest(path) != record["sha256"].upper():
            raise SystemExit(f"payload hash mismatch: {path}")
        total += path.stat().st_size
    if total != int(section.get("totalBytes", -1)):
        raise SystemExit(f"payload total size mismatch: {tree_name}")

if not (root / "payload" / "payload-manifest.json").is_file():
    raise SystemExit("top-level payload manifest is missing")
print(f"Verified build manifest: {manifest_path}")
PY
}

adb_cmd() {
  "${ADB_BASE[@]}" "$@"
}

select_quest() {
  local output line found model manufacturer identity device_confirm device_serial state rest requested_state
  local devices=()
  local unauthorized=()
  local offline=()
  local no_permissions=()
  requested_state=""
  output="$($ADB devices -l 2>&1)" || die "adb devices failed: $output"
  while IFS= read -r line; do
    line="${line//$'\r'/}"
    [ -n "$line" ] || continue
    case "$line" in 'List of devices attached'*) continue ;; esac
    read -r device_serial state rest <<< "$line"
    [ -n "$device_serial" ] && [ -n "$state" ] || continue
    case "$state" in
      device) devices+=("$device_serial") ;;
      unauthorized) unauthorized+=("$device_serial") ;;
      offline) offline+=("$device_serial") ;;
      no) no_permissions+=("$device_serial") ;;
    esac
    if [ -n "$SERIAL" ] && [ "$device_serial" = "$SERIAL" ]; then requested_state="$state"; fi
  done <<< "$output"
  if [ -n "$SERIAL" ]; then
    found=0
    if [ "${#devices[@]}" -gt 0 ]; then
      for line in "${devices[@]}"; do [ "$line" = "$SERIAL" ] && found=1; done
    fi
    if [ "$found" -ne 1 ]; then
      case "$requested_state" in
        unauthorized) die "Quest '$SERIAL' is connected but unauthorized; unlock it and accept the USB debugging prompt" ;;
        offline) die "Quest '$SERIAL' is offline; reconnect USB, then restart ADB if needed" ;;
        no) die "Quest '$SERIAL' has no host USB permission; install Android udev rules and re-login" ;;
        *)
          [ "${#no_permissions[@]}" -eq 0 ] ||
            die "ADB cannot read a connected Android serial because Linux denied USB access; install Android udev rules and re-login"
          die "Quest '$SERIAL' is not connected and authorized"
          ;;
      esac
    fi
  elif [ "${#devices[@]}" -eq 0 ]; then
    if [ "${#unauthorized[@]}" -gt 0 ]; then
      die "Quest is connected but unauthorized; unlock it and accept the USB debugging prompt"
    elif [ "${#offline[@]}" -gt 0 ]; then
      die "Quest is offline; reconnect USB, then restart ADB if needed"
    elif [ "${#no_permissions[@]}" -gt 0 ]; then
      die "Quest is visible but Linux denied USB access; install Android udev rules, add the user to the required device group, and re-login"
    else
      die "no authorized Quest found; connect USB and accept the headset prompt"
    fi
  elif [ "${#devices[@]}" -eq 1 ]; then
    SERIAL="${devices[0]}"
  elif [ "$NON_INTERACTIVE" -eq 1 ]; then
    die "multiple Android devices are connected; pass --serial"
  else
    echo "Connected Android devices:"
    local index selection
    for ((index=0; index<${#devices[@]}; index++)); do echo "  $((index+1)). ${devices[index]}"; done
    read -r -p "Select the Quest number: " selection
    [[ "$selection" =~ ^[0-9]+$ ]] || die "invalid device selection"
    [ "$selection" -ge 1 ] && [ "$selection" -le "${#devices[@]}" ] || die "invalid device selection"
    SERIAL="${devices[selection-1]}"
  fi
  ADB_BASE=("$ADB" -s "$SERIAL")
  model="$(adb_cmd shell getprop ro.product.model | tr -d '\r')" || die "could not read selected device model"
  manufacturer="$(adb_cmd shell getprop ro.product.manufacturer | tr -d '\r')" ||
    die "could not read selected device manufacturer"
  identity="$manufacturer $model"
  echo "Selected device: $SERIAL ($identity)"
  if ! printf '%s\n' "$identity" | grep -Eqi 'quest|oculus|meta'; then
    [ "$NON_INTERACTIVE" -eq 0 ] ||
      die "selected device is not identified as a Meta/Oculus Quest: $identity"
    echo "WARNING: selected device is not identified as a Meta/Oculus Quest: '$identity'."
    read -r -p "Type INSTALL to approve modifying this device: " device_confirm
    [ "$device_confirm" = "INSTALL" ] || die "installation was not approved for the selected non-Quest device"
  fi
}

stop_game_and_verify() {
  local process_output
  adb_cmd shell am force-stop "$PACKAGE_NAME" >/dev/null || die "could not force-stop GTA SA"
  process_output="$(adb_cmd shell pidof "$PACKAGE_NAME" 2>/dev/null || true)"
  process_output="${process_output//$'\r'/}"
  [ -z "$process_output" ] || die "GTA SA process is still running after force-stop: $process_output"
}

assert_quest_free_space() {
  local sizes apk_bytes payload_bytes required_kib df_output available_kib
  sizes="$("$PYTHON_EXE" - "$BUILD_ROOT/build-manifest.json" <<'PY'
import json, pathlib, sys
data = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
print(sum(int(item["size"]) for item in data["outputs"]), int(data["payload"]["totalBytes"]))
PY
)"
  read -r apk_bytes payload_bytes <<< "$sizes"
  required_kib=$(( (2 * apk_bytes + payload_bytes + 536870912 + 1023) / 1024 ))
  df_output="$(adb_cmd shell toybox df -k /sdcard)" || die "could not query Quest free space"
  available_kib="$(printf '%s\n' "$df_output" | tr -d '\r' | awk 'NR > 1 && NF >= 4 {value=$4} END {print value}')"
  [[ "$available_kib" =~ ^[0-9]+$ ]] || die "could not parse Quest free space"
  awk -v have="$available_kib" -v need="$required_kib" \
    'BEGIN {printf "Quest free space: %.2f GiB; conservative requirement: %.2f GiB\n", have/1048576, need/1048576}'
  [ "$available_kib" -ge "$required_kib" ] ||
    die "Quest does not have enough free space for verified staging and rollback"
}

confirm_install_plan() {
  local values apk_count apk_bytes payload_bytes answer
  values="$("$PYTHON_EXE" - "$BUILD_ROOT/build-manifest.json" <<'PY'
import json, pathlib, sys
data = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
print(len(data["outputs"]), sum(int(item["size"]) for item in data["outputs"]), int(data["payload"]["totalBytes"]))
PY
)"
  read -r apk_count apk_bytes payload_bytes <<< "$values"
  echo ""
  echo "INSTALL PLAN — no Quest changes have been made yet"
  echo "  Device: $SERIAL"
  echo "  Package: $PACKAGE_NAME"
  awk -v count="$apk_count" -v apk="$apk_bytes" -v payload="$payload_bytes" \
    'BEGIN {printf "  APK set: %d files, %.2f GiB\n  Payload: %.2f GiB\n", count, apk/1073741824, payload/1073741824}'
  echo "  Signing key: $KEYSTORE"
  echo "  Actions: force-stop, install verified APK set, preserve saves/settings when signature replacement is required, upload hash-verified payload"
  echo "  The game will not be launched."
  if [ "$ASSUME_YES" -eq 1 ]; then
    echo "  Approval: --yes"
    return 0
  fi
  [ "$NON_INTERACTIVE" -eq 0 ] || die "Quest installation requires --yes with --non-interactive"
  read -r -p "Type INSTALL to execute this plan: " answer
  [ "$answer" = "INSTALL" ] || die "Quest installation was not approved"
}

cleanup_on_exit() {
  local rc="$?" path index target backup had_existing
  set +e
  if [ "$INSTALL_ACTIVE" -eq 1 ] && [ "${#ADB_BASE[@]}" -gt 0 ]; then
    if [ "$rc" -ne 0 ]; then
      for ((index=0; index<${#REMOTE_ROLLBACK_TARGETS[@]}; index++)); do
        target="${REMOTE_ROLLBACK_TARGETS[index]-}"
        backup="${REMOTE_ROLLBACK_BACKUPS[index]-}"
        had_existing="${REMOTE_ROLLBACK_HAD_EXISTING[index]-}"
        [ -n "$target" ] && [ -n "$backup" ] || continue
        case "$had_existing" in 0|1) ;; *) continue ;; esac
        if [ "$had_existing" -eq 1 ]; then
          if adb_cmd shell test -d "$backup" >/dev/null 2>&1; then
            if adb_cmd shell test -e "$target" >/dev/null 2>&1 &&
               ! adb_cmd shell rm -rf "$target" >/dev/null 2>&1; then
              echo "WARNING: could not remove interrupted Quest payload: $target"
              echo "WARNING: rollback remains at: $backup"
              continue
            fi
            if adb_cmd shell test -e "$target" >/dev/null 2>&1; then
              echo "WARNING: interrupted Quest payload still exists after removal: $target"
              echo "WARNING: rollback remains at: $backup"
              continue
            fi
            if adb_cmd shell mv "$backup" "$target" >/dev/null 2>&1; then
              echo "Restored interrupted Quest payload from: $backup"
            else
              echo "WARNING: could not restore interrupted Quest payload; rollback remains at: $backup"
            fi
          fi
        elif adb_cmd shell test -e "$target" >/dev/null 2>&1; then
          if adb_cmd shell rm -rf "$target" >/dev/null 2>&1; then
            echo "Removed interrupted Quest payload that had no previous target: $target"
          else
            echo "WARNING: could not remove interrupted Quest payload: $target"
          fi
        fi
      done
    fi
    if [ "${#REMOTE_STAGING[@]}" -gt 0 ]; then
      for path in "${REMOTE_STAGING[@]}"; do
        adb_cmd shell rm -rf "$path" >/dev/null 2>&1 || true
      done
    fi
    adb_cmd shell am force-stop "$PACKAGE_NAME" >/dev/null 2>&1 || true
  fi
  if [ -n "$KEY_LOCK" ]; then
    rmdir "$KEY_LOCK" >/dev/null 2>&1 || true
  fi
  if [ -n "$KEY_TEMP" ] && [ -n "$SIGNING_ROOT" ] && path_is_within "$KEY_TEMP" "$SIGNING_ROOT"; then
    rm -f "$KEY_TEMP" >/dev/null 2>&1 || true
  fi
  if [ "$rc" -ne 0 ]; then
    echo "FAILED. No failure was treated as success."
    [ -n "$LOG_PATH" ] && echo "Diagnostic log: $LOG_PATH"
  fi
}
trap cleanup_on_exit EXIT

verify_host_backup_against_remote() {
  local backup="$1" remote_root="$2" relative expected remote_hash
  while IFS=$'\t' read -r relative expected; do
    [ -n "$relative" ] || continue
    remote_hash="$(adb_cmd shell sh -c "toybox sha256sum '$remote_root/$relative'" |
      awk '{print toupper($1); exit}')" || die "could not hash Quest backup source: $relative"
    [ "$remote_hash" = "$expected" ] ||
      die "Quest-to-host backup hash mismatch before uninstall: $relative"
  done < "$backup/file-manifest.tsv"
}

backup_top_level_files() {
  local backup="$1" remote_root="$2" listing name rc
  mkdir -p "$backup/files"
  : > "$backup/top-level.txt"
  if ! adb_cmd shell test -d "$remote_root" >/dev/null 2>&1; then
    listing=""
  else
    set +e
    listing="$(adb_cmd shell ls -1A "$remote_root" 2>&1)"
    rc="$?"
    set -e
    [ "$rc" -eq 0 ] || die "could not enumerate Quest saves/settings: $listing"
  fi
  while IFS= read -r name; do
    [ -n "$name" ] || continue
    case "$name" in audio|vrhands|.savr-stage-*|.savr-backup-*) continue ;; esac
    if [ "$name" = "." ] || [ "$name" = ".." ] ||
       [[ ! "$name" =~ ^[A-Za-z0-9_.-]+$ ]]; then
      die "unsafe top-level Quest filename: $name"
    fi
    echo "Backing up $name"
    adb_cmd pull "$remote_root/$name" "$backup/files" >/dev/null || die "could not back up $remote_root/$name"
    printf '%s\n' "$name" >> "$backup/top-level.txt"
  done <<< "$listing"
  "$PYTHON_EXE" - "$backup" <<'PY'
import hashlib, json, pathlib, sys
root = pathlib.Path(sys.argv[1])
top = [line for line in (root / "top-level.txt").read_text(encoding="utf-8").splitlines() if line]
items = []
files_root = (root / "files").resolve()
for path in sorted((root / "files").rglob("*")):
    if path.is_symlink():
        raise SystemExit(f"refusing symlink in Quest backup: {path}")
    if not path.is_file():
        continue
    try:
        path.resolve().relative_to(files_root)
    except ValueError as error:
        raise SystemExit(f"Quest backup path escapes the host backup: {path}") from error
    relative = path.relative_to(root / "files").as_posix()
    if any(character in relative for character in ("'", "\t", "\r", "\n")):
        raise SystemExit(f"unsafe backed-up relative path: {relative!r}")
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    digest = value.hexdigest().upper()
    items.append({"path": relative, "size": path.stat().st_size, "sha256": digest})
(root / "file-manifest.tsv").write_text(
    "".join(f'{item["path"]}\t{item["sha256"]}\n' for item in items), encoding="utf-8"
)
(root / "backup-manifest.json").write_text(
    json.dumps({
        "formatVersion": 1,
        "package": "com.rockstargames.gtasa",
        "remoteRoot": "/sdcard/Android/data/com.rockstargames.gtasa/files",
        "excluded": ["audio", "vrhands", ".savr-stage-*", ".savr-backup-*"],
        "topLevelItems": top,
        "files": items,
    }, indent=2) + "\n", encoding="utf-8"
)
PY
  verify_host_backup_against_remote "$backup" "$remote_root"
  echo "Verified device backup: $backup"
}

restore_top_level_files() {
  local backup="$1" remote_root="/sdcard/Android/data/$PACKAGE_NAME/files"
  local name relative expected remote_hash
  [ -f "$backup/top-level.txt" ] || return 0
  adb_cmd shell mkdir -p "$remote_root" || die "could not create Quest files directory"
  while IFS= read -r name; do
    [ -n "$name" ] || continue
    if [ "$name" = "." ] || [ "$name" = ".." ] ||
       [[ ! "$name" =~ ^[A-Za-z0-9_.-]+$ ]]; then
      die "unsafe backed-up top-level filename: $name"
    fi
    adb_cmd shell sh -c "rm -rf '$remote_root/$name'" || die "could not prepare restore destination for $name"
    adb_cmd push "$backup/files/$name" "$remote_root/$name" >/dev/null || die "could not restore $name"
  done < "$backup/top-level.txt"
  while IFS=$'\t' read -r relative expected; do
    [ -n "$relative" ] || continue
    remote_hash="$(adb_cmd shell sh -c "toybox sha256sum '$remote_root/$relative'" |
      awk '{print toupper($1); exit}')" || die "could not hash restored Quest file: $relative"
    [ "$remote_hash" = "$expected" ] || die "restored hash mismatch for $relative"
  done < "$backup/file-manifest.tsv"
}

verify_remote_tree() {
  local remote="$1"
  adb_cmd shell "cd '$remote' && toybox sha256sum -c SHA256SUMS >/dev/null"
}

install_payload_tree() {
  local local_tree="$1" target="$2" label="$3"
  local parent tree_name stage_container stage backup id had_existing=0 rollback_index
  parent="${target%/*}"
  tree_name="${local_tree##*/}"
  case "$tree_name" in data_main|audio|vrhands) ;; *) die "refusing unexpected local payload tree: $local_tree" ;; esac
  [ "${target##*/}" = "$tree_name" ] || die "payload tree does not match its Quest destination: $tree_name"
  id="$("$PYTHON_EXE" -c 'import uuid; print(uuid.uuid4().hex)')"
  # Do not let adb push touch Android/data: scoped-storage secure_mkdirs fails
  # on some Quest firmware even for shell-created directories. Upload and
  # verify under ordinary shared storage, then move with the device shell.
  stage_container="/sdcard/savr/.savr-stage-$id"
  stage="$stage_container/$tree_name"
  backup="$parent/.savr-backup-$id"
  REMOTE_STAGING+=("$stage_container")
  echo "==> Uploading $label"
  adb_cmd shell mkdir -p "$parent" || die "could not create $parent"
  adb_cmd shell rm -rf "$stage_container" "$backup" || die "could not prepare $label staging paths"
  adb_cmd shell mkdir -p "$stage" || die "could not create $label staging directory"
  while IFS= read -r -d '' local_dir; do
    relative_dir="${local_dir#"$local_tree"/}"
    [[ "$relative_dir" =~ ^[A-Za-z0-9_.-]+(/[A-Za-z0-9_.-]+)*$ ]] ||
      die "payload contains a directory name that is unsafe for ADB: $relative_dir"
    adb_cmd shell mkdir -p "$stage/$relative_dir" ||
      die "could not create staged $label directory: $relative_dir"
  done < <(find "$local_tree" -mindepth 1 -type d -print0)
  adb_cmd push "$local_tree" "$stage_container" || die "$label transfer failed"
  adb_cmd shell test -f "$stage/SHA256SUMS" || die "$label transfer created an unexpected directory layout"
  verify_remote_tree "$stage" || die "Quest checksum verification failed for staged $label"
  if adb_cmd shell test -d "$target" >/dev/null 2>&1; then
    had_existing=1
  elif adb_cmd shell test -e "$target" >/dev/null 2>&1; then
    die "existing $label target is not a directory: $target"
  fi
  rollback_index="${#REMOTE_ROLLBACK_TARGETS[@]}"
  REMOTE_ROLLBACK_TARGETS+=("$target")
  REMOTE_ROLLBACK_BACKUPS+=("$backup")
  REMOTE_ROLLBACK_HAD_EXISTING+=("$had_existing")
  if [ "$had_existing" -eq 1 ]; then
    adb_cmd shell mv "$target" "$backup" || die "could not preserve existing $label"
  fi
  if ! adb_cmd shell mv "$stage" "$target"; then
    die "could not activate $label; rollback will be attempted"
  fi
  adb_cmd shell chmod -R a+rX "$target" ||
    die "could not make the verified $label payload readable by the game"
  if ! verify_remote_tree "$target"; then
    die "final Quest checksum verification failed for $label; rollback will be attempted"
  fi
  REMOTE_ROLLBACK_TARGETS[rollback_index]=""
  REMOTE_ROLLBACK_BACKUPS[rollback_index]=""
  REMOTE_ROLLBACK_HAD_EXISTING[rollback_index]=""
  if ! adb_cmd shell rm -rf "$stage_container" >/dev/null 2>&1; then
    echo "WARNING: $label is active, but its empty staging container could not be removed: $stage_container"
  fi
  if [ "$had_existing" -eq 1 ]; then
    adb_cmd shell rm -rf "$backup" || die "$label is active, but its rollback backup could not be removed: $backup"
  fi
}

verify_installed_apks() {
  local output base_remote arm_remote line expected observed package_dump
  local verify_dir="$WORK_DIR/device-verification/$RUN_ID"
  output="$(adb_cmd shell pm path "$PACKAGE_NAME")" || die "pm path failed after installation"
  if ! "$PYTHON_EXE" - "$BUILD_ROOT/build-manifest.json" "$output" <<'PY'
import json, pathlib, re, sys

manifest = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
expected = []
for record in manifest.get("outputs") or []:
    split = record.get("split")
    leaf = "base.apk" if split is None else f"split_{re.sub(r'[^A-Za-z0-9_.-]+', '_', split)}.apk"
    if record.get("file") != leaf:
        raise SystemExit(f"manifest output name does not match its split id: {record.get('file')!r} != {leaf!r}")
    expected.append(leaf)

remote = []
for line in sys.argv[2].replace("\r", "").splitlines():
    if not line:
        continue
    if not line.startswith("package:") or not line.endswith(".apk"):
        raise SystemExit(f"unexpected pm path output: {line!r}")
    remote.append(pathlib.PurePosixPath(line.removeprefix("package:")).name)

if len(expected) != len(set(expected)):
    raise SystemExit(f"duplicate expected APK leaves: {expected}")
if len(remote) != len(set(remote)):
    raise SystemExit(f"duplicate installed APK leaves: {remote}")
if sorted(remote) != sorted(expected):
    raise SystemExit(f"installed APK set mismatch; expected {sorted(expected)}, got {sorted(remote)}")
print(f"Installed APK leaf set verified: {len(remote)} files")
PY
  then
    die "installed APK leaf set does not exactly match the verified build manifest"
  fi
  package_dump="$(adb_cmd shell dumpsys package "$PACKAGE_NAME")" || die "dumpsys package failed"
  printf '%s\n' "$package_dump" | grep -Eq 'versionCode=4234641([^0-9]|$)' ||
    die "installed GTA SA versionCode is not 4234641"
  base_remote=""
  arm_remote=""
  while IFS= read -r line; do
    line="${line//$'\r'/}"
    line="${line#package:}"
    case "$line" in
      */base.apk) base_remote="$line" ;;
      */split_config.arm64_v8a.apk) arm_remote="$line" ;;
    esac
  done <<< "$output"
  [ -n "$base_remote" ] || die "installed base APK path was not reported"
  remove_managed_tree "$verify_dir"
  mkdir -p "$verify_dir"
  adb_cmd pull "$base_remote" "$verify_dir/base.apk" >/dev/null || die "could not pull installed base APK"
  expected="$(sha256_file "$BUILD_ROOT/out/base.apk")"
  observed="$(sha256_file "$verify_dir/base.apk")"
  [ "$observed" = "$expected" ] || die "installed base APK hash mismatch"
  if [ -f "$BUILD_ROOT/out/split_config.arm64_v8a.apk" ]; then
    [ -n "$arm_remote" ] || die "installed ARM64 split path was not reported"
    adb_cmd pull "$arm_remote" "$verify_dir/split_config.arm64_v8a.apk" >/dev/null || die "could not pull installed ARM64 split"
    expected="$(sha256_file "$BUILD_ROOT/out/split_config.arm64_v8a.apk")"
    observed="$(sha256_file "$verify_dir/split_config.arm64_v8a.apk")"
    [ "$observed" = "$expected" ] || die "installed ARM64 split hash mismatch"
  fi
  echo "Installed base/ARM64 APK hashes verified."
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --game-package|-GamePackage) GAME_PACKAGE="${2:?missing game package}"; shift 2 ;;
    --audio-source|-AudioSource) AUDIO_SOURCE="${2:?missing audio source}"; shift 2 ;;
    --work-dir|-WorkDir) WORK_DIR="${2:?missing work directory}"; shift 2 ;;
    --android-sdk|-AndroidSdk) ANDROID_SDK="${2:?missing Android SDK}"; shift 2 ;;
    --java-home|-JavaHome) JAVA_HOME_ARG="${2:?missing Java home}"; shift 2 ;;
    --serial|-Serial) SERIAL="${2:?missing serial}"; shift 2 ;;
    --log-path|-LogPath) LOG_PATH="${2:?missing log path}"; shift 2 ;;
    --build-only|--no-install|-BuildOnly) BUILD_ONLY=1; shift ;;
    --non-interactive|-NonInteractive) NON_INTERACTIVE=1; shift ;;
    --yes|-Yes) ASSUME_YES=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown argument: $1 (run --help)" ;;
  esac
done

if [ "$NON_INTERACTIVE" -eq 1 ] && [ "$BUILD_ONLY" -eq 0 ] && [ "$ASSUME_YES" -eq 0 ]; then
  die "--non-interactive Quest installation requires explicit --yes"
fi
PYTHON_EXE="$(find_python)" || die "Python 3.10+ is required"
WORK_DIR="$(normalize_prompt_path "$WORK_DIR")"
[ -n "$WORK_DIR" ] || die "--work-dir must be a dedicated directory"
WORK_DIR="$(canonicalize_directory_target "$WORK_DIR")"
[ "$WORK_DIR" != "/" ] || die "--work-dir resolved to a filesystem root"
if path_is_within "$WORK_DIR" "$ROOT" || path_is_within "$ROOT" "$WORK_DIR"; then
  die "--work-dir and the public source kit must be separate, non-nested directories"
fi
mkdir -p "$WORK_DIR"
WORK_DIR="$(cd "$WORK_DIR" && pwd -P)"
for managed_name in .downloads .tools .android-sdk runs device-backups device-verification logs; do
  prepare_managed_directory "$WORK_DIR/$managed_name"
done
STAMP="$(date +%Y%m%d-%H%M%S)"
RUN_ID="$STAMP-$$-${RANDOM}${RANDOM}"
BUILD_ROOT="$WORK_DIR/runs/$RUN_ID"
prepare_managed_directory "$BUILD_ROOT"
if [ -z "$LOG_PATH" ]; then
  LOG_PATH="$WORK_DIR/logs/build-and-install-$RUN_ID.log"
else
  LOG_PATH="$(normalize_prompt_path "$LOG_PATH")"
fi
mkdir -p "$(dirname "$LOG_PATH")"
exec > >(tee -a "$LOG_PATH") 2>&1

echo "GTA San Andreas VR - personal Quest source builder"
echo "No APK, GTA data, or audio mod is included in this source kit."
echo "Work directory: $WORK_DIR"
echo "Run directory: $BUILD_ROOT"

GAME_PACKAGE_PROMPTED=0
if [ -z "$GAME_PACKAGE" ]; then
  [ "$NON_INTERACTIVE" -eq 0 ] && [ -t 0 ] || die "--game-package is required in non-interactive input"
  read -r -p "Original GTA SA APK, split folder, or archive: " GAME_PACKAGE
  GAME_PACKAGE_PROMPTED=1
fi
AUDIO_SOURCE_PROMPTED=0
if [ -z "$AUDIO_SOURCE" ]; then
  [ "$NON_INTERACTIVE" -eq 0 ] && [ -t 0 ] || die "--audio-source is required in non-interactive input"
  read -r -p "Supported sound-mod folder or archive: " AUDIO_SOURCE
  AUDIO_SOURCE_PROMPTED=1
fi
if [ "$GAME_PACKAGE_PROMPTED" -eq 1 ]; then GAME_PACKAGE="$(normalize_prompt_path "$GAME_PACKAGE")"; fi
if [ "$AUDIO_SOURCE_PROMPTED" -eq 1 ]; then AUDIO_SOURCE="$(normalize_prompt_path "$AUDIO_SOURCE")"; fi
[ -e "$GAME_PACKAGE" ] || die "game package does not exist: $GAME_PACKAGE"
[ -e "$AUDIO_SOURCE" ] || die "audio source does not exist: $AUDIO_SOURCE"
GAME_PACKAGE="$(absolute_existing "$GAME_PACKAGE")"
AUDIO_SOURCE="$(absolute_existing "$AUDIO_SOURCE")"

command -v tar >/dev/null 2>&1 || die "tar is required"
command -v unzip >/dev/null 2>&1 || die "unzip is required"
if archive_requires_7zip "$GAME_PACKAGE" || archive_requires_7zip "$AUDIO_SOURCE"; then
  SAVR_ARCHIVE_TOOL="$(ensure_7zip)"
  export SAVR_ARCHIVE_TOOL
  echo "Archive reader: $SAVR_ARCHIVE_TOOL (pinned 7-Zip $SEVENZIP_VERSION)"
fi

DATA_HOME="$(normalize_prompt_path "${XDG_DATA_HOME:-${HOME}/.local/share}")"
SIGNING_CANDIDATE="$DATA_HOME/gtasavr-builder/signing"
[ ! -L "$SIGNING_CANDIDATE" ] || die "persistent signing directory must not be a symlink: $SIGNING_CANDIDATE"
SIGNING_ROOT="$(canonicalize_directory_target "$SIGNING_CANDIDATE")"
if path_is_within "$SIGNING_ROOT" "$ROOT" || path_is_within "$SIGNING_ROOT" "$WORK_DIR"; then
  die "persistent signing directory must stay outside the source kit and --work-dir: $SIGNING_ROOT"
fi
mkdir -p "$SIGNING_ROOT"
[ ! -L "$SIGNING_ROOT" ] || die "persistent signing directory must not be a symlink: $SIGNING_ROOT"
[ "$(cd "$SIGNING_ROOT" && pwd -P)" = "$SIGNING_ROOT" ] || die "persistent signing directory resolved unexpectedly"
[ -O "$SIGNING_ROOT" ] || die "persistent signing directory must be owned by the current user: $SIGNING_ROOT"
chmod 700 "$SIGNING_ROOT" || die "could not protect persistent signing directory"
KEYSTORE="$SIGNING_ROOT/savr.keystore"
[ ! -L "$KEYSTORE" ] || die "persistent signing key must not be a symlink: $KEYSTORE"
if [ -e "$KEYSTORE" ]; then
  [ -f "$KEYSTORE" ] || die "persistent signing key is not a regular file: $KEYSTORE"
  [ -O "$KEYSTORE" ] || die "persistent signing key must be owned by the current user: $KEYSTORE"
  chmod 600 "$KEYSTORE" || die "could not protect persistent signing key"
  BUILD_KEYSTORE="$KEYSTORE"
else
  KEY_LOCK="$SIGNING_ROOT/.keystore-create.lock"
  mkdir "$KEY_LOCK" || die "signing-key lock exists: $KEY_LOCK. If no builder is running, remove only this empty lock directory and retry"
  KEY_TEMP="$SIGNING_ROOT/.savr.keystore.new-$RUN_ID"
  [ ! -e "$KEY_TEMP" ] && [ ! -L "$KEY_TEMP" ] || die "temporary signing key path already exists: $KEY_TEMP"
  BUILD_KEYSTORE="$KEY_TEMP"
fi

echo "==> Preparing pinned build tools"
JAVA_HOME_ARG="$(find_java_home)"
export JAVA_HOME="$JAVA_HOME_ARG"
ANDROID_SDK="$(find_android_sdk)"
export ANDROID_HOME="$ANDROID_SDK"
export ANDROID_SDK_ROOT="$ANDROID_SDK"
ensure_sdk_packages "$ANDROID_SDK"
APKTOOL="$(ensure_apktool)"
OPENXR_LOADER="$(ensure_openxr_loader)"

echo "JDK: $JAVA_HOME_ARG"
echo "Android SDK: $ANDROID_SDK"
echo "Python: $PYTHON_EXE"
echo "Persistent signing key: $KEYSTORE (keep it for future updates)"

echo "==> Validating the selected GTA SA package and audio before compilation"
"$PYTHON_EXE" "$ROOT/tools/assemble.py" \
  --game-package "$GAME_PACKAGE" \
  --audio-source "$AUDIO_SOURCE" \
  --build-dir "$BUILD_ROOT" \
  --sdk "$ANDROID_SDK" \
  --java-home "$JAVA_HOME_ARG" \
  --apktool "$APKTOOL" \
  --validate-only

bash "$ROOT/tools/build.sh" \
  --configuration RelWithDebInfo \
  --android-sdk "$ANDROID_SDK" \
  --java-home "$JAVA_HOME_ARG" \
  --openxr-loader "$OPENXR_LOADER" \
  --build-dir "$BUILD_ROOT" \
  --python "$PYTHON_EXE" \
  --apktool "$APKTOOL" \
  --game-package "$GAME_PACKAGE" \
  --audio-source "$AUDIO_SOURCE" \
  --keystore "$BUILD_KEYSTORE" \
  --package

[ -f "$BUILD_KEYSTORE" ] && [ ! -L "$BUILD_KEYSTORE" ] || die "build did not produce a regular signing key"
[ -O "$BUILD_KEYSTORE" ] || die "signing key must be owned by the current user"
chmod 600 "$BUILD_KEYSTORE" || die "could not protect signing key"
if [ -n "$KEY_TEMP" ]; then
  [ ! -e "$KEYSTORE" ] || die "persistent signing key appeared while the builder held its creation lock"
  mv "$KEY_TEMP" "$KEYSTORE" || die "could not publish the persistent signing key"
  KEY_TEMP=""
  chmod 600 "$KEYSTORE" || die "could not protect persistent signing key"
fi
if [ -n "$KEY_LOCK" ]; then
  rmdir "$KEY_LOCK" || die "could not release signing-key creation lock: $KEY_LOCK"
  KEY_LOCK=""
fi

[ -f "$BUILD_ROOT/build-manifest.json" ] || die "verified build manifest was not produced"
verify_build_manifest "$BUILD_ROOT/build-manifest.json"
verify_local_tree "$BUILD_ROOT/payload/data_main"
verify_local_tree "$BUILD_ROOT/payload/audio"
verify_local_tree "$BUILD_ROOT/payload/vrhands"

if [ "$BUILD_ONLY" -eq 1 ]; then
  echo ""
  echo "BUILD AND LOCAL VERIFICATION COMPLETED."
  echo "APK output: $BUILD_ROOT/out"
  echo "Quest payload: $BUILD_ROOT/payload"
  echo "Build manifest: $BUILD_ROOT/build-manifest.json"
  echo "No Quest was modified."
  echo "Diagnostic log: $LOG_PATH"
  exit 0
fi

ADB="$(ensure_platform_tools)"
[ -x "$ADB" ] || die "pinned ADB was not installed at $ADB"
select_quest
echo "Quest: $SERIAL"
assert_quest_free_space
confirm_install_plan
INSTALL_ACTIVE=1
stop_game_and_verify

APKS=()
while IFS= read -r apk_name; do
  [ -n "$apk_name" ] && APKS+=("$BUILD_ROOT/out/$apk_name")
done < <("$PYTHON_EXE" - "$BUILD_ROOT/build-manifest.json" <<'PY'
import json, pathlib, sys
data = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
records = list(data["outputs"])
records.sort(key=lambda item: (item.get("split") is not None, item["file"]))
for record in records:
    print(record["file"])
PY
)
[ "${#APKS[@]}" -gt 0 ] || die "no assembled APKs found under $BUILD_ROOT/out"

echo "==> Installing the complete personal APK set"
set +e
INSTALL_OUTPUT="$(adb_cmd install-multiple -r "${APKS[@]}" 2>&1)"
INSTALL_RC="$?"
set -e
printf '%s\n' "$INSTALL_OUTPUT"
DEVICE_BACKUP=""
if [ "$INSTALL_RC" -ne 0 ] || ! printf '%s\n' "$INSTALL_OUTPUT" | grep -Eq '^Success[[:space:]]*$'; then
  if printf '%s' "$INSTALL_OUTPUT" | grep -Eq 'INSTALL_FAILED_UPDATE_INCOMPATIBLE|signatures do not match'; then
    DEVICE_BACKUP="$WORK_DIR/device-backups/$RUN_ID"
    backup_top_level_files "$DEVICE_BACKUP" "/sdcard/Android/data/$PACKAGE_NAME/files"
    echo ""
    echo "WARNING: the installed GTA SA uses a different signing certificate."
    echo "Android requires uninstalling it, which can erase application data."
    echo "Verified top-level saves/settings were backed up to: $DEVICE_BACKUP"
    echo "Private internal app data that Android does not expose to ADB was not and cannot be backed up."
    [ "$NON_INTERACTIVE" -eq 0 ] || die "signature replacement requires an interactive destructive confirmation"
    read -r -p "Type UNINSTALL to remove the old package and continue: " CONFIRM
    [ "$CONFIRM" = "UNINSTALL" ] || die "the old package was left installed"
    stop_game_and_verify
    verify_host_backup_against_remote "$DEVICE_BACKUP" "/sdcard/Android/data/$PACKAGE_NAME/files"
    set +e
    UNINSTALL_OUTPUT="$(adb_cmd uninstall "$PACKAGE_NAME" 2>&1)"
    UNINSTALL_RC="$?"
    set -e
    printf '%s\n' "$UNINSTALL_OUTPUT"
    [ "$UNINSTALL_RC" -eq 0 ] && printf '%s\n' "$UNINSTALL_OUTPUT" | grep -Eq '^Success[[:space:]]*$' ||
      die "could not uninstall the incompatible package"
    set +e
    FRESH_INSTALL_OUTPUT="$(adb_cmd install-multiple "${APKS[@]}" 2>&1)"
    FRESH_INSTALL_RC="$?"
    set -e
    printf '%s\n' "$FRESH_INSTALL_OUTPUT"
    [ "$FRESH_INSTALL_RC" -eq 0 ] && printf '%s\n' "$FRESH_INSTALL_OUTPUT" | grep -Eq '^Success[[:space:]]*$' ||
      die "fresh personal APK installation failed"
  else
    die "APK installation failed"
  fi
fi

adb_cmd shell pm path "$PACKAGE_NAME" >/dev/null || die "package is not installed after adb install-multiple"
if [ -n "$DEVICE_BACKUP" ]; then
  adb_cmd shell mkdir -p "/sdcard/Android/data/$PACKAGE_NAME/files" || die "could not create application files directory"
  restore_top_level_files "$DEVICE_BACKUP"
fi

install_payload_tree "$BUILD_ROOT/payload/data_main" "/sdcard/savr/data_main" "data_main assets"
install_payload_tree "$BUILD_ROOT/payload/audio" "/sdcard/Android/data/$PACKAGE_NAME/files/audio" "verified audio"
install_payload_tree "$BUILD_ROOT/payload/vrhands" "/sdcard/Android/data/$PACKAGE_NAME/files/vrhands" "VR hands"

verify_installed_apks
stop_game_and_verify
INSTALL_ACTIVE=0

echo ""
echo "BUILD, INSTALL, PAYLOAD, AND FILE-LEVEL HASH VERIFICATION COMPLETED."
echo "The installer did not launch or runtime-test gameplay; the game was left stopped."
echo "APK output: $BUILD_ROOT/out"
echo "Quest payload: $BUILD_ROOT/payload"
echo "Build manifest: $BUILD_ROOT/build-manifest.json"
echo "Diagnostic log: $LOG_PATH"
