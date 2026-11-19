#!/bin/bash
# Launch native macOS Cyberpunk 2077 (ARM64) in full head-tracked Stereo 3D VR
# streamed to Apple Vision Pro via VeRantyx MacVR + HardwareEncoder.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

MACVR_DYLIB="$REPO_ROOT/MacVR/build/libverantyx_macvr.dylib"
ENCODER_BIN="$REPO_ROOT/build/HardwareEncoder"

# 1. Build MacVR interposer dylib and HardwareEncoder if needed
if [ -f "$REPO_ROOT/MacVR/Makefile" ]; then
    make -C "$REPO_ROOT/MacVR" "$MACVR_DYLIB" >/dev/null
fi
if [ ! -f "$MACVR_DYLIB" ]; then
    echo "[MacVR] Error: Missing $MACVR_DYLIB" >&2
    exit 1
fi
if [ ! -x "$ENCODER_BIN" ]; then
    mkdir -p "$REPO_ROOT/build"
    swiftc -O "$REPO_ROOT/MacEncoder/HardwareEncoder.swift" \
        -framework VideoToolbox -framework CoreMedia -framework CoreVideo \
        -framework Metal -framework Foundation -framework Network \
        -o "$ENCODER_BIN"
fi

# 2. Prepare / locate runnable wrapper bundle and VR container home
DRIVE_DIR="${VERANTYX_DRIVE:-$HOME/Verantyx_VR_Drive}"
if [ -d "$HOME/Verantyx_VR_Drive/avp_mac/macstream/home" ]; then
    MACVR_HOME="${VERANTYX_MACVR_HOME:-$HOME/Verantyx_VR_Drive/avp_mac/macstream/home}"
else
    MACVR_HOME="${VERANTYX_MACVR_HOME:-$DRIVE_DIR/mac_vr/home}"
fi
mkdir -p "$MACVR_HOME"

CP_RESOLUTION="${VERANTYX_CP_RESOLUTION:-1600x900}"
CP_FOV="${VERANTYX_CP_FOV:-100.0}"

APP_BUNDLE="$(python3 "$REPO_ROOT/tools/mac_cyberpunk_vr.py" \
    --home "$MACVR_HOME" \
    --resolution "$CP_RESOLUTION" \
    --fov "$CP_FOV" \
    --print-bundle)"

GAME_BIN="$APP_BUNDLE/Contents/MacOS/Cyberpunk2077"
if [ ! -x "$GAME_BIN" ]; then
    echo "[MacVR] Error: Executable not found at $GAME_BIN" >&2
    exit 1
fi

export VERANTYX_SHARED_FRAME="${VERANTYX_SHARED_FRAME:-/tmp/verantyx_vr_frame}"
export VERANTYX_SHARED_HANDS="${VERANTYX_SHARED_HANDS:-/tmp/verantyx_vr_hands}"
export VERANTYX_MACVR_LOG="${VERANTYX_MACVR_LOG:-/tmp/verantyx_macvr.log}"
export VERANTYX_EYE_W="${VERANTYX_EYE_W:-1920}"
export VERANTYX_EYE_H="${VERANTYX_EYE_H:-1824}"

rm -f "$VERANTYX_MACVR_LOG"

echo "================================================================"
echo " VeRantyx MacVR — Native macOS Cyberpunk 2077 Stereo VR Stream"
echo " Bundle:     $APP_BUNDLE"
echo " Resolution: $CP_RESOLUTION (FOV $CP_FOV deg)"
echo " Eye Target: ${VERANTYX_EYE_W}x${VERANTYX_EYE_H} per eye (SBS $((VERANTYX_EYE_W*2))x${VERANTYX_EYE_H})"
echo " Log:        $VERANTYX_MACVR_LOG"
echo "================================================================"

# 3. Start HardwareEncoder in background (unless VERANTYX_NO_ENCODER=1)
ENCODER_PID=""
cleanup() {
    if [ -n "$ENCODER_PID" ] && kill -0 "$ENCODER_PID" 2>/dev/null; then
        kill "$ENCODER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

if [ "${VERANTYX_NO_ENCODER:-0}" != "1" ]; then
    pkill -f "$ENCODER_BIN" 2>/dev/null || true
    VERANTYX_SHARED_FRAME="$VERANTYX_SHARED_FRAME" \
    VERANTYX_FPS="${VERANTYX_FPS:-90}" \
    VERANTYX_MAX_EYE_W="$VERANTYX_EYE_W" \
    VERANTYX_MAX_EYE_H="$VERANTYX_EYE_H" \
    "$ENCODER_BIN" >/tmp/verantyx_macvr_encoder.log 2>&1 &
    ENCODER_PID=$!
    echo "[MacVR] Started HardwareEncoder (PID $ENCODER_PID)"
fi

if [ "${VERANTYX_DRY_RUN:-0}" = "1" ]; then
    echo "[MacVR] Dry-run verification complete."
    exit 0
fi

# 4. Launch macOS Cyberpunk 2077 with libverantyx_macvr.dylib injected
cd "$(dirname "$GAME_BIN")"
HOME="$MACVR_HOME" \
CFFIXED_USER_HOME="$MACVR_HOME" \
DYLD_INSERT_LIBRARIES="$MACVR_DYLIB" \
exec "$GAME_BIN" -skipStartScreen "$@"
