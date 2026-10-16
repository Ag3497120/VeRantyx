#!/usr/bin/env bash
# Runs SteamVR without Wine: vrserver, vrcompositor and the VeRantyx driver
# under VXRuntime (VXRuntime/), plus the Mac HEVC encoder that streams the
# driver's frames to the Vision Pro app. Experimental; no game yet (Half-Life:
# Alyx still needs a Steam client, which does not run on VXRuntime yet).
#
#   scripts/launch_vxrt_steamvr.sh            # until Ctrl-C
#   VXRT_VR_SECONDS=60 scripts/launch_vxrt_steamvr.sh
#
# SteamVR comes from ~/Verantyx_VR_Drive/vxrt_steam (tools/vxrt_steam.py) and the
# VC++ runtime DLLs from ~/Verantyx_VR_Drive/vxrt_redist (tools/vxrt_redist.py):
# no Wine prefix (scripts/vxrt_paths.sh; the Wine version's is still used, read
# only, until those exist). The install is used read-only: everything SteamVR
# writes under the Steam folder goes to $VXRT_VR_OVERLAY instead, and C:\ is
# $VXRT_VR_ROOT/drive_c (the driver's shared frame file is there).
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
source "$SCRIPT_DIR/vxrt_paths.sh"                  # STEAM_ROOT, VXRT_DLL_DIRS (no Wine prefix needed)
STEAMVR_BIN="$STEAM_ROOT/steamapps/common/SteamVR/bin/win64"
VXRT_VR_ROOT="${VXRT_VR_ROOT:-$VERANTYX_VR_DRIVE/vxrt_root}"
VXRT_VR_OVERLAY="${VXRT_VR_OVERLAY:-$VERANTYX_VR_DRIVE/vxrt_steam_overlay}"
VXRT_VR_NO_STANDBY="${VXRT_VR_NO_STANDBY:-1}"
VXRT="$REPO_ROOT/VXRuntime/build/vxrt"
[[ "${VXRT_ARM64:-0}" == 1 ]] && VXRT="$VXRT-arm64"   # (the ARM64 runtime, the program translated: experimental)
ENCODER="$REPO_ROOT/build/HardwareEncoder"
LOG_DIR="${VXRT_VR_LOG_DIR:-$REPO_ROOT/logs}"

[[ -d "$STEAMVR_BIN" ]] || { echo "SteamVR not found at $STEAMVR_BIN" >&2; exit 1; }
make -C "$REPO_ROOT/VXRuntime" "${VXRT#$REPO_ROOT/VXRuntime/}" >/dev/null
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
[[ -n "${VXRT_VR_RENDER_SCALE:-}" ]] && export VERANTYX_RENDER_SCALE="$VXRT_VR_RENDER_SCALE"
# VERANTYX_ALVR=1: the stream goes to the ALVR client apps (Quest, Pico, the Vision Pro's "ALVR") through ALVR's own
# streaming core (ALVRBridge/README.md), built here the first time (Rust); SteamVR's display rate is the one the last
# ALVR headset asked for (90 Hz before any).
if [[ "${VERANTYX_ALVR:-0}" == 1 ]]; then
    ALVR_LIB="$REPO_ROOT/ALVRBridge/target/release/libverantyx_alvr_bridge.dylib"
    if [[ ! -f "$ALVR_LIB" || -n "$(find "$REPO_ROOT/ALVRBridge/src" -newer "$ALVR_LIB" -name '*.rs' 2>/dev/null)" ]]; then
        CARGO="$(command -v cargo || echo "$HOME/.cargo/bin/cargo")"
        [[ -x "$CARGO" ]] || { echo "VERANTYX_ALVR=1 needs Rust (https://rustup.rs) to build ALVRBridge" >&2; exit 1; }
        (cd "$REPO_ROOT/ALVRBridge" && "$CARGO" build --release --lib) || exit 1
    fi
    export VERANTYX_ALVR_LIB="$ALVR_LIB"
    ALVR_SESSION="$HOME/Library/Application Support/VeRantyx/ALVR/session.json"
    ALVR_HZ="$(python3 -c 'import json,sys; print(int(json.load(open(sys.argv[1]))["openvr_config"]["refresh_rate"]))' "$ALVR_SESSION" 2>/dev/null || echo 90)"
    export VERANTYX_DISPLAY_HZ="${VERANTYX_DISPLAY_HZ:-$ALVR_HZ}"
    export VERANTYX_AUDIO="${VERANTYX_AUDIO:-0}"   # (no game audio over ALVR yet: the Mac's speakers play it, not muted)
fi
# The repository's driver, in the overlay's copy of SteamVR's drivers folder (SteamVR as downloaded has no
# drivers/verantyx; the overlay's entries show in its listing).
DRIVER_DLL="$REPO_ROOT/VRDriver/driver_verantyx.dll"
if [[ ! -f "$DRIVER_DLL" || "$REPO_ROOT/VRDriver/src/driver_verantyx.cpp" -nt "$DRIVER_DLL" ]]; then
    "$SCRIPT_DIR/build_openvr_driver.sh" >/dev/null
fi
OVERLAY_DRIVER="$VXRT_VR_OVERLAY/steamapps/common/SteamVR/drivers/verantyx/bin/win64"
mkdir -p "$OVERLAY_DRIVER"
cmp -s "$DRIVER_DLL" "$OVERLAY_DRIVER/driver_verantyx.dll" || cp -p "$DRIVER_DLL" "$OVERLAY_DRIVER/driver_verantyx.dll"

# VXRT_VR_SETTINGS='{"steamvr": {"framesToThrottle": 0}}': settings merged in, section by section (experiments).
python3 - "$STEAM_ROOT/config/steamvr.vrsettings" "$VXRT_VR_OVERLAY/config/steamvr.vrsettings" "$VXRT_VR_NO_STANDBY" "${VXRT_VR_RENDER_SCALE:-}" "${VXRT_VR_SETTINGS:-}" <<'EOF'
import json, os, sys
settings = json.load(open(sys.argv[1])) if os.path.exists(sys.argv[1]) else {}   # (a new library has none yet)
# The VeRantyx headset only: no other driver is started (SteamVR's own, vrlink, calls Windows networking functions
# VXRuntime lacks and stops vrserver), no SteamVR Home or dashboard, no safe mode. The Wine version's setup wrote
# these into its Steam config; here they are set every start.
for section, values in {"steamvr": {"forcedDriver": "verantyx", "activateMultipleDrivers": False, "requireHmd": False,
                                    "enableHomeApp": False, "enableSafeMode": False, "showMirrorView": False,
                                    "startDashboardFromAppLaunch": False, "motionSmoothing": False},
                        "dashboard": {"enableDashboard": False}, "verantyx": {"enable": True},
                        "driver_verantyx": {"blocked_by_safe_mode": False}, "driver_vrlink": {"enable": False},
                        "lighthouse": {"enable": False}, "oculus": {"enable": False}}.items():
    settings.setdefault(section, {}).update(values)
# The default picture size the measurements without a preset were made with (the Wine version's setting).
settings["steamvr"].setdefault("supersampleManualOverride", True)
settings["steamvr"].setdefault("supersampleScale", 0.55)
settings["steamvr"]["allowDisplayLockedMode"] = True
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
export VXRT_DLL_PATH="$VXRT_DLL_DIRS"
export VXRT_NO_ACTIVATE=1

VXRT_PATTERN="VXRuntime/build/vxrt(-arm64)? .*[Ss]team[Vv][Rr]/bin/win64/vr"   # (either library's spelling; the path may have "(x86)")
cleanup() {
    [[ -n "${ENCODER_PID:-}" ]] && kill -TERM "$ENCODER_PID" 2>/dev/null || true
    pkill -TERM -f "^$ENCODER\$" 2>/dev/null || true             # (the encoder itself: ENCODER_PID is its restart loop)
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

# (exit 75: an ALVR headset brought other display settings, stored: the encoder again, which connects with them)
(while :; do VERANTYX_SHARED_FRAME="$VXRT_VR_ROOT/drive_c/vr_shared_frame.dat" "$ENCODER"; [[ $? == 75 ]] || break; done) >"$LOG_DIR/hardware-encoder.log" 2>&1 &
ENCODER_PID=$!

# Games' SteamVR web UIs need vrwebhelper (Chromium), which does not run on VXRuntime: this answers for
# Half-Life: Alyx's (it gives up after a minute without the page's "ready").
python3 "$SCRIPT_DIR/vxrt_webui_standin.py" >"$LOG_DIR/webui-standin.log" 2>&1 &
WEBUI_PID=$!
if [[ "${VERANTYX_ALVR:-0}" == 1 ]]; then
    echo "Encoder started with ALVR (log: $LOG_DIR/hardware-encoder.log); open an ALVR client app (20.x) on this network"
else
    echo "Encoder started (log: $LOG_DIR/hardware-encoder.log); open the Vision Pro app to connect"
fi

if [[ -n "${VXRT_VR_SECONDS:-}" ]]; then
    sleep "$VXRT_VR_SECONDS"
else
    wait "$VR_PID"
fi
