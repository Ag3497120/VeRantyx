#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CXX="${CXX:-x86_64-w64-mingw32-g++}"
OUTPUT_DLL="${OUTPUT_DLL:-$REPO_ROOT/build/openvr_api_capture_proxy.dll}"

if ! command -v "$CXX" >/dev/null 2>&1; then
    echo "Windows x86_64 MinGW compiler not found: $CXX" >&2
    exit 1
fi

mkdir -p "$(dirname "$OUTPUT_DLL")"
"$CXX" \
    -std=c++17 \
    -O2 \
    -shared \
    -static \
    -static-libgcc \
    -Wl,--no-insert-timestamp \
    -s \
    -static-libstdc++ \
    -I"$REPO_ROOT/VRDriver/include" \
    "$REPO_ROOT/VRDriver/src/openvr_capture_proxy.cpp" \
    "$REPO_ROOT/VRDriver/openvr_capture_proxy.def" \
    -ld3d11 \
    -ldxgi \
    -lopengl32 \
    -o "$OUTPUT_DLL"

if ! file "$OUTPUT_DLL" | grep -Eq 'PE32\+ executable.*x86-64'; then
    echo "Built proxy is not a Windows x86_64 DLL: $OUTPUT_DLL" >&2
    exit 1
fi
echo "Built Windows x86_64 OpenVR capture proxy: $OUTPUT_DLL"
