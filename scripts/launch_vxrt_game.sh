#!/usr/bin/env bash
# Any Windows SteamVR game on VXRuntime: SteamVR (with the encoder) as scripts/launch_vxrt_steamvr.sh starts it,
# then the game, until the game exits or Ctrl-C. Half-Life: Alyx has its own launcher (launch_vxrt_alyx.sh).
# VXRT_GAME_CONTROL (and optionally VXRT_GAME_CONTROL_LOG) enables Command Clips for only the game process;
# SteamVR's helper processes do not claim that socket.
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
source "$SCRIPT_DIR/vxrt_paths.sh"                  # STEAM_ROOT, WINE_STEAM, VXRT_DLL_DIRS (no Wine prefix needed)
export VXRT_VR_ROOT="${VXRT_VR_ROOT:-$VERANTYX_VR_DRIVE/vxrt_root}"
export VXRT_VR_OVERLAY="${VXRT_VR_OVERLAY:-$VERANTYX_VR_DRIVE/vxrt_steam_overlay}"
LOG_DIR="${VXRT_VR_LOG_DIR:-$REPO_ROOT/logs}"
export VXRT_VR_LOG_DIR="$LOG_DIR"
VXRT="$REPO_ROOT/VXRuntime/build/vxrt"
[[ "${VXRT_ARM64:-0}" == 1 ]] && VXRT="$VXRT-arm64"   # (the ARM64 runtime, the program translated: experimental)

[[ $# -ge 1 && -f "$1" ]] || { echo "usage: launch_vxrt_game.sh <game .exe> [arguments...]" >&2; exit 2; }
EXE="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
shift
GAME_DIR="$(dirname "$EXE")"
EXE_NAME="$(basename "$EXE")"
pgrep -f "Steam.AppBundle/Steam/Contents/MacOS/steam_osx" >/dev/null || echo "Note: Mac Steam is not running; games that use Steam will not start" >&2
if pgrep -f "build/vxrt.*(vrserver|vrcompositor)" >/dev/null; then echo "SteamVR on VXRuntime is already running" >&2; exit 1; fi
mkdir -p "$LOG_DIR"
for f in vxrt-steamvr-launch.log; do [[ -s "$LOG_DIR/$f" ]] && mv -f "$LOG_DIR/$f" "$LOG_DIR/${f%.log}.prev.log"; done   # (the last start's, kept)

# Where the game's writes go: the Steam library's overlay, or one of the game's own.
LIB="$(steam_library_of "$EXE")"
case "$LIB" in
    ?*) OVERLAY="$LIB:$VXRT_VR_OVERLAY" ;;
    *)  top="$GAME_DIR"                                   # the install's top folder: where steamapps/ is, else the exe's
        d="$GAME_DIR"
        while [[ "$d" != "/" && "$d" != "$HOME" ]]; do [[ -d "$d/steamapps" ]] && { top="$d"; break; }; d="$(dirname "$d")"; done
        OVERLAY="$top:$VXRT_VR_OVERLAY/games/$(basename "$top")" ;;
esac

# The Steam app ID, for the Steam bridge.
APP_ID="${VXRT_APP_ID:-}"
[[ -z "$APP_ID" && -f "$GAME_DIR/steam_appid.txt" ]] && APP_ID="$(tr -dc 0-9 < "$GAME_DIR/steam_appid.txt")"
# A Steam library holds several manifests: the game's is the one whose install folder holds the executable
# (steamapps/common/<installdir>; taking the first one gave Google Earth VR SteamVR's id). A steamcmd install
# (force_install_dir) has the game's manifest beside it.
if [[ -z "$APP_ID" ]]; then
    d="$GAME_DIR"
    while [[ "$d" != "/" && -z "$APP_ID" ]]; do
        only=""
        for m in "$d"/steamapps/appmanifest_*.acf; do
            [[ -f "$m" ]] || continue
            id="$(basename "$m" | tr -dc 0-9)"
            [[ "$id" == 228980 ]] && continue                 # (Steamworks Common Redistributables)
            inst="$(sed -n 's/.*"installdir"[[:space:]]*"\(.*\)".*/\1/p' "$m" | head -1)"
            if [[ -n "$inst" && "$EXE" == "$d/steamapps/common/$inst/"* ]]; then APP_ID="$id"; break; fi
            only="${only:+many}${only:-$id}"
        done
        [[ -z "$APP_ID" && -n "$only" && "$only" != many* && "$EXE" != "$d/steamapps/common/"* ]] && APP_ID="$only"
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
[[ -s "$LOG" ]] && mv -f "$LOG" "${LOG%.log}.prev.log"   # (the last start's, kept)
echo "SteamVR on VXRuntime is up; starting $EXE_NAME${APP_ID:+ (Steam app $APP_ID)} (log: $LOG)"
# (the quality presets are the Vision Pro app's; an ALVR headset shows its own rate, 90 Hz, and stretches the last
#  frame to the head's turn until the next comes: Half-Life: Alyx at 3k on a MacBook Air M5 made some 44 fps and
#  warped when turning, with ALVR's defaults it did not, 2026-10-17)
[[ "${VERANTYX_ALVR:-0}" == 1 && -n "${VXRT_VR_QUALITY:-}" ]] && echo "ALVR with VXRT_VR_QUALITY=$VXRT_VR_QUALITY: if turning your head warps the picture, the game runs too far below the headset's rate; leave VXRT_VR_QUALITY out for ALVR's lighter defaults"

cd "$GAME_DIR"
# Keep the optional control socket scoped to the game. SteamVR is already running in separate VXRuntime
# processes, and sharing the same Unix socket path would let those processes replace each other's endpoint.
GAME_ENV=("VXRT_ROOT=$VXRT_VR_ROOT" "VXRT_OVERLAY=$OVERLAY" "VXRT_DLL_PATH=$VXRT_DLL_DIRS"
    "VXRT_STEAM_BRIDGE=1" "VXRT_NO_ACTIVATE=1" "VXRT_DEBUGGER=1" "VXRT_DEBUG_OUTPUT=1"
    "VXRT_LOG_TIME=1")
if [[ -n "${VXRT_GAME_CONTROL:-}" ]]; then GAME_ENV+=("VXRT_CONTROL=$VXRT_GAME_CONTROL"); fi
if [[ -n "${VXRT_GAME_CONTROL_LOG:-}" ]]; then GAME_ENV+=("VXRT_CONTROL_LOG=$VXRT_GAME_CONTROL_LOG"); fi
if [[ -n "$APP_ID" ]]; then GAME_ENV+=("SteamAppId=$APP_ID" "SteamGameId=$APP_ID"); fi
# The game's own debug output (VXRT_DEBUGGER, VXRT_DEBUG_OUTPUT) goes to the log with the time: why it quit, if it does.
env "${GAME_ENV[@]}" "$VXRT" "$EXE_NAME" "$@" >"$LOG" 2>&1 &
GAME_PID=$!
wait "$GAME_PID" || true
GAME_PID=
echo "$EXE_NAME exited (log: $LOG)"
