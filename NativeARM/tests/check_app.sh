#!/usr/bin/env bash
set -euo pipefail
NATIVE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCES=()
for source in "$NATIVE/../MacApp/Sources/"*.swift; do
    [[ "$(basename "$source")" == App.swift ]] || SOURCES+=("$source")
done
swiftc -swift-version 5 -parse-as-library "$NATIVE/shared/NativeSupport.swift" "${SOURCES[@]}" "$NATIVE/tests/AppModelTests.swift" -o "$NATIVE/build/tests/app_model_test"
"$NATIVE/build/tests/app_model_test" "$NATIVE/build/tests"
