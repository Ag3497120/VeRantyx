#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
LOG_DIR="${LOG_DIR:-$REPO_ROOT/logs}"

VERANTYX_VR_DRIVE="${VERANTYX_VR_DRIVE:-$HOME/Verantyx_VR_Drive}"
WINEPREFIX="${WINEPREFIX:-$VERANTYX_VR_DRIVE/SteamVR_Prefix_Wine11_Test}"
WINE_BIN="${WINE_BIN:-$REPO_ROOT/VRDriver/wine_launcher.sh}"
WINE_RUNTIME_BIN_RESOLVED="${WINE_RUNTIME_BIN:-$VERANTYX_VR_DRIVE/Beer/wine/WineStaging.app/Contents/Resources/wine/bin/wine}"
DXVK_SOURCE_DIR="${DXVK_SOURCE_DIR:-$VERANTYX_VR_DRIVE/Beer/dxvk/dxvk-macOS-async-v1.10.3-20230507-repack/x64}"
STEAM_ROOT="${STEAM_ROOT:-$WINEPREFIX/drive_c/Program Files (x86)/Steam}"
STEAM_EXE="${STEAM_EXE:-$STEAM_ROOT/steam.exe}"
STEAMVR_DIR="${STEAMVR_DIR:-$STEAM_ROOT/steamapps/common/SteamVR}"
VRSTARTUP_EXE="${VRSTARTUP_EXE:-$STEAMVR_DIR/bin/win64/vrstartup.exe}"
# VERANTYX_APP picks the SteamVR title: alyx (default), earth (Google Earth VR),
# or a Steam app ID with VERANTYX_APP_EXE set to its .exe inside the prefix.
# Only Alyx gets the Alyx-specific steps (launch options, fidelity, dxvk.conf).
VERANTYX_APP="${VERANTYX_APP:-alyx}"
case "$VERANTYX_APP" in
    alyx)
        APP_ID=546560; APP_NAME="Half-Life: Alyx"
        GAME_EXE="${HLA_EXE:-$STEAM_ROOT/steamapps/common/half-life alyx/game/bin/win64/hlvr.exe}" ;;
    earth)
        APP_ID=348250; APP_NAME="Google Earth VR"
        GAME_EXE="${VERANTYX_APP_EXE:-$STEAM_ROOT/steamapps/common/EarthVR/Earth.exe}" ;;
    *)
        if [[ ! "$VERANTYX_APP" =~ ^[0-9]+$ || -z "${VERANTYX_APP_EXE:-}" ]]; then
            echo "VERANTYX_APP must be alyx, earth, or a Steam app ID together with VERANTYX_APP_EXE (its .exe in the prefix)." >&2
            exit 1
        fi
        APP_ID="$VERANTYX_APP"; APP_NAME="${VERANTYX_APP_NAME:-Steam app $APP_ID}"
        GAME_EXE="$VERANTYX_APP_EXE" ;;
esac
IS_ALYX=0; [[ "$VERANTYX_APP" == "alyx" ]] && IS_ALYX=1
GAME_BIN_DIR="$(dirname "$GAME_EXE")"
# pgrep -f pattern for the game's process; the bracket keeps pgrep from matching itself.
GAME_EXE_BASE="$(basename "$GAME_EXE")"
GAME_PROC="[${GAME_EXE_BASE:0:1}]$(printf '%s' "${GAME_EXE_BASE:1}" | sed 's/[.]/\\./g')"
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
# VERANTYX_BENCH=1 runs the game without a headset: the driver publishes
# synthetic poses and this script does not wait for the Vision Pro app.
# VERANTYX_BENCH_MAP loads that map straight away (scripts/bench_alyx.sh).
VERANTYX_BENCH="${VERANTYX_BENCH:-0}"
export VERANTYX_BENCH
# Defaults for this Mac's hardware class (scripts/mac_profile.sh); anything
# already set in the environment wins. VERANTYX_MAC_PROFILE=0 skips it.
if [[ "${VERANTYX_MAC_PROFILE:-1}" == "1" ]]; then
    eval "$("$REPO_ROOT/scripts/mac_profile.sh")"
    echo "Mac profile: $("$REPO_ROOT/scripts/mac_profile.sh" --describe | sed -n 2p | sed 's/Tier: //') tier (render scale $VERANTYX_RENDER_TARGET_SCALE, display $VERANTYX_DISPLAY_HZ Hz, HEVC $VERANTYX_BITRATE_MBPS Mbps, stream ${VERANTYX_STREAM_WIDTH}x${VERANTYX_STREAM_HEIGHT}${HLA_FIDELITY_LEVEL:+, Alyx fidelity $HLA_FIDELITY_LEVEL})"
fi

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
    echo "$APP_NAME executable was not found: $GAME_EXE" >&2
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
"$REPO_ROOT/scripts/build_openvr_capture_proxy.sh"
if pgrep -f "$GAME_PROC" >/dev/null 2>&1; then
    # Windows locks a loaded DLL, and the running game already has the proxy.
    echo "$APP_NAME is running; keeping its loaded OpenVR capture proxy (a rebuilt proxy is installed on the next game launch)."
else
    WINEPREFIX="$WINEPREFIX" VERANTYX_VR_DRIVE="$VERANTYX_VR_DRIVE" \
        STEAM_ROOT="$STEAM_ROOT" STEAMVR_DIR="$STEAMVR_DIR" HLA_GAME_BIN="$GAME_BIN_DIR" \
        "$REPO_ROOT/scripts/install_steamvr_openvr_api.sh"
    WINEPREFIX="$WINEPREFIX" VERANTYX_VR_DRIVE="$VERANTYX_VR_DRIVE" \
        STEAM_ROOT="$STEAM_ROOT" STEAMVR_DIR="$STEAMVR_DIR" HLA_GAME_BIN="$GAME_BIN_DIR" \
        "$REPO_ROOT/scripts/install_openvr_capture_proxy.sh"
fi
"$REPO_ROOT/scripts/build_steamvr_d3d11_proxy.sh"
WINEPREFIX="$WINEPREFIX" VERANTYX_VR_DRIVE="$VERANTYX_VR_DRIVE" \
    "$REPO_ROOT/scripts/install_steamvr_d3d11_proxy.sh"

# DXVK names an Apple GPU "NVIDIA GeForce 6800" (its fallback for an unknown
# vendor), so Alyx shows its "below the recommended specification" dialog on
# every start. A dxvk.conf beside hlvr.exe gives the game a current GPU name
# instead; the compositor's own DXVK is not affected. Read at game start.
HLA_DXVK_CONF="$(dirname "$GAME_EXE")/dxvk.conf"
if (( IS_ALYX == 0 )); then
    :   # the GPU-name dxvk.conf is Alyx's
elif [[ "${VERANTYX_DXVK_GPU_SPOOF:-1}" == "1" ]]; then
    hla_dxvk_conf_content="$(printf '%s\n' \
        '# Written by VeRantyx (launch_vision_pro_vr.sh); VERANTYX_DXVK_GPU_SPOOF=0 removes it.' \
        'dxgi.customVendorId = 10de' \
        'dxgi.customDeviceId = 2206' \
        "dxgi.customDeviceDesc = \"${VERANTYX_DXVK_GPU_NAME:-NVIDIA GeForce RTX 3080}\"" \
        'dxvk.enableAsync = True')"
    if [[ ! -f "$HLA_DXVK_CONF" ]] || [[ "$(cat "$HLA_DXVK_CONF")" != "$hla_dxvk_conf_content" ]]; then
        printf '%s\n' "$hla_dxvk_conf_content" >"$HLA_DXVK_CONF.verantyx-tmp" && mv -f "$HLA_DXVK_CONF.verantyx-tmp" "$HLA_DXVK_CONF"
        echo "Wrote $HLA_DXVK_CONF so Alyx sees a current GPU name instead of DXVK's GeForce 6800 placeholder."
    fi
elif [[ -f "$HLA_DXVK_CONF" ]] && grep -q 'Written by VeRantyx' "$HLA_DXVK_CONF"; then
    rm -f "$HLA_DXVK_CONF"
fi
# The adapter description the game sees comes from Wine's own DXGI (the DXVK
# package has no dxgi.dll), which takes the GPU identity from these registry
# values: 0x10de/0x2206 is an RTX 3080 in wined3d's table.
if [[ "${VERANTYX_DXVK_GPU_SPOOF:-1}" == "1" ]]; then
    if ! WINEDEBUG=-all "$WINE_BIN" reg query 'HKCU\Software\Wine\Direct3D' /v VideoPciDeviceID 2>/dev/null | grep -q '0x2206'; then
        WINEDEBUG=-all "$WINE_BIN" reg add 'HKCU\Software\Wine\Direct3D' /v VideoPciVendorID /t REG_DWORD /d 0x10de /f >/dev/null 2>&1 || true
        WINEDEBUG=-all "$WINE_BIN" reg add 'HKCU\Software\Wine\Direct3D' /v VideoPciDeviceID /t REG_DWORD /d 0x2206 /f >/dev/null 2>&1 || true
        echo "Set Wine's Direct3D GPU identity to an RTX 3080 so Alyx stops asking about the specification."
    fi
fi
STEAM_WEBHELPER_EXE="$STEAM_ROOT/bin/cef/cef.win64/steamwebhelper.exe"
if [[ ! -f "$STEAM_WEBHELPER_EXE" ]]; then
    # SteamSetup.exe only installs the bootstrapper; the client (including
    # the WebHelper the shim replaces) is downloaded on Steam's first run.
    echo "Fresh Windows Steam: letting it download its client files once (a few hundred MB)."
    mkdir -p "$LOG_DIR"
    "$WINE_BIN" "$STEAM_EXE" -no-cef-sandbox -cef-single-process >"$LOG_DIR/steam-bootstrap.log" 2>&1 &
    bootstrap_ready=0
    for ((elapsed = 0; elapsed < 1200; elapsed++)); do
        if [[ -f "$STEAM_WEBHELPER_EXE" ]]; then
            # The file appears before the update is complete; wait for it to settle.
            size_before="$(stat -f %z "$STEAM_WEBHELPER_EXE")"
            sleep 20
            if [[ "$(stat -f %z "$STEAM_WEBHELPER_EXE")" == "$size_before" ]] && \
                [[ -f "$STEAM_ROOT/bin/cef/cef.win64/steamwebhelper.exe" ]]; then
                bootstrap_ready=1
                break
            fi
        fi
        if (( elapsed > 0 && elapsed % 30 == 0 )); then
            echo "Waiting for Steam to finish its first update (${elapsed}s/1200s)…"
        fi
        sleep 1
    done
    echo "Stopping Steam after its first update so the WebHelper shim can be installed."
    "$(dirname "$WINE_RUNTIME_BIN_RESOLVED")/wineserver" -k 2>/dev/null || true
    sleep 5
    if (( bootstrap_ready == 0 )); then
        echo "Steam did not finish downloading its client within 1200s. See $LOG_DIR/steam-bootstrap.log" >&2
        exit 1
    fi
fi
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

# VERANTYX_D3D_BACKEND=dxmt: the game renders through DXMT (D3D11 → Metal in
# one layer) instead of DXVK → MoltenVK; SteamVR's compositor keeps DXVK.
# The game inherits its environment from the Steam client, so Steam is
# restarted when the backend differs from the one it was started with.
D3D_BACKEND="${VERANTYX_D3D_BACKEND:-dxvk}"
if [[ "$D3D_BACKEND" == "dxmt" ]]; then
    dxmt_output="$(WINEPREFIX="$WINEPREFIX" VERANTYX_VR_DRIVE="$VERANTYX_VR_DRIVE" STEAM_ROOT="$STEAM_ROOT" \
        DXVK_SOURCE_DIR="$DXVK_SOURCE_DIR" "$REPO_ROOT/scripts/install_dxmt.sh")"
    printf '%s\n' "$dxmt_output" | grep -v '^DXMT_UNIX_DIR='
    DXMT_UNIX_DIR="$(printf '%s\n' "$dxmt_output" | sed -n 's/^DXMT_UNIX_DIR=//p')"
    export WINEDLLPATH_PREPEND="$DXMT_UNIX_DIR${WINEDLLPATH_PREPEND:+:$WINEDLLPATH_PREPEND}"
    export WINEDLLOVERRIDES="dxgi=n,b;${WINEDLLOVERRIDES:-}"
    export DXMT_LOG_LEVEL="${DXMT_LOG_LEVEL:-error}"
    export DXMT_LOG_PATH="${DXMT_LOG_PATH:-$LOG_DIR}"
elif [[ -f "$STEAM_ROOT/steamapps/common/Half-Life Alyx/game/bin/win64/.verantyx-dxmt" ]] && ! pgrep -f "$GAME_PROC" >/dev/null 2>&1; then
    WINEPREFIX="$WINEPREFIX" VERANTYX_VR_DRIVE="$VERANTYX_VR_DRIVE" STEAM_ROOT="$STEAM_ROOT" \
        DXVK_SOURCE_DIR="$DXVK_SOURCE_DIR" "$REPO_ROOT/scripts/install_dxmt.sh" remove
fi
# VERANTYX_D3D_BACKEND=d3dmetal: Apple's D3DMetal from your own Game Porting
# Toolkit copy (GPTK_ROOT) for the whole prefix, as the project ran in June.
if [[ "$D3D_BACKEND" == "d3dmetal" ]]; then
    if [[ "$USE_DXVK" == "1" ]]; then
        echo "Note: VERANTYX_D3D_BACKEND=d3dmetal replaces the DXVK DLLs installed above for this prefix."
    fi
    VERANTYX_VR_DRIVE="$VERANTYX_VR_DRIVE" "$REPO_ROOT/scripts/install_d3dmetal.sh"
    # Builtin only: the DXVK d3d11.dll still in system32 must not be picked.
    export WINEDLLOVERRIDES="d3d10,d3d10_1,d3d10core,d3d11,d3d12,d3d12core,dxgi=b;nvapi,nvapi64,nvngx,nvngx-on-metalfx,atidxx64="
    export D3DM_SUPPORT_DXR="${D3DM_SUPPORT_DXR:-0}"
elif [[ -f "$VERANTYX_VR_DRIVE/Beer/wine/WineStaging.app/Contents/Resources/wine/lib/wine/.verantyx-d3dmetal" ]] && \
    ! pgrep -f "[v]rcompositor\.exe|$GAME_PROC" >/dev/null 2>&1; then
    VERANTYX_VR_DRIVE="$VERANTYX_VR_DRIVE" "$REPO_ROOT/scripts/install_d3dmetal.sh" remove
fi
STEAM_BACKEND_MARKER="$LOG_DIR/.steam-d3d-backend"
if pgrep -f '[s]team\.exe' >/dev/null 2>&1 && [[ -f "$STEAM_BACKEND_MARKER" ]] && \
    [[ "$(cat "$STEAM_BACKEND_MARKER")" != "$D3D_BACKEND" ]] && ! pgrep -f "$GAME_PROC" >/dev/null 2>&1; then
    echo "Windows Steam was started for the $(cat "$STEAM_BACKEND_MARKER") backend; restarting it for $D3D_BACKEND."
    pkill -TERM -f '[v]rstartup\.exe|[v]rmonitor\.exe|[v]rserver\.exe|[v]rcompositor\.exe|[v]rwebhelper|[v]rdashboard' 2>/dev/null || true
    pkill -TERM -f '[s]teamwebhelper|[s]team\.exe' 2>/dev/null || true
    for ((waited = 0; waited < 30; waited++)); do
        pgrep -f '[s]team\.exe|[v]rserver\.exe' >/dev/null 2>&1 || break
        sleep 1
    done
    pkill -KILL -f '[s]teamwebhelper|[s]team\.exe|[v]rserver\.exe|[v]rcompositor\.exe' 2>/dev/null || true
    sleep 2
fi
mkdir -p "$LOG_DIR"
pgrep -f '[s]team\.exe' >/dev/null 2>&1 || echo "$D3D_BACKEND" >"$STEAM_BACKEND_MARKER"

export WINEPREFIX
# An encoder left over from an interrupted run still holds UDP port 9999.
if pgrep -f "$ENCODER" >/dev/null 2>&1; then
    echo "Stopping a previous HEVC encoder."
    pkill -TERM -f "$ENCODER" || true
    sleep 2
    pkill -KILL -f "$ENCODER" 2>/dev/null || true
    sleep 1
fi
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
    grep -aEo 'Logged On|Logged Off' "$STEAM_CONNECTION_LOG" 2>/dev/null | tail -1
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
            grep -Eq 'Done initializing CGraphicsDevice resources' "$VR_COMPOSITOR_LOG"
            return
            ;;
    esac
    for dxvk_log in "$DXVK_LOG_DIR"/vrcompositor*.log; do
        [[ -f "$dxvk_log" ]] || continue
        [[ "$(stat -f %m "$dxvk_log")" -ge "${STEAMVR_START_EPOCH:-0}" ]] || continue
        grep -Eq 'Using feature level D3D_FEATURE_LEVEL_11_(0|1)' "$dxvk_log" && return 0
    done
    return 1
}

compositor_startup_ready() {
    [[ -f "$VR_COMPOSITOR_LOG" ]] || return 1
    [[ "$(stat -f %m "$VR_COMPOSITOR_LOG")" -ge "${STEAMVR_START_EPOCH:-0}" ]] || return 1
    # A previous Startup Complete must not validate a session that shut down.
    local last_lifecycle
    last_lifecycle="$(grep -aEo 'Startup Complete|vrcompositor .*startup with PID=|VR compositor .*starting up|VR compositor shutting down|Failed to init graphics device|VRInitError_Compositor_[A-Za-z0-9_]+' "$VR_COMPOSITOR_LOG" | tail -1)" || return 1
    [[ "$last_lifecycle" == "Startup Complete" ]]
}

steamvr_is_already_ready() {
    pgrep -f '[v]rserver\.exe' >/dev/null 2>&1 &&
        pgrep -f '[v]rcompositor\.exe' >/dev/null 2>&1 &&
        [[ -f "$DRIVER_LOG" ]] && grep -Eq 'CVerantyxDeviceDriver Activated' "$DRIVER_LOG" &&
        compositor_startup_ready &&
        { [[ "$USE_DXVK" != "1" ]] || dxvk_compositor_feature_ready; }
}

steam_ui_is_ready() {
    for log in "$STEAM_WEBHELPER_LOG" "$STEAM_UI_LOG"; do
        [[ -f "$log" ]] || continue
        if [[ "$(stat -f %m "$log")" -lt "$STEAM_START_EPOCH" ]]; then
            pgrep -f '[s]teamwebhelper(_real)?\.exe' >/dev/null 2>&1 || continue
        fi
        grep -Eq 'BrowserReady|AfterCreated|Sign in to' "$log" && return 0
    done
    return 1
}

steam_webhelper_failed() {
    for log in "$STEAM_WEBHELPER_LOG" "$STEAM_UI_LOG" "$STEAM_LOG_DIR/cef_log.txt" "$STEAM_LOG_DIR/console_log.txt"; do
        [[ -f "$log" ]] || continue
        [[ "$(stat -f %m "$log")" -ge "$STEAM_START_EPOCH" ]] || continue
        grep -Eqi 'Timed out waiting for webhelper init|Failed creating offscreen shared JS context|Restart webhelper process, counter 2|Client version: no bootstrapper found' "$log" && return 0
    done
    return 1
}

# Proven options (Alyx ran an hour with them on 2026-09-30). Stored as Alyx's
# Steam launch options too, because Steam starts Alyx by itself and ignores
# our launch request while it does. Resolution is limited through SteamVR's
# render target scale instead of Alyx's fidelity flags, which made Alyx exit
# during startup.
HLA_BASE_OPTIONS=(-vr -nowindow -novid -console -vconsole -condebug -dx11)
set_options_status=0
if (( IS_ALYX == 1 )); then
WINEPREFIX="$WINEPREFIX" STEAM_ROOT="$STEAM_ROOT" \
    "$REPO_ROOT/scripts/set_alyx_launch_options.sh" "${HLA_BASE_OPTIONS[*]}" || set_options_status=$?
fi
if (( set_options_status == 3 )) && ! pgrep -f "$GAME_PROC" >/dev/null 2>&1; then
    echo "Stopping Windows Steam to store Alyx's launch options and overlay setting."
    pkill -TERM -f '[v]rstartup\.exe|[v]rmonitor\.exe|[v]rserver\.exe|[v]rcompositor\.exe|[v]rwebhelper|[v]rdashboard' 2>/dev/null || true
    pkill -TERM -f '[s]teamwebhelper|[s]team\.exe' 2>/dev/null || true
    for ((waited = 0; waited < 30; waited++)); do
        pgrep -f '[s]team\.exe|[v]rserver\.exe' >/dev/null 2>&1 || break
        sleep 1
    done
    pkill -KILL -f '[s]teamwebhelper|[s]team\.exe|[v]rserver\.exe|[v]rcompositor\.exe' 2>/dev/null || true
    sleep 2
    WINEPREFIX="$WINEPREFIX" STEAM_ROOT="$STEAM_ROOT" \
        "$REPO_ROOT/scripts/set_alyx_launch_options.sh" "${HLA_BASE_OPTIONS[*]}" || true
elif (( set_options_status != 0 && set_options_status != 3 )); then
    exit "$set_options_status"
fi

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
    echo "A signed-in Windows Steam session is available for SteamVR and $APP_NAME."
fi

STEAMVR_ALREADY_RUNNING=0
driver_ready=0
compositor_ready=0
if pgrep -f '[v]rserver\.exe' >/dev/null 2>&1 && [[ "$VERANTYX_BENCH" != "1" ]] && \
    [[ -f "$DRIVER_LOG" ]] && grep -q 'VERANTYX_BENCH=1' "$DRIVER_LOG"; then
    # A SteamVR left over from scripts/bench_alyx.sh would keep publishing
    # synthetic poses whenever the headset pauses. Start a fresh one.
    echo "Stopping the SteamVR session that scripts/bench_alyx.sh started."
    pkill -TERM -f '[v]rstartup\.exe|[v]rmonitor\.exe|[v]rserver\.exe|[v]rcompositor\.exe|[v]rwebhelper|[v]rdashboard' 2>/dev/null || true
    for ((waited = 0; waited < 20; waited++)); do
        pgrep -f '[v]rserver\.exe|[v]rcompositor\.exe' >/dev/null 2>&1 || break
        sleep 1
    done
    pkill -KILL -f '[v]rserver\.exe|[v]rcompositor\.exe|[v]rmonitor\.exe' 2>/dev/null || true
fi
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
    if [[ -f "$DRIVER_LOG" ]] && grep -Eq "CVerantyxDeviceDriver Activated" "$DRIVER_LOG"; then
        driver_ready=1
    fi
    if (( STEAMVR_ALREADY_RUNNING == 0 )) && [[ -f "$VR_COMPOSITOR_LOG" ]] && \
        tail -n 160 "$VR_COMPOSITOR_LOG" | grep -Eqi \
        'DX11 level hardware required|Failed to init graphics device|VRInitError_Compositor_|Failed CreateTexture2D|Error allocating MSAA render texture|Failed to create shared frame info constant buffer|CreateMirrorTextures'; then
        echo "SteamVR compositor rejected its D3D11 device. DXVK diagnostics:" >&2
        grep -En 'DXVK:|feature level|Failed|VK_ERROR' "$DXVK_LOG_DIR"/vrcompositor*.log >&2 || true
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

if [[ "$VERANTYX_BENCH" == "1" ]]; then
    echo "VERANTYX_BENCH=1: not waiting for the Vision Pro app; the driver supplies synthetic poses."
elif [[ "${WAIT_FOR_VISION_PRO_TRACKING:-1}" == "1" ]]; then
    VISION_TRACKING_TIMEOUT="${VISION_TRACKING_TIMEOUT:-1200}"
    echo "Waiting for the Vision Pro app to send its first tracked pose before launching $APP_NAME."
    pose_ready=0
    steamvr_restart_started=0
    for ((elapsed = 0; elapsed < VISION_TRACKING_TIMEOUT; elapsed++)); do
        if [[ -f "$LOG_DIR/hardware-encoder.log" ]] && grep -Eq 'Received Vision Pro pose packet #[1-9]' "$LOG_DIR/hardware-encoder.log"; then
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
        echo "No Vision Pro pose packet arrived within ${VISION_TRACKING_TIMEOUT}s. Open VeRantyx and start stereo VR, then rerun this script." >&2
        exit 1
    fi
    echo "Vision Pro pose packets are arriving. Starting $APP_NAME."
fi

steamvr_home_pid="$(pgrep -f '[s]teamvr_environments[/\\]game[/\\]bin[/\\]win64[/\\]steamtours\.exe' || true)"
if [[ -n "$steamvr_home_pid" ]]; then
    echo "Stopping SteamVR Home before launching $APP_NAME."
    while IFS= read -r pid; do
        [[ -z "$pid" ]] || kill -TERM "$pid" 2>/dev/null || true
    done <<< "$steamvr_home_pid"
    sleep 2
fi

steamvr_room_setup_pid="$(pgrep -f '[s]teamvr_room_setup\.exe' || true)"
if [[ -n "$steamvr_room_setup_pid" ]]; then
    echo "Stopping SteamVR Room Setup before launching $APP_NAME."
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
    # Started by Steam the app key is steam.app.<id>; started any other way
    # SteamVR names it system.generated.<exe in lower case>.
    log_since_checkpoint "$VR_SERVER_LOG" "$GAME_SERVER_CHECKPOINT" | awk \
        -v k1="appkey=steam.app.$APP_ID pid=" \
        -v k2="appkey=system.generated.$(printf '%s' "$GAME_EXE_BASE" | tr '[:upper:]' '[:lower:]') pid=" '
        /SetApplicationPid / && (index($0, k1) || index($0, k2)) {
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
        $3 == "pid=" pid && $4 == "Captured" && ($5 == "Alyx" || $5 == "game") &&
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
# Steam can take two minutes to start Alyx, and a fallback relaunch adds more.
HLA_STARTUP_TIMEOUT="${HLA_STARTUP_TIMEOUT:-480}"
GAME_LOG_NAME="half-life-alyx.log"
(( IS_ALYX == 1 )) || GAME_LOG_NAME="steamvr-app-$APP_ID.log"
# Optional: HLA_FIDELITY_LEVEL=0..8 adds Alyx's fidelity console variables.
HLA_FIDELITY_LEVEL_REQUESTED="${HLA_FIDELITY_LEVEL:-}"
HLA_FIDELITY_LEVEL="${HLA_FIDELITY_LEVEL:-0}"
if [[ ! "$HLA_FIDELITY_LEVEL" =~ ^[0-8]$ ]]; then
    echo "HLA_FIDELITY_LEVEL must be an integer from 0 through 8." >&2
    exit 1
fi
# Adding -noautofidelity -forcefidelitylevel N and +engine_no_focus_sleep 0
# made Alyx exit with code 1 right after its fidelity system started, every
# time, after which Steam relaunched it without options. Fidelity console
# variables are only added when HLA_FIDELITY_LEVEL is set explicitly, and if
# Alyx then exits during startup the script relaunches it without them.
HLA_FIDELITY_OPTIONS=()
if [[ -n "${HLA_FIDELITY_LEVEL_REQUESTED:-}" ]]; then
    HLA_FIDELITY_OPTIONS=(+vr_fidelity_level_auto 0 +vr_fidelity_level "$HLA_FIDELITY_LEVEL")
fi
game_has_script_options() {
    # Only Alyx gets options from this script; any other title runs as Steam starts it.
    (( IS_ALYX == 0 )) && return 0
    ps -o args= -p "$1" 2>/dev/null | grep -q -- '-condebug'
}
launch_alyx() {
    # Steam starts here when it is not running, with this script's environment.
    pgrep -f '[s]team\.exe' >/dev/null 2>&1 || echo "$D3D_BACKEND" >"$STEAM_BACKEND_MARKER"
    "$WINE_BIN" "$STEAM_EXE" -no-cef-sandbox -cef-single-process -noverifyfiles \
        -applaunch "$APP_ID" "$@" >>"$LOG_DIR/$GAME_LOG_NAME" 2>&1 &
}
stop_game_process() {
    local pid="$1" waited
    kill -TERM "$pid" 2>/dev/null || true
    for ((waited = 0; waited < 20; waited++)); do
        kill -0 "$pid" 2>/dev/null || return 0
        sleep 1
    done
    kill -KILL "$pid" 2>/dev/null || true
}

# Steam sometimes starts Alyx by itself (about 40 s after Steam starts, and
# again after Alyx exits) without this script's options, so it renders at its
# highest resolution. Steam then ignores our launch request. Quit such a copy
# while it is still at the menu and launch it again with the options.
existing_game_pid="$(pgrep -f "$GAME_PROC" | head -1 || true)"
if [[ -n "$existing_game_pid" ]]; then
    if game_has_script_options "$existing_game_pid"; then
        echo "$APP_NAME is already running with this script's options; following that process."
        GAME_SERVER_CHECKPOINT="0:0"
    else
        echo "Steam started $APP_NAME without this script's options; restarting it with the fixed fidelity level."
        stop_game_process "$existing_game_pid"
        sleep 3
        if pgrep -f "$GAME_PROC" >/dev/null 2>&1; then
            echo "Steam started Alyx again immediately; this script will follow that process."
            GAME_SERVER_CHECKPOINT="0:0"
        fi
    fi
fi
echo "Launching $APP_NAME through Windows Steam App ID $APP_ID. Waiting for completed stereo frames."
: >"$LOG_DIR/$GAME_LOG_NAME"
fidelity_retry_available=0
if (( ${#HLA_FIDELITY_OPTIONS[@]} > 0 )); then
    echo "Requesting Alyx fidelity level $HLA_FIDELITY_LEVEL with automatic fidelity off."
    fidelity_retry_available=1
fi
HLA_BENCH_OPTIONS=()
if [[ "$VERANTYX_BENCH" == "1" && -n "${VERANTYX_BENCH_MAP:-}" ]]; then
    HLA_BENCH_OPTIONS=(+map "$VERANTYX_BENCH_MAP")
    echo "Bench: loading map $VERANTYX_BENCH_MAP at startup."
fi
# Source stops rendering while its window is unfocused unless told otherwise;
# nothing focuses the Wine window when the game is started from a script.
# HLA_EXTRA_OPTIONS appends further launch options (space separated).
HLA_EXTRA_OPTIONS_ARRAY=()
read -r -a HLA_EXTRA_OPTIONS_ARRAY <<< "${HLA_EXTRA_OPTIONS:-+engine_no_focus_sleep 0}"
if (( IS_ALYX == 1 )); then
launch_alyx "${HLA_BASE_OPTIONS[@]}" ${HLA_FIDELITY_OPTIONS[@]+"${HLA_FIDELITY_OPTIONS[@]}"} \
    ${HLA_BENCH_OPTIONS[@]+"${HLA_BENCH_OPTIONS[@]}"} ${HLA_EXTRA_OPTIONS_ARRAY[@]+"${HLA_EXTRA_OPTIONS_ARRAY[@]}"}
else
    fidelity_retry_available=0
    launch_alyx
fi

# Alyx can exit with code 1 and have Steam start it again. Follow the new
# process instead of treating that as a failed launch and stopping the encoder.
HLA_RELAUNCH_GRACE="${HLA_RELAUNCH_GRACE:-60}"
adopt_relaunched_game() {
    local waited pid
    echo "$APP_NAME process exited; waiting up to ${HLA_RELAUNCH_GRACE}s in case Steam is restarting it."
    for ((waited = 0; waited < HLA_RELAUNCH_GRACE; waited++)); do
        pid="$(pgrep -f "$GAME_PROC" | head -1 || true)"
        if [[ -n "$pid" ]]; then
            GAME_MAC_PID="$pid"
            # The restarted process is not in the checkpointed log tail yet
            # when Steam started it before our checkpoint; read the whole log.
            GAME_SERVER_CHECKPOINT="0:0"
            echo "$APP_NAME was restarted by Steam; following the new process."
            warn_if_game_lacks_options
            return 0
        fi
        kill -0 "$ENCODER_PID" 2>/dev/null || return 1
        sleep 1
    done
    return 1
}

warned_missing_options=""
warn_if_game_lacks_options() {
    [[ -n "$GAME_MAC_PID" && "$warned_missing_options" != "$GAME_MAC_PID" ]] || return 0
    warned_missing_options="$GAME_MAC_PID"
    if ! game_has_script_options "$GAME_MAC_PID"; then
        echo "Note: this Alyx process was started without this script's options, so it renders at its highest resolution and runs slower." >&2
        echo "      Quit Alyx (pkill -f 'hlvr\.exe') and rerun this script to get the fixed fidelity level." >&2
    fi
}

game_frames_ready=0
for ((elapsed = 0; elapsed < HLA_STARTUP_TIMEOUT; elapsed++)); do
    if [[ -z "$GAME_MAC_PID" ]]; then
        GAME_MAC_PID="$(pgrep -f "$GAME_PROC" | head -1 || true)"
        if [[ -n "$GAME_MAC_PID" ]]; then
            echo "$APP_NAME process started; graphics are still initializing (no completed stereo frame confirmed)."
            warn_if_game_lacks_options
        fi
    elif ! kill -0 "$GAME_MAC_PID" 2>/dev/null; then
        if (( fidelity_retry_available == 1 )); then
            fidelity_retry_available=0
            echo "$APP_NAME exited during startup with the fidelity settings; relaunching it with the options that worked before (Alyx picks its own resolution)." >&2
            # Steam relaunches Alyx without options shortly after such an exit.
            sleep 5
            relaunched_pid="$(pgrep -f "$GAME_PROC" | head -1 || true)"
            [[ -z "$relaunched_pid" ]] || stop_game_process "$relaunched_pid"
            launch_alyx "${HLA_BASE_OPTIONS[@]}"
            GAME_MAC_PID=""
            continue
        fi
        adopt_relaunched_game && continue
        echo "$APP_NAME exited during graphics initialization." >&2
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
        echo "Waiting for $APP_NAME to complete stereo rendering (${elapsed}s/${HLA_STARTUP_TIMEOUT}s)."
    fi
    sleep 1
done

if (( game_frames_ready == 0 )); then
    echo "$APP_NAME startup was not verified: no completed stereo eye pair from this launch was captured." >&2
    echo "Check $LOG_DIR/$GAME_LOG_NAME and $GAME_CAPTURE_LOG (for Alyx also game/hlvr/console.log) for graphics or content errors." >&2
    echo "Current-launch capture diagnostics (historical entries excluded):" >&2
    log_since_checkpoint "$GAME_CAPTURE_LOG" "$GAME_CAPTURE_CHECKPOINT" | tail -30 >&2 || true
    tail -30 "$LOG_DIR/$GAME_LOG_NAME" >&2 || true
    if [[ -z "$GAME_MAC_PID" ]] || ! kill -0 "$GAME_MAC_PID" 2>/dev/null || \
        ! kill -0 "$ENCODER_PID" 2>/dev/null || ! pgrep -f '[v]rcompositor\.exe' >/dev/null 2>&1; then
        exit 1
    fi
    echo "Startup timed out. Keeping the encoder supervised for headset diagnostics until this game exits or Ctrl+C is pressed."
else
    echo "$APP_NAME completed stereo eye-pair capture. The encoder will stay active until the game exits or this script is stopped."
fi

while true; do
    if ! kill -0 "$GAME_MAC_PID" 2>/dev/null; then
        adopt_relaunched_game || break
    fi
    if ! kill -0 "$ENCODER_PID" 2>/dev/null; then
        echo "The HEVC encoder exited while $APP_NAME was active." >&2
        exit 1
    fi
    if ! pgrep -f '[v]rcompositor\.exe' >/dev/null 2>&1; then
        echo "SteamVR's compositor exited while $APP_NAME was running." >&2
        exit 1
    fi
    if (( game_frames_ready == 0 )) && game_stereo_capture_ready; then
        game_frames_ready=1
        echo "$APP_NAME completed stereo eye-pair capture after the startup timeout."
    fi
    sleep 2
done
echo "$APP_NAME exited."
(( game_frames_ready == 1 ))
