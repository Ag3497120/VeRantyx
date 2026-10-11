#!/usr/bin/env bash
# Uses Apple's D3DMetal (from a Game Porting Toolkit copy you obtained from
# developer.apple.com yourself; nothing is downloaded or shipped here) as the
# Direct3D backend for the whole prefix, the way this project ran in June
# 2026 when it felt smoothest:
#
#   VERANTYX_D3D_BACKEND=d3dmetal VERANTYX_USE_DXVK=0 ./scripts/launch_vision_pro_vr.sh
#
# GPTK's d3d11/dxgi/d3d10/d3d12 DLLs are Wine builtins with a Unix half
# (d3d11.so, which links libd3dshared.dylib and D3DMetal.framework), so they
# cannot be dropped into system32 as native DLLs. They replace the Wine
# runtime's own builtins of the same names (lib/wine/x86_64-windows and
# x86_64-unix inside the downloaded Wine.app; the originals are kept as
# *.verantyx-wine-original and put back by "remove"). The launcher adds
# GPTK's lib/external to DYLD_LIBRARY_PATH. The prefix itself is untouched;
# the DLL names are forced to "builtin" so DXVK's native d3d11 in system32
# is not used.
set -euo pipefail

VERANTYX_VR_DRIVE="${VERANTYX_VR_DRIVE:-$HOME/Verantyx_VR_Drive}"
GPTK_ROOT="${GPTK_ROOT:-$VERANTYX_VR_DRIVE/gptk_wine_local}"
WINE_LIB="${WINE_LIB:-$VERANTYX_VR_DRIVE/Beer/wine/WineStaging.app/Contents/Resources/wine/lib/wine}"
WINDOWS_DLLS=(d3d11.dll dxgi.dll d3d10core.dll d3d10.dll d3d10_1.dll d3d12.dll d3d12core.dll)
UNIX_LIBS=(d3d11.so dxgi.so d3d10.so d3d12.so)
MARKER="$WINE_LIB/.verantyx-d3dmetal"

if [[ ! -d "$WINE_LIB/x86_64-windows" || ! -d "$WINE_LIB/x86_64-unix" ]]; then
    echo "Wine runtime library directory not found: $WINE_LIB" >&2
    exit 1
fi

restore() {
    local restored=0 f
    for f in "$WINE_LIB"/x86_64-windows/*.verantyx-wine-original "$WINE_LIB"/x86_64-unix/*.verantyx-wine-original; do
        [[ -e "$f" ]] || continue
        mv -f "$f" "${f%.verantyx-wine-original}"
        restored=1
    done
    # A GPTK Unix half that had no Wine counterpart (e.g. d3d12.so) is removed.
    for lib in "${UNIX_LIBS[@]}"; do
        [[ -f "$WINE_LIB/x86_64-unix/$lib.verantyx-added" ]] && { rm -f "$WINE_LIB/x86_64-unix/$lib" "$WINE_LIB/x86_64-unix/$lib.verantyx-added"; restored=1; }
    done
    rm -f "$MARKER"
    (( restored )) && echo "Restored the Wine runtime's own D3D builtins." || true
}

if [[ "${1:-install}" == "remove" ]]; then
    [[ -f "$MARKER" ]] && restore
    exit 0
fi

# GPTK 1.x keeps lib/wine and lib/external at the root; GPTK 2.x ships them
# under redist/. GPTK 1.1's DLLs call ntdll.__wine_unix_call, which Wine 8
# removed, so they abort on this Wine 11 runtime ("unimplemented function
# ntdll.dll.__wine_unix_call"); GPTK 2.x targets newer Wine and is untested here.
SOURCE="$GPTK_ROOT/lib/wine"
[[ -d "$SOURCE/x86_64-windows" ]] || SOURCE="$GPTK_ROOT/redist/lib/wine"
EXTERNAL="$GPTK_ROOT/lib/external"
[[ -e "$EXTERNAL/libd3dshared.dylib" ]] || EXTERNAL="$GPTK_ROOT/redist/lib/external"
if [[ ! -f "$SOURCE/x86_64-windows/d3d11.dll" || ! -f "$SOURCE/x86_64-unix/d3d11.so" || ! -e "$EXTERNAL/libd3dshared.dylib" ]]; then
    echo "D3DMetal was not found under GPTK_ROOT=$GPTK_ROOT (needs lib/wine/x86_64-windows/d3d11.dll, lib/wine/x86_64-unix/d3d11.so and lib/external/libd3dshared.dylib)." >&2
    echo "Apple's Game Porting Toolkit is not distributed with VeRantyx; obtain it from developer.apple.com and point GPTK_ROOT at it." >&2
    exit 1
fi
if "$GPTK_ROOT/bin/wine64" --version 2>/dev/null | grep -q 'Game Porting Toolkit 1\.'; then
    echo "This is Game Porting Toolkit 1.x: its D3DMetal needs Wine 7's ntdll.__wine_unix_call and aborts on Wine 11. Use GPTK 2.x, or run the June runtime (GPTK's own Wine 7.7) instead." >&2
    exit 1
fi
if pgrep -f '[v]rcompositor\.exe|[h]lvr\.exe' >/dev/null 2>&1 && [[ ! -f "$MARKER" ]]; then
    echo "Stop SteamVR and the game before switching the Wine runtime's D3D builtins to D3DMetal." >&2
    exit 1
fi

changed=0
install_one() {
    local source="$1" target="$2"
    cmp -s "$source" "$target" && return 0
    if [[ -f "$target" && ! -f "$target.verantyx-wine-original" && ! -f "$target.verantyx-added" ]]; then
        cp -p "$target" "$target.verantyx-wine-original"
    elif [[ ! -f "$target" ]]; then
        : >"$target.verantyx-added"
    fi
    cp -p "$source" "$target.verantyx-tmp" && mv -f "$target.verantyx-tmp" "$target"
    changed=1
}
for dll in "${WINDOWS_DLLS[@]}"; do
    [[ -f "$SOURCE/x86_64-windows/$dll" ]] && install_one "$SOURCE/x86_64-windows/$dll" "$WINE_LIB/x86_64-windows/$dll"
done
for lib in "${UNIX_LIBS[@]}"; do
    [[ -f "$SOURCE/x86_64-unix/$lib" ]] && install_one "$SOURCE/x86_64-unix/$lib" "$WINE_LIB/x86_64-unix/$lib"
done
echo "D3DMetal from $GPTK_ROOT" >"$MARKER"
if (( changed )); then
    echo "Installed D3DMetal (${WINDOWS_DLLS[*]}) as the Wine runtime's D3D builtins from $GPTK_ROOT."
else
    echo "D3DMetal is already the Wine runtime's D3D builtin."
fi
