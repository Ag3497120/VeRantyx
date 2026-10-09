#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUTPUT="${OUTPUT:-$REPO_ROOT/VRDriver/driver_verantyx.dll}"
CXX="${CXX:-x86_64-w64-mingw32-g++}"

if ! command -v "$CXX" >/dev/null 2>&1; then
    echo "Windows x64 MinGW compiler not found: $CXX" >&2
    echo "Set CXX to an x86_64-w64-mingw32-g++ compiler." >&2
    exit 1
fi

mkdir -p "$(dirname "$OUTPUT")"
temporary_output="${OUTPUT}.tmp.$$"
trap 'rm -f "$temporary_output"' EXIT

"$CXX" \
    -std=c++17 \
    -O2 \
    -shared \
    -static \
    -static-libgcc \
    -static-libstdc++ \
    -I "$REPO_ROOT/VRDriver/include" \
    "$REPO_ROOT/VRDriver/src/driver_verantyx.cpp" \
    -ld3d11 \
    -ldxgi \
    -o "$temporary_output"

if ! file "$temporary_output" | grep -q 'PE32+ executable.*x86-64'; then
    echo "Compiler output is not a 64-bit Windows DLL: $temporary_output" >&2
    exit 1
fi

mv "$temporary_output" "$OUTPUT"
trap - EXIT
echo "Built SteamVR driver: $OUTPUT"
