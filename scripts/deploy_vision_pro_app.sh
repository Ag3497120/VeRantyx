#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PROJECT="$REPO_ROOT/VisionProClient/VisionSpatialTools/VisionSpatialTools.xcodeproj"
SCHEME="${SCHEME:-VisionSpatialTools}"
BUNDLE_ID="${BUNDLE_ID:-marimokodai.VeRantyx.vision}"
# First paired Apple Vision Pro unless DEVICE_ID is given.
DEVICE_ID="${DEVICE_ID:-$(xcrun devicectl list devices 2>/dev/null | grep -i 'Vision Pro' | grep -oE '[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12}' | head -1)}"
if [[ -z "$DEVICE_ID" ]]; then
    echo "No paired Apple Vision Pro found. Pair the headset with this Mac (Settings > General > Remote Devices) or set DEVICE_ID." >&2
    exit 1
fi
DERIVED_DATA_PATH="${DERIVED_DATA_PATH:-$REPO_ROOT/build/visionos-device}"
LAUNCH_TIMEOUT="${LAUNCH_TIMEOUT:-45}"
APP_PATH="$DERIVED_DATA_PATH/Build/Products/Debug-xros/VisionSpatialTools.app"

if ! xcrun devicectl list devices | grep -Fq "$DEVICE_ID"; then
    echo "Vision Pro is not listed by devicectl: $DEVICE_ID" >&2
    echo "Connect and unlock the paired headset, then retry." >&2
    exit 1
fi

xcodebuild \
    -quiet \
    -project "$PROJECT" \
    -scheme "$SCHEME" \
    -configuration Debug \
    -destination 'generic/platform=visionOS' \
    -derivedDataPath "$DERIVED_DATA_PATH" \
    -allowProvisioningUpdates \
    build

if [[ ! -d "$APP_PATH" ]]; then
    echo "visionOS app build product was not created: $APP_PATH" >&2
    exit 1
fi

xcrun devicectl device install app --timeout 60 --device "$DEVICE_ID" "$APP_PATH"
echo "Launching the Vision Pro app. Unlock the headset and accept its first-run prompts if they appear."
if ! xcrun devicectl device process launch --timeout "$LAUNCH_TIMEOUT" --device "$DEVICE_ID" "$BUNDLE_ID"; then
    echo "The app is installed, but remote launch did not complete. Open VeRantyx on Vision Pro and accept its first-run prompts."
    exit 0
fi
echo "Installed and launched $BUNDLE_ID on Vision Pro ($DEVICE_ID)."
