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
if pgrep -f '[s]team\.exe|[s]teamwebhelper' >/dev/null 2>&1; then
    echo "Note: the Wine version's Windows Steam is still running (it uses CPU and restarts its windows)."
    echo "      Stop it with: scripts/stop_wine_vr.sh"
fi
# A previous native session (this script) is replaced; a Wine session is not touched.
for pid in $(pgrep -f '[l]aunch_native_vr\.sh' 2>/dev/null); do
    [[ "$pid" == "$$" ]] && continue
    echo "Stopping the previous native session ($pid)."
    kill -TERM "$pid" 2>/dev/null || true
done
for _ in $(seq 1 15); do pgrep -f "$HOST" >/dev/null 2>&1 || break; sleep 1; done
pkill -KILL -f "$HOST" 2>/dev/null || true
for pid in $(pgrep -f "$ENCODER" 2>/dev/null); do
    parent="$(ps -o command= -p "$(ps -o ppid= -p "$pid" | tr -d ' ')" 2>/dev/null)"
    if [[ "$parent" == *launch_vision_pro_vr* ]]; then
        die "The Wine version's encoder is running. Stop the Wine version first: scripts/stop_wine_vr.sh"
    fi
    echo "Stopping a leftover HEVC encoder ($pid)."
    kill -TERM "$pid" 2>/dev/null || true
done
sleep 1
pkill -KILL -f "$ENCODER" 2>/dev/null || true

# Picture quality: VERANTYX_QUALITY=standard (default) | high | ultra.
#   standard  ~1.0 MP rendered per eye, 1920x1080 HEVC stream, 20 Mbps
#   high      ~2.2 MP per eye,          2880x1620 stream,      40 Mbps
#   ultra     ~4.0 MP per eye (≈3K),    3840x2160 stream,      70 Mbps
# Measured on an M1 Max (menu, no headset): standard 60 fps; high ~39 fps
# (1982x1536 per eye); ultra ~20 fps and the encoder produced no frames at
# that size — ultra is for faster Macs and is unverified.
# The engine's auto-fidelity still lowers its own render scale when the GPU
# cannot keep up; the stream size and bitrate are what the headset receives.
# Any of the variables set explicitly wins over the preset.
case "${VERANTYX_QUALITY:-standard}" in
    high)  : "${VERANTYX_EYE_PIXELS:=2200000}" "${VERANTYX_STREAM_WIDTH:=2880}" "${VERANTYX_STREAM_HEIGHT:=1620}" "${VERANTYX_BITRATE_MBPS:=40}" ;;
    ultra) : "${VERANTYX_EYE_PIXELS:=4000000}" "${VERANTYX_STREAM_WIDTH:=3840}" "${VERANTYX_STREAM_HEIGHT:=2160}" "${VERANTYX_BITRATE_MBPS:=70}" ;;
    *) ;;
esac
export VERANTYX_EYE_PIXELS VERANTYX_STREAM_WIDTH VERANTYX_STREAM_HEIGHT VERANTYX_BITRATE_MBPS 2>/dev/null || true

if [[ "${VERANTYX_MAC_PROFILE:-1}" == "1" ]]; then
    eval "$("$REPO_ROOT/scripts/mac_profile.sh")"
fi

mkdir -p "$LOG_DIR" "$REPO_ROOT/build"
# Keep the previous sessions' logs (a problem is often noticed only after the
# next start): alyx_host.log -> alyx_host.<time it was last written>.log, the
# newest 5 sessions of each kept.
for f in alyx_host hardware-encoder; do
    if [[ -s "$LOG_DIR/$f.log" ]]; then
        mv -f "$LOG_DIR/$f.log" "$LOG_DIR/$f.$(stat -f '%Sm' -t '%Y%m%d-%H%M%S' "$LOG_DIR/$f.log").log"
    fi
    ls -1t "$LOG_DIR/$f".2*.log 2>/dev/null | tail -n +6 | while read -r old; do rm -f "$old"; done
done
ls -1t "$LOG_DIR"/slow-*.txt 2>/dev/null | tail -n +11 | while read -r old; do rm -f "$old"; done
command -v swiftc >/dev/null 2>&1 || die "swiftc is required to build the Mac HEVC encoder."
swiftc -O "$REPO_ROOT/MacEncoder/HardwareEncoder.swift" -o "$ENCODER"

# The runtime creates the frame file; the encoder waits for it.
"$ENCODER" >"$LOG_DIR/hardware-encoder.log" 2>&1 &
ENCODER_PID=$!
HOST_PID=""
cleanup() {
    # The engine handles SIGTERM itself and writes its Vulkan pipeline cache
    # (shadercache/vulkan) on the way out, which is what makes the next run
    # stutter less; give it up to 30 s before ending it.
    if [[ -n "$HOST_PID" ]] && kill -0 "$HOST_PID" 2>/dev/null; then
        echo "Stopping the game (saving its shader cache)..."
        kill -TERM "$HOST_PID" 2>/dev/null || true
        for _ in $(seq 1 30); do kill -0 "$HOST_PID" 2>/dev/null || break; sleep 1; done
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
# When the frame rate stays below 20 fps for 15 s after the first minute, the
# game's threads are sampled once (macOS `sample`, 3 s) into
# logs/native/slow-<time>.txt together with the runtime's frame timing, so a
# slowdown can be traced afterwards; guest frames are read against the
# "[image] <base> <library>" lines of alyx_host.log (slow-<time>.sample.txt). At most one sample per
# 2 minutes. VERANTYX_SLOW_SAMPLE=0 turns it off.
prev_pairs=""; slow=0; last_sample=-1000; started=$SECONDS
while kill -0 "$HOST_PID" 2>/dev/null; do
    sleep 5
    line="$(grep -a 'eye pairs published' "$LOG_DIR/alyx_host.log" 2>/dev/null | tail -1 || true)"
    kill -0 "$ENCODER_PID" 2>/dev/null || { echo "The HEVC encoder stopped. See $LOG_DIR/hardware-encoder.log" >&2; break; }
    [[ -n "$line" ]] || continue
    echo "${line#*compositor: }"
    pairs="$(sed -E 's/.* ([0-9]+) eye pairs published.*/\1/' <<<"$line")"
    if [[ -n "$prev_pairs" && "$pairs" =~ ^[0-9]+$ ]] && (( SECONDS - started > 60 && pairs - prev_pairs < 100 )); then
        slow=$((slow + 1))
    else
        slow=0
    fi
    prev_pairs="$pairs"
    if (( slow >= 3 && SECONDS - last_sample > 120 )) && [[ "${VERANTYX_SLOW_SAMPLE:-1}" != "0" ]]; then
        last_sample=$SECONDS
        out="$LOG_DIR/slow-$(date +%Y%m%d-%H%M%S).txt"
        { echo "Frame rate below 20 fps for 15 s at $(date)"; grep -a 'frame timing' "$LOG_DIR/alyx_host.log" | tail -4; echo; } >"$out"
        sample "$HOST_PID" 3 -mayDie -file "${out%.txt}.sample.txt" >/dev/null 2>&1 &
        echo "Frame rate stayed low; recording what the game is doing to $out"
    fi
done
wait "$HOST_PID" 2>/dev/null; rc=$?
HOST_PID=""
grep -a '\[klepton\] fault:\|\[engine messagebox\]' "$LOG_DIR/alyx_host.log" | head -3 || true
echo "alyx_host exited ($rc)."
exit "$rc"
