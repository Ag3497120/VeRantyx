#!/usr/bin/env bash
# Stops everything the Wine path leaves running: the game, SteamVR, the
# Windows Steam client and its web helper (which Steam keeps restarting),
# the HEVC encoder of a Wine session, and finally the prefix's wineserver.
# launch_vision_pro_vr.sh only stops its encoder on exit, so Windows Steam
# stays up and brings its windows back after the game is closed.
#
#   scripts/stop_wine_vr.sh
#
# Only touches the VeRantyx test prefix (SteamVR_Prefix_*_Test); the native
# ARM build (NativeARM/scripts/launch_native_vr.sh) is not affected.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
VERANTYX_VR_DRIVE="${VERANTYX_VR_DRIVE:-$HOME/Verantyx_VR_Drive}"
WINEPREFIX="${WINEPREFIX:-$VERANTYX_VR_DRIVE/SteamVR_Prefix_Wine11_Test}"
WINE_RUNTIME_BIN="${WINE_RUNTIME_BIN:-$VERANTYX_VR_DRIVE/Beer/wine/WineStaging.app/Contents/Resources/wine/bin/wine}"
WINESERVER="$(dirname "$WINE_RUNTIME_BIN")/wineserver"

case "$(basename "$WINEPREFIX")" in
    SteamVR_Prefix_*_Test) ;;
    *) echo "Refusing: $WINEPREFIX is not a VeRantyx test prefix (SteamVR_Prefix_*_Test)." >&2; exit 1 ;;
esac

running() { pgrep -f "$1" >/dev/null 2>&1; }
stop_matching() {   # stop_matching <label> <pattern>
    if running "$2"; then
        echo "Stopping $1"
        pkill -TERM -f "$2" 2>/dev/null || true
    fi
}

# Game first, then SteamVR, then Steam (so nothing restarts what was stopped).
stop_matching "Half-Life: Alyx (Wine)" '[h]lvr\.exe'
stop_matching "SteamVR" '[v]rstartup\.exe|[v]rmonitor\.exe|[v]rcompositor\.exe|[v]rserver\.exe|[v]rdashboard\.exe|[v]rwebhelper\.exe'
stop_matching "Windows Steam" '[s]team\.exe'
stop_matching "Steam web helper" '[s]teamwebhelper'
# The Wine session's encoder: one whose parent is launch_vision_pro_vr.sh.
for pid in $(pgrep -f "$REPO_ROOT/build/HardwareEncoder" 2>/dev/null); do
    parent="$(ps -o ppid= -p "$pid" 2>/dev/null | tr -d ' ')"
    if [[ -n "$parent" ]] && ps -o command= -p "$parent" 2>/dev/null | grep -q 'launch_vision_pro_vr'; then
        echo "Stopping the Wine session's HEVC encoder ($pid)"
        kill -TERM "$pid" 2>/dev/null || true
    fi
done
pkill -TERM -f '[l]aunch_vision_pro_vr\.sh' 2>/dev/null || true

for _ in 1 2 3 4 5 6 7 8 9 10; do
    running '[h]lvr\.exe|[s]team\.exe|[s]teamwebhelper|[v]rserver\.exe|[v]rcompositor\.exe' || break
    sleep 1
done
if [[ -x "$WINESERVER" ]]; then
    echo "Ending the prefix's Wine session (wineserver -k)"
    WINEPREFIX="$WINEPREFIX" "$WINESERVER" -k 2>/dev/null || true
fi
pkill -KILL -f '[h]lvr\.exe|[s]team\.exe|[s]teamwebhelper|[v]rserver\.exe|[v]rcompositor\.exe|[v]rmonitor\.exe|[v]rstartup\.exe' 2>/dev/null || true

if running '[h]lvr\.exe|[s]team\.exe|[s]teamwebhelper'; then
    echo "Some Wine processes are still running:" >&2
    pgrep -fl '[h]lvr\.exe|[s]team\.exe|[s]teamwebhelper' >&2
    exit 1
fi
echo "The Wine version is stopped."
