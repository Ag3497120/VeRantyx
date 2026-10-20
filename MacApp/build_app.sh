#!/usr/bin/env bash
# Builds the VeRantyx app for Mac: VeRantyx.app with the runtime inside (VXRuntime, the encoder, the SteamVR driver and
# capture proxy, ALVR's streaming core, the launch scripts), so that a player's Mac needs nothing but macOS and Steam,
# and a disk image to hand out.
#
#   MacApp/build_app.sh                    # build/app/VeRantyx.app and build/app/VeRantyx-<version>.dmg
#   VERANTYX_SIGN_IDENTITY="Developer ID Application: <name> (<team>)" VERANTYX_NOTARY_PROFILE=<profile> MacApp/build_app.sh
#
# Builds with what the repository's setup uses (Xcode or its command line tools, mingw-w64, Rust). Signing: with a
# Developer ID identity everything is signed with the hardened runtime (MacApp/entitlements) and, given a notarytool
# keychain profile (made once by the publisher: xcrun notarytool store-credentials), notarized and stapled. Without
# one the app is signed ad hoc: it runs on this Mac, and other Macs ask once to allow it (System Settings > Privacy &
# Security > Open Anyway). VERANTYX_HARDENED=1 uses the hardened runtime with ad-hoc signing too (to try it).
set -euo pipefail

MACAPP="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$MACAPP/.." && pwd)"
OUT="$REPO/build/app"
VERSION="$(tr -d '[:space:]' < "$MACAPP/VERSION")"
BUILD="$(date -u +%Y%m%d%H%M)"
# The commit the app is built from ("-modified": with changes not committed yet), in the app's Info.plist and the
# runtime's COMMIT file: the About card, problem reports and the launch log show it.
COMMIT="$(git -C "$REPO" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
[[ "$COMMIT" != unknown && -n "$(git -C "$REPO" status --porcelain --untracked-files=no 2>/dev/null)" ]] && COMMIT="$COMMIT-modified"
APP="$OUT/VeRantyx.app"
RT="$APP/Contents/Resources/runtime"
TARGET="arm64-apple-macos14.0"
IDENTITY="${VERANTYX_SIGN_IDENTITY:--}"
CARGO="$(command -v cargo || echo "$HOME/.cargo/bin/cargo")"
step() { printf '\n== %s\n' "$*"; }

step "Runtime: VXRuntime, the encoder, the helper, the SteamVR driver, the capture proxy, the ALVR bridge"
make -C "$REPO/VXRuntime" build/vxrt >/dev/null
mkdir -p "$OUT/obj"
swiftc -O -target "$TARGET" "$REPO/MacEncoder/HardwareEncoder.swift" -o "$OUT/obj/HardwareEncoder"
swiftc -O -target "$TARGET" "$REPO/scripts/vxrt_helper.swift" -o "$OUT/obj/vxrt-helper"
"$REPO/scripts/build_openvr_driver.sh" >/dev/null
"$REPO/scripts/build_openvr_capture_proxy.sh" >/dev/null
(cd "$REPO/ALVRBridge" && "$CARGO" build --release --lib --locked)

step "App: $VERSION ($BUILD, commit $COMMIT)"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/licenses"
swiftc -O -swift-version 5 -parse-as-library -target "$TARGET" "$MACAPP"/Sources/*.swift -o "$APP/Contents/MacOS/VeRantyx"
sed -e "s/@VERSION@/$VERSION/" -e "s/@BUILD@/$BUILD/" -e "s/@COMMIT@/$COMMIT/" "$MACAPP/Info.plist" > "$APP/Contents/Info.plist"
rm -rf "$OUT/AppIcon.iconset"
swift "$MACAPP/make_icon.swift" "$OUT/AppIcon.iconset"
iconutil -c icns "$OUT/AppIcon.iconset" -o "$APP/Contents/Resources/AppIcon.icns"
# The translations beyond English and Japanese (MacApp/Localizations/<code>.json; check_localizations.py checks them).
python3 "$MACAPP/check_localizations.py" >/dev/null || echo "warning: translations incomplete (python3 MacApp/check_localizations.py)" >&2
mkdir -p "$APP/Contents/Resources/Localizations"
cp "$MACAPP"/Localizations/*.json "$APP/Contents/Resources/Localizations/"

# The runtime, laid out as in the repository: the scripts find everything where they look (REPO_ROOT is runtime/).
mkdir -p "$RT/scripts" "$RT/VXRuntime/build" "$RT/build" "$RT/VRDriver" "$RT/ALVRBridge/target/release" "$RT/games"
cp "$REPO"/scripts/{launch_vxrt_steamvr.sh,launch_vxrt_alyx.sh,launch_vxrt_game.sh,vxrt_paths.sh} "$RT/scripts/"
cp "$REPO/VXRuntime/build/vxrt" "$RT/VXRuntime/build/"
cp "$OUT/obj/HardwareEncoder" "$OUT/obj/vxrt-helper" "$REPO/build/openvr_api_capture_proxy.dll" "$RT/build/"
cp "$REPO/VRDriver/driver_verantyx.dll" "$RT/VRDriver/"
cp "$REPO/ALVRBridge/target/release/libverantyx_alvr_bridge.dylib" "$RT/ALVRBridge/target/release/"
strip -x "$RT/ALVRBridge/target/release/libverantyx_alvr_bridge.dylib"
cp "$REPO"/games/*.json "$RT/games/"
echo "$COMMIT" > "$RT/COMMIT"

# Licenses: VeRantyx's own, the notices, ALVR's and OpenVR's (from the checkout Cargo built), the Rust crates'.
L="$APP/Contents/Resources/licenses"
cp "$REPO/LICENSE" "$L/VeRantyx-LICENSE.txt"
cp "$REPO/THIRD_PARTY_NOTICES.md" "$L/THIRD_PARTY_NOTICES.md"
ALVR_CHECKOUT="$(ls -d "${CARGO_HOME:-$HOME/.cargo}"/git/checkouts/alvr-*/*/ 2>/dev/null | head -1)"
[[ -n "$ALVR_CHECKOUT" ]] || { echo "ALVR's checkout not found (cargo build ALVRBridge first)" >&2; exit 1; }
cp "$ALVR_CHECKOUT/LICENSE" "$L/ALVR-LICENSE.txt"
cp "$ALVR_CHECKOUT/openvr/LICENSE" "$L/OpenVR-LICENSE.txt"
python3 "$MACAPP/crate_licenses.py" "$L/ALVR-bridge-Rust-crates.txt"

step "Sign: $([[ "$IDENTITY" == - ]] && echo "ad hoc" || echo "$IDENTITY")"
RUNTIME_FLAGS=()
if [[ "$IDENTITY" != - ]]; then RUNTIME_FLAGS=(--options runtime --timestamp)
elif [[ "${VERANTYX_HARDENED:-0}" == 1 ]]; then RUNTIME_FLAGS=(--options runtime)
fi
sign() { codesign --force --sign "$IDENTITY" ${RUNTIME_FLAGS[@]+"${RUNTIME_FLAGS[@]}"} "$@"; }
sign "$RT/ALVRBridge/target/release/libverantyx_alvr_bridge.dylib"
sign "$RT/build/vxrt-helper"
sign --entitlements "$MACAPP/entitlements/encoder.plist" "$RT/build/HardwareEncoder"
sign --entitlements "$MACAPP/entitlements/vxrt.plist" "$RT/VXRuntime/build/vxrt"
sign "$APP"
codesign --verify --deep --strict "$APP"

step "Disk image"
DMG="$OUT/VeRantyx-$VERSION.dmg"
STAGE="$OUT/dmg"
rm -rf "$STAGE" "$DMG"
mkdir -p "$STAGE"
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
hdiutil create -quiet -volname "VeRantyx $VERSION" -srcfolder "$STAGE" -fs HFS+ -format UDZO -ov "$DMG"
rm -rf "$STAGE"
[[ "$IDENTITY" != - ]] && codesign --force --sign "$IDENTITY" --timestamp "$DMG"
if [[ -n "${VERANTYX_NOTARY_PROFILE:-}" ]]; then
    [[ "$IDENTITY" != - ]] || { echo "notarization needs a Developer ID identity (VERANTYX_SIGN_IDENTITY)" >&2; exit 1; }
    step "Notarize"
    xcrun notarytool submit "$DMG" --keychain-profile "$VERANTYX_NOTARY_PROFILE" --wait
    xcrun stapler staple "$DMG"
fi
du -sh "$APP" "$DMG"
echo "Done: $APP and $DMG"
