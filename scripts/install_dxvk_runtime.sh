#!/usr/bin/env bash
set -euo pipefail

VERANTYX_VR_DRIVE="${VERANTYX_VR_DRIVE:-$HOME/Verantyx_VR_Drive}"
WINEPREFIX="${WINEPREFIX:-$VERANTYX_VR_DRIVE/SteamVR_Prefix_Wine11_Test}"
DXVK_SOURCE_DIR="${DXVK_SOURCE_DIR:-$VERANTYX_VR_DRIVE/Beer/dxvk/dxvk-macOS-async-v1.10.3-20230507-repack/x64}"
SYSTEM32="$WINEPREFIX/drive_c/windows/system32"
BACKUP_DIR="$SYSTEM32/.verantyx-dxvk-originals"

production_prefix="$VERANTYX_VR_DRIVE/SteamVR_Prefix"
if [[ "$(cd "$(dirname "$WINEPREFIX")" && pwd)/$(basename "$WINEPREFIX")" == "$production_prefix" ]]; then
    echo "Refusing to change the production SteamVR prefix. Use a test clone." >&2
    exit 1
fi

if [[ ! -d "$SYSTEM32" ]]; then
    echo "Windows system32 folder was not found in WINEPREFIX: $WINEPREFIX" >&2
    exit 1
fi
if [[ ! -d "$DXVK_SOURCE_DIR" ]]; then
    echo "Transferred DXVK x64 folder was not found: $DXVK_SOURCE_DIR" >&2
    exit 1
fi

mkdir -p "$BACKUP_DIR"
for dll in d3d11.dll d3d10core.dll; do
    source="$DXVK_SOURCE_DIR/$dll"
    target="$SYSTEM32/$dll"
    backup="$BACKUP_DIR/$dll"

    if [[ ! -f "$source" ]]; then
        echo "Required DXVK DLL is missing: $source" >&2
        exit 1
    fi
    if ! file "$source" | rg -q 'PE32\+ executable.*x86-64'; then
        echo "DXVK DLL is not a Windows x64 DLL: $source" >&2
        exit 1
    fi

    if [[ -f "$target" ]] && ! cmp -s "$target" "$source" && [[ ! -f "$backup" ]]; then
        cp -p "$target" "$backup"
        echo "Saved original system32/$dll."
    fi
    if ! cmp -s "$target" "$source"; then
        cp -p "$source" "$target"
        echo "Installed DXVK $dll into the selected test prefix."
    else
        echo "DXVK $dll is already installed."
    fi
done

if [[ ! -f "$DXVK_SOURCE_DIR/dxgi.dll" ]]; then
    echo "DXVK package has no dxgi.dll; Wine's built-in DXGI remains in use."
fi
echo "DXVK files are ready in: $WINEPREFIX"
