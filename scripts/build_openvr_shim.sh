#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CXX="${CXX:-x86_64-w64-mingw32-g++}"
OUTPUT_DLL="${OUTPUT_DLL:-$REPO_ROOT/build/openvr_api.dll}"

if ! command -v "$CXX" >/dev/null 2>&1; then
    echo "Windows x86_64 MinGW compiler not found: $CXX" >&2
    echo "Install MinGW-w64 or set CXX to x86_64-w64-mingw32-g++." >&2
    exit 1
fi

mkdir -p "$(dirname "$OUTPUT_DLL")"
"$CXX" \
    -std=c++17 \
    -O2 \
    -shared \
    -static \
    -static-libgcc \
    -static-libstdc++ \
    -I"$REPO_ROOT/VRDriver/include" \
    "$REPO_ROOT/VRDriver/src/openvr_emulator.cpp" \
    -ld3d11 \
    -ldxgi \
    -o "$OUTPUT_DLL"

echo "Built Windows x86_64 OpenVR shim: $OUTPUT_DLL"
