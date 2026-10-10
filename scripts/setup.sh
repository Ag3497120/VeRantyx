#!/usr/bin/env bash
# One-command setup for VeRantyx on an Apple Silicon Mac.
#
#   ./scripts/setup.sh
#
# Installs the build tools, downloads the public Wine Staging build, DXVK and
# the Windows Steam installer, creates the Wine prefix, installs Steam, and
# builds every Mac and Windows component of this repository. Nothing that is
# not freely redistributable is downloaded by this script except through its
# publisher's own download link (Wine, DXVK, Steam). SteamVR and the game are
# installed through your own Steam account afterwards; see docs/SETUP.md.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
VERANTYX_VR_DRIVE="${VERANTYX_VR_DRIVE:-$HOME/Verantyx_VR_Drive}"
WINEPREFIX="${WINEPREFIX:-$VERANTYX_VR_DRIVE/SteamVR_Prefix_Wine11_Test}"
WINE_VERSION="${VERANTYX_WINE_VERSION:-11.18}"
WINE_URL="${VERANTYX_WINE_URL:-https://github.com/Gcenx/macOS_Wine_builds/releases/download/$WINE_VERSION/wine-staging-$WINE_VERSION-osx64.tar.xz}"
DXVK_RELEASE="${VERANTYX_DXVK_RELEASE:-v1.10.3-20230507-repack}"
# The release asset keeps the "v": dxvk-macOS-async-v1.10.3-20230507-repack.tar.gz
DXVK_NAME="dxvk-macOS-async-${DXVK_RELEASE}"
DXVK_URL="${VERANTYX_DXVK_URL:-https://github.com/Gcenx/DXVK-macOS/releases/download/$DXVK_RELEASE/$DXVK_NAME.tar.gz}"
STEAM_URL="${VERANTYX_STEAM_URL:-https://cdn.cloudflare.steamstatic.com/client/installer/SteamSetup.exe}"
WINE_APP="$VERANTYX_VR_DRIVE/Beer/wine/WineStaging.app"
WINE_BIN="$WINE_APP/Contents/Resources/wine/bin/wine"
DXVK_DIR="$VERANTYX_VR_DRIVE/Beer/dxvk/$DXVK_NAME"
STEAM_SETUP="$VERANTYX_VR_DRIVE/SteamSetup.exe"
SKIP_STEAM="${VERANTYX_SKIP_STEAM_INSTALL:-0}"

step() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

step "Checking the Mac"
if [[ "$(uname -m)" != "arm64" ]]; then
    echo "VeRantyx needs an Apple Silicon Mac (found $(uname -m))." >&2
    exit 1
fi
if ! sw_vers -productVersion | awk -F. '{ exit !($1 >= 14) }'; then
    echo "macOS 14 or later is required (found $(sw_vers -productVersion))." >&2
    exit 1
fi
if ! xcode-select -p >/dev/null 2>&1 || ! command -v xcodebuild >/dev/null 2>&1; then
    echo "Xcode is required (for swiftc and the visionOS app). Install it from the App Store, then rerun." >&2
    exit 1
fi
if ! command -v brew >/dev/null 2>&1; then
    echo "Homebrew is required: https://brew.sh" >&2
    exit 1
fi

step "Installing build tools (Homebrew)"
brew list --versions mingw-w64 >/dev/null 2>&1 || brew install mingw-w64
command -v python3 >/dev/null 2>&1 || brew install python
if ! command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
    echo "x86_64-w64-mingw32-g++ is not on PATH after installing mingw-w64." >&2
    exit 1
fi

mkdir -p "$VERANTYX_VR_DRIVE/Beer/wine" "$VERANTYX_VR_DRIVE/Beer/dxvk"

step "Wine Staging $WINE_VERSION (Gcenx macOS build)"
if [[ -x "$WINE_BIN" ]]; then
    echo "Already present: $WINE_APP"
else
    archive="$VERANTYX_VR_DRIVE/Beer/wine/wine-staging.tar.xz"
    curl -L --fail --progress-bar -o "$archive" "$WINE_URL"
    tar -xJf "$archive" -C "$VERANTYX_VR_DRIVE/Beer/wine"
    rm -f "$archive"
    extracted="$(find "$VERANTYX_VR_DRIVE/Beer/wine" -maxdepth 1 -name '*.app' | head -1)"
    [[ -n "$extracted" ]] || { echo "No .app in the Wine archive." >&2; exit 1; }
    [[ "$extracted" == "$WINE_APP" ]] || mv "$extracted" "$WINE_APP"
    xattr -dr com.apple.quarantine "$WINE_APP" 2>/dev/null || true
    echo "Installed $WINE_APP"
fi
"$WINE_BIN" --version

step "DXVK $DXVK_RELEASE (D3D11 for the SteamVR compositor)"
if [[ -f "$DXVK_DIR/x64/d3d11.dll" ]]; then
    echo "Already present: $DXVK_DIR"
else
    archive="$VERANTYX_VR_DRIVE/Beer/dxvk/dxvk.tar.gz"
    curl -L --fail --progress-bar -o "$archive" "$DXVK_URL"
    tar -xzf "$archive" -C "$VERANTYX_VR_DRIVE/Beer/dxvk"
    rm -f "$archive"
    [[ -f "$DXVK_DIR/x64/d3d11.dll" ]] || { echo "DXVK archive did not contain $DXVK_NAME/x64/d3d11.dll" >&2; exit 1; }
fi

step "Windows Steam installer"
if [[ -f "$STEAM_SETUP" ]]; then
    echo "Already present: $STEAM_SETUP"
else
    curl -L --fail --progress-bar -o "$STEAM_SETUP" "$STEAM_URL"
fi

step "Wine prefix: $WINEPREFIX"
export WINEPREFIX
if [[ -d "$WINEPREFIX/drive_c" ]]; then
    echo "Already present."
else
    WINEDEBUG=-all "$REPO_ROOT/VRDriver/wine_launcher.sh" wineboot --init >/dev/null 2>&1 || true
    [[ -d "$WINEPREFIX/drive_c" ]] || { echo "wineboot did not create the prefix." >&2; exit 1; }
    echo "Created."
fi

step "Building the Mac encoder and the Windows DLLs"
mkdir -p "$REPO_ROOT/build" "$REPO_ROOT/logs"
swiftc -O "$REPO_ROOT/MacEncoder/HardwareEncoder.swift" -o "$REPO_ROOT/build/HardwareEncoder"
"$REPO_ROOT/scripts/build_openvr_driver.sh"
"$REPO_ROOT/scripts/build_openvr_capture_proxy.sh"
"$REPO_ROOT/scripts/build_steamvr_d3d11_proxy.sh"
"$REPO_ROOT/scripts/build_steamwebhelper_shim.sh"

step "Registering the Verantyx SteamVR driver folder"
driver_dir="$WINEPREFIX/drive_c/vr_driver/verantyx"
mkdir -p "$driver_dir/bin/win64" "$driver_dir/resources"
cat >"$driver_dir/driver.vrdrivermanifest" <<'EOF'
{
	"name" : "verantyx",
	"version" : "1.0",
	"alwaysActivate" : true
}
EOF
cp "$REPO_ROOT/VRDriver/driver_verantyx.dll" "$driver_dir/bin/win64/driver_verantyx.dll"

if [[ "$SKIP_STEAM" != "1" ]] && [[ ! -f "$WINEPREFIX/drive_c/Program Files (x86)/Steam/steam.exe" ]]; then
    step "Installing Windows Steam (the installer window opens; keep the default path)"
    "$REPO_ROOT/scripts/install_windows_steam.sh" --silent || true
fi

cat <<EOF

Setup finished. Next:
  1. Sign in to Windows Steam, install SteamVR (App ID 250820) and the game
     (Half-Life: Alyx, App ID 546560) into this prefix:
       WINEPREFIX="$WINEPREFIX" $REPO_ROOT/VRDriver/wine_launcher.sh "$WINEPREFIX/drive_c/Program Files (x86)/Steam/steam.exe" -no-cef-sandbox -cef-single-process
  2. Build the Vision Pro app once in Xcode with your own team, or run
       ./scripts/deploy_vision_pro_app.sh
     with the headset paired to this Mac.
  3. Play:
       WINEPREFIX="$WINEPREFIX" ./scripts/launch_vision_pro_vr.sh
See docs/SETUP.md for details and troubleshooting.
EOF
