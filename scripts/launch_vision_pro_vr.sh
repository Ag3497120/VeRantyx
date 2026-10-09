#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
LOG_DIR="${LOG_DIR:-$REPO_ROOT/logs}"

VERANTYX_VR_DRIVE="${VERANTYX_VR_DRIVE:-$HOME/Verantyx_VR_Drive}"
WINEPREFIX="${WINEPREFIX:-$VERANTYX_VR_DRIVE/SteamVR_Prefix_Wine11_Test}"
WINE_BIN="${WINE_BIN:-$REPO_ROOT/VRDriver/wine_launcher.sh}"
DXVK_SOURCE_DIR="${DXVK_SOURCE_DIR:-$VERANTYX_VR_DRIVE/Beer/dxvk/dxvk-macOS-async-v1.10.3-20230507-repack/x64}"
STEAM_ROOT="${STEAM_ROOT:-$WINEPREFIX/drive_c/Program Files (x86)/Steam}"
STEAM_EXE="${STEAM_EXE:-$STEAM_ROOT/steam.exe}"
STEAMVR_DIR="${STEAMVR_DIR:-$STEAM_ROOT/steamapps/common/SteamVR}"
VRSTARTUP_EXE="${VRSTARTUP_EXE:-$STEAMVR_DIR/bin/win64/vrstartup.exe}"
GAME_EXE="${HLA_EXE:-$STEAM_ROOT/steamapps/common/half-life alyx/game/bin/win64/hlvr.exe}"
VR_DRIVER_DIR="${VR_DRIVER_DIR:-$WINEPREFIX/drive_c/vr_driver/verantyx/bin/win64}"
STEAMVR_DRIVER_DIR="${STEAMVR_DRIVER_DIR:-$STEAMVR_DIR/drivers/verantyx/bin/win64}"
DRIVER_LOG="$WINEPREFIX/drive_c/vr_shared_frame.log"
STEAMVR_MANIFEST="$STEAM_ROOT/steamapps/appmanifest_250820.acf"
ENCODER="$BUILD_DIR/HardwareEncoder"
STARTUP_TIMEOUT="${STEAMVR_STARTUP_TIMEOUT:-300}"
STEAM_LOGIN_FILE="$STEAM_ROOT/config/loginusers.vdf"
STEAM_LOGIN_TIMEOUT="${STEAM_LOGIN_TIMEOUT:-600}"
STEAM_LOG_DIR="$STEAM_ROOT/logs"
STEAM_CONNECTION_LOG="$STEAM_LOG_DIR/connection_log.txt"
STEAM_WEBHELPER_LOG="$STEAM_LOG_DIR/webhelper.txt"
STEAM_UI_LOG="$STEAM_LOG_DIR/steamui_html.txt"
WEBHELPER_STARTUP_TIMEOUT="${STEAM_WEBHELPER_STARTUP_TIMEOUT:-120}"
VR_COMPOSITOR_LOG="$STEAM_LOG_DIR/vrcompositor.txt"
VR_SERVER_LOG="$STEAM_LOG_DIR/vrserver.txt"
GAME_CAPTURE_LOG="$WINEPREFIX/drive_c/vr_openvr_capture_proxy.log"
DXVK_LOG_DIR="${DXVK_LOG_DIR:-$LOG_DIR/dxvk}"
USE_DXVK="${VERANTYX_USE_DXVK:-1}"

if [[ ! -x "$WINE_BIN" ]]; then
    echo "Project Windows runtime launcher was not found: $WINE_BIN" >&2
    exit 1
fi
if [[ ! -f "$STEAM_EXE" ]]; then
    echo "Windows Steam was not found: $STEAM_EXE" >&2
    echo "Run ./scripts/install_windows_steam.sh first." >&2
    exit 1
fi
if [[ ! -f "$STEAMVR_DIR/bin/win64/vrserver.exe" ]]; then
    echo "SteamVR runtime files were not found: $STEAMVR_DIR" >&2
    exit 1
fi
if [[ ! -f "$VRSTARTUP_EXE" ]]; then
    echo "SteamVR startup executable was not found: $VRSTARTUP_EXE" >&2
    exit 1
fi
if [[ ! -f "$GAME_EXE" ]]; then
    echo "Half-Life: Alyx executable was not found: $GAME_EXE" >&2
    echo "The transferred installation should be in the Steam library above." >&2
    exit 1
fi
if [[ ! -f "$STEAMVR_MANIFEST" ]]; then
    transferred_manifest="$STEAMVR_DIR/steamapps/appmanifest_250820.acf"
    if [[ ! -f "$transferred_manifest" ]]; then
        echo "SteamVR appmanifest_250820.acf was not found in either Steam location." >&2
        exit 1
    fi
    cp "$transferred_manifest" "$STEAMVR_MANIFEST"
    echo "Registered the transferred SteamVR install with Windows Steam."
fi

if ! command -v swiftc >/dev/null 2>&1; then
    echo "swiftc is required to build the Mac HEVC encoder." >&2
    exit 1
fi

mkdir -p "$BUILD_DIR" "$LOG_DIR"
swiftc -O "$REPO_ROOT/MacEncoder/HardwareEncoder.swift" -o "$ENCODER"
WINEPREFIX="$WINEPREFIX" VERANTYX_VR_DRIVE="$VERANTYX_VR_DRIVE" \
    STEAMVR_SETTINGS="$STEAM_ROOT/config/steamvr.vrsettings" \
    "$REPO_ROOT/scripts/disable_steamvr_home.sh"
"$REPO_ROOT/scripts/build_openvr_driver.sh"
VR_DRIVER_DIR="$VR_DRIVER_DIR" STEAMVR_DRIVER_DIR="$STEAMVR_DRIVER_DIR" \
    "$REPO_ROOT/scripts/install_openvr_driver.sh"
WINEPREFIX="$WINEPREFIX" VERANTYX_VR_DRIVE="$VERANTYX_VR_DRIVE" \
    STEAM_ROOT="$STEAM_ROOT" STEAMVR_DIR="$STEAMVR_DIR" \
    "$REPO_ROOT/scripts/install_steamvr_openvr_api.sh"
"$REPO_ROOT/scripts/build_openvr_capture_proxy.sh"
WINEPREFIX="$WINEPREFIX" VERANTYX_VR_DRIVE="$VERANTYX_VR_DRIVE" \
    STEAM_ROOT="$STEAM_ROOT" STEAMVR_DIR="$STEAMVR_DIR" \
    "$REPO_ROOT/scripts/install_openvr_capture_proxy.sh"
"$REPO_ROOT/scripts/build_steamvr_d3d11_proxy.sh"
WINEPREFIX="$WINEPREFIX" VERANTYX_VR_DRIVE="$VERANTYX_VR_DRIVE" \
    "$REPO_ROOT/scripts/install_steamvr_d3d11_proxy.sh"
"$REPO_ROOT/scripts/build_steamwebhelper_shim.sh"
STEAM_ROOT="$STEAM_ROOT" STEAMWEBHELPER_SHIM="$BUILD_DIR/steamwebhelper_shim.exe" \
    "$REPO_ROOT/scripts/install_steamwebhelper_shim.sh"

if [[ "$USE_DXVK" == "1" ]]; then
    WINEPREFIX="$WINEPREFIX" VERANTYX_VR_DRIVE="$VERANTYX_VR_DRIVE" \
        DXVK_SOURCE_DIR="$DXVK_SOURCE_DIR" "$REPO_ROOT/scripts/install_dxvk_runtime.sh"
    export DXVK_ASYNC="${DXVK_ASYNC:-1}"
    export DXVK_LOG_LEVEL="${DXVK_LOG_LEVEL:-error}"
    export DXVK_LOG_PATH="$DXVK_LOG_DIR"
    mkdir -p "$DXVK_LOG_DIR"
    # The transferred GPTK vendor shims import __wine_unix_call, which Wine 11
    # no longer exports. DXVK does not need those NVIDIA/MetalFX/AMD helpers.
    export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-d3d11,d3d10core=n,b};nvapi,nvapi64,nvngx,nvngx-on-metalfx,atidxx64="
else
    echo "VERANTYX_USE_DXVK=0: using the runtime's built-in D3D translation."
fi

export WINEPREFIX
"$ENCODER" >"$LOG_DIR/hardware-encoder.log" 2>&1 &
ENCODER_PID=$!

cleanup() {
    if kill -0 "$ENCODER_PID" 2>/dev/null; then
        kill -TERM "$ENCODER_PID" 2>/dev/null || true
        wait "$ENCODER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

sleep 1
if ! kill -0 "$ENCODER_PID" 2>/dev/null; then
    echo "The HEVC encoder exited during startup. See $LOG_DIR/hardware-encoder.log" >&2
    exit 1
fi

steam_connection_state() {
    [[ -f "$STEAM_CONNECTION_LOG" ]] || return 1
    rg -o 'Logged On|Logged Off' "$STEAM_CONNECTION_LOG" 2>/dev/null | tail -1
}

steam_client_is_running() {
    # steamwebhelper.exe embeds the Steam executable path in -steampath, so
    # matching only "steam.exe" mistakes an orphaned WebHelper for the client.
    pgrep -f '[s]team\.exe.*(-cef-single-process|-applaunch)' >/dev/null 2>&1
}

steam_is_authenticated() {
    [[ -s "$STEAM_LOGIN_FILE" ]] &&
        steam_client_is_running &&
        [[ "$(steam_connection_state || true)" == "Logged On" ]]
}

dxvk_compositor_feature_ready() {
    local dxvk_log
    # Quiet DXVK levels omit feature-level messages. SteamVR records completed
    # graphics initialization independently, before its Startup Complete marker.
    case "${DXVK_LOG_LEVEL:-error}" in
        warn|error|none)
            rg -q 'Done initializing CGraphicsDevice resources' "$VR_COMPOSITOR_LOG"
            return
            ;;
    esac
    for dxvk_log in "$DXVK_LOG_DIR"/vrcompositor*.log; do
        [[ -f "$dxvk_log" ]] || continue
        [[ "$(stat -f %m "$dxvk_log")" -ge "${STEAMVR_START_EPOCH:-0}" ]] || continue
        rg -q 'Using feature level D3D_FEATURE_LEVEL_11_(0|1)' "$dxvk_log" && return 0
    done
    return 1
}

compositor_startup_ready() {
    [[ -f "$VR_COMPOSITOR_LOG" ]] || return 1
    [[ "$(stat -f %m "$VR_COMPOSITOR_LOG")" -ge "${STEAMVR_START_EPOCH:-0}" ]] || return 1
    # A previous Startup Complete must not validate a session that shut down.
    local last_lifecycle
    last_lifecycle="$(rg -o 'Startup Complete|vrcompositor .*startup with PID=|VR compositor .*starting up|VR compositor shutting down|Failed to init graphics device|VRInitError_Compositor_[A-Za-z0-9_]+' "$VR_COMPOSITOR_LOG" | tail -1)" || return 1
    [[ "$last_lifecycle" == "Startup Complete" ]]
}

steamvr_is_already_ready() {
    pgrep -f '[v]rserver\.exe' >/dev/null 2>&1 &&
        pgrep -f '[v]rcompositor\.exe' >/dev/null 2>&1 &&
        [[ -f "$DRIVER_LOG" ]] && rg -q 'CVerantyxDeviceDriver Activated' "$DRIVER_LOG" &&
        compositor_startup_ready &&
        { [[ "$USE_DXVK" != "1" ]] || dxvk_compositor_feature_ready; }
}

steam_ui_is_ready() {
    for log in "$STEAM_WEBHELPER_LOG" "$STEAM_UI_LOG"; do
        [[ -f "$log" ]] || continue
        if [[ "$(stat -f %m "$log")" -lt "$STEAM_START_EPOCH" ]]; then
            pgrep -f '[s]teamwebhelper(_real)?\.exe' >/dev/null 2>&1 || continue
        fi
        rg -q 'BrowserReady|AfterCreated|Sign in to' "$log" && return 0
    done
    return 1
}

steam_webhelper_failed() {
    for log in "$STEAM_WEBHELPER_LOG" "$STEAM_UI_LOG" "$STEAM_LOG_DIR/cef_log.txt" "$STEAM_LOG_DIR/console_log.txt"; do
        [[ -f "$log" ]] || continue
        [[ "$(stat -f %m "$log")" -ge "$STEAM_START_EPOCH" ]] || continue
        rg -qi 'Timed out waiting for webhelper init|Failed creating offscreen shared JS context|Restart webhelper process, counter 2|Client version: no bootstrapper found' "$log" && return 0
    done
    return 1
}

if ! steam_is_authenticated; then
    STEAM_START_EPOCH="$(date +%s)"
    echo "Starting Windows Steam once for sign-in with the WebHelper compatibility flags."
    "$WINE_BIN" "$STEAM_EXE" -no-cef-sandbox -cef-single-process -noverifyfiles \
        >"$LOG_DIR/steam-client-launch.log" 2>&1 &
    echo "Waiting for Steam WebHelper to initialize before checking sign-in."
    ui_ready=0
    for ((elapsed = 0; elapsed < WEBHELPER_STARTUP_TIMEOUT; elapsed++)); do
        if steam_ui_is_ready; then
            ui_ready=1
            break
        fi
        if steam_webhelper_failed; then
            echo "Steam WebHelper failed to initialize. See $STEAM_WEBHELPER_LOG and $STEAM_UI_LOG." >&2
            tail -40 "$STEAM_WEBHELPER_LOG" >&2 || true
            exit 1
        fi
        if ! kill -0 "$ENCODER_PID" 2>/dev/null; then
            echo "The HEVC encoder stopped while Steam was initializing." >&2
            exit 1
        fi
        sleep 1
    done
    if (( ui_ready == 0 )); then
        echo "Steam's sign-in UI did not become ready within ${WEBHELPER_STARTUP_TIMEOUT}s." >&2
        echo "See $LOG_DIR/steam-client-launch.log and $STEAM_LOG_DIR for diagnostics." >&2
        exit 1
    fi

    echo "Steam sign-in UI is ready. Complete sign-in in the Windows Steam window."
    login_ready=0
    for ((elapsed = 0; elapsed < STEAM_LOGIN_TIMEOUT; elapsed++)); do
        if steam_is_authenticated; then
            login_ready=1
            break
        fi
        if ! kill -0 "$ENCODER_PID" 2>/dev/null; then
            echo "The HEVC encoder stopped while Steam was waiting for sign-in." >&2
            exit 1
        fi
        if (( elapsed > 0 && elapsed % 30 == 0 )); then
            echo "Waiting for Steam sign-in (${elapsed}s/${STEAM_LOGIN_TIMEOUT}s)."
        fi
        sleep 1
    done

    if (( login_ready == 0 )); then
        echo "Steam sign-in was not detected within ${STEAM_LOGIN_TIMEOUT}s." >&2
        echo "Sign in through the Windows Steam client and rerun this script." >&2
        exit 1
    fi
else
    echo "A signed-in Windows Steam session is available for SteamVR and Half-Life: Alyx."
fi

STEAMVR_ALREADY_RUNNING=0
driver_ready=0
compositor_ready=0
if pgrep -f '[v]rserver\.exe' >/dev/null 2>&1 || pgrep -f '[v]rcompositor\.exe' >/dev/null 2>&1; then
    if steamvr_is_already_ready; then
        STEAMVR_ALREADY_RUNNING=1
        driver_ready=1
        compositor_ready=1
        echo "Reusing the running Windows SteamVR session; its Verantyx driver and compositor are ready."
    else
        echo "SteamVR processes are already running, but the server and compositor are not in a verified ready state." >&2
        echo "Stop those SteamVR processes and retry after checking $VR_COMPOSITOR_LOG." >&2
        exit 1
    fi
else
    echo "Starting SteamVR with vrstartup.exe while the Windows Steam client remains active."
    rm -f "$DRIVER_LOG"
    STEAMVR_START_EPOCH="$(date +%s)"
    if [[ "$USE_DXVK" == "1" ]]; then
        rm -f "$DXVK_LOG_DIR"/vrcompositor*.log
    fi
    if [[ -f "$VR_COMPOSITOR_LOG" ]]; then
        cp -p "$VR_COMPOSITOR_LOG" "$LOG_DIR/vrcompositor-before-dxvk-$STEAMVR_START_EPOCH.txt"
        : > "$VR_COMPOSITOR_LOG"
    fi
    "$WINE_BIN" "$VRSTARTUP_EXE" >"$LOG_DIR/steamvr-launch.log" 2>&1 &
fi

for ((elapsed = 0; elapsed < STARTUP_TIMEOUT; elapsed++)); do
    if [[ -f "$DRIVER_LOG" ]] && rg -q "CVerantyxDeviceDriver Activated" "$DRIVER_LOG"; then
        driver_ready=1
    fi
    if (( STEAMVR_ALREADY_RUNNING == 0 )) && [[ -f "$VR_COMPOSITOR_LOG" ]] && \
        tail -n 160 "$VR_COMPOSITOR_LOG" | rg -qi \
        'DX11 level hardware required|Failed to init graphics device|VRInitError_Compositor_|Failed CreateTexture2D|Error allocating MSAA render texture|Failed to create shared frame info constant buffer|CreateMirrorTextures'; then
        echo "SteamVR compositor rejected its D3D11 device. DXVK diagnostics:" >&2
        rg -n 'DXVK:|feature level|Failed|VK_ERROR' "$DXVK_LOG_DIR"/vrcompositor*.log >&2 || true
        tail -30 "$VR_COMPOSITOR_LOG" >&2
        exit 1
    fi
    if (( compositor_ready == 0 )) && pgrep -f '[v]rcompositor\.exe' >/dev/null 2>&1 && \
        compositor_startup_ready && \
        { [[ "$USE_DXVK" != "1" ]] || dxvk_compositor_feature_ready; }; then
        compositor_ready=1
    fi
    if (( driver_ready == 1 && compositor_ready == 1 )); then
        break
    fi
    if ! kill -0 "$ENCODER_PID" 2>/dev/null; then
        echo "The HEVC encoder stopped. See $LOG_DIR/hardware-encoder.log" >&2
        exit 1
    fi
    if (( elapsed > 0 && elapsed % 15 == 0 )); then
        echo "Waiting for SteamVR driver and D3D11 compositor (${elapsed}s/${STARTUP_TIMEOUT}s)…"
    fi
    sleep 1
done

if (( driver_ready == 0 || compositor_ready == 0 )); then
    echo "SteamVR did not verify both the Verantyx driver and D3D 11.0 compositor within ${STARTUP_TIMEOUT}s." >&2
    echo "Driver log: $DRIVER_LOG" >&2
    echo "Compositor log: $VR_COMPOSITOR_LOG" >&2
    echo "DXVK log directory: $DXVK_LOG_DIR" >&2
    [[ ! -f "$DRIVER_LOG" ]] || tail -30 "$DRIVER_LOG" >&2
    [[ ! -f "$VR_COMPOSITOR_LOG" ]] || tail -30 "$VR_COMPOSITOR_LOG" >&2
    exit 1
fi

echo "SteamVR activated the Verantyx driver and completed compositor graphics initialization."
if [[ "${STEAMVR_ONLY:-0}" == "1" ]]; then
    echo "STEAMVR_ONLY=1: SteamVR and the HEVC stream remain active; press Ctrl+C to stop the encoder."
    while pgrep -f '[v]rserver\.exe' >/dev/null 2>&1; do
        if ! pgrep -f '[v]rcompositor\.exe' >/dev/null 2>&1; then
            echo "SteamVR's compositor exited while the server was still running." >&2
            exit 1
        fi
        sleep 2
    done
    exit 0
fi

if [[ "${WAIT_FOR_VISION_PRO_TRACKING:-1}" == "1" ]]; then
    VISION_TRACKING_TIMEOUT="${VISION_TRACKING_TIMEOUT:-300}"
    echo "Waiting for the Vision Pro app to send its first tracked pose before launching Half-Life: Alyx."
    pose_ready=0
    steamvr_restart_started=0
    for ((elapsed = 0; elapsed < VISION_TRACKING_TIMEOUT; elapsed++)); do
        if [[ -f "$LOG_DIR/hardware-encoder.log" ]] && rg -q 'Received Vision Pro pose packet #[1-9]' "$LOG_DIR/hardware-encoder.log"; then
            pose_ready=1
            break
        fi
        if ! kill -0 "$ENCODER_PID" 2>/dev/null; then
            echo "The HEVC encoder stopped while waiting for Vision Pro tracking." >&2
            exit 1
        fi
        if (( elapsed > 0 && elapsed % 15 == 0 )); then
            if pgrep -f '[v]rserver\.exe' >/dev/null 2>&1 && pgrep -f '[v]rcompositor\.exe' >/dev/null 2>&1; then
                steamvr_restart_started=0
            elif ! pgrep -f '[v]rserver\.exe' >/dev/null 2>&1 &&
                 ! pgrep -f '[v]rcompositor\.exe' >/dev/null 2>&1 &&
                 (( steamvr_restart_started == 0 )); then
                echo "SteamVR stopped while waiting for the headset; restarting it so tracking can connect."
                rm -f "$DRIVER_LOG"
                STEAMVR_START_EPOCH="$(date +%s)"
                if [[ -f "$VR_COMPOSITOR_LOG" ]]; then
                    cp -p "$VR_COMPOSITOR_LOG" "$LOG_DIR/vrcompositor-before-restart-$STEAMVR_START_EPOCH.txt"
                    : > "$VR_COMPOSITOR_LOG"
                fi
                "$WINE_BIN" "$VRSTARTUP_EXE" >>"$LOG_DIR/steamvr-launch.log" 2>&1 &
                steamvr_restart_started=1
            fi
            echo "Waiting for Vision Pro tracking (${elapsed}s/${VISION_TRACKING_TIMEOUT}s)…"
        fi
        sleep 1
    done
    if (( pose_ready == 0 )); then
        echo "No Vision Pro pose packet arrived within ${VISION_TRACKING_TIMEOUT}s. Open MBP AVP VR and start stereo VR, then rerun this script." >&2
        exit 1
    fi
    echo "Vision Pro pose packets are arriving. Starting Half-Life: Alyx."
fi

steamvr_home_pid="$(pgrep -f '[s]teamvr_environments[/\\]game[/\\]bin[/\\]win64[/\\]steamtours\.exe' || true)"
if [[ -n "$steamvr_home_pid" ]]; then
    echo "Stopping SteamVR Home before launching Half-Life: Alyx."
    while IFS= read -r pid; do
        [[ -z "$pid" ]] || kill -TERM "$pid" 2>/dev/null || true
    done <<< "$steamvr_home_pid"
    sleep 2
fi

steamvr_room_setup_pid="$(pgrep -f '[s]teamvr_room_setup\.exe' || true)"
if [[ -n "$steamvr_room_setup_pid" ]]; then
    echo "Stopping SteamVR Room Setup before launching Half-Life: Alyx."
    while IFS= read -r pid; do
        [[ -z "$pid" ]] || kill -TERM "$pid" 2>/dev/null || true
    done <<< "$steamvr_room_setup_pid"
    sleep 2
fi

log_checkpoint() {
    stat -f '%i:%z' "$1" 2>/dev/null || printf '0:0\n'
}

log_since_checkpoint() {
    local file="$1" checkpoint="$2" current inode offset
    [[ -f "$file" ]] || return 1
    current="$(log_checkpoint "$file")"
    inode="${checkpoint%%:*}"
    offset="${checkpoint#*:}"
    # Fail closed if a pre-existing log was replaced or truncated during startup.
    # Reading it from byte zero could otherwise reintroduce historical evidence.
    [[ "$inode" == "0" || "${current%%:*}" == "$inode" ]] || return 1
    [[ "${current#*:}" -ge "$offset" ]] || return 1
    tail -c "+$((offset + 1))" "$file"
}

current_game_windows_pid() {
    log_since_checkpoint "$VR_SERVER_LOG" "$GAME_SERVER_CHECKPOINT" | awk '
        /SetApplicationPid appkey=steam[.]app[.]546560 pid=/ {
            value = $0; sub(/^.* pid=/, "", value); sub(/[^0-9].*$/, "", value)
            pid = value
        }
        /AppInfoManager.ProcessDisconnected processid=/ {
            value = $0; sub(/^.* processid=/, "", value); sub(/[^0-9].*$/, "", value)
            if (value == pid) pid = ""
        }
        END { if (pid ~ /^[1-9][0-9]*$/) print pid; else exit 1 }
    '
}

game_stereo_capture_ready() {
    local windows_pid
    [[ -n "${GAME_MAC_PID:-}" ]] && kill -0 "$GAME_MAC_PID" 2>/dev/null || return 1
    windows_pid="$(current_game_windows_pid)" || return 1
    log_since_checkpoint "$GAME_CAPTURE_LOG" "$GAME_CAPTURE_CHECKPOINT" | awk -v pid="$windows_pid" '
        $3 == "pid=" pid && $4 == "Captured" && $5 == "Alyx" &&
        $6 == "eye" && $7 == "pair" && $8 ~ /^[1-9][0-9]*$/ &&
        $9 ~ /^\([1-9][0-9]*x[1-9][0-9]*$/ { ready = 1 }
        END { exit !ready }
    '
}

# Only log records appended after this launch can establish its game PID and
# completed eye-pair capture. Submit/WaitGetPoses alone do not prove rendering.
GAME_SERVER_CHECKPOINT="$(log_checkpoint "$VR_SERVER_LOG")"
GAME_CAPTURE_CHECKPOINT="$(log_checkpoint "$GAME_CAPTURE_LOG")"
GAME_MAC_PID=""
HLA_STARTUP_TIMEOUT="${HLA_STARTUP_TIMEOUT:-300}"
# The installed vr.dll supports these controls. Level 0 is its lowest fidelity
# tier (currently about 800x900 per eye); automatic changes caused oversized
# render targets and repeated readback/encoder resource changes on this Mac.
HLA_FIDELITY_LEVEL="${HLA_FIDELITY_LEVEL:-0}"
if [[ ! "$HLA_FIDELITY_LEVEL" =~ ^[0-8]$ ]]; then
    echo "HLA_FIDELITY_LEVEL must be an integer from 0 through 8." >&2
    exit 1
fi
echo "Launching Half-Life: Alyx through Windows Steam App ID 546560. Waiting for completed stereo frames."
echo "Using fixed Alyx fidelity level $HLA_FIDELITY_LEVEL (automatic resolution changes disabled)."
"$WINE_BIN" "$STEAM_EXE" -no-cef-sandbox -cef-single-process -noverifyfiles \
    -applaunch 546560 -vr -nowindow -novid -console -vconsole -condebug -dx11 \
    -noautofidelity -forcefidelitylevel "$HLA_FIDELITY_LEVEL" \
    +vr_fidelity_level_auto 0 +vr_fidelity_level "$HLA_FIDELITY_LEVEL" \
    +engine_no_focus_sleep 0 \
    >"$LOG_DIR/half-life-alyx.log" 2>&1 &

game_frames_ready=0
for ((elapsed = 0; elapsed < HLA_STARTUP_TIMEOUT; elapsed++)); do
    if [[ -z "$GAME_MAC_PID" ]]; then
        GAME_MAC_PID="$(pgrep -f '[h]lvr\.exe' | head -1 || true)"
        if [[ -n "$GAME_MAC_PID" ]]; then
            echo "Half-Life: Alyx process started; graphics are still initializing (no completed stereo frame confirmed)."
        fi
    elif ! kill -0 "$GAME_MAC_PID" 2>/dev/null; then
        echo "Half-Life: Alyx exited during graphics initialization." >&2
        break
    fi
    if ! kill -0 "$ENCODER_PID" 2>/dev/null || ! pgrep -f '[v]rcompositor\.exe' >/dev/null 2>&1; then
        echo "The encoder or SteamVR compositor stopped while Alyx was initializing." >&2
        break
    fi
    if game_stereo_capture_ready; then
        game_frames_ready=1
        break
    fi
    if (( elapsed > 0 && elapsed % 15 == 0 )); then
        echo "Waiting for Half-Life: Alyx to complete stereo rendering (${elapsed}s/${HLA_STARTUP_TIMEOUT}s)."
    fi
    sleep 1
done

if (( game_frames_ready == 0 )); then
    echo "Half-Life: Alyx startup was not verified: no completed stereo eye pair from this launch was captured." >&2
    echo "Check $LOG_DIR/half-life-alyx.log, $GAME_CAPTURE_LOG, and the game's game/hlvr/console.log for graphics or content errors." >&2
    echo "Current-launch capture diagnostics (historical entries excluded):" >&2
    log_since_checkpoint "$GAME_CAPTURE_LOG" "$GAME_CAPTURE_CHECKPOINT" | tail -30 >&2 || true
    tail -30 "$LOG_DIR/half-life-alyx.log" >&2 || true
    if [[ -z "$GAME_MAC_PID" ]] || ! kill -0 "$GAME_MAC_PID" 2>/dev/null || \
        ! kill -0 "$ENCODER_PID" 2>/dev/null || ! pgrep -f '[v]rcompositor\.exe' >/dev/null 2>&1; then
        exit 1
    fi
    echo "Startup timed out. Keeping the encoder supervised for headset diagnostics until this game exits or Ctrl+C is pressed."
else
    echo "Half-Life: Alyx completed stereo eye-pair capture. The encoder will stay active until the game exits or this script is stopped."
fi

while kill -0 "$GAME_MAC_PID" 2>/dev/null; do
    if ! kill -0 "$ENCODER_PID" 2>/dev/null; then
        echo "The HEVC encoder exited while Half-Life: Alyx was active." >&2
        exit 1
    fi
    if ! pgrep -f '[v]rcompositor\.exe' >/dev/null 2>&1; then
        echo "SteamVR's compositor exited while Half-Life: Alyx was running." >&2
        exit 1
    fi
    if (( game_frames_ready == 0 )) && game_stereo_capture_ready; then
        game_frames_ready=1
        echo "Half-Life: Alyx completed stereo eye-pair capture after the startup timeout."
    fi
    sleep 2
done
echo "Half-Life: Alyx exited."
(( game_frames_ready == 1 ))
