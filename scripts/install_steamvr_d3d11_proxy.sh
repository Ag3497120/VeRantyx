#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
VERANTYX_VR_DRIVE="${VERANTYX_VR_DRIVE:-$HOME/Verantyx_VR_Drive}"
WINEPREFIX="${WINEPREFIX:-$VERANTYX_VR_DRIVE/SteamVR_Prefix_Wine11_Test}"
SOURCE_DLL="${SOURCE_DLL:-$REPO_ROOT/build/steamvr-d3d11-proxy/d3d11.dll}"
STEAMVR_BIN="$WINEPREFIX/drive_c/Program Files (x86)/Steam/steamapps/common/SteamVR/bin/win64"
TARGET_DLL="$STEAMVR_BIN/d3d11.dll"
BACKUP_DIR="$STEAMVR_BIN/.verantyx-originals"

if [[ "$(basename "$WINEPREFIX")" != "SteamVR_Prefix_Wine11_Test" ]]; then
    echo "Refusing to install into a non-test prefix: $WINEPREFIX" >&2
    echo "This compatibility proxy is experimental and restricted to the test clone." >&2
    exit 1
fi
if [[ ! -f "$SOURCE_DLL" ]]; then
    echo "Built proxy DLL was not found: $SOURCE_DLL" >&2
    echo "Run scripts/build_steamvr_d3d11_proxy.sh first." >&2
    exit 1
fi
if [[ ! -d "$STEAMVR_BIN" ]]; then
    echo "SteamVR x64 runtime directory was not found: $STEAMVR_BIN" >&2
    exit 1
fi

if ! file "$SOURCE_DLL" | grep -Eq 'PE32\+ executable.*x86-64'; then
    echo "Proxy DLL is not a Windows x64 DLL: $SOURCE_DLL" >&2
    exit 1
fi

mkdir -p "$BACKUP_DIR"
if [[ -f "$TARGET_DLL" ]] && ! cmp -s "$TARGET_DLL" "$SOURCE_DLL" && [[ ! -f "$BACKUP_DIR/d3d11.dll" ]]; then
    cp -p "$TARGET_DLL" "$BACKUP_DIR/d3d11.dll"
    echo "Saved the prior SteamVR-local d3d11.dll."
fi
if ! cmp -s "$TARGET_DLL" "$SOURCE_DLL"; then
    # Rename into place so a running compositor keeps its mapped copy.
    cp -p "$SOURCE_DLL" "$TARGET_DLL.verantyx-tmp" && mv -f "$TARGET_DLL.verantyx-tmp" "$TARGET_DLL"
    echo "Installed the D3D11 proxy in the test clone's SteamVR bin directory."
else
    echo "The SteamVR-local D3D11 proxy is already current."
fi
echo "The proxy forwards D3D11CreateDevice to the prefix system32 DXVK runtime."
