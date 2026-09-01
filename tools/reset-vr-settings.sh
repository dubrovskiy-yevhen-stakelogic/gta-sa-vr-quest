#!/usr/bin/env bash
set -euo pipefail

PACKAGE_NAME="com.rockstargames.gtasa"
REMOTE_FILES_ROOT="/sdcard/Android/data/${PACKAGE_NAME}/files"
WORK_DIR="${HOME}/SAVRBuild"
ADB_ARG=""
ANDROID_SDK_ARG=""
SERIAL=""
ASSUME_YES=0
NON_INTERACTIVE=0
ADB=""
ADB_BASE=()
SETTINGS_FILES=(
  vr_appearance.ini
  vr_basketball.ini
  vr_calib.ini
  vr_calib.ini.tmp
  vr_driving.ini
  vr_graphics.ini
  vr_holsters.ini
  vr_hud.ini
  vr_locomotion.ini
)

usage() {
  cat <<'EOF'
Usage: tools/reset-vr-settings.sh [options]

  --adb PATH           ADB executable
  --android-sdk DIR    Android SDK root containing platform-tools/adb
  --work-dir DIR       Builder workspace (default: ~/SAVRBuild)
  --serial SERIAL      Quest USB serial when more than one device is connected
  --yes                Remove the listed files without the RESET prompt
  --non-interactive    Never prompt; requires --yes
  -h, --help           Show this help

Only GTA San Andreas VR configuration files are removed. Saves, audio, game
data, hand assets, performance logs, and the APK are preserved. The game is
force-stopped and is never launched by this tool.
EOF
}

die() {
  echo "ERROR: $*" >&2
  exit 1
}

validate_adb() {
  local candidate="$1" output
  [ -x "$candidate" ] || return 1
  output="$("$candidate" version 2>&1)" || return 1
  printf '%s\n' "$output" | grep -q "Android Debug Bridge"
}

resolve_adb() {
  local candidate sdk host_tag
  if [ -n "$ADB_ARG" ]; then
    validate_adb "$ADB_ARG" || die "--adb is not a working ADB executable: $ADB_ARG"
    printf '%s\n' "$ADB_ARG"
    return
  fi

  for sdk in "$ANDROID_SDK_ARG" "${ANDROID_SDK_ROOT:-}" "${ANDROID_HOME:-}"; do
    [ -n "$sdk" ] || continue
    candidate="$sdk/platform-tools/adb"
    if validate_adb "$candidate"; then printf '%s\n' "$candidate"; return; fi
  done

  case "$(uname -s):$(uname -m)" in
    Linux:x86_64|Linux:amd64) host_tag="linux-x64" ;;
    Darwin:x86_64) host_tag="mac-x64" ;;
    Darwin:arm64|Darwin:aarch64) host_tag="mac-arm64" ;;
    *) host_tag="" ;;
  esac
  if [ -n "$host_tag" ]; then
    candidate="$WORK_DIR/.tools/platform-tools-37.0.1-${host_tag}/platform-tools/adb"
    if validate_adb "$candidate"; then printf '%s\n' "$candidate"; return; fi
  fi
  candidate="$WORK_DIR/.android-sdk/platform-tools/adb"
  if validate_adb "$candidate"; then printf '%s\n' "$candidate"; return; fi
  if command -v adb >/dev/null 2>&1; then
    candidate="$(command -v adb)"
    if validate_adb "$candidate"; then printf '%s\n' "$candidate"; return; fi
  fi
  die "ADB was not found. Run BUILD_AND_INSTALL once, install Android Platform-Tools, or pass --adb/--android-sdk/--work-dir."
}

adb_cmd() {
  [ "${#ADB_BASE[@]}" -gt 0 ] || die "no Quest has been selected"
  "${ADB_BASE[@]}" "$@"
}

select_quest() {
  local output line state detail manufacturer model identity choice index
  local ready=()
  "$ADB" start-server >/dev/null 2>&1 || true
  output="$("$ADB" devices -l 2>&1)" || die "adb devices failed: $output"

  if [ -n "$SERIAL" ]; then
    line="$(printf '%s\n' "$output" | awk -v serial="$SERIAL" '$1==serial {print; exit}')"
    [ -n "$line" ] || die "requested ADB device was not found: $SERIAL"
    state="$(printf '%s\n' "$line" | awk '{print $2}')"
    [ "$state" = "device" ] || die "requested ADB device is ${state}: $SERIAL"
  else
    while IFS= read -r line; do
      [ -n "$line" ] || continue
      state="$(printf '%s\n' "$line" | awk '{print $2}')"
      [ "$state" = "device" ] && ready+=("$(printf '%s\n' "$line" | awk '{print $1}')")
    done <<EOF
$(printf '%s\n' "$output" | tail -n +2)
EOF
    if [ "${#ready[@]}" -eq 0 ]; then
      printf '%s\n' "$output" | grep -q "unauthorized" &&
        die "Quest is connected but unauthorized. Put on the headset and allow USB debugging."
      printf '%s\n' "$output" | grep -q "offline" &&
        die "Quest is offline. Reconnect USB and restart ADB."
      printf '%s\n' "$output" | grep -q "no permissions" &&
        die "ADB cannot access the Quest. Install Android udev rules and re-login."
      die "no authorized Quest was found"
    elif [ "${#ready[@]}" -eq 1 ]; then
      SERIAL="${ready[0]}"
    elif [ "$NON_INTERACTIVE" -eq 1 ]; then
      die "more than one ADB device is connected; pass --serial with --non-interactive"
    else
      echo "Connected ADB devices:"
      index=1
      for line in "${ready[@]}"; do echo "  [$index] $line"; index=$((index + 1)); done
      printf 'Choose a device number: '
      IFS= read -r choice
      case "$choice" in ''|*[!0-9]*) die "invalid device selection" ;; esac
      [ "$choice" -ge 1 ] && [ "$choice" -le "${#ready[@]}" ] || die "invalid device selection"
      SERIAL="${ready[$((choice - 1))]}"
    fi
  fi

  ADB_BASE=("$ADB" -s "$SERIAL")
  manufacturer="$(adb_cmd shell getprop ro.product.manufacturer | tr -d '\r')" || die "could not read device manufacturer"
  model="$(adb_cmd shell getprop ro.product.model | tr -d '\r')" || die "could not read device model"
  identity="$(printf '%s %s' "$manufacturer" "$model" | tr '[:upper:]' '[:lower:]')"
  case "$identity" in *quest*|*oculus*|*meta*) ;; *) die "selected device does not identify itself as a Meta/Oculus Quest: $manufacturer $model" ;; esac
  echo "Selected Quest: $SERIAL ($manufacturer $model)"
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --adb) ADB_ARG="${2:?missing ADB path}"; shift 2 ;;
    --android-sdk) ANDROID_SDK_ARG="${2:?missing Android SDK path}"; shift 2 ;;
    --work-dir) WORK_DIR="${2:?missing work directory}"; shift 2 ;;
    --serial) SERIAL="${2:?missing Quest serial}"; shift 2 ;;
    --yes) ASSUME_YES=1; shift ;;
    --non-interactive) NON_INTERACTIVE=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
done

[ "$NON_INTERACTIVE" -eq 0 ] || [ "$ASSUME_YES" -eq 1 ] ||
  die "--non-interactive requires --yes before settings can be removed"

ADB="$(resolve_adb)"
echo "ADB: $ADB"
select_quest
package_output="$(adb_cmd shell pm path "$PACKAGE_NAME" 2>&1 || true)"
printf '%s\n' "$package_output" | grep -q '^package:' ||
  die "$PACKAGE_NAME is not installed on the selected Quest"

echo
echo "The following VR settings will be removed:"
for name in "${SETTINGS_FILES[@]}"; do echo "  $REMOTE_FILES_ROOT/$name"; done
echo
echo "Saves, audio, game data, vrhands, performance logs, and the APK will be preserved."
echo "The game will remain stopped. New defaults load the next time you start it."

if [ "$ASSUME_YES" -ne 1 ]; then
  printf 'Type RESET to continue: '
  IFS= read -r answer
  [ "$answer" = "RESET" ] || die "reset was cancelled; no settings were removed"
fi

adb_cmd shell am force-stop "$PACKAGE_NAME" >/dev/null || die "could not force-stop GTA San Andreas"
pid_output="$(adb_cmd shell pidof "$PACKAGE_NAME" 2>/dev/null || true)"
[ -z "$pid_output" ] || die "$PACKAGE_NAME is still running after force-stop: $pid_output"

removed=0
for name in "${SETTINGS_FILES[@]}"; do
  path="$REMOTE_FILES_ROOT/$name"
  if adb_cmd shell test -e "$path" >/dev/null 2>&1; then removed=$((removed + 1)); fi
  adb_cmd shell rm -f -- "$path" >/dev/null || die "could not remove $path"
done
for name in "${SETTINGS_FILES[@]}"; do
  path="$REMOTE_FILES_ROOT/$name"
  adb_cmd shell test ! -e "$path" >/dev/null 2>&1 ||
    die "reset verification failed; file still exists: $path"
done

echo
echo "VR settings reset verified: $removed existing file(s) removed."
echo "The game was not launched."
