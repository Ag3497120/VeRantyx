#!/usr/bin/env bash
# Runs SteamVR without Wine: vrserver, vrcompositor and the VeRantyx driver
# under VXRuntime (VXRuntime/), plus the Mac HEVC encoder that streams the
# driver's frames to the Vision Pro app. Experimental; no game yet (Half-Life:
# Alyx still needs a Steam client, which does not run on VXRuntime yet).
#
#   scripts/launch_vxrt_steamvr.sh            # until Ctrl-C
#   VXRT_VR_SECONDS=60 scripts/launch_vxrt_steamvr.sh
#
# The SteamVR install in the Wine prefix is used read-only: everything SteamVR
# writes under the Steam folder goes to $VXRT_VR_OVERLAY instead, and C:\ is
# $VXRT_VR_ROOT/drive_c (the driver's shared frame file is there). The VC++
# runtime DLLs come from the prefix's system32.
#
# The overlay's copy of steamvr.vrsettings (never the original) sets
# steamvr/allowDisplayLockedMode: without Windows' DisplayManager (WinRT) the
# compositor believes the display is locked and covers everything in a green
# colour otherwise. VXRT_VR_NO_STANDBY=1 (default) also keeps the compositor
# running when the headset looks idle (SteamVR otherwise drops to a few frames
# a second after ~5 s).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
VERANTYX_VR_DRIVE="${VERANTYX_VR_DRIVE:-$HOME/Verantyx_VR_Drive}"
WINEPREFIX="${WINEPREFIX:-$VERANTYX_VR_DRIVE/SteamVR_Prefix_Wine11_Test}"
STEAM_ROOT="$WINEPREFIX/drive_c/Program Files (x86)/steam"
STEAMVR_BIN="$STEAM_ROOT/steamapps/common/steamvr/bin/win64"
VXRT_VR_ROOT="${VXRT_VR_ROOT:-$VERANTYX_VR_DRIVE/vxrt_root}"
VXRT_VR_OVERLAY="${VXRT_VR_OVERLAY:-$VERANTYX_VR_DRIVE/vxrt_steam_overlay}"
VXRT_VR_NO_STANDBY="${VXRT_VR_NO_STANDBY:-1}"
VXRT="$REPO_ROOT/VXRuntime/build/vxrt"
ENCODER="$REPO_ROOT/build/HardwareEncoder"
LOG_DIR="${VXRT_VR_LOG_DIR:-$REPO_ROOT/logs}"

[[ -d "$STEAMVR_BIN" ]] || { echo "SteamVR not found at $STEAMVR_BIN" >&2; exit 1; }
make -C "$REPO_ROOT/VXRuntime" build/vxrt >/dev/null
if [[ ! -x "$ENCODER" || "$REPO_ROOT/MacEncoder/HardwareEncoder.swift" -nt "$ENCODER" ]]; then
    mkdir -p "$(dirname "$ENCODER")"
    swiftc -O "$REPO_ROOT/MacEncoder/HardwareEncoder.swift" -o "$ENCODER"
fi
mkdir -p "$LOG_DIR" "$VXRT_VR_OVERLAY/config"

# SteamVR finds its runtime through openvrpaths.vrpath in the user's local app data (C:\ = the root above).
to_windows() { printf 'Z:%s' "$1" | tr '/' '\\' | sed 's/\\/\\\\/g'; }
OPENVR_DIR="$VXRT_VR_ROOT/drive_c/users/vxrt/AppData/Local/openvr"
mkdir -p "$OPENVR_DIR"
cat >"$OPENVR_DIR/openvrpaths.vrpath" <<EOF
{
	"config" : [ "$(to_windows "$STEAM_ROOT/config")" ],
	"external_drivers" : null,
	"jsonid" : "vrpathreg",
	"log" : [ "$(to_windows "$STEAM_ROOT/logs")" ],
	"runtime" : [ "$(to_windows "$STEAM_ROOT/steamapps/common/SteamVR")" ],
	"version" : 1
}
EOF

# Picture size. VXRT_VR_QUALITY=3k: 1440x1620 per eye (2880x1620 for both, about 4.7 megapixels) rendered,
# composited and streamed, at 40 Mbps HEVC and 60 Hz; VXRT_VR_RENDER_SCALE=1.25 instead scales the driver's default
# 800x900 per eye (the stream keeps its own size, VERANTYX_STREAM_WIDTH/HEIGHT). Either way SteamVR's own
# resolution scaling is set to 100% (the copied settings may lower it), and the repository's driver, which
# takes VERANTYX_RENDER_SCALE, is used from the overlay (the Wine prefix keeps its own).
# VXRT_VR_QUALITY=2.5k: the game renders 2560x2880 per eye (Half-Life: Alyx: 50-55 fps on an M1 Max with 2x
# multisampling, scripts/launch_vxrt_alyx.sh); SteamVR's compositor and the stream stay at the 3k preset's 2880x1620
# (one HEVC session encodes about 60 frames a second of that).
# VXRT_VR_QUALITY=max: the game renders 3200x3600 per eye (as many pixels as a Vision Pro eye's display, about 11.5
# megapixels) and the stream is 3840x2160 (1920x2160 per eye: what one HEVC session and Wi-Fi can carry at 60 fps);
# otherwise as 3k. Expect a much lower frame rate than 2.5k (1.6 times its pixels).
if [[ "${VXRT_VR_QUALITY:-}" == max ]]; then
    VXRT_VR_RENDER_SCALE="${VXRT_VR_RENDER_SCALE:-4.0}"
    export VERANTYX_DISPLAY_SCALE="${VERANTYX_DISPLAY_SCALE:-2.4}"
    export VERANTYX_STREAM_WIDTH="${VERANTYX_STREAM_WIDTH:-3840}" VERANTYX_STREAM_HEIGHT="${VERANTYX_STREAM_HEIGHT:-2160}"
    VXRT_VR_QUALITY=3k
fi
if [[ "${VXRT_VR_QUALITY:-}" == 2.5k ]]; then
    VXRT_VR_RENDER_SCALE="${VXRT_VR_RENDER_SCALE:-3.2}"
    export VERANTYX_DISPLAY_SCALE="${VERANTYX_DISPLAY_SCALE:-1.8}"
    VXRT_VR_QUALITY=3k
fi
if [[ "${VXRT_VR_QUALITY:-}" == 3k ]]; then
    VXRT_VR_RENDER_SCALE="${VXRT_VR_RENDER_SCALE:-1.8}"
    export VERANTYX_STREAM_WIDTH="${VERANTYX_STREAM_WIDTH:-2880}" VERANTYX_STREAM_HEIGHT="${VERANTYX_STREAM_HEIGHT:-1620}"
    export VERANTYX_BITRATE_MBPS="${VERANTYX_BITRATE_MBPS:-40}"
    # 60 Hz: with the game's poses decoupled from SteamVR's compositor (scripts/launch_vxrt_alyx.sh) the game holds
    # 60 fps at this size on an M1 Max and the encoder streams all 60 (at 72 Hz the game makes 72, but one HEVC
    # session encodes about 60 of them: frames would be dropped unevenly).
    export VERANTYX_DISPLAY_HZ="${VERANTYX_DISPLAY_HZ:-60}"
    # SteamVR does not hold the game back by whole frames when it misses one (it switched into that mode now and
    # then: 45 fps fell to 30 for the rest of the run).
    VXRT_VR_SETTINGS="${VXRT_VR_SETTINGS:-{\"steamvr\": {\"framesToThrottle\": 0, \"additionalFramesToPredict\": 0}}}"
fi
if [[ -n "${VXRT_VR_RENDER_SCALE:-}" ]]; then
    export VERANTYX_RENDER_SCALE="$VXRT_VR_RENDER_SCALE"
    DRIVER_DLL="$REPO_ROOT/VRDriver/driver_verantyx.dll"
    if [[ ! -f "$DRIVER_DLL" || "$REPO_ROOT/VRDriver/src/driver_verantyx.cpp" -nt "$DRIVER_DLL" ]]; then
        "$SCRIPT_DIR/build_openvr_driver.sh" >/dev/null
    fi
    OVERLAY_DRIVER="$VXRT_VR_OVERLAY/steamapps/common/SteamVR/drivers/verantyx/bin/win64"
    mkdir -p "$OVERLAY_DRIVER"
    cp -p "$DRIVER_DLL" "$OVERLAY_DRIVER/driver_verantyx.dll"
fi

# VXRT_VR_SETTINGS='{"steamvr": {"framesToThrottle": 0}}': settings merged in, section by section (experiments).
python3 - "$STEAM_ROOT/config/steamvr.vrsettings" "$VXRT_VR_OVERLAY/config/steamvr.vrsettings" "$VXRT_VR_NO_STANDBY" "${VXRT_VR_RENDER_SCALE:-}" "${VXRT_VR_SETTINGS:-}" <<'EOF'
import json, sys
settings = json.load(open(sys.argv[1]))
settings.setdefault("steamvr", {})["allowDisplayLockedMode"] = True
if sys.argv[4]:   # the driver's render size as given (SteamVR's automatic resolution would scale it back down)
    settings["steamvr"].update({"supersampleManualOverride": True, "supersampleScale": 1.0})
if sys.argv[3] == "1":
    settings.setdefault("power", {}).update({"pauseCompositorOnStandby": False, "turnOffScreensTimeout": 3600.0})
for section, values in (json.loads(sys.argv[5]) if sys.argv[5] else {}).items():
    settings.setdefault(section, {}).update(values)
json.dump(settings, open(sys.argv[2], "w"), indent=3)
EOF

# Frames are scaled and converted for the encoder on the GPU: the encoder's own CPU/GPU timing picks the CPU when the
# Mac is idle, and the CPU then loses to a game running at full rate (60 frames captured, 20 encoded).
export VERANTYX_GPU_CONVERT="${VERANTYX_GPU_CONVERT:-1}"
export VXRT_ROOT="$VXRT_VR_ROOT"
export VXRT_OVERLAY="$STEAM_ROOT:$VXRT_VR_OVERLAY"
export VXRT_DLL_PATH="$WINEPREFIX/drive_c/windows/system32"
export VXRT_NO_ACTIVATE=1

VXRT_PATTERN="VXRuntime/build/vxrt .*steamvr/bin/win64/vr"   # no regex characters (the path has "(x86)")
cleanup() {
    [[ -n "${ENCODER_PID:-}" ]] && kill -TERM "$ENCODER_PID" 2>/dev/null || true
    [[ -n "${WEBUI_PID:-}" ]] && kill -TERM "$WEBUI_PID" 2>/dev/null || true
    [[ -n "${VR_PID:-}" ]] && kill -TERM "$VR_PID" 2>/dev/null || true
    pkill -TERM -f "$VXRT_PATTERN" 2>/dev/null || true   # vrserver and vrcompositor outlive vrcmd, as on Windows
    sleep 1
    pkill -KILL -f "$VXRT_PATTERN" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# vrcmd starts vrserver (which starts vrcompositor) and keeps a client connected.
(cd "$STEAMVR_BIN" && exec "$VXRT" vrcmd.exe --pollposes) >"$LOG_DIR/vxrt-steamvr.log" 2>&1 &
VR_PID=$!
echo "SteamVR on VXRuntime started (log: $LOG_DIR/vxrt-steamvr.log)"

VERANTYX_SHARED_FRAME="$VXRT_VR_ROOT/drive_c/vr_shared_frame.dat" "$ENCODER" >"$LOG_DIR/hardware-encoder.log" 2>&1 &
ENCODER_PID=$!

# Games' SteamVR web UIs need vrwebhelper (Chromium), which does not run on VXRuntime: this answers for
# Half-Life: Alyx's (it gives up after a minute without the page's "ready").
python3 "$SCRIPT_DIR/vxrt_webui_standin.py" >"$LOG_DIR/webui-standin.log" 2>&1 &
WEBUI_PID=$!
echo "Encoder started (log: $LOG_DIR/hardware-encoder.log); open the Vision Pro app to connect"

if [[ -n "${VXRT_VR_SECONDS:-}" ]]; then
    sleep "$VXRT_VR_SECONDS"
else
    wait "$VR_PID"
fi
