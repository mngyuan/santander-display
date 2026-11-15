#!/usr/bin/env bash
# Builds docks_config.h from docks.json, then compiles and (with a port) uploads.
#   ./tools/flash.sh                          # compile only
#   ./tools/flash.sh /dev/cu.wchusbserial10   # compile and upload
# Override the board with FQBN=... if needed. MOCK=1 builds with random dock
# numbers instead of fetching from TfL.
set -euo pipefail
cd "$(dirname "$0")/.."
FQBN="${FQBN:-esp32:esp32:esp32s3:FlashSize=8M}"
SKETCH=firmware/dock_display
EXTRA=()
if [ "${MOCK:-0}" = 1 ]; then EXTRA=(--build-property "compiler.cpp.extra_flags=-DMOCK_DATA"); fi
python3 tools/docks_json.py
arduino-cli compile -b "$FQBN" ${EXTRA[@]+"${EXTRA[@]}"} "$SKETCH"
if [ $# -ge 1 ]; then
  arduino-cli upload -b "$FQBN" -p "$1" "$SKETCH"
fi
