#!/usr/bin/env bash
# Half-Life: Alyx in VR without Wine: SteamVR (vrserver, vrcompositor, the VeRantyx driver) and the game run on
# VXRuntime, the HEVC encoder streams the driver's frames to the Vision Pro app, and Steam is the Mac Steam client
# (sign in there first): the game reaches it through VXRuntime's Steam bridge.
#
#   scripts/launch_vxrt_alyx.sh                     # main menu, until the game exits or Ctrl-C
#   scripts/launch_vxrt_alyx.sh +map a1_intro_world_2 +hlvr_pause_after_realmap 0   # extra game arguments
#   VXRT_VR_QUALITY=3k scripts/launch_vxrt_alyx.sh  # 1440x1620 per eye, 2880x1620 stream at 40 Mbps, 60 Hz
#                                                   # (60 fps on an M1 Max)
#   VXRT_VR_QUALITY=2.5k scripts/launch_vxrt_alyx.sh  # 2560x2880 per eye, 2x multisampling, the 3k stream
#                                                   # (50-55 fps on an M1 Max)
#   VXRT_VR_QUALITY=max scripts/launch_vxrt_alyx.sh   # 3200x3600 per eye (a Vision Pro eye's pixels), 3840x2160 stream
#
# The game and SteamVR come from ~/Verantyx_VR_Drive/vxrt_steam (tools/vxrt_steam.py; the Wine version's Steam
# library while they are not there, scripts/vxrt_paths.sh) and stay read-only (writes go to the overlay folder, as
# for scripts/launch_vxrt_steamvr.sh). The first start compiles thousands of shaders and takes a few
# minutes; later starts use Metal's cache.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
source "$SCRIPT_DIR/vxrt_paths.sh"                  # STEAM_ROOT, WINE_STEAM, VXRT_DLL_DIRS (no Wine prefix needed)
ALYX_STEAM="$STEAM_ROOT"                              # the library the game is in (SteamVR's, else the Wine version's)
[[ ! -d "$ALYX_STEAM/steamapps/common/Half-Life Alyx" && -n "$WINE_STEAM" ]] && ALYX_STEAM="$WINE_STEAM"
ALYX_BIN="$ALYX_STEAM/steamapps/common/Half-Life Alyx/game/bin/win64"
export VXRT_VR_ROOT="${VXRT_VR_ROOT:-$VERANTYX_VR_DRIVE/vxrt_root}"
export VXRT_VR_OVERLAY="${VXRT_VR_OVERLAY:-$VERANTYX_VR_DRIVE/vxrt_steam_overlay}"
LOG_DIR="${VXRT_VR_LOG_DIR:-$REPO_ROOT/logs}"
export VXRT_VR_LOG_DIR="$LOG_DIR"
VXRT="$REPO_ROOT/VXRuntime/build/vxrt"
[[ "${VXRT_ARM64:-0}" == 1 ]] && VXRT="$VXRT-arm64"   # (the ARM64 runtime, the program translated: experimental)

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
# (the quality presets are the Vision Pro app's; an ALVR headset shows its own rate, 90 Hz, and stretches the last
#  frame to the head's turn until the next comes: Half-Life: Alyx at 3k on a MacBook Air M5 made some 44 fps and
#  warped when turning, with ALVR's defaults it did not, 2026-10-17)
[[ "${VERANTYX_ALVR:-0}" == 1 && -n "${VXRT_VR_QUALITY:-}" ]] && echo "ALVR with VXRT_VR_QUALITY=$VXRT_VR_QUALITY: if turning your head warps the picture, the game runs too far below the headset's rate; leave VXRT_VR_QUALITY out for ALVR's lighter defaults"

# The game's capture proxy (openvr_api.dll beside the game, from this repository) streams the game's frames straight
# from its Submit and paces poses by the virtual display, as the driver does (3k: 60 Hz). It goes into the overlay
# folder (read before the game's own folder), so the Steam library stays as it is.
[[ "${VXRT_VR_QUALITY:-}" == 3k || "${VXRT_VR_QUALITY:-}" == 2.5k || "${VXRT_VR_QUALITY:-}" == max ]] && export VERANTYX_DISPLAY_HZ="${VERANTYX_DISPLAY_HZ:-60}"
# 2.5k: 2x multisampling (4x stores and reloads about 1.5 GB of samples a frame at this size: 45 fps falling to 35 within
# minutes; 2x holds 50-55), and the game's fidelity level fixed (its automatic choice raises the resolution
# past what the GPU can draw).
# 3k: fidelity level 4 fixed as well (left automatic, the game raised its own size to 2407x1942 per eye in the
# headset, 32-36 fps).
GAME_ARGS=()
if [[ "${VXRT_VR_QUALITY:-}" == 2.5k || "${VXRT_VR_QUALITY:-}" == max ]]; then
    export VXRT_MSAA_MAX="${VXRT_MSAA_MAX:-2}"
    GAME_ARGS=(-noautofidelity +vr_fidelity_level 3)
elif [[ "${VXRT_VR_QUALITY:-}" == 3k ]]; then
    GAME_ARGS=(-noautofidelity +vr_fidelity_level 4)
fi
PROXY_DLL="$REPO_ROOT/build/openvr_api_capture_proxy.dll"
if [[ ! -f "$PROXY_DLL" || "$REPO_ROOT/VRDriver/src/openvr_capture_proxy.cpp" -nt "$PROXY_DLL" ]]; then
    "$SCRIPT_DIR/build_openvr_capture_proxy.sh" >/dev/null
fi
OVERLAY_ALYX_BIN="$VXRT_VR_OVERLAY/steamapps/common/Half-Life Alyx/game/bin/win64"
mkdir -p "$OVERLAY_ALYX_BIN"
cp -p "$PROXY_DLL" "$OVERLAY_ALYX_BIN/openvr_api.dll"
# Poses at the display's rate from the headset instead of SteamVR's compositor, which handed out the next ones only
# once the GPU had finished the last frame (a frame took the CPU's time plus the GPU's: 45 fps at 3K, now 60).
# VERANTYX_DECOUPLED_POSES=0: SteamVR's.
# The game's own console log (VXRT_DEBUGGER, VXRT_DEBUG_OUTPUT) goes to the log with the time: why it quit, if it does.
export VERANTYX_DECOUPLED_POSES="${VERANTYX_DECOUPLED_POSES:-1}"
cd "$ALYX_BIN"
VXRT_ROOT="$VXRT_VR_ROOT" VXRT_OVERLAY="$ALYX_STEAM:$VXRT_VR_OVERLAY" VXRT_DLL_PATH="$VXRT_DLL_DIRS" \
VXRT_STEAM_BRIDGE=1 VXRT_NO_ACTIVATE=1 VXRT_DEBUGGER=1 VXRT_DEBUG_OUTPUT=1 VXRT_LOG_TIME=1 \
    "$VXRT" hlvr.exe -vr -steam -dx11 -noasserts -nopassiveasserts ${GAME_ARGS[@]+"${GAME_ARGS[@]}"} +map startup "$@" >"$LOG_DIR/vxrt-alyx.log" 2>&1 &
GAME_PID=$!
wait "$GAME_PID" || true
GAME_PID=
echo "Half-Life: Alyx exited"
