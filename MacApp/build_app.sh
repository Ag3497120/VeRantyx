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
OUT="${VERANTYX_APP_BUILD_DIR:-$REPO/build/app}"
VERSION="$(tr -d '[:space:]' < "$MACAPP/VERSION")"
BUILD="$(date -u +%Y%m%d%H%M)"
# The commit the app is built from ("-modified": with changes not committed yet), in the app's Info.plist and the
# runtime's COMMIT file: the About card, problem reports and the launch log show it.
COMMIT="$(git -C "$REPO" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
[[ "$COMMIT" != unknown && -n "$(git -C "$REPO" status --porcelain --untracked-files=no 2>/dev/null)" ]] && COMMIT="$COMMIT-modified"
APP="$OUT/VeRantyx.app"
RT="$APP/Contents/Resources/runtime"
TARGET="arm64-apple-macos14.2"
IDENTITY="${VERANTYX_SIGN_IDENTITY:--}"
CARGO="$(command -v cargo || echo "$HOME/.cargo/bin/cargo")"
step() { printf '\n== %s\n' "$*"; }

step "Runtime: VXRuntime, the encoder, the helper, the SteamVR driver, the capture proxy, the ALVR bridge"
make -C "$REPO/VXRuntime" build/vxrt >/dev/null
mkdir -p "$OUT/obj"
swiftc -O -target "$TARGET" "$REPO/MacEncoder/HardwareEncoder.swift" -o "$OUT/obj/HardwareEncoder"
swiftc -O -target "$TARGET" "$REPO/scripts/vxrt_helper.swift" -o "$OUT/obj/vxrt-helper"
# Optional, local developer rebuild: reuse these unchanged components from an installed app.
# Prove their source revision matches first; never silently substitute old DLLs or an old ALVR ABI.
BASE_APP="${VERANTYX_BASE_APP:-}"
component_source_hash() {
    (cd "$REPO" && git ls-files -z --cached --others --exclude-standard -- VRDriver ALVRBridge scripts/build_openvr_driver.sh scripts/build_openvr_capture_proxy.sh |
        while IFS= read -r -d '' source; do shasum -a 256 "$source"; done) | shasum -a 256 | awk '{print $1}'
}
COMPONENT_SOURCE_HASH="$(component_source_hash)"
if [[ -z "$BASE_APP" ]] && ! command -v "${CXX:-x86_64-w64-mingw32-g++}" >/dev/null 2>&1; then
    if [[ -f "$RT/COMPONENTS.sha256" ]]; then BASE_APP="$APP"
    elif [[ -f /Applications/VeRantyx.app/Contents/Resources/runtime/COMMIT ]]; then BASE_APP=/Applications/VeRantyx.app
    fi
fi
if [[ -n "$BASE_APP" ]]; then
    BASE_RT="$BASE_APP/Contents/Resources/runtime"
    BASE_COMMIT="$(tr -d '[:space:]' < "$BASE_RT/COMMIT")"
    if [[ -f "$BASE_RT/COMPONENTS.sha256" ]]; then
        read -r marker base_sources < "$BASE_RT/COMPONENTS.sha256"
        [[ "$marker" == sources && "$base_sources" == "$COMPONENT_SOURCE_HASH" ]] || { echo "Base app's component sources differ; build from source without VERANTYX_BASE_APP" >&2; exit 1; }
        (cd "$BASE_RT" && tail -n +2 COMPONENTS.sha256 | shasum -a 256 --check --status) || { echo "Base app's component hashes differ" >&2; exit 1; }
    else
    [[ "$BASE_COMMIT" =~ ^[0-9a-f]{7,40}$ ]] || { echo "Base app needs component source hashes or a clean source revision" >&2; exit 1; }
    git -C "$REPO" cat-file -e "$BASE_COMMIT^{commit}"
    git -C "$REPO" diff --exit-code "$BASE_COMMIT" -- VRDriver ALVRBridge scripts/build_openvr_driver.sh scripts/build_openvr_capture_proxy.sh >/dev/null || {
        echo "Base app's driver / proxy / ALVR sources differ; build them from source without VERANTYX_BASE_APP" >&2; exit 1;
    }
    fi
    mkdir -p "$REPO/build" "$REPO/ALVRBridge/target/release"
    cp "$BASE_RT/VRDriver/driver_verantyx.dll" "$REPO/VRDriver/"
    cp "$BASE_RT/build/openvr_api_capture_proxy.dll" "$REPO/build/"
    cp "$BASE_RT/ALVRBridge/target/release/libverantyx_alvr_bridge.dylib" "$REPO/ALVRBridge/target/release/"
    BASE_LICENSES="$OUT/obj/base-licenses"
    mkdir -p "$BASE_LICENSES"
    cp "$BASE_APP/Contents/Resources/licenses/"{ALVR-LICENSE.txt,OpenVR-LICENSE.txt,ALVR-bridge-Rust-crates.txt} "$BASE_LICENSES/"
    echo "Reusing source-matched driver, capture proxy and ALVR from $BASE_COMMIT"
else
    "$REPO/scripts/build_openvr_driver.sh" >/dev/null
    "$REPO/scripts/build_openvr_capture_proxy.sh" >/dev/null
    (cd "$REPO/ALVRBridge" && "$CARGO" build --release --lib --locked)
fi
step "Native ARM64: Klepton, OpenVR, MoltenVK, the session helper (build dependencies only)"
bash "$REPO/NativeARM/scripts/build_native.sh" > "$OUT/obj/native-build.log" 2>&1 || { tail -40 "$OUT/obj/native-build.log" >&2; exit 1; }

step "App: $VERSION ($BUILD, commit $COMMIT)"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/licenses"
swiftc -O -swift-version 5 -parse-as-library -target "$TARGET" "$REPO/NativeARM/shared/NativeSupport.swift" "$MACAPP"/Sources/*.swift -o "$APP/Contents/MacOS/VeRantyx"
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
cp "$REPO"/scripts/{launch_vxrt_steamvr.sh,launch_vxrt_alyx.sh,launch_vxrt_game.sh,vxrt_paths.sh,launch_vxrt_windows_steam.sh,launch_vxrt_nms_vr.sh,vxrt_windows_steam_ask.sh} "$RT/scripts/"
cp "$REPO/VXRuntime/build/vxrt" "$RT/VXRuntime/build/"
cp "$OUT/obj/HardwareEncoder" "$OUT/obj/vxrt-helper" "$REPO/build/openvr_api_capture_proxy.dll" "$RT/build/"
cp "$REPO/VRDriver/driver_verantyx.dll" "$RT/VRDriver/"
cp "$REPO/ALVRBridge/target/release/libverantyx_alvr_bridge.dylib" "$RT/ALVRBridge/target/release/"
strip -x "$RT/ALVRBridge/target/release/libverantyx_alvr_bridge.dylib"
cp "$REPO"/games/*.json "$RT/games/"
echo "$COMMIT" > "$RT/COMMIT"
mkdir -p "$RT/NativeARM/MoltenVK"
cp "$REPO/NativeARM/build/host/alyx_host" "$REPO/NativeARM/build/native-session" "$REPO/NativeARM/dependencies.json" "$RT/NativeARM/"
cp "$REPO/NativeARM/build/Klepton/vendor-moltenvk/out/macos/libMoltenVK.dylib" "$RT/NativeARM/MoltenVK/"

# Licenses: VeRantyx's own, the notices, ALVR's and OpenVR's (from the checkout Cargo built), the Rust crates'.
L="$APP/Contents/Resources/licenses"
cp "$REPO/LICENSE" "$L/VeRantyx-LICENSE.txt"
cp "$REPO/THIRD_PARTY_NOTICES.md" "$L/THIRD_PARTY_NOTICES.md"
if [[ -n "$BASE_APP" ]]; then
    cp "$BASE_LICENSES/"{ALVR-LICENSE.txt,OpenVR-LICENSE.txt,ALVR-bridge-Rust-crates.txt} "$L/"
else
ALVR_CHECKOUT="$(ls -d "${CARGO_HOME:-$HOME/.cargo}"/git/checkouts/alvr-*/*/ 2>/dev/null | head -1)"
[[ -n "$ALVR_CHECKOUT" ]] || { echo "ALVR's checkout not found (cargo build ALVRBridge first)" >&2; exit 1; }
cp "$ALVR_CHECKOUT/LICENSE" "$L/ALVR-LICENSE.txt"
cp "$ALVR_CHECKOUT/openvr/LICENSE" "$L/OpenVR-LICENSE.txt"
python3 "$MACAPP/crate_licenses.py" "$L/ALVR-bridge-Rust-crates.txt"
fi
cp "$REPO/NativeARM/build/Klepton/LICENSE" "$L/Klepton-LICENSE.txt"
cp "$REPO/NativeARM/build/Klepton/vendor-moltenvk/dist/MoltenVK/LICENSE" "$L/MoltenVK-LICENSE.txt"
cp "$REPO/NativeARM/THIRD_PARTY.md" "$L/NativeARM-THIRD_PARTY.md"

step "Sign: $([[ "$IDENTITY" == - ]] && echo "ad hoc" || echo "$IDENTITY")"
RUNTIME_FLAGS=()
if [[ "$IDENTITY" != - ]]; then RUNTIME_FLAGS=(--options runtime --timestamp)
elif [[ "${VERANTYX_HARDENED:-0}" == 1 ]]; then RUNTIME_FLAGS=(--options runtime)
fi
sign() { codesign --force --sign "$IDENTITY" ${RUNTIME_FLAGS[@]+"${RUNTIME_FLAGS[@]}"} "$@"; }
sign "$RT/ALVRBridge/target/release/libverantyx_alvr_bridge.dylib"
sign "$RT/NativeARM/MoltenVK/libMoltenVK.dylib"
sign "$RT/NativeARM/native-session"
sign --entitlements "$MACAPP/entitlements/vxrt.plist" "$RT/NativeARM/alyx_host"
sign "$RT/build/vxrt-helper"
sign --entitlements "$MACAPP/entitlements/encoder.plist" "$RT/build/HardwareEncoder"
sign --entitlements "$MACAPP/entitlements/vxrt.plist" "$RT/VXRuntime/build/vxrt"
{
    printf 'sources %s\n' "$COMPONENT_SOURCE_HASH"
    (cd "$RT" && shasum -a 256 VRDriver/driver_verantyx.dll build/openvr_api_capture_proxy.dll ALVRBridge/target/release/libverantyx_alvr_bridge.dylib)
} > "$RT/COMPONENTS.sha256"
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
