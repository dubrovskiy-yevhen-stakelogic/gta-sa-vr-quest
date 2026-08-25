#!/usr/bin/env bash
# Portable Linux/macOS equivalent of build.ps1.
# Builds the arm64 VR layer and loader DEX. With --package it also invokes
# assemble.py against the player's separately supplied GTA package and audio.
set -euo pipefail
umask 077

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
CONFIGURATION="RelWithDebInfo"
ANDROID_SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}"
JAVA_HOME_ARG="${JAVA_HOME:-}"
OPENXR_LOADER="${SAVR_OPENXR_LOADER_SO:-}"
BUILD_ROOT="$ROOT/build"
PYTHON_EXE=""
PYTHON_PATH=""
APKTOOL=""
GAME_PACKAGE=""
AUDIO_SOURCE=""
KEYSTORE=""
PACKAGE=0
ALLOW_UNOFFICIAL_SOURCE=0

usage() {
  cat <<'EOF'
Usage: tools/build.sh [options]

Build options:
  --configuration NAME       Debug, RelWithDebInfo (default), or Release
  --android-sdk DIR          Android SDK root (or ANDROID_SDK_ROOT/ANDROID_HOME)
  --java-home DIR            JDK 21 root (or JAVA_HOME)
  --openxr-loader FILE       Verified Khronos 1.1.43 arm64 libopenxr_loader.so
  --build-dir DIR            Managed build directory (default: <kit>/build)

Personal package options:
  --package                  Build and verify the personal APK set and payload
  --game-package PATH        Original APK, split folder, or APK archive
  --audio-source PATH        Supported sound-mod folder or archive
  --python EXE               Python 3 executable (default: python3/python)
  --apktool FILE             apktool 3.0.3 jar
  --keystore FILE            Personal signing key (default: build/signing)
  --allow-unofficial-source  Developer escape hatch; public master never uses it

The low-level build script never installs or launches the game.
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

path_is_within() {
  local candidate="$1" parent="$2"
  case "$candidate/" in
    "$parent/"*) return 0 ;;
    *) return 1 ;;
  esac
}

paths_overlap() {
  path_is_within "$1" "$2" || path_is_within "$2" "$1"
}

canonicalize_directory_target() {
  local target="$1" current component resolved index
  local pending=()
  [ -n "$target" ] || die "directory path must not be empty"
  [[ "$target" != *$'\n'* && "$target" != *$'\r'* ]] || die "directory path must stay on one line"
  case "$target" in /*) current="$target" ;; *) current="$PWD/$target" ;; esac
  while [ ! -e "$current" ]; do
    component="$(basename "$current")"
    [ -n "$component" ] && [ "$component" != "/" ] || die "could not resolve directory target: $target"
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
      *) if [ "$resolved" = "/" ]; then resolved="/$component"; else resolved="$resolved/$component"; fi ;;
    esac
  done
  printf '%s\n' "$resolved"
}

canonical_existing_path() {
  local value="$1"
  [ -e "$value" ] || die "path does not exist: $value"
  if [ -d "$value" ]; then
    (cd "$value" && pwd -P)
  else
    [ ! -L "$value" ] || die "file input must not be a symlink: $value"
    absolute_existing "$value"
  fi
}

canonicalize_file_target() {
  local value="$1" parent base resolved_parent
  [ -n "$value" ] || die "file path must not be empty"
  [ ! -L "$value" ] || die "file target must not be a symlink: $value"
  parent="$(dirname "$value")"
  base="$(basename "$value")"
  resolved_parent="$(canonicalize_directory_target "$parent")"
  if [ "$resolved_parent" = "/" ]; then printf '/%s\n' "$base"; else printf '%s/%s\n' "$resolved_parent" "$base"; fi
}

reject_scratch_overlap() {
  local label="$1" candidate="$2" scratch
  for scratch in "$NATIVE_BUILD" "$LOADER_CLASSES" "$DEX_FILE"; do
    if paths_overlap "$candidate" "$scratch"; then
      die "$label must not overlap a build scratch path: $candidate"
    fi
  done
}

find_python() {
  local candidate
  if [ -n "$PYTHON_EXE" ]; then
    command -v "$PYTHON_EXE" 2>/dev/null || [ -x "$PYTHON_EXE" ] || return 1
    printf '%s\n' "$PYTHON_EXE"
    return 0
  fi
  for candidate in python3 python python.exe; do
    if command -v "$candidate" >/dev/null 2>&1 &&
       "$candidate" -c 'import sys; raise SystemExit(0 if sys.version_info >= (3, 10) else 1)' >/dev/null 2>&1; then
      command -v "$candidate"
      return 0
    fi
  done
  return 1
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

while [ "$#" -gt 0 ]; do
  case "$1" in
    --configuration|-Configuration) CONFIGURATION="${2:?missing configuration}"; shift 2 ;;
    --android-sdk|-AndroidSdk) ANDROID_SDK="${2:?missing Android SDK}"; shift 2 ;;
    --java-home|-JavaHome) JAVA_HOME_ARG="${2:?missing Java home}"; shift 2 ;;
    --openxr-loader|-OpenXrLoader) OPENXR_LOADER="${2:?missing OpenXR loader}"; shift 2 ;;
    --build-dir|--build-root|-BuildRoot) BUILD_ROOT="${2:?missing build directory}"; shift 2 ;;
    --python|--python-exe|-PythonExe) PYTHON_EXE="${2:?missing Python executable}"; shift 2 ;;
    --apktool|-Apktool) APKTOOL="${2:?missing apktool jar}"; shift 2 ;;
    --game-package|-GamePackage) GAME_PACKAGE="${2:?missing game package}"; shift 2 ;;
    --audio-source|-AudioSource) AUDIO_SOURCE="${2:?missing audio source}"; shift 2 ;;
    --keystore|-Keystore) KEYSTORE="${2:?missing keystore}"; shift 2 ;;
    --package|-Package) PACKAGE=1; shift ;;
    --allow-unofficial-source|-AllowUnofficialSource) ALLOW_UNOFFICIAL_SOURCE=1; shift ;;
    -h|--help) usage; exit 0 ;;
    Debug|RelWithDebInfo|Release)
      # Retain the original positional configuration form.
      CONFIGURATION="$1"
      shift
      ;;
    *) die "unknown argument: $1 (run --help)" ;;
  esac
done

case "$CONFIGURATION" in
  Debug|RelWithDebInfo|Release) ;;
  *) die "invalid configuration '$CONFIGURATION'" ;;
esac

[ -n "$ANDROID_SDK" ] || die "Android SDK path is required (--android-sdk or ANDROID_SDK_ROOT)"
[ -n "$JAVA_HOME_ARG" ] || die "JDK 21 path is required (--java-home or JAVA_HOME)"
[ -n "$OPENXR_LOADER" ] || die "OpenXR loader is required (--openxr-loader or SAVR_OPENXR_LOADER_SO)"
[ -d "$ANDROID_SDK" ] || die "Android SDK directory does not exist: $ANDROID_SDK"
[ -d "$JAVA_HOME_ARG" ] || die "JDK directory does not exist: $JAVA_HOME_ARG"
[ -f "$OPENXR_LOADER" ] || die "OpenXR loader does not exist: $OPENXR_LOADER"
[ ! -L "$OPENXR_LOADER" ] || die "OpenXR loader must not be a symlink: $OPENXR_LOADER"

ANDROID_SDK="$(cd "$ANDROID_SDK" && pwd -P)"
JAVA_HOME_ARG="$(cd "$JAVA_HOME_ARG" && pwd -P)"
OPENXR_LOADER="$(absolute_existing "$OPENXR_LOADER")"
BUILD_ROOT="$(canonicalize_directory_target "$BUILD_ROOT")"
[ "$BUILD_ROOT" != "/" ] || die "--build-dir must be a dedicated directory, not the filesystem root"
if path_is_within "$ROOT" "$BUILD_ROOT"; then
  die "--build-dir must not be the source-kit root or one of its ancestors: $BUILD_ROOT"
fi

NDK="$ANDROID_SDK/ndk/27.2.12479018"
CMAKE="$ANDROID_SDK/cmake/3.22.1/bin/cmake"
NINJA="$ANDROID_SDK/cmake/3.22.1/bin/ninja"
JAVAC="$JAVA_HOME_ARG/bin/javac"
BUILD_TOOLS="$ANDROID_SDK/build-tools/35.0.0"
D8="$BUILD_TOOLS/d8"
ANDROID_JAR="$ANDROID_SDK/platforms/android-35/android.jar"
NATIVE_BUILD="$BUILD_ROOT/native"
LOADER_CLASSES="$BUILD_ROOT/loader-classes"
DEX_FILE="$BUILD_ROOT/classes.dex"

for required in "$CMAKE" "$NINJA" "$JAVAC" "$D8" "$ANDROID_JAR" \
  "$NDK/build/cmake/android.toolchain.cmake" "$OPENXR_LOADER"; do
  [ -f "$required" ] || die "required build input not found: $required"
done

if [ "$PACKAGE" -eq 1 ]; then
  [ -n "$GAME_PACKAGE" ] || die "--game-package is required with --package"
  [ -n "$AUDIO_SOURCE" ] || die "--audio-source is required with --package"
  GAME_PACKAGE="$(canonical_existing_path "$GAME_PACKAGE")"
  AUDIO_SOURCE="$(canonical_existing_path "$AUDIO_SOURCE")"
  if paths_overlap "$GAME_PACKAGE" "$BUILD_ROOT"; then
    die "game package must stay outside the disposable build directory: $GAME_PACKAGE"
  fi
  if paths_overlap "$AUDIO_SOURCE" "$BUILD_ROOT"; then
    die "audio source must stay outside the disposable build directory: $AUDIO_SOURCE"
  fi
  PYTHON_EXE="$(find_python)" || die "Python 3.10+ is required for APK assembly"
  PYTHON_PATH="$(command -v "$PYTHON_EXE" 2>/dev/null || printf '%s\n' "$PYTHON_EXE")"
  [ ! -f "$PYTHON_PATH" ] || PYTHON_PATH="$(absolute_existing "$PYTHON_PATH")"
  if [ -z "$APKTOOL" ]; then APKTOOL="$ROOT/tools/vendor/apktool_3.0.3.jar"; fi
  [ -f "$APKTOOL" ] || die "apktool 3.0.3 jar not found: $APKTOOL"
  [ ! -L "$APKTOOL" ] || die "apktool input must not be a symlink: $APKTOOL"
  APKTOOL="$(absolute_existing "$APKTOOL")"
  if [ -z "$KEYSTORE" ]; then KEYSTORE="$BUILD_ROOT/signing/savr.keystore"; fi
  KEYSTORE="$(canonicalize_file_target "$KEYSTORE")"
fi

for protected in "$CMAKE" "$NINJA" "$JAVAC" "$D8" "$ANDROID_JAR" \
  "$NDK/build/cmake/android.toolchain.cmake" "$OPENXR_LOADER"; do
  reject_scratch_overlap "required build input" "$protected"
done
if [ "$PACKAGE" -eq 1 ]; then
  reject_scratch_overlap "Python" "$PYTHON_PATH"
  reject_scratch_overlap "Apktool" "$APKTOOL"
  reject_scratch_overlap "keystore" "$KEYSTORE"
fi

mkdir -p "$BUILD_ROOT"
BUILD_ROOT="$(cd "$BUILD_ROOT" && pwd -P)"
[ ! -L "$NATIVE_BUILD" ] || die "native build directory must not be a symlink: $NATIVE_BUILD"
[ ! -e "$NATIVE_BUILD" ] || [ -d "$NATIVE_BUILD" ] || die "native build path is not a directory: $NATIVE_BUILD"

EXPECTED_OPENXR_HASH="713E3BB8D955254C670ACC1C4899A65CB8C930E97DD9958BF37EA922D72B7A06"
OBSERVED_OPENXR_HASH="$(sha256_file "$OPENXR_LOADER")"
[ "$OBSERVED_OPENXR_HASH" = "$EXPECTED_OPENXR_HASH" ] ||
  die "OpenXR loader hash mismatch. Expected $EXPECTED_OPENXR_HASH, got $OBSERVED_OPENXR_HASH"

export JAVA_HOME="$JAVA_HOME_ARG"
export ANDROID_HOME="$ANDROID_SDK"
export ANDROID_SDK_ROOT="$ANDROID_SDK"

echo "==> libsavr.so ($CONFIGURATION, arm64-v8a)"
"$CMAKE" \
  -S "$ROOT/native" \
  -B "$NATIVE_BUILD" \
  -G Ninja \
  "-DCMAKE_MAKE_PROGRAM=$NINJA" \
  "-DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake" \
  "-DOPENXR_LOADER_SO=$OPENXR_LOADER" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-28 \
  "-DCMAKE_BUILD_TYPE=$CONFIGURATION"
"$CMAKE" --build "$NATIVE_BUILD"

echo "==> loader DEX"
rm -rf "$LOADER_CLASSES"
mkdir -p "$LOADER_CLASSES"
"$JAVAC" \
  -source 8 -target 8 -nowarn \
  -bootclasspath "$ANDROID_JAR" -classpath "$ANDROID_JAR" \
  -d "$LOADER_CLASSES" \
  "$ROOT/loader/src/com/savr/SavrApplication.java"

shopt -s nullglob
LOADER_CLASS_FILES=("$LOADER_CLASSES"/com/savr/*.class)
shopt -u nullglob
[ "${#LOADER_CLASS_FILES[@]}" -gt 0 ] || die "Java loader produced no class files"
rm -f "$DEX_FILE"
"$D8" --release --min-api 28 --lib "$ANDROID_JAR" \
  --output "$BUILD_ROOT" "${LOADER_CLASS_FILES[@]}"
[ -f "$DEX_FILE" ] || die "D8 completed but did not produce $DEX_FILE"

NATIVE_LIBRARY="$NATIVE_BUILD/libsavr.so"
[ -f "$NATIVE_LIBRARY" ] || die "native build did not produce $NATIVE_LIBRARY"
echo "libsavr.so : $NATIVE_LIBRARY"
echo "SHA256      : $(sha256_file "$NATIVE_LIBRARY")"
echo "loader DEX  : $DEX_FILE"
echo "SHA256      : $(sha256_file "$DEX_FILE")"

if [ "$PACKAGE" -eq 0 ]; then
  exit 0
fi

ASSEMBLE_ARGS=(
  "$ROOT/tools/assemble.py"
  --game-package "$GAME_PACKAGE"
  --audio-source "$AUDIO_SOURCE"
  --build-dir "$BUILD_ROOT"
  --sdk "$ANDROID_SDK"
  --java-home "$JAVA_HOME_ARG"
  --apktool "$APKTOOL"
  --native-lib "$NATIVE_LIBRARY"
  --loader-dex "$DEX_FILE"
  --openxr-loader "$OPENXR_LOADER"
  --keystore "$KEYSTORE"
)
if [ "$ALLOW_UNOFFICIAL_SOURCE" -eq 1 ]; then
  ASSEMBLE_ARGS+=(--allow-unofficial-source)
fi

"$PYTHON_EXE" "${ASSEMBLE_ARGS[@]}"
