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
# Build under the final file name in a fixed staging directory: the linker
# stores the name in the export table and derives the image base from the
# full path, so a random path made every build differ. Identical bytes let the
# installer skip replacing a DLL that SteamVR already has loaded.
temporary_dir="$(dirname "$OUTPUT")/.build-$(basename "$OUTPUT" .dll)"
rm -rf "$temporary_dir"
mkdir -p "$temporary_dir"
temporary_output="$temporary_dir/$(basename "$OUTPUT")"
trap 'rm -rf "$temporary_dir"' EXIT

"$CXX" \
    -std=c++17 \
    -O2 \
    -shared \
    -static \
    -static-libgcc \
    -Wl,--no-insert-timestamp \
    -s \
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
rm -rf "$temporary_dir"
trap - EXIT
echo "Built SteamVR driver: $OUTPUT"
