#!/usr/bin/env bash
# Build-time dependencies only. No Steam, game downloads, package-manager installs or headset access.
set -euo pipefail
NATIVE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KL="$NATIVE/build/Klepton"
PIN=ce74582cb1e434db210014dbcd0ca0aad0e46ba2
mkdir -p "$NATIVE/build"
if [[ ! -d "$KL/.git" ]]; then git clone --quiet https://github.com/shinyquagsire23/Klepton.git "$KL"; fi
if [[ "$(git -C "$KL" rev-parse HEAD)" != "$PIN" ]]; then
    [[ -z "$(git -C "$KL" status --porcelain)" ]] || { echo "Klepton has local edits at another revision; preserve them before changing the pin" >&2; exit 1; }
    git -C "$KL" fetch --quiet origin "$PIN"
    git -C "$KL" checkout --quiet "$PIN"
fi
python3 "$NATIVE/klepton-patches/apply.py" "$KL"
bash "$KL/tools/mvk_fetch.sh"
# The original macOS slice already targets macOS. No visionOS SDK or retargeting step is needed.
MVK="$KL/vendor-moltenvk"
mkdir -p "$MVK/out/macos"
if [[ ! -d "$MVK/out/include" ]]; then cp -R "$MVK/dist/MoltenVK/MoltenVK/include" "$MVK/out/include"; fi
cp "$MVK/dist/MoltenVK/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib" "$MVK/out/macos/"
make -C "$NATIVE/host" -j "$(sysctl -n hw.logicalcpu)"
swiftc -O -swift-version 5 -target arm64-apple-macos14.2 "$NATIVE/shared/NativeSupport.swift" "$NATIVE/session/main.swift" -o "$NATIVE/build/native-session"
echo "Native components built (no game or encoder started)"
