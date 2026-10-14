#!/usr/bin/env bash
# Any Windows SteamVR game on VXRuntime: SteamVR (with the encoder) as scripts/launch_vxrt_steamvr.sh starts it,
# then the game, until the game exits or Ctrl-C. Half-Life: Alyx has its own launcher (launch_vxrt_alyx.sh).
#
#   scripts/launch_vxrt_game.sh "<path to the game's .exe>" [game arguments...]
#   VXRT_VR_QUALITY=3k scripts/launch_vxrt_game.sh ~/Verantyx_VR_Drive/vxrt_library/450390/TheLab/win64/TheLab.exe
#
# The game folder stays as installed: what the game writes under it goes to $VXRT_VR_OVERLAY (a game in the Wine
# prefix's Steam library) or to $VXRT_VR_OVERLAY/games/<folder> (anywhere else). The Steam app ID is taken from a
# steam_appid.txt beside the game or from the Steam app manifest above it (VXRT_APP_ID sets it). Mac Steam must be
# running and signed in for games that use Steam. Log: logs/vxrt-<exe name>.log (tools/vxrt_doctor.py reads it).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
VERANTYX_VR_DRIVE="${VERANTYX_VR_DRIVE:-$HOME/Verantyx_VR_Drive}"
WINEPREFIX="${WINEPREFIX:-$VERANTYX_VR_DRIVE/SteamVR_Prefix_Wine11_Test}"
STEAM_ROOT="$WINEPREFIX/drive_c/Program Files (x86)/steam"
export VXRT_VR_ROOT="${VXRT_VR_ROOT:-$VERANTYX_VR_DRIVE/vxrt_root}"
export VXRT_VR_OVERLAY="${VXRT_VR_OVERLAY:-$VERANTYX_VR_DRIVE/vxrt_steam_overlay}"
LOG_DIR="${VXRT_VR_LOG_DIR:-$REPO_ROOT/logs}"
export VXRT_VR_LOG_DIR="$LOG_DIR"
VXRT="$REPO_ROOT/VXRuntime/build/vxrt"

[[ $# -ge 1 && -f "$1" ]] || { echo "usage: launch_vxrt_game.sh <game .exe> [arguments...]" >&2; exit 2; }
EXE="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
shift
GAME_DIR="$(dirname "$EXE")"
EXE_NAME="$(basename "$EXE")"
pgrep -f "Steam.AppBundle/Steam/Contents/MacOS/steam_osx" >/dev/null || echo "Note: Mac Steam is not running; games that use Steam will not start" >&2
if pgrep -f "build/vxrt.*(vrserver|vrcompositor)" >/dev/null; then echo "SteamVR on VXRuntime is already running" >&2; exit 1; fi
mkdir -p "$LOG_DIR"

# Where the game's writes go: the Steam library's overlay, or one of the game's own.
case "$EXE" in
    "$STEAM_ROOT"/*) OVERLAY="$STEAM_ROOT:$VXRT_VR_OVERLAY" ;;
    *)  top="$GAME_DIR"                                   # the install's top folder: where steamapps/ is, else the exe's
        d="$GAME_DIR"
        while [[ "$d" != "/" && "$d" != "$HOME" ]]; do [[ -d "$d/steamapps" ]] && { top="$d"; break; }; d="$(dirname "$d")"; done
        OVERLAY="$top:$VXRT_VR_OVERLAY/games/$(basename "$top")" ;;
esac

# The Steam app ID, for the Steam bridge.
APP_ID="${VXRT_APP_ID:-}"
[[ -z "$APP_ID" && -f "$GAME_DIR/steam_appid.txt" ]] && APP_ID="$(tr -dc 0-9 < "$GAME_DIR/steam_appid.txt")"
if [[ -z "$APP_ID" ]]; then
    d="$GAME_DIR"
    while [[ "$d" != "/" && -z "$APP_ID" ]]; do
        for m in "$d"/steamapps/appmanifest_*.acf; do
            [[ -f "$m" ]] || continue
            id="$(basename "$m" | tr -dc 0-9)"
            [[ "$id" != 228980 ]] && APP_ID="$id" && break   # (not Steamworks Common Redistributables)
        done
        d="$(dirname "$d")"
    done
fi

# SteamVR on VXRuntime (with the encoder and the web UI stand-in), until this script ends.
"$SCRIPT_DIR/launch_vxrt_steamvr.sh" >"$LOG_DIR/vxrt-steamvr-launch.log" 2>&1 &
STEAMVR_PID=$!
cleanup() {
    [[ -n "${GAME_PID:-}" ]] && kill -TERM "$GAME_PID" 2>/dev/null || true
    kill -TERM "$STEAMVR_PID" 2>/dev/null || true
    wait "$STEAMVR_PID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
for _ in $(seq 1 40); do                               # vrserver's web server is up once SteamVR has started
    sleep 1
    lsof -nP -iTCP:27062 -sTCP:LISTEN >/dev/null 2>&1 && break
done
LOG="$LOG_DIR/vxrt-${EXE_NAME%.*}.log"
echo "SteamVR on VXRuntime is up; starting $EXE_NAME${APP_ID:+ (Steam app $APP_ID)} (log: $LOG)"

cd "$GAME_DIR"
# The game's own debug output (VXRT_DEBUGGER, VXRT_DEBUG_OUTPUT) goes to the log with the time: why it quit, if it does.
env VXRT_ROOT="$VXRT_VR_ROOT" VXRT_OVERLAY="$OVERLAY" VXRT_DLL_PATH="$WINEPREFIX/drive_c/windows/system32" \
    VXRT_STEAM_BRIDGE=1 VXRT_NO_ACTIVATE=1 VXRT_DEBUGGER=1 VXRT_DEBUG_OUTPUT=1 VXRT_LOG_TIME=1 \
    ${APP_ID:+SteamAppId=$APP_ID SteamGameId=$APP_ID} \
    "$VXRT" "$EXE_NAME" "$@" >"$LOG" 2>&1 &
GAME_PID=$!
wait "$GAME_PID" || true
GAME_PID=
echo "$EXE_NAME exited (log: $LOG)"
