#!/usr/bin/env bash
# Builds the VeRantyx Vision Pro app for TestFlight.
#
#   scripts/testflight_visionos.sh            # archive only (build/testflight/VeRantyx.xcarchive)
#   scripts/testflight_visionos.sh --export   # + a signed .ipa for App Store Connect in build/testflight/export
#   scripts/testflight_visionos.sh --upload   # + upload that build to App Store Connect (TestFlight)
#
# Before the first --export / --upload (once, by the account holder):
#   1. App Store Connect > Apps > "+" > New App: platform visionOS, bundle ID
#      marimokodai.VeRantyx.vision (VERANTYX_BUNDLE_ID to change it), SKU of your choice.
#   2. Xcode > Settings > Accounts: the Apple ID of team 7YNW843VY6 is signed in.
#      The first export creates an Apple Distribution certificate in that account.
#
# Every run gets a new build number (UTC yyyymmddHHMM), which App Store Connect requires;
# VERANTYX_BUILD sets one by hand (for example 1 for the first upload of a version).
# VERANTYX_TEAM overrides the team, VERANTYX_VERSION the version shown in TestFlight.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PROJECT="$REPO_ROOT/VisionProClient/VisionSpatialTools/VisionSpatialTools.xcodeproj"
OUT="$REPO_ROOT/build/testflight"
ARCHIVE="$OUT/VeRantyx.xcarchive"
TEAM="${VERANTYX_TEAM:-7YNW843VY6}"
BUNDLE_ID="${VERANTYX_BUNDLE_ID:-marimokodai.VeRantyx.vision}"
VERSION="${VERANTYX_VERSION:-0.3.0}"
BUILD_NUMBER="${VERANTYX_BUILD:-$(date -u +%Y%m%d%H%M)}"
MODE="${1:-}"

case "$MODE" in
    ""|--export|--upload) ;;
    *) echo "Usage: $0 [--export|--upload]" >&2; exit 1 ;;
esac

mkdir -p "$OUT"
rm -rf "$ARCHIVE"
echo "Archiving VeRantyx $VERSION ($BUILD_NUMBER) for team $TEAM, bundle $BUNDLE_ID..."
xcodebuild archive \
    -project "$PROJECT" -scheme VisionSpatialTools -configuration Release \
    -destination 'generic/platform=visionOS' -archivePath "$ARCHIVE" \
    -allowProvisioningUpdates \
    DEVELOPMENT_TEAM="$TEAM" PRODUCT_BUNDLE_IDENTIFIER="$BUNDLE_ID" \
    MARKETING_VERSION="$VERSION" CURRENT_PROJECT_VERSION="$BUILD_NUMBER" \
    | grep -E "error:|warning: .*(Info.plist|icon|Icon|privacy)|ARCHIVE (SUCCEEDED|FAILED)" || true
[[ -d "$ARCHIVE" ]] || { echo "The archive was not created; run without the grep filter in Xcode to see why." >&2; exit 1; }
echo "Archive: $ARCHIVE"
[[ -n "$MODE" ]] || exit 0

DESTINATION=export
[[ "$MODE" == "--upload" ]] && DESTINATION=upload
OPTIONS="$OUT/ExportOptions.plist"
cat >"$OPTIONS" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>method</key><string>app-store-connect</string>
	<key>destination</key><string>$DESTINATION</string>
	<key>teamID</key><string>$TEAM</string>
	<key>signingStyle</key><string>automatic</string>
	<key>uploadSymbols</key><true/>
	<key>manageAppVersionAndBuildNumber</key><false/>
</dict>
</plist>
PLIST
rm -rf "$OUT/export"
xcodebuild -exportArchive -archivePath "$ARCHIVE" -exportOptionsPlist "$OPTIONS" \
    -exportPath "$OUT/export" -allowProvisioningUpdates
if [[ "$DESTINATION" == upload ]]; then
    echo "Uploaded build $BUILD_NUMBER. It appears in App Store Connect > TestFlight after processing (about 10-30 minutes)."
else
    echo "Exported: $OUT/export (upload it with the Transporter app, or rerun with --upload)."
fi
