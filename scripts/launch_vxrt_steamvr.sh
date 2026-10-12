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

python3 - "$STEAM_ROOT/config/steamvr.vrsettings" "$VXRT_VR_OVERLAY/config/steamvr.vrsettings" "$VXRT_VR_NO_STANDBY" <<'EOF'
import json, sys
settings = json.load(open(sys.argv[1]))
settings.setdefault("steamvr", {})["allowDisplayLockedMode"] = True
if sys.argv[3] == "1":
    settings.setdefault("power", {}).update({"pauseCompositorOnStandby": False, "turnOffScreensTimeout": 3600.0})
json.dump(settings, open(sys.argv[2], "w"), indent=3)
EOF

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
