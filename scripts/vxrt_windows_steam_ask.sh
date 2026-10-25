#!/usr/bin/env bash
# Hands steam.exe arguments to the Windows Steam already running on VXRuntime, as on Windows: a second steam.exe passes
# its command line to the running one and ends (VXRuntime/README.md, "Starting a SteamStub game through this Steam").
# For the Mac app's "get and play": steam://install/<appid> asks that Steam to install a game, and Steam starts the
# download (the app asked first); a window Steam opens of its own (where to install, an agreement) is yours to answer,
# and it installs only games your account owns.
#
#   scripts/vxrt_windows_steam_ask.sh -- steam://install/275850   # (the form Steam registers for steam:// links)
#   VXRT_WSTEAM_DRY_RUN=1 scripts/vxrt_windows_steam_ask.sh ...    # only the checks; prints what it would run
#
# It starts no Steam (Windows Steam must be running: you open it and sign in yourself, scripts/launch_vxrt_windows_steam.sh)
# and answers none of Steam's windows: signing in, agreements and every confirmation are yours.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
VERANTYX_VR_DRIVE="${VERANTYX_VR_DRIVE:-$HOME/Verantyx_VR_Drive}"
WSTEAM_ROOT="${VXRT_WSTEAM_ROOT:-$VERANTYX_VR_DRIVE/vxrt_wsteam}"
STEAM_DIR="$WSTEAM_ROOT/drive_c/Program Files (x86)/Steam"
VXRT_REDIST="${VXRT_REDIST:-$VERANTYX_VR_DRIVE/vxrt_redist}"
VXRT="$REPO_ROOT/VXRuntime/build/vxrt"
STEAM_EXE_PATTERN='vxrt(-arm64)? (.*/)?[Ss][Tt][Ee][Aa][Mm]\.[Ee][Xx][Ee]( |$)'

[[ $# -ge 1 ]] || { echo "usage: $0 <steam.exe arguments...>" >&2; exit 2; }
[[ -f "$STEAM_DIR/steam.exe" ]] || { echo "No Windows Steam in $STEAM_DIR (scripts/launch_vxrt_windows_steam.sh)" >&2; exit 1; }
running=1
pgrep -f "$STEAM_EXE_PATTERN" >/dev/null || running=0

if [[ "${VXRT_WSTEAM_DRY_RUN:-0}" == 1 ]]; then
    echo "Dry run, nothing started. Windows Steam running: $([[ $running == 1 ]] && echo yes || echo no). Would run, in $STEAM_DIR:"
    printf ' '; printf ' %q' env -u VXRT_STEAM_BRIDGE "VXRT_ROOT=$WSTEAM_ROOT" "VXRT_DLL_PATH=$VXRT_REDIST" "$VXRT" steam.exe "$@"; echo
    exit 0
fi
[[ $running == 1 ]] || { echo "Windows Steam is not running: open it and sign in first" >&2; exit 1; }
[[ -x "$VXRT" ]] || { echo "No VXRuntime build at $VXRT" >&2; exit 1; }
cd "$STEAM_DIR"
env -u VXRT_STEAM_BRIDGE VXRT_ROOT="$WSTEAM_ROOT" VXRT_DLL_PATH="$VXRT_REDIST" "$VXRT" steam.exe "$@"
