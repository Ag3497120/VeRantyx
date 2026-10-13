#!/usr/bin/env bash
# Half-Life: Alyx in VR without Wine: SteamVR (vrserver, vrcompositor, the VeRantyx driver) and the game run on
# VXRuntime, the HEVC encoder streams the driver's frames to the Vision Pro app, and Steam is the Mac Steam client
# (sign in there first): the game reaches it through VXRuntime's Steam bridge.
#
#   scripts/launch_vxrt_alyx.sh                     # main menu, until the game exits or Ctrl-C
#   scripts/launch_vxrt_alyx.sh +map a1_intro_world_2 +hlvr_pause_after_realmap 0   # extra game arguments
#   VXRT_VR_QUALITY=3k scripts/launch_vxrt_alyx.sh  # 1440x1620 per eye, 2880x1620 stream at 40 Mbps (about 35 fps
#                                                   # on an M1 Max; add -noautofidelity +vr_fidelity_level 4 for
#                                                   # the game's full detail at that size)
#
# The game and SteamVR come from the Wine prefix's Steam library and stay read-only (writes go to the overlay
# folder, as for scripts/launch_vxrt_steamvr.sh). The first start compiles thousands of shaders and takes a few
# minutes; later starts use Metal's cache. Geometry shaders are not supported yet (some cables and effects are
# missing).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
VERANTYX_VR_DRIVE="${VERANTYX_VR_DRIVE:-$HOME/Verantyx_VR_Drive}"
WINEPREFIX="${WINEPREFIX:-$VERANTYX_VR_DRIVE/SteamVR_Prefix_Wine11_Test}"
STEAM_ROOT="$WINEPREFIX/drive_c/Program Files (x86)/steam"
ALYX_BIN="$STEAM_ROOT/steamapps/common/Half-Life Alyx/game/bin/win64"
export VXRT_VR_ROOT="${VXRT_VR_ROOT:-$VERANTYX_VR_DRIVE/vxrt_root}"
export VXRT_VR_OVERLAY="${VXRT_VR_OVERLAY:-$VERANTYX_VR_DRIVE/vxrt_steam_overlay}"
LOG_DIR="${VXRT_VR_LOG_DIR:-$REPO_ROOT/logs}"
export VXRT_VR_LOG_DIR="$LOG_DIR"
VXRT="$REPO_ROOT/VXRuntime/build/vxrt"

[[ -d "$ALYX_BIN" ]] || { echo "Half-Life: Alyx not found at $ALYX_BIN" >&2; exit 1; }
pgrep -f "Steam.AppBundle/Steam/Contents/MacOS/steam_osx" >/dev/null || { echo "Start Steam on the Mac and sign in first" >&2; exit 1; }
if pgrep -f "wine.*(vrserver|hlvr)|build/vxrt.*(vrserver|hlvr)" >/dev/null; then echo "SteamVR or the game is already running" >&2; exit 1; fi
mkdir -p "$LOG_DIR"

# SteamVR on VXRuntime (with the encoder and the web UI stand-in), until this script ends.
"$SCRIPT_DIR/launch_vxrt_steamvr.sh" >"$LOG_DIR/vxrt-steamvr-launch.log" 2>&1 &
STEAMVR_PID=$!
cleanup() {
    [[ -n "${GAME_PID:-}" ]] && kill -TERM "$GAME_PID" 2>/dev/null || true
    kill -TERM "$STEAMVR_PID" 2>/dev/null || true
    wait "$STEAMVR_PID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
for _ in $(seq 1 30); do                               # vrserver's web server is up once SteamVR has started
    sleep 1
    lsof -nP -iTCP:27062 -sTCP:LISTEN >/dev/null 2>&1 && break
done
echo "SteamVR on VXRuntime is up; starting Half-Life: Alyx (log: $LOG_DIR/vxrt-alyx.log)"

cd "$ALYX_BIN"
VXRT_ROOT="$VXRT_VR_ROOT" VXRT_OVERLAY="$STEAM_ROOT:$VXRT_VR_OVERLAY" VXRT_DLL_PATH="$WINEPREFIX/drive_c/windows/system32" \
VXRT_STEAM_BRIDGE=1 VXRT_NO_ACTIVATE=1 \
    "$VXRT" hlvr.exe -vr -steam -dx11 -noasserts -nopassiveasserts +map startup "$@" >"$LOG_DIR/vxrt-alyx.log" 2>&1 &
GAME_PID=$!
wait "$GAME_PID" || true
GAME_PID=
echo "Half-Life: Alyx exited"
