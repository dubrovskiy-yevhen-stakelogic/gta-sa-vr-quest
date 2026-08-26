#!/usr/bin/env bash
# Build the VR layer: libsavr.so (arm64-v8a) and the loader dex.
#   usage: tools/build.sh [Debug|RelWithDebInfo]
set -euo pipefail

CONFIG="${1:-RelWithDebInfo}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLCHAIN="${ANDROID_TOOLCHAIN:-/c/Dev/android-toolchain}"

SDK="$TOOLCHAIN/sdk"
NDK="$SDK/ndk/27.2.12479018"
CMAKE="$SDK/cmake/3.22.1/bin/cmake.exe"
NINJA="$SDK/cmake/3.22.1/bin/ninja.exe"
JAVA_HOME="${JAVA_HOME:-$TOOLCHAIN/jdk21}"
BUILD_TOOLS="$SDK/build-tools/35.0.0"
ANDROID_JAR="$SDK/platforms/android-35/android.jar"

OUT="$ROOT/build"
mkdir -p "$OUT"

echo "==> libsavr.so ($CONFIG, arm64-v8a)"
"$CMAKE" -S "$ROOT/native" -B "$OUT/native" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$NINJA" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-28 \
    -DCMAKE_BUILD_TYPE="$CONFIG" >/dev/null
"$CMAKE" --build "$OUT/native"

echo "==> loader dex"
rm -rf "$OUT/loader-classes" && mkdir -p "$OUT/loader-classes"
"$JAVA_HOME/bin/javac" -source 8 -target 8 -nowarn \
    -bootclasspath "$ANDROID_JAR" -classpath "$ANDROID_JAR" \
    -d "$OUT/loader-classes" \
    "$ROOT/loader/src/com/savr/SavrApplication.java"
"$BUILD_TOOLS/d8.bat" --release --min-api 28 --lib "$ANDROID_JAR" \
    --output "$OUT" \
    "$OUT"/loader-classes/com/savr/*.class >/dev/null

echo
echo "libsavr.so : $OUT/native/libsavr.so"
echo "loader dex : $OUT/classes.dex  (goes into the APK as classes7.dex)"
