#!/usr/bin/env bash
# Setup for the vxrt version: Windows VR games on VXRuntime, without Wine, Windows Steam or a Wine prefix.
#
#   scripts/setup_vxrt.sh <steam-login-name> [appid ...]   # SteamVR (and the apps) downloaded with steamcmd
#   scripts/setup_vxrt.sh --import "<a Steam library>" [appid ...]   # or copied from a library you already have
#
#   e.g. scripts/setup_vxrt.sh me 546560 348250             # SteamVR, Half-Life: Alyx, Google Earth VR
#
# It installs the build tools it needs with Homebrew (mingw-w64 for the SteamVR driver, steamcmd), builds VXRuntime,
# the Mac encoder, the SteamVR driver and the capture proxy, puts SteamVR (and the apps) in
# ~/Verantyx_VR_Drive/vxrt_steam (tools/vxrt_steam.py), and gathers the Microsoft runtime DLLs from the redistributables
# those installs carry (tools/vxrt_redist.py). steamcmd asks for your password and Steam Guard itself; nothing is
# stored by this script, and nothing of Valve's or Microsoft's is in this repository.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
[[ $# -ge 1 ]] || { sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

step() { printf '\n== %s\n' "$*"; }

step "Tools"
[[ "$(uname -m)" == arm64 ]] || { echo "An Apple silicon Mac is needed" >&2; exit 1; }
/usr/bin/arch -x86_64 /usr/bin/true 2>/dev/null || { echo "Rosetta 2 is needed: softwareupdate --install-rosetta" >&2; exit 1; }
xcode-select -p >/dev/null 2>&1 || { echo "Xcode (or its command line tools) is needed: xcode-select --install" >&2; exit 1; }
command -v brew >/dev/null || { echo "Homebrew is needed: https://brew.sh" >&2; exit 1; }
command -v x86_64-w64-mingw32-g++ >/dev/null || brew install mingw-w64
[[ "$1" == --import ]] || command -v steamcmd >/dev/null || brew install --cask steamcmd
command -v bsdtar >/dev/null || { echo "bsdtar is missing (it comes with macOS)" >&2; exit 1; }

step "Build: VXRuntime, the encoder, the SteamVR driver, the capture proxy"
make -C "$REPO_ROOT/VXRuntime" check
mkdir -p "$REPO_ROOT/build"
swiftc -O "$REPO_ROOT/MacEncoder/HardwareEncoder.swift" -o "$REPO_ROOT/build/HardwareEncoder"
"$SCRIPT_DIR/build_openvr_driver.sh"
"$SCRIPT_DIR/build_openvr_capture_proxy.sh"

step "SteamVR and games: ~/Verantyx_VR_Drive/vxrt_steam"
if [[ "$1" == --import ]]; then
    [[ $# -ge 2 ]] || { echo "usage: setup_vxrt.sh --import <Steam library> [appid ...]" >&2; exit 2; }
    python3 "$REPO_ROOT/tools/vxrt_steam.py" import "$2" "${@:3}"
else
    python3 "$REPO_ROOT/tools/vxrt_steam.py" fetch "$1" 250820 "${@:2}"
fi

step "Microsoft runtime DLLs: ~/Verantyx_VR_Drive/vxrt_redist"
python3 "$REPO_ROOT/tools/vxrt_redist.py" collect | tail -1

step "Ready?"
python3 "$REPO_ROOT/tools/vxrt_steam.py" status
cat <<EOF

Next:
  1. Sign in to Steam on the Mac (the games reach it for your licences).
  2. Install the Vision Pro app: scripts/deploy_vision_pro_app.sh (or TestFlight, docs/TESTFLIGHT.md), and open it.
  3. Play: VXRT_VR_QUALITY=3k scripts/launch_vxrt_alyx.sh, or scripts/launch_vxrt_game.sh "<game .exe>"
     More games: tools/vxrt_steam.py fetch $([[ "$1" == --import ]] && echo "<steam-login>" || echo "$1") <appid>.
     With an AI coding tool in this repository: /vxrt-games, /vxrt-tune <game>, /vxrt-support <game>.
EOF
