#!/usr/bin/env bash
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
RESET_SCRIPT="$ROOT/tools/reset-vr-settings.sh"

if [ ! -f "$RESET_SCRIPT" ]; then
  echo "ERROR: The source kit is incomplete. Missing: $RESET_SCRIPT" >&2
  exit 1
fi

exec bash "$RESET_SCRIPT" "$@"
