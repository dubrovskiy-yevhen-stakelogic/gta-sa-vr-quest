#!/usr/bin/env bash
# Supported Linux/macOS entry point. The internal script owns all prompting,
# logging, tool bootstrap, build verification and optional Quest installation.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
MASTER="$SCRIPT_DIR/tools/build-and-install.sh"

if [ ! -f "$MASTER" ]; then
  echo "ERROR: the source kit is incomplete; missing $MASTER" >&2
  exit 1
fi

exec bash "$MASTER" "$@"
