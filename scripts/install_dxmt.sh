#!/usr/bin/env bash
# Installs DXMT (Direct3D 11 → Metal, https://github.com/3Shain/dxmt) for the
# game only, as an alternative to DXVK + MoltenVK:
#
#   VERANTYX_D3D_BACKEND=dxmt ./scripts/launch_vision_pro_vr.sh
#
# DXMT's d3d11/d3d10core/dxgi/winemetal DLLs go beside the game's executable,
# where Wine loads them as native DLLs for that process alone; SteamVR's
# compositor keeps its DXVK. DXMT's Unix half (winemetal.so) is reached
# through WINEDLLPATH_PREPEND pointing at a directory that holds nothing
# else, so no other process picks up DXMT DLLs. Called with "remove" it
# restores the game directory to DXVK.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERANTYX_VR_DRIVE="${VERANTYX_VR_DRIVE:-$HOME/Verantyx_VR_Drive}"
WINEPREFIX="${WINEPREFIX:-$VERANTYX_VR_DRIVE/SteamVR_Prefix_Wine11_Test}"
STEAM_ROOT="${STEAM_ROOT:-$WINEPREFIX/drive_c/Program Files (x86)/Steam}"
HLA_GAME_BIN="${HLA_GAME_BIN:-$STEAM_ROOT/steamapps/common/Half-Life Alyx/game/bin/win64}"
DXMT_VERSION="${VERANTYX_DXMT_VERSION:-0.80}"
DXMT_URL="${VERANTYX_DXMT_URL:-https://github.com/3Shain/dxmt/releases/download/v$DXMT_VERSION/dxmt-v$DXMT_VERSION-builtin.tar.gz}"
DXMT_ROOT="$VERANTYX_VR_DRIVE/Beer/dxmt/v$DXMT_VERSION"
# Wine attaches a DLL's Unix half only when the PE itself is loaded from a
# builtin directory (<dir>/x86_64-windows beside <dir>/x86_64-unix), so
# winemetal.dll lives in this directory, reached through WINEDLLPATH_PREPEND,
# and nowhere else; the D3D DLLs beside the game import it from there.
DXMT_UNIX_DIR="$DXMT_ROOT/winemetal-only"
MARKER="$HLA_GAME_BIN/.verantyx-dxmt"
# Alyx loads d3d11.dll from system32 explicitly (not from its own directory),
# so DXMT's d3d11 goes to system32 and DXVK's is kept there as d3d11_dxvk.dll
# for SteamVR's compositor (its D3D11 proxy prefers that name). dxgi and
# d3d10core go beside the game.
SYSTEM32="$WINEPREFIX/drive_c/windows/system32"
DXVK_SOURCE_DIR="${DXVK_SOURCE_DIR:-$VERANTYX_VR_DRIVE/Beer/dxvk/dxvk-macOS-async-v1.10.3-20230507-repack/x64}"
DLLS=(d3d10core.dll dxgi.dll)

if [[ "$(basename "$WINEPREFIX")" != "SteamVR_Prefix_Wine11_Test" ]]; then
    echo "Refusing to modify a non-test prefix: $WINEPREFIX" >&2
    exit 1
fi
if [[ ! -d "$HLA_GAME_BIN" ]]; then
    echo "Half-Life: Alyx game directory not found: $HLA_GAME_BIN" >&2
    exit 1
fi

if [[ "${1:-install}" == "remove" ]]; then
    if [[ -f "$MARKER" ]]; then
        for dll in "${DLLS[@]}" d3d11.dll winemetal.dll; do rm -f "$HLA_GAME_BIN/$dll"; done
        if [[ -f "$DXVK_SOURCE_DIR/d3d11.dll" ]] && ! cmp -s "$DXVK_SOURCE_DIR/d3d11.dll" "$SYSTEM32/d3d11.dll"; then
            cp -p "$DXVK_SOURCE_DIR/d3d11.dll" "$SYSTEM32/d3d11.dll.verantyx-tmp" && mv -f "$SYSTEM32/d3d11.dll.verantyx-tmp" "$SYSTEM32/d3d11.dll"
        fi
        rm -f "$MARKER"
        echo "Removed DXMT; the game uses DXVK again."
    fi
    exit 0
fi

if pgrep -f '[h]lvr\.exe' >/dev/null 2>&1 && [[ ! -f "$MARKER" ]]; then
    echo "Refusing to change the game's D3D11 DLLs while Half-Life: Alyx is running." >&2
    exit 1
fi

if [[ ! -f "$DXMT_ROOT/x86_64-windows/d3d11.dll" || ! -f "$DXMT_ROOT/x86_64-unix/winemetal.so" ]]; then
    echo "Downloading DXMT v$DXMT_VERSION…"
    mkdir -p "$DXMT_ROOT"
    archive="$DXMT_ROOT/dxmt.tar.gz"
    curl -L --fail --progress-bar -o "$archive" "$DXMT_URL"
    tmp="$(mktemp -d "$DXMT_ROOT/extract.XXXXXX")"
    tar -xzf "$archive" -C "$tmp"
    payload="$tmp"
    [[ -d "$payload/x86_64-windows" ]] || payload="$tmp/v$DXMT_VERSION"
    [[ -d "$payload/x86_64-windows" && -d "$payload/x86_64-unix" ]] || { echo "Unexpected DXMT archive layout." >&2; exit 1; }
    rm -rf "$DXMT_ROOT/x86_64-windows" "$DXMT_ROOT/x86_64-unix" "$DXMT_ROOT/i386-windows"
    mv "$payload/x86_64-windows" "$payload/x86_64-unix" "$DXMT_ROOT/"
    [[ -d "$payload/i386-windows" ]] && mv "$payload/i386-windows" "$DXMT_ROOT/"
    rm -rf "$tmp" "$archive"
fi
mkdir -p "$DXMT_UNIX_DIR/x86_64-unix" "$DXMT_UNIX_DIR/x86_64-windows"
cp -p "$DXMT_ROOT/x86_64-unix/winemetal.so" "$DXMT_UNIX_DIR/x86_64-unix/winemetal.so"
cp -p "$DXMT_ROOT/x86_64-windows/winemetal.dll" "$DXMT_UNIX_DIR/x86_64-windows/winemetal.dll"
rm -f "$HLA_GAME_BIN/winemetal.dll"
# Also put winemetal into the Wine runtime's own builtin directories, so a
# game started by the Steam client finds it without any environment variable.
WINE_LIB="${WINE_LIB:-$VERANTYX_VR_DRIVE/Beer/wine/WineStaging.app/Contents/Resources/wine/lib/wine}"
if [[ -d "$WINE_LIB/x86_64-windows" && -d "$WINE_LIB/x86_64-unix" ]]; then
    for pair in "x86_64-unix/winemetal.so" "x86_64-windows/winemetal.dll"; do
        if ! cmp -s "$DXMT_ROOT/$pair" "$WINE_LIB/$pair"; then
            cp -p "$DXMT_ROOT/$pair" "$WINE_LIB/$pair.verantyx-tmp" && mv -f "$WINE_LIB/$pair.verantyx-tmp" "$WINE_LIB/$pair"
            echo "Installed $(basename "$pair") into the Wine runtime ($WINE_LIB)."
        fi
    done
fi

changed=0
for dll in "${DLLS[@]}"; do
    if ! cmp -s "$DXMT_ROOT/x86_64-windows/$dll" "$HLA_GAME_BIN/$dll"; then
        cp "$DXMT_ROOT/x86_64-windows/$dll" "$HLA_GAME_BIN/$dll.verantyx-tmp" && mv -f "$HLA_GAME_BIN/$dll.verantyx-tmp" "$HLA_GAME_BIN/$dll"
        changed=1
    fi
done
rm -f "$HLA_GAME_BIN/d3d11.dll"
[[ -f "$DXVK_SOURCE_DIR/d3d11.dll" ]] || { echo "DXVK d3d11.dll not found: $DXVK_SOURCE_DIR" >&2; exit 1; }
if ! cmp -s "$DXVK_SOURCE_DIR/d3d11.dll" "$SYSTEM32/d3d11_dxvk.dll"; then
    cp -p "$DXVK_SOURCE_DIR/d3d11.dll" "$SYSTEM32/d3d11_dxvk.dll.verantyx-tmp" && mv -f "$SYSTEM32/d3d11_dxvk.dll.verantyx-tmp" "$SYSTEM32/d3d11_dxvk.dll"
fi
if ! cmp -s "$DXMT_ROOT/x86_64-windows/d3d11.dll" "$SYSTEM32/d3d11.dll"; then
    # A running compositor keeps its already-loaded copy; the file is replaced atomically.
    cp "$DXMT_ROOT/x86_64-windows/d3d11.dll" "$SYSTEM32/d3d11.dll.verantyx-tmp" && mv -f "$SYSTEM32/d3d11.dll.verantyx-tmp" "$SYSTEM32/d3d11.dll"
    changed=1
fi
echo "DXMT v$DXMT_VERSION" >"$MARKER"
if (( changed )); then
    echo "Installed DXMT v$DXMT_VERSION for the game (system32 d3d11 = DXMT, d3d11_dxvk kept for the compositor; dxgi and d3d10core beside the game)."
else
    echo "DXMT v$DXMT_VERSION is already installed beside Half-Life: Alyx."
fi
echo "DXMT_UNIX_DIR=$DXMT_UNIX_DIR"
