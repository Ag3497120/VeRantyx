#!/bin/bash
# Build RemoteSpatialProbe.app (does not launch it).
#   scripts/build.sh            ad-hoc signed (default)
#   SIGN=dev scripts/build.sh   signed with the local "Apple Development" identity
#                               (TEAM=<team id> to set DEVELOPMENT_TEAM as well)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
mkdir -p "$ROOT/build"
xcodegen generate --spec "$ROOT/project.yml" --project "$ROOT" >/dev/null
SIGN_ARGS=()
if [[ "${SIGN:-adhoc}" == "dev" ]]; then
  SIGN_ARGS=(CODE_SIGN_IDENTITY="Apple Development" CODE_SIGN_STYLE=Manual)
  [[ -n "${TEAM:-}" ]] && SIGN_ARGS+=(DEVELOPMENT_TEAM="$TEAM")
fi
xcodebuild -project "$ROOT/RemoteSpatialProbe.xcodeproj" -scheme RemoteSpatialProbe \
  -configuration "${CONFIG:-Release}" -destination 'platform=macOS,arch=arm64' \
  -derivedDataPath "$ROOT/build/DerivedData" ${SIGN_ARGS[@]+"${SIGN_ARGS[@]}"} build \
  > "$ROOT/build/xcodebuild.log" 2>&1 && STATUS=0 || STATUS=$?
grep -E "^/.*: (error|warning):|\*\* BUILD (SUCCEEDED|FAILED)" "$ROOT/build/xcodebuild.log" | sort -u || true
APP="$ROOT/build/DerivedData/Build/Products/${CONFIG:-Release}/RemoteSpatialProbe.app"
if [[ $STATUS -eq 0 ]]; then echo "app: $APP"; else echo "build failed (full log: build/xcodebuild.log)"; exit $STATUS; fi
