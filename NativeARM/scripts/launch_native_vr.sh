#!/usr/bin/env bash
# Half-Life: Alyx, Valve's Linux ARM64 build, running natively on this Mac and
# streamed to Vision Pro: the HEVC encoder (MacEncoder) and alyx_host share one
# frame file, exactly as the Wine path shares vr_shared_frame.dat.
#
#   NativeARM/scripts/launch_native_vr.sh            # play
#   VERANTYX_BENCH=1 NativeARM/scripts/launch_native_vr.sh   # no headset needed
#
# Needs: macOS Steam.app running and signed in (the game checks Steam), the
# game root from make_game_root.sh, a built host (make -C NativeARM/host).
# Never stops a running Wine session: if its game or encoder is up, this exits.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NATIVE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$NATIVE_DIR/.." && pwd)"
LOG_DIR="${LOG_DIR:-$REPO_ROOT/logs/native}"
GAME_ROOT="${ALYX_GAME_ROOT:-$HOME/Verantyx_VR_Drive/alyx_linux}"
HOST="$NATIVE_DIR/build/host/alyx_host"
LIBDIR="$NATIVE_DIR/build/guest/lib"
ENCODER="$REPO_ROOT/build/HardwareEncoder"
export VERANTYX_SHARED_FRAME="${VERANTYX_SHARED_FRAME:-$GAME_ROOT/vr_shared_frame.dat}"
GAME_ARGS=(-vr -steam -noasserts -nopassiveasserts -vulkan -nowindow)
GAME_ARGS+=(+map "${ALYX_MAP:-startup}")   # the official launch options load the startup map

die() { echo "$*" >&2; exit 1; }

[[ -x "$HOST" ]] || die "alyx_host is not built: make -C $NATIVE_DIR/host"
[[ -d "$LIBDIR" ]] || die "guest library directory missing: $LIBDIR (scripts/make_guest_dir.sh)"
[[ -d "$GAME_ROOT/game" ]] || die "game root missing: $GAME_ROOT (scripts/make_game_root.sh)"
pgrep -x steam_osx >/dev/null 2>&1 || die "macOS Steam is not running. Start Steam.app and sign in first."
if pgrep -f '[h]lvr\.exe' >/dev/null 2>&1; then
    die "The Wine version of Half-Life: Alyx is running; quit it first (it uses the same encoder port)."
fi
if pgrep -f "$ENCODER" >/dev/null 2>&1; then
    die "A HEVC encoder is already running (Wine session?). Quit that session first."
fi
pgrep -f "$HOST" >/dev/null 2>&1 && die "alyx_host is already running."

if [[ "${VERANTYX_MAC_PROFILE:-1}" == "1" ]]; then
    eval "$("$REPO_ROOT/scripts/mac_profile.sh")"
fi

mkdir -p "$LOG_DIR" "$REPO_ROOT/build"
command -v swiftc >/dev/null 2>&1 || die "swiftc is required to build the Mac HEVC encoder."
swiftc -O "$REPO_ROOT/MacEncoder/HardwareEncoder.swift" -o "$ENCODER"

# The runtime creates the frame file; the encoder waits for it.
"$ENCODER" >"$LOG_DIR/hardware-encoder.log" 2>&1 &
ENCODER_PID=$!
HOST_PID=""
cleanup() {
    # The engine installs its own SIGTERM handler and may not leave; give it
    # five seconds, then end it.
    if [[ -n "$HOST_PID" ]] && kill -0 "$HOST_PID" 2>/dev/null; then
        kill -TERM "$HOST_PID" 2>/dev/null || true
        for _ in 1 2 3 4 5; do kill -0 "$HOST_PID" 2>/dev/null || break; sleep 1; done
        kill -KILL "$HOST_PID" 2>/dev/null || true
    fi
    if kill -0 "$ENCODER_PID" 2>/dev/null; then
        kill -TERM "$ENCODER_PID" 2>/dev/null || true
        wait "$ENCODER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

echo "Native Half-Life: Alyx (Linux ARM64) — frame file $VERANTYX_SHARED_FRAME"
echo "Logs: $LOG_DIR/alyx_host.log, $LOG_DIR/hardware-encoder.log"
"$HOST" --libdir "$LIBDIR" --game "$GAME_ROOT" -- "${GAME_ARGS[@]}" >"$LOG_DIR/alyx_host.log" 2>&1 &
HOST_PID=$!

# Progress: one line every 5 s from the runtime's compositor report.
while kill -0 "$HOST_PID" 2>/dev/null; do
    sleep 5
    line="$(grep -a 'eye pairs published' "$LOG_DIR/alyx_host.log" 2>/dev/null | tail -1 || true)"
    [[ -n "$line" ]] && echo "${line#*compositor: }"
    kill -0 "$ENCODER_PID" 2>/dev/null || { echo "The HEVC encoder stopped. See $LOG_DIR/hardware-encoder.log" >&2; break; }
done
wait "$HOST_PID" 2>/dev/null; rc=$?
HOST_PID=""
grep -a '\[klepton\] fault:\|\[engine messagebox\]' "$LOG_DIR/alyx_host.log" | head -3 || true
echo "alyx_host exited ($rc)."
exit "$rc"
