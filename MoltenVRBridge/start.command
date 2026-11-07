#!/bin/bash
set -euo pipefail
# Forward Steam launch options without shell evaluation.
has_auto=0; has_level=0; has_map=0
for argument in "$@"; do
 case "$argument" in
  +vr_fidelity_level_auto) has_auto=1 ;;
  +vr_fidelity_level) has_level=1 ;;
  +map) has_map=1 ;;
 esac
done
if (( !has_auto )); then
 if (( has_level )); then set -- +vr_fidelity_level_auto 0 "$@"
 else set -- +vr_fidelity_level_auto 1 "$@"; fi
fi
if (( !has_map )); then set -- +map "${ALYX_MAP:-startup}" "$@"; fi
if [[ "${ALYX_NATIVE_DRY_RUN:-0}" == 1 ]]; then
 printf 'native-argument: <%s>\n' "$@"
 exit 0
fi
install="$(cd "$(dirname "$0")" && pwd)"
base="$install/verantyx"
test="$base/preparation"
if [[ "${ALYX_SESSION_LOCKED:-0}" != 1 ]]; then
 exec /usr/bin/python3 "$test/session_runner.py" "$0" "$@"
fi
mkdir -p "$test"
# Experimental profile integration disabled; use the native MoltenVR runtime.
unset MVR_NATIVE_AGREEMENT_FILE
if [[ "${STEAM_COMPAT_APP_ID:-${SteamAppId:-}}" != 546560 ]] && ! /usr/bin/pgrep -x steam_osx >/dev/null; then
 echo 'Start native Steam before launching Alyx.'; exit 1
fi
game="$base/native-game"
logs=$(mktemp -d "${TMPDIR:-/tmp}/alyx-native.XXXXXX")
if pgrep -f "$base/alyx_host" >/dev/null; then
 echo 'Close the previous Alyx session before starting another.';exit 1
fi
if pgrep -f '[/]HardwareEncoder' >/dev/null; then
 echo 'Close the previous VeRantyx encoder before starting Alyx.';exit 1
fi
export KL_MVK_DIR="$base/moltenvk-game"
export VERANTYX_SHADER_DIR="$base"
export VERANTYX_SETTINGS_FILE="$base/native-vr-settings.txt"
export VERANTYX_SHARED_FRAME="$test/vr_shared_frame.dat"
export VERANTYX_BRIDGE_READY="$logs/ready"
export MVK_CONFIG_LOG_LEVEL=0
export ALYX_ENGINE_LOG=0
export VERANTYX_QUIET=1
export KL_VK_TRACE=0
export KL_TRACE=0
unset KL_TRACE_FS ALYX_SETTINGS_TRACE KL_VERBOSE_ALL KL_FULL
unset ALYX_PIPELINE_RECORDER_LIBRARY ALYX_PIPELINE_RECORD_FILE
# Report the actual Mac memory through the Linux compatibility layer.
mem_bytes=$(/usr/sbin/sysctl -n hw.memsize)
export KL_MEM_TOTAL_MB="$((mem_bytes / 1048576))"
export KL_VK_ASYNC_CACHE_DIR="$test/cache/workers"
mkdir -p "$KL_VK_ASYNC_CACHE_DIR"
export VERANTYX_ASYNC_PIPELINES=1
export KL_VK_ASYNC_PIPELINES=1
unset KL_VK_WAIT_FIRST_USE
# Persistent converted shader archive, separate from the previous cache format.
export KL_VK_PIPELINE_ARCHIVE="$test/cache/native.bin"
mkdir -p "$test/cache"
mkdir -p "$test/cache/tmp" "$test/cache/xdg"
export XDG_CACHE_HOME="$test/cache/xdg"
export TMPDIR="$test/cache/tmp/"
export KL_VK_PRIVATE_CACHE=1
export KL_VK_ALYX_STATIC_COLOR_WRITE=1
export MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0
export VERANTYX_NATIVE_EXPOSURE_EV=-1
export VERANTYX_NATIVE_QUIT_FILE="$logs/quit.request"
export VERANTYX_NATIVE_LOADING_FILE="$logs/loading.json"
export VERANTYX_NATIVE_STAGE_FILE="$logs/stage.json"
export VERANTYX_NATIVE_GAME_DIR="$install/game/hlvr"
export VERANTYX_LOADING_LIBRARY="$base/libalyxloading.dylib"
export VERANTYX_LOADING_CAPABILITY_FILE="$logs/loading-capability"
export KL_VK_FOZ_REPLAY="$test/alyx.foz"
if [[ "${ALYX_NATIVE_EXTENDED_PREWARM:-0}" == 1 && -s "$test/supplemental.foz" ]]; then
 export KL_VK_FOZ_REPLAY="$KL_VK_FOZ_REPLAY:$test/supplemental.foz"
fi
export KL_VK_FOZ_RECORD="$test/recorded.next.foz"
export KL_VK_FOZ_DIR="$test"
export KL_VK_FOZ_RECORD_SYNC=0
export KL_VK_FOZ_REPLAY_BLOCK=1
export KL_VK_FOZ_THREADS="${ALYX_NATIVE_PREPARE_THREADS:-3}"
export KL_VK_ASYNC_THREADS="${ALYX_NATIVE_PREPARE_THREADS:-3}"
# Preparation can use the CPU; during play, preserve the original worker budget.
export KL_VK_ASYNC_GAME_THREADS=3
export KL_VK_FOZ_REBUILD_CACHE=1
export KL_VK_FOZ_SLOW_MS=0
export ALYX_PREWARM_PROGRESS="$test/progress.json"
unset ALYX_V2_COMPILE_METRICS
printf '%s\n' '{"state":0,"phase":"checking","done":0,"total":0}' > "$ALYX_PREWARM_PROGRESS"
# Recover complete recordings from an interrupted previous session.
if [[ -s "$test/recorded.next.foz" ]]; then
 /usr/bin/python3 "$test/merge_recordings.py" "$test/recorded.foz" "$test/recorded.next.foz"
fi
# Read the previous session's recording while writing the new session separately.
if [[ -s "$test/recorded.foz" ]]; then
 export KL_VK_FOZ_REPLAY="$KL_VK_FOZ_REPLAY:$test/recorded.foz"
fi


export VERANTYX_BENCH=0
unset VERANTYX_EYE_WIDTH VERANTYX_EYE_HEIGHT
unset VERANTYX_OUTPUT_EYE_WIDTH VERANTYX_OUTPUT_EYE_HEIGHT
export VERANTYX_RUNTIME_RESOLUTION_FILE="$logs/runtime-resolution.txt"
export VERANTYX_DISPLAY_HZ=72
export VERANTYX_SYNC_TRACKING=1
export VERANTYX_LOCAL_ROTATION_WARP="${VERANTYX_LOCAL_ROTATION_WARP:-1}"
export VB_FPS=72
export MOLTENVR_FPS_FILE="$logs/runtime-fps"
printf '%s\n' "$VERANTYX_DISPLAY_HZ" > "$MOLTENVR_FPS_FILE"
unset VERANTYX_SUBMIT_CURRENT_POSE
export VERANTYX_SHARED_FRUSTUM=1
unset VERANTYX_IPD
rm -f "$VERANTYX_BRIDGE_READY" "$VERANTYX_RUNTIME_RESOLUTION_FILE"
bridge_pid='';game_pid='';settings_pid='';progress_pid=''

cleanup(){
 if [[ -n "$game_pid" ]] && kill -0 "$game_pid" 2>/dev/null;then
  echo 'Closing Alyx normally and saving its shader cache...'
  printf 'quit\n' > "$VERANTYX_NATIVE_QUIT_FILE.tmp"
  mv "$VERANTYX_NATIVE_QUIT_FILE.tmp" "$VERANTYX_NATIVE_QUIT_FILE"
  closing_seconds=0
  while kill -0 "$game_pid" 2>/dev/null; do
   sleep 1
   closing_seconds=$((closing_seconds+1))
   if (( closing_seconds == 30 )); then
    echo 'Alyx is still closing; waiting for it to finish saving.'
   fi
  done
  wait "$game_pid" 2>/dev/null || true
 fi
 if [[ -n "$progress_pid" ]]; then kill -TERM "$progress_pid" 2>/dev/null || true; wait "$progress_pid" 2>/dev/null || true; fi
 if [[ -s "$test/recorded.next.foz" ]]; then
  /usr/bin/python3 "$test/merge_recordings.py" "$test/recorded.foz" "$test/recorded.next.foz"
 fi
 if [[ -n "$settings_pid" ]];then kill -TERM "$settings_pid" 2>/dev/null || true;wait "$settings_pid" 2>/dev/null || true;fi
 if [[ -n "$bridge_pid" ]];then kill -TERM "$bridge_pid" 2>/dev/null || true;wait "$bridge_pid" 2>/dev/null || true;fi
 rm -rf "$logs"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
"$base/bridge" > >( /usr/bin/python3 "$test/bridge_log.py" "$test/bridge-startup.log" ) 2>&1 &
bridge_pid=$!
# Runtime configuration is available without a worn headset. Never gate the
# playable process on tracking, a stable pose, or a separate preparation guest.
for _ in {1..50}; do
 [[ -s "$VERANTYX_RUNTIME_RESOLUTION_FILE" ]] && break
 if ! kill -0 "$bridge_pid" 2>/dev/null; then echo 'MoltenVR runtime could not initialize.'; exit 1; fi
 sleep 0.1
done
if [[ ! -s "$VERANTYX_RUNTIME_RESOLUTION_FILE" ]]; then
 echo 'MoltenVR runtime configuration unavailable. Start MoltenVR and retry.'; exit 1
fi
/usr/bin/python3 "$test/manage_cache.py" verify
printf '%s\n' '{"state":0,"phase":"initializing","done":0,"total":0}' > "$ALYX_PREWARM_PROGRESS"
read -r runtime_width runtime_height < "$VERANTYX_RUNTIME_RESOLUTION_FILE"
if ! [[ "$runtime_width" =~ ^[0-9]+$ && "$runtime_height" =~ ^[0-9]+$ ]] || (( runtime_width < 1 || runtime_width > 8192 || runtime_height < 1 || runtime_height > 8192 )); then
 echo 'Invalid MoltenVR runtime resolution.'; exit 1
fi
export VERANTYX_OUTPUT_EYE_WIDTH="$runtime_width"
export VERANTYX_OUTPUT_EYE_HEIGHT="$runtime_height"
echo "MoltenVR resolution: ${runtime_width}x${runtime_height} per eye"
# Do not make the guest wait for an unavailable optional stage renderer.
if [[ ! -s "$VERANTYX_LOADING_CAPABILITY_FILE" ]]; then unset VERANTYX_NATIVE_STAGE_FILE; fi
/usr/bin/python3 "$base/normalize_save_names.py" "$game/game/hlvr/save"
cd "$game"
"$base/alyx_host" --libdir "$base/guest-lib" --game "$game" -- -vr -steam -novid -net_no_parallel_build -noasserts -nopassiveasserts -vulkan -nowindow +g_ragdoll_maxcount 8 "$@" > >( /usr/bin/python3 "$test/filter_log.py" "$test/prewarm.log" ) 2>&1 &
game_pid=$!
/usr/bin/python3 "$test/show_progress.py" "$ALYX_PREWARM_PROGRESS" "$game_pid" &
progress_pid=$!
echo "Native Alyx → MoltenVR. Preparing known shaders before play."
echo 'Stop: Ctrl+C. Close through the game menu to save settings and cache.'
while kill -0 "$game_pid" 2>/dev/null && kill -0 "$bridge_pid" 2>/dev/null;do
 sleep 5

done
echo 'Alyx session ended.'
