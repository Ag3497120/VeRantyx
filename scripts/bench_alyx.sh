#!/usr/bin/env bash
# Runs Half-Life: Alyx through the whole VeRantyx pipeline without a headset
# and measures it: the game's submit rate from the capture proxy, CPU per
# process and per game thread, and GPU utilization. Used to compare settings
# (VERANTYX_* variables, HLA_FIDELITY_LEVEL, MoltenVK/DXVK knobs) on one Mac.
#
#   ./scripts/bench_alyx.sh <label> [VAR=value ...]
#
# SteamVR is restarted so the driver picks up the bench mode and the current
# settings, the game loads VERANTYX_BENCH_MAP (default a1_intro_world), and
# after it has been running for BENCH_WARMUP_SECONDS the next BENCH_SECONDS
# are measured. The report is written to logs/bench/<time>-<label>.txt.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
LABEL="${1:-baseline}"
shift || true
restart_steam=0
for assignment in "$@"; do
    [[ "$assignment" == *=* ]] || { echo "Expected VAR=value, got: $assignment" >&2; exit 1; }
    export "${assignment?}"
    # The game inherits its environment from the Steam client, so MoltenVK,
    # DXVK and Wine variables only reach it through a restarted Steam.
    [[ "$assignment" =~ ^(MVK_|DXVK_|WINE|HLA_|VERANTYX_DISPLAY_HZ) ]] && restart_steam=1
done
[[ "${BENCH_RESTART_STEAM:-auto}" == "1" ]] && restart_steam=1

VERANTYX_VR_DRIVE="${VERANTYX_VR_DRIVE:-$HOME/Verantyx_VR_Drive}"
WINEPREFIX="${WINEPREFIX:-$VERANTYX_VR_DRIVE/SteamVR_Prefix_Wine11_Test}"
export WINEPREFIX
STEAM_ROOT="${STEAM_ROOT:-$WINEPREFIX/drive_c/Program Files (x86)/Steam}"
PROXY_LOG="$WINEPREFIX/drive_c/vr_openvr_capture_proxy.log"
CONSOLE_LOG="$STEAM_ROOT/steamapps/common/Half-Life Alyx/game/hlvr/console.log"
ENCODER_LOG="${LOG_DIR:-$REPO_ROOT/logs}/hardware-encoder.log"
OUT_DIR="$REPO_ROOT/logs/bench"
STAMP="$(date +%Y%m%d-%H%M%S)"
REPORT="$OUT_DIR/$STAMP-$LABEL.txt"
LAUNCH_LOG="$OUT_DIR/$STAMP-$LABEL.launch.log"
BENCH_MAP="${VERANTYX_BENCH_MAP:-a1_intro_world}"
WARMUP="${BENCH_WARMUP_SECONDS:-20}"
DURATION="${BENCH_SECONDS:-60}"
LAUNCH_TIMEOUT="${BENCH_LAUNCH_TIMEOUT:-600}"
MAP_TIMEOUT="${BENCH_MAP_TIMEOUT:-300}"
KEEP_STEAMVR="${BENCH_KEEP_STEAMVR:-0}"
mkdir -p "$OUT_DIR"

say() { printf '%s %s\n' "$(date +%H:%M:%S)" "$*" | tee -a "$REPORT"; }

stop_processes() {
    local pattern="$1" waited
    pgrep -f "$pattern" >/dev/null 2>&1 || return 0
    pkill -TERM -f "$pattern" 2>/dev/null || true
    for ((waited = 0; waited < 20; waited++)); do
        pgrep -f "$pattern" >/dev/null 2>&1 || return 0
        sleep 1
    done
    pkill -KILL -f "$pattern" 2>/dev/null || true
    sleep 1
}

stop_game() { stop_processes '[h]lvr\.exe'; }
stop_steamvr() {
    stop_processes '[v]rstartup\.exe|[v]rmonitor\.exe|[v]rserver\.exe|[v]rcompositor\.exe|[v]rwebhelper|[v]rdashboard|[s]teamtours\.exe|[s]teamvr_room_setup\.exe'
}

cleanup() {
    stop_game
    if [[ -n "${LAUNCH_PID:-}" ]] && kill -0 "$LAUNCH_PID" 2>/dev/null; then
        kill -TERM "$LAUNCH_PID" 2>/dev/null || true
        wait "$LAUNCH_PID" 2>/dev/null || true
    fi
    # Leave SteamVR to a normal (headset) launch unless asked to keep it: its
    # driver has VERANTYX_BENCH=1 in its environment.
    if [[ "$KEEP_STEAMVR" != "1" ]]; then stop_steamvr; fi
}
trap cleanup EXIT

gpu_utilization() {
    ioreg -r -d 1 -c IOAccelerator 2>/dev/null | grep -oE '"Device Utilization %"=[0-9]+' | head -1 | grep -oE '[0-9]+$' || echo "?"
}

cpu_of() {
    # Sum of %CPU over processes matching the pattern (one core = 100).
    local total=0 value
    while IFS= read -r value; do total=$(awk -v a="$total" -v b="$value" 'BEGIN { print a + b }'); done < <(
        pgrep -f "$1" 2>/dev/null | xargs -I{} ps -o %cpu= -p {} 2>/dev/null)
    printf '%s' "$total"
}

{
    echo "VeRantyx bench: $LABEL ($STAMP)"
    echo "map=$BENCH_MAP warmup=${WARMUP}s duration=${DURATION}s"
    echo "Mac: $(sysctl -n machdep.cpu.brand_string), $(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || echo ?)P+$(sysctl -n hw.perflevel1.physicalcpu 2>/dev/null || echo 0)E cores, $(( $(sysctl -n hw.memsize) / 1073741824 )) GB, macOS $(sw_vers -productVersion)"
    echo "Settings:"
    env | grep -E '^(VERANTYX_|HLA_|MVK_|DXVK_|WINE)' | sort | sed 's/^/  /'
    echo
} >"$REPORT"

say "Stopping any running game."
stop_game
if [[ "${BENCH_REUSE_STEAMVR:-0}" == "1" ]] && pgrep -f '[v]rserver\.exe' >/dev/null 2>&1; then
    say "Reusing the running SteamVR (BENCH_REUSE_STEAMVR=1)."
else
    say "Stopping SteamVR so the driver starts with this run's settings."
    stop_steamvr
fi
if (( restart_steam == 1 )) && pgrep -f '[S]team[/\\]steam\.exe|[s]team\.exe' >/dev/null 2>&1; then
    say "Restarting the Windows Steam client so the game inherits the new environment."
    stop_processes '[s]teamwebhelper|[s]team\.exe'
    "$REPO_ROOT/VRDriver/wine_launcher.sh" "$STEAM_ROOT/steam.exe" -no-cef-sandbox -cef-single-process -noverifyfiles \
        >>"$OUT_DIR/$STAMP-$LABEL.steam.log" 2>&1 &
    sleep 30
fi

proxy_offset="$(stat -f %z "$PROXY_LOG" 2>/dev/null || echo 0)"
console_offset="$(stat -f %z "$CONSOLE_LOG" 2>/dev/null || echo 0)"
say "Launching the pipeline in bench mode (log: $LAUNCH_LOG)."
VERANTYX_BENCH=1 VERANTYX_BENCH_MAP="$BENCH_MAP" "$REPO_ROOT/scripts/launch_vision_pro_vr.sh" >"$LAUNCH_LOG" 2>&1 &
LAUNCH_PID=$!

game_started=0
for ((elapsed = 0; elapsed < LAUNCH_TIMEOUT; elapsed++)); do
    if tail -c "+$((proxy_offset + 1))" "$PROXY_LOG" 2>/dev/null | grep -q 'Captured Alyx eye pair'; then
        game_started=1
        break
    fi
    if ! kill -0 "$LAUNCH_PID" 2>/dev/null; then
        say "The launch script exited before the game rendered. Last lines:"
        tail -20 "$LAUNCH_LOG" | tee -a "$REPORT"
        exit 1
    fi
    sleep 1
done
(( game_started == 1 )) || { say "No stereo frame from the game within ${LAUNCH_TIMEOUT}s."; exit 1; }
say "Game is rendering. Waiting for the map to finish loading."

# console.log is rewritten per run; the game logs 'unpaused the game' once the
# level is up and faded in.
map_ready=0
for ((elapsed = 0; elapsed < MAP_TIMEOUT; elapsed++)); do
    if [[ -f "$CONSOLE_LOG" ]]; then
        current="$(stat -f %z "$CONSOLE_LOG")"
        start=$(( current < console_offset ? 0 : console_offset ))
        if tail -c "+$((start + 1))" "$CONSOLE_LOG" | grep -q 'unpaused the game'; then
            map_ready=1
            break
        fi
    fi
    pgrep -f '[h]lvr\.exe' >/dev/null 2>&1 || { say "The game exited while loading the map."; exit 1; }
    sleep 1
done
if (( map_ready == 1 )); then say "Map loaded. Warming up ${WARMUP}s."; else say "No 'unpaused' line within ${MAP_TIMEOUT}s; measuring anyway after ${WARMUP}s."; fi
sleep "$WARMUP"

game_pid="$(pgrep -f '[h]lvr\.exe' | head -1 || true)"
[[ -n "$game_pid" ]] || { say "The game is not running."; exit 1; }
measure_offset="$(stat -f %z "$PROXY_LOG")"
say "Measuring ${DURATION}s (game pid $game_pid)."
samples=0
declare -a gpu_samples=() game_cpu=() comp_cpu=() server_cpu=() wineserver_cpu=() steam_cpu=() encoder_cpu=() total_cpu=()
for ((elapsed = 0; elapsed < DURATION; elapsed += 5)); do
    pgrep -f '[h]lvr\.exe' >/dev/null 2>&1 || { say "The game exited during the measurement."; break; }
    gpu_samples+=("$(gpu_utilization)")
    game_cpu+=("$(cpu_of '[h]lvr\.exe')")
    comp_cpu+=("$(cpu_of '[v]rcompositor\.exe')")
    server_cpu+=("$(cpu_of '[v]rserver\.exe')")
    wineserver_cpu+=("$(cpu_of '[w]ineserver')")
    steam_cpu+=("$(cpu_of '[s]team\.exe|[s]teamwebhelper')")
    encoder_cpu+=("$(cpu_of '[H]ardwareEncoder')")
    # top's first sample covers the time since boot; the second is current.
    total_cpu+=("$(top -l 2 -n 0 -s 1 2>/dev/null | awk '/CPU usage/ { gsub("%", "", $3); gsub("%", "", $5); v = $3 + $5 } END { print v }')")
    if (( elapsed == 20 )); then
        {
            echo "Hottest game threads at +20 s (ps -M: %CPU is the thread's share since it started):"
            ps -M -p "$game_pid" 2>/dev/null | awk 'NR > 1 { print $4, $0 }' | sort -rn | head -12 | cut -d' ' -f2- | sed 's/^/  /'
        } >>"$REPORT"
    fi
    samples=$((samples + 1))
    sleep 5
done

stats() {
    # mean / median / min / max of the numbers on stdin
    sort -n | awk '{ a[NR] = $1; s += $1 } END { if (NR == 0) { print "n/a"; exit } m = (NR % 2) ? a[(NR + 1) / 2] : (a[NR / 2] + a[NR / 2 + 1]) / 2; printf "mean %.1f median %.1f min %.1f max %.1f (n=%d)", s / NR, m, a[1], a[NR], NR }'
}
metrics="$(tail -c "+$((measure_offset + 1))" "$PROXY_LOG" | grep 'Capture metrics' || true)"
{
    echo
    echo "Game submit rate (frames/s, capture proxy, 5 s windows): $(printf '%s\n' "$metrics" | grep -oE 'submit_fps=[0-9.]+' | cut -d= -f2 | stats)"
    echo "WaitGetPoses time per frame (ms): $(printf '%s\n' "$metrics" | grep -oE 'wait_poses_ms=[0-9.]+' | cut -d= -f2 | stats)"
    echo "Captured pair age (ms): $(printf '%s\n' "$metrics" | grep -oE 'pair_age_ms=[0-9.]+' | cut -d= -f2 | stats)"
    echo "CPU (one core = 100):"
    echo "  hlvr.exe:        $(printf '%s\n' "${game_cpu[@]}" | stats)"
    echo "  vrcompositor:    $(printf '%s\n' "${comp_cpu[@]}" | stats)"
    echo "  vrserver:        $(printf '%s\n' "${server_cpu[@]}" | stats)"
    echo "  wineserver:      $(printf '%s\n' "${wineserver_cpu[@]}" | stats)"
    echo "  Steam client:    $(printf '%s\n' "${steam_cpu[@]}" | stats)"
    echo "  HardwareEncoder: $(printf '%s\n' "${encoder_cpu[@]}" | stats)"
    echo "  whole Mac (%):   $(printf '%s\n' "${total_cpu[@]}" | stats)"
    echo "GPU utilization (%): $(printf '%s\n' "${gpu_samples[@]}" | grep -E '^[0-9]+$' | stats)"
    echo "Encoder (last pipeline line): $(grep 'Pipeline 5.0s' "$ENCODER_LOG" 2>/dev/null | tail -1 || echo n/a)"
    echo "Alyx fidelity: $(grep -aE 'fidelity level|Fidelity.*(Level|level) [0-9]' "$CONSOLE_LOG" 2>/dev/null | tail -2 | tr '\n' ' ' || echo n/a)"
    echo "Resolution: $(tail -c "+$((measure_offset + 1))" "$PROXY_LOG" | grep -oE 'eye pair [0-9]+ \([0-9]+x[0-9]+' | tail -1 | grep -oE '[0-9]+x[0-9]+' || echo n/a) (both eyes)"
} >>"$REPORT"
say "Done."
echo
cat "$REPORT"
