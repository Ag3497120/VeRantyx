#!/usr/bin/env bash
# Synthetic C++ IPC / input / Vulkan tests only. All temporary files stay in the repository.
set -euo pipefail
NATIVE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST="${1:-$NATIVE/build/tests/runtime_test}"
TMP="$(mktemp -d "$NATIVE/build/tests/runtime-XXXXXX")"
trap 'rm -rf "$TMP"' EXIT
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ASAN_OPTIONS=halt_on_error=1 TSAN_OPTIONS=halt_on_error=1
printf '%s\n' '-1.2 0.8 -1 1 -0.8 1.2 -1 1' > "$TMP/vr_verantyx_tangents.txt"
"$TEST" "$TMP/frame.dat" "$NATIVE/tests/fixtures/actions.json"
mkdir -p "$TMP/prefix/drive_c"
"$TEST" --without-shared "$TMP/prefix"
[[ -z "$(ls -A "$TMP/prefix/drive_c")" ]]
