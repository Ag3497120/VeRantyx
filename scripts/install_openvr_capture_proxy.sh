#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
VERANTYX_VR_DRIVE="${VERANTYX_VR_DRIVE:-$HOME/Verantyx_VR_Drive}"
WINEPREFIX="${WINEPREFIX:-$VERANTYX_VR_DRIVE/SteamVR_Prefix_Wine11_Test}"
STEAM_ROOT="${STEAM_ROOT:-$WINEPREFIX/drive_c/Program Files (x86)/Steam}"
STEAMVR_DIR="${STEAMVR_DIR:-$STEAM_ROOT/steamapps/common/SteamVR}"
HLA_GAME_BIN="${HLA_GAME_BIN:-$STEAM_ROOT/steamapps/common/Half-Life Alyx/game/bin/win64}"
SOURCE_DLL="$STEAMVR_DIR/bin/win64/openvr_api.dll"
VALVE_DLL="$HLA_GAME_BIN/openvr_api_valve.dll"
TARGET_DLL="$HLA_GAME_BIN/openvr_api.dll"
BACKUP_DIR="$HLA_GAME_BIN/.verantyx-originals"
PROXY_DLL="${PROXY_DLL:-$REPO_ROOT/build/openvr_api_capture_proxy.dll}"

if [[ "$(basename "$WINEPREFIX")" != "SteamVR_Prefix_Wine11_Test" ]]; then
    echo "Refusing to modify a non-test prefix: $WINEPREFIX" >&2
    exit 1
fi
if pgrep -f '[h]lvr\.exe' >/dev/null 2>&1; then
    echo "Refusing to replace OpenVR files while Half-Life: Alyx is running." >&2
    exit 1
fi
if [[ ! -f "$PROXY_DLL" ]]; then
    "$SCRIPT_DIR/build_openvr_capture_proxy.sh"
fi
if [[ ! -f "$SOURCE_DLL" || ! -d "$HLA_GAME_BIN" ]]; then
    echo "SteamVR's OpenVR client DLL or the Alyx game directory is missing." >&2
    exit 1
fi

mkdir -p "$BACKUP_DIR"
if [[ -f "$TARGET_DLL" ]] && ! cmp -s "$TARGET_DLL" "$PROXY_DLL" && \
    ! cmp -s "$TARGET_DLL" "$SOURCE_DLL" && \
    [[ ! -f "$BACKUP_DIR/openvr_api.dll.pre-capture" ]]; then
    cp -p "$TARGET_DLL" "$BACKUP_DIR/openvr_api.dll.pre-capture"
    echo "Saved the current HLA OpenVR DLL: $BACKUP_DIR/openvr_api.dll.pre-capture"
fi
if [[ -f "$VALVE_DLL" ]] && ! cmp -s "$VALVE_DLL" "$SOURCE_DLL" && \
    [[ ! -f "$BACKUP_DIR/openvr_api_valve.dll.pre-capture" ]]; then
    cp -p "$VALVE_DLL" "$BACKUP_DIR/openvr_api_valve.dll.pre-capture"
fi
if ! cmp -s "$VALVE_DLL" "$SOURCE_DLL"; then
    cp -p "$SOURCE_DLL" "$VALVE_DLL.verantyx-tmp" && mv -f "$VALVE_DLL.verantyx-tmp" "$VALVE_DLL"
    echo "Installed the original SteamVR OpenVR client as openvr_api_valve.dll."
fi
if ! cmp -s "$TARGET_DLL" "$PROXY_DLL"; then
    cp "$PROXY_DLL" "$TARGET_DLL.verantyx-tmp" && mv -f "$TARGET_DLL.verantyx-tmp" "$TARGET_DLL"
    echo "Installed the Submit capture proxy beside Half-Life: Alyx."
fi

echo "Alyx OpenVR capture proxy is ready. The original Submit call is forwarded to SteamVR."
