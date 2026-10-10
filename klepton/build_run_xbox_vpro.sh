#!/bin/bash
# Build, install and launch the Xbox Cloud Gaming window on a physical Vision
# Pro. NOT a Klepton guest: the Quest "Xbox" app (com.xbox.pwa) carries no
# native code at all — it is Meta's PWA wrapper around xbox.com/play — so the
# faithful port is visionos/XboxPWA, the same shell built natively (one flat
# window, one WKWebView, the same START_URL). Controllers reach the page
# through WebKit's Gamepad API, as they do inside the Quest browser.
#
#   ./build_run_xbox_vpro.sh            # generate + build + install + launch
#   KLEPTON_TEAM=XXXXXXXXXX ...         # signing team override (else detected)
set -euo pipefail
cd "$(dirname "$0")/visionos/XboxPWA"

python3 gen_xbox_xcodeproj.py

DD=../build/dd-device-xbox
xcodebuild -project XboxPWA.xcodeproj -scheme XboxPWA -configuration Debug \
  -destination generic/platform=visionOS -derivedDataPath "$DD" \
  -allowProvisioningUpdates build 2>&1 | grep -E 'error:|Signing|\*\* BUILD' || true
APP="$DD/Build/Products/Debug-xros/XboxPWA.app"
[ -d "$APP" ] || { echo "!! build produced no app" >&2; exit 1; }

# Same device discovery as build_run_vpro.sh, VERBATIM — it keys on
# hardwareProperties.reality/platform, which is what devicectl actually emits
# (a deviceType guess found nothing).
find_device() {
  local json; json=$(mktemp)
  xcrun devicectl list devices --json-output "$json" >/dev/null 2>&1 || true
  python3 -c '
import json, sys
try:
    devs = json.load(open(sys.argv[1]))["result"]["devices"]
except Exception:
    sys.exit(0)
phys = [d for d in devs
        if d.get("hardwareProperties", {}).get("reality") == "physical"
        and d.get("hardwareProperties", {}).get("platform") == "visionOS"]
if not phys:
    sys.exit(0)
phys.sort(key=lambda d: d.get("connectionProperties", {}).get("tunnelState") != "connected")
print(phys[0]["identifier"])
' "$json"
  rm -f "$json"
}
DEVID="${KLEPTON_DEVICE:-$(find_device || true)}"
[ -n "$DEVID" ] || { echo "!! no physical Vision Pro — pair it in Xcode, or set KLEPTON_DEVICE" >&2; exit 1; }

xcrun devicectl device install app --device "$DEVID" "$APP" | tail -2
BUNDLE=$(python3 -c "
import re, os
scope = os.environ.get('KLEPTON_BUNDLE_SCOPE', os.environ.get('USER',''))
scope = re.sub(r'[^A-Za-z0-9-]','-',scope).strip('-').lower()
print((scope+'.' if scope else '')+'dev.klepton.target.xbox')")
xcrun devicectl device process launch --device "$DEVID" "$BUNDLE"
echo "Xbox window launched ($BUNDLE)"
