#!/usr/bin/env bash
# No Man's Sky in VR on VXRuntime, answered by the real Windows Steam you signed in to (No Man's Sky's executable is
# wrapped in Steam's DRM, which only that Steam answers: scripts/launch_vxrt_windows_steam.sh). Run it from a Terminal
# window (the encoder's capture of the game's sound needs that terminal's permission under Privacy & Security >
# Screen & System Audio Recording; started from elsewhere the sound it sends is silence).
#
#   scripts/launch_vxrt_nms_vr.sh                  # the ALVR app on the headset (App Store "ALVR", default)
#   VERANTYX_ALVR=0 scripts/launch_vxrt_nms_vr.sh  # the VeRantyx Vision Pro app instead
#   VXRT_VR_QUALITY=3k ...                         # a VeRantyx quality preset (scripts/launch_vxrt_steamvr.sh)
#   VXRT_FLAT=1 scripts/launch_vxrt_nms_vr.sh      # on the Mac's screen: no SteamVR, steam.exe -applaunch 275850
#   VXRT_WSTEAM_NO_START=1 ...                     # Windows Steam must be running already (the Mac app: the player
#                                                  # opened it and signed in; nothing is started or clicked here)
#
# What it does, until No Man's Sky exits or Ctrl-C:
# 1. Windows Steam: used if it runs already, else started (scripts/launch_vxrt_windows_steam.sh; its "Steam Service
#    Error" window is closed with Cancel, as a user would) and given time to sign in with the saved login.
# 2. SteamVR with the encoder (scripts/launch_vxrt_steamvr.sh) in the same VXRuntime root as Steam and the game
#    (VXRT_VR_ROOT): the game finds SteamVR through openvrpaths.vrpath in that root's C:, and shares its eye images
#    with SteamVR's compositor through that root's shared textures. With ALVR the first headset is awaited first.
# 3. The game, as Steam's own "No Man's Sky VR" launch option starts it (steam.exe -applaunch 275850 -HmdEnable 1).
# When the game exits, SteamVR and the encoder are stopped; Windows Steam keeps running (quit it from its window or
# tray, or leave it for the next start). Logs: logs/vxrt-steamvr*.log, logs/hardware-encoder.log, and Steam's and the
# game's in ~/Library/Logs/VeRantyx/vxrt-windows-steam.log.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
VERANTYX_VR_DRIVE="${VERANTYX_VR_DRIVE:-$HOME/Verantyx_VR_Drive}"
WSTEAM_ROOT="${VXRT_WSTEAM_ROOT:-$VERANTYX_VR_DRIVE/vxrt_wsteam}"
STEAM_DIR="$WSTEAM_ROOT/drive_c/Program Files (x86)/Steam"
VXRT_REDIST="${VXRT_REDIST:-$VERANTYX_VR_DRIVE/vxrt_redist}"
LOG_DIR="${VXRT_VR_LOG_DIR:-$REPO_ROOT/logs}"
VXRT="$REPO_ROOT/VXRuntime/build/vxrt"
STEAM_SOCKET="${VXRT_WSTEAM_CONTROL:-/tmp/vxrt-wsteam.sock}"
APP_ID=275850
export VERANTYX_ALVR="${VERANTYX_ALVR:-1}"

[[ -f "$STEAM_DIR/steam.exe" ]] || { echo "No Windows Steam in $STEAM_DIR (scripts/launch_vxrt_windows_steam.sh)" >&2; exit 1; }
[[ -f "$STEAM_DIR/steamapps/appmanifest_$APP_ID.acf" ]] || { echo "No Man's Sky is not installed in that Steam" >&2; exit 1; }
if pgrep -f "build/vxrt.*(vrserver|vrcompositor)" >/dev/null; then echo "SteamVR on VXRuntime is already running" >&2; exit 1; fi
if pgrep -if "build/vxrt.*No Man's Sky/Binaries/NMS.exe" >/dev/null; then echo "No Man's Sky is already running" >&2; exit 1; fi
mkdir -p "$LOG_DIR"
[[ -f "$REPO_ROOT/VXRuntime/Makefile" ]] && make -C "$REPO_ROOT/VXRuntime" build/vxrt >/dev/null   # (the Mac app's runtime has none)

steam_cmd() { (cd "$STEAM_DIR" && env -u VXRT_STEAM_BRIDGE VXRT_ROOT="$WSTEAM_ROOT" VXRT_DLL_PATH="$VXRT_REDIST" "$VXRT" steam.exe "$@") >/dev/null 2>&1; }

# 1. Windows Steam.
STEAM_PID=""
if ! pgrep -f "build/vxrt steam.exe" >/dev/null; then
    [[ "${VXRT_WSTEAM_NO_START:-0}" == 1 ]] && { echo "Windows Steam is not running: open it and sign in first" >&2; exit 1; }
    echo "Starting Windows Steam (signs in with its saved login)"
    VXRT_CONTROL="$STEAM_SOCKET" VXRT_CONTROL_FOR=steam.exe VXRT_WSTEAM_GPU="${VXRT_WSTEAM_GPU:-1}" \
        VXRT_WSTEAM_WEBHELPER_ARGS="${VXRT_WSTEAM_WEBHELPER_ARGS:---single-process --disable-gpu-compositing}" \
        "$SCRIPT_DIR/launch_vxrt_windows_steam.sh" >"$LOG_DIR/vxrt-windows-steam-launch.log" 2>&1 &
    STEAM_PID=$!
    # Its "Steam Service Error" window (no Steam Client Service here): Cancel, as a user would.
    for _ in $(seq 1 90); do
        sleep 1
        hwnd="$(python3 "$REPO_ROOT/tools/vxrt_clip.py" --socket "$STEAM_SOCKET" windows 2>/dev/null |
                python3 -c 'import json,sys
try:
    d = json.load(sys.stdin)
    print(next((w["hwnd"] for w in d["windows"] if w["title"] == "Steam Service Error" and w["visible"]), ""))
except Exception: print("")' || true)"
        if [[ -n "$hwnd" ]]; then python3 "$REPO_ROOT/tools/vxrt_clip.py" --socket "$STEAM_SOCKET" click "$hwnd" 525 120 >/dev/null || true; break; fi
    done
    sleep "${VXRT_WSTEAM_SIGNIN_WAIT:-30}"                    # (Steam signs in before it takes -applaunch)
fi

# On the Mac's screen: the game as Steam's own Play starts it, without SteamVR; then until it exits.
if [[ "${VXRT_FLAT:-0}" == 1 ]]; then
    echo "Starting No Man's Sky on the Mac's screen through Windows Steam"
    steam_cmd -applaunch "$APP_ID"
    for _ in $(seq 1 120); do pgrep -if "build/vxrt.*No Man's Sky/Binaries/NMS.exe" >/dev/null && break; sleep 1; done
    pgrep -if "build/vxrt.*No Man's Sky/Binaries/NMS.exe" >/dev/null || { echo "No Man's Sky did not start (log: ~/Library/Logs/VeRantyx/vxrt-windows-steam.log)" >&2; exit 1; }
    echo "No Man's Sky is running (log: ~/Library/Logs/VeRantyx/vxrt-windows-steam.log); quit the game from its menu"
    while pgrep -if "build/vxrt.*No Man's Sky/Binaries/NMS.exe" >/dev/null; do sleep 2; done
    echo "No Man's Sky exited"
    exit 0
fi

# 2. SteamVR and the encoder, in Steam's root.
cleanup() {
    [[ -n "${STEAMVR_PID:-}" ]] && { kill -TERM "$STEAMVR_PID" 2>/dev/null || true; wait "$STEAMVR_PID" 2>/dev/null || true; }
}
trap cleanup EXIT INT TERM
# (a SteamVR that has just ended may still hold its web server's port for a moment: the new vrserver then cannot
#  bind it and ends at once, while the old one's listener made it look up, and the game, finding no SteamVR, started
#  its own without the VeRantyx driver and ran flat, 2026-10-25)
for _ in $(seq 1 30); do lsof -nP -iTCP:27062 -sTCP:LISTEN >/dev/null 2>&1 || break; sleep 1; done
if lsof -nP -iTCP:27062 -sTCP:LISTEN >/dev/null 2>&1; then echo "Port 27062 (SteamVR's) is still in use: $(lsof -nP -iTCP:27062 -sTCP:LISTEN | tail -1)" >&2; exit 1; fi
VXRT_VR_ROOT="$WSTEAM_ROOT" "$SCRIPT_DIR/launch_vxrt_steamvr.sh" >"$LOG_DIR/vxrt-steamvr-launch.log" 2>&1 &
STEAMVR_PID=$!
[[ "$VERANTYX_ALVR" == 1 ]] && echo "SteamVR starting for the ALVR app: open ALVR on the headset (the first time it is awaited before SteamVR starts)"
waited=0
while (( waited < 60 )); do
    sleep 1
    # (up once our vrserver listens on the port and its compositor has started)
    listener="$(lsof -nP -t -iTCP:27062 -sTCP:LISTEN 2>/dev/null | head -1 || true)"
    if [[ -n "$listener" ]] && ps -o command= -p "$listener" | grep -q "vrserver.exe" && pgrep -f "build/vxrt.*vrcompositor.exe" >/dev/null; then break; fi
    [[ -f "$LOG_DIR/alvr-first-connection.wait" ]] || waited=$((waited + 1))
    kill -0 "$STEAMVR_PID" 2>/dev/null || { echo "SteamVR did not start (log: $LOG_DIR/vxrt-steamvr-launch.log)" >&2; exit 1; }
done
if ! pgrep -f "build/vxrt.*vrcompositor.exe" >/dev/null; then echo "SteamVR did not come up (log: $LOG_DIR/vxrt-steamvr.log)" >&2; exit 1; fi
echo "SteamVR is up; starting No Man's Sky in VR through Windows Steam"

# 3. The game, as Steam's VR launch option starts it; then until it exits.
steam_cmd -applaunch "$APP_ID" -HmdEnable 1
for _ in $(seq 1 120); do pgrep -if "build/vxrt.*No Man's Sky/Binaries/NMS.exe" >/dev/null && break; sleep 1; done
pgrep -if "build/vxrt.*No Man's Sky/Binaries/NMS.exe" >/dev/null || { echo "No Man's Sky did not start (log: ~/Library/Logs/VeRantyx/vxrt-windows-steam.log)" >&2; exit 1; }
echo "No Man's Sky is running (log: ~/Library/Logs/VeRantyx/vxrt-windows-steam.log); Ctrl-C stops SteamVR, quit the game from its menu"
while pgrep -if "build/vxrt.*No Man's Sky/Binaries/NMS.exe" >/dev/null; do sleep 2; done
echo "No Man's Sky exited; stopping SteamVR (Windows Steam keeps running)"
