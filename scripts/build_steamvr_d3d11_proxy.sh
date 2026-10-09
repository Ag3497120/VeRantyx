#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUTPUT="${OUTPUT:-$REPO_ROOT/build/steamvr-d3d11-proxy/d3d11.dll}"
CXX="${CXX:-x86_64-w64-mingw32-g++}"

if ! command -v "$CXX" >/dev/null 2>&1; then
    echo "Windows x64 MinGW compiler not found: $CXX" >&2
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
    "$REPO_ROOT/VRDriver/src/d3d11_proxy_exports.cpp" \
    -ld3d11 \
    -ldxgi \
    -o "$temporary_output"

if ! file "$temporary_output" | rg -q 'PE32\+ executable.*x86-64'; then
    echo "Compiler output is not a Windows x64 DLL: $temporary_output" >&2
    exit 1
fi

mv "$temporary_output" "$OUTPUT"
trap - EXIT
echo "Built SteamVR D3D11 proxy: $OUTPUT"
