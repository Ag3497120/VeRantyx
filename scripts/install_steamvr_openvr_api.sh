#!/usr/bin/env bash
set -euo pipefail

VERANTYX_VR_DRIVE="${VERANTYX_VR_DRIVE:-$HOME/Verantyx_VR_Drive}"
WINEPREFIX="${WINEPREFIX:-$VERANTYX_VR_DRIVE/SteamVR_Prefix_Wine11_Test}"
STEAM_ROOT="${STEAM_ROOT:-$WINEPREFIX/drive_c/Program Files (x86)/Steam}"
STEAMVR_DIR="${STEAMVR_DIR:-$STEAM_ROOT/steamapps/common/SteamVR}"
HLA_GAME_BIN="${HLA_GAME_BIN:-$STEAM_ROOT/steamapps/common/Half-Life Alyx/game/bin/win64}"
SOURCE_DLL="$STEAMVR_DIR/bin/win64/openvr_api.dll"
TARGET_DLL="$HLA_GAME_BIN/openvr_api.dll"
BACKUP_DIR="$HLA_GAME_BIN/.verantyx-originals"

if [[ "$(basename "$WINEPREFIX")" != "SteamVR_Prefix_Wine11_Test" ]]; then
    echo "Refusing to modify a non-test prefix: $WINEPREFIX" >&2
    exit 1
fi
if pgrep -f '[h]lvr\.exe' >/dev/null 2>&1; then
    echo "Refusing to replace openvr_api.dll while Half-Life: Alyx is running." >&2
    exit 1
fi
if [[ ! -f "$SOURCE_DLL" || ! -d "$HLA_GAME_BIN" ]]; then
    echo "SteamVR's OpenVR client DLL or the Alyx game directory is missing." >&2
    echo "Expected source: $SOURCE_DLL" >&2
    echo "Expected target directory: $HLA_GAME_BIN" >&2
    exit 1
fi
if ! file "$SOURCE_DLL" | grep -Eq 'PE32\+ executable.*x86-64'; then
    echo "SteamVR OpenVR client DLL is not a Windows x64 DLL: $SOURCE_DLL" >&2
    exit 1
fi
if ! x86_64-w64-mingw32-objdump -p "$SOURCE_DLL" | grep -E 'VR_GetGenericInterface' >/dev/null; then
    echo "SteamVR OpenVR client DLL is missing VR_GetGenericInterface: $SOURCE_DLL" >&2
    exit 1
fi

mkdir -p "$BACKUP_DIR"
if [[ -f "$TARGET_DLL" ]] && ! cmp -s "$TARGET_DLL" "$SOURCE_DLL" && \
    [[ ! -f "$BACKUP_DIR/openvr_api.dll.user-version" ]]; then
    cp -p "$TARGET_DLL" "$BACKUP_DIR/openvr_api.dll.user-version"
    echo "Saved the HLA-local OpenVR DLL at $BACKUP_DIR/openvr_api.dll.user-version"
fi
if ! cmp -s "$TARGET_DLL" "$SOURCE_DLL"; then
    cp -p "$SOURCE_DLL" "$TARGET_DLL.verantyx-tmp" && mv -f "$TARGET_DLL.verantyx-tmp" "$TARGET_DLL"
    echo "Installed SteamVR's Valve OpenVR client DLL beside Half-Life: Alyx."
else
    echo "Half-Life: Alyx already uses SteamVR's OpenVR client DLL."
fi
