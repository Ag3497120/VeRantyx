#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"
SOURCE="$REPO_ROOT/VRDriver/src/steamwebhelper_shim.c"
OUTPUT="${STEAMWEBHELPER_SHIM:-$BUILD_DIR/steamwebhelper_shim.exe}"
MINGW_CC="${MINGW_CC:-x86_64-w64-mingw32-gcc}"

if ! command -v "$MINGW_CC" >/dev/null 2>&1; then
    echo "MinGW-w64 compiler not found: $MINGW_CC" >&2
    exit 1
fi

mkdir -p "$(dirname "$OUTPUT")"
"$MINGW_CC" -municode -O2 -Wall -Wextra -static -mwindows \
    -o "$OUTPUT" "$SOURCE" -lshell32

if ! file -b "$OUTPUT" | rg -q '^PE32\+ executable.*x86-64'; then
    echo "The WebHelper shim is not a Windows x86-64 executable: $OUTPUT" >&2
    exit 1
fi
echo "Built Windows Steam WebHelper shim: $OUTPUT"
