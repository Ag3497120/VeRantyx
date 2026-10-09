#!/bin/bash
set -euo pipefail

VERANTYX_VR_DRIVE="${VERANTYX_VR_DRIVE:-$HOME/Verantyx_VR_Drive}"
WINEPREFIX="${WINEPREFIX:-$VERANTYX_VR_DRIVE/SteamVR_Prefix_Wine11_Test}"
WINE_RUNTIME_BIN="${WINE_RUNTIME_BIN:-$VERANTYX_VR_DRIVE/Beer/wine/WineStaging.app/Contents/Resources/wine/bin/wine}"
GPTK_ROOT="${GPTK_ROOT:-$VERANTYX_VR_DRIVE/gptk_wine_local}"
WINE_WRAPPER="${WINE_WRAPPER:-$VERANTYX_VR_DRIVE/wine_wrapper}"

export WINEPREFIX
export WINEDEBUG="${WINEDEBUG:--all,err+all}"
export WINEESYNC=1
export WINEDLLOVERRIDES="dxgi,d3d9,d3d10core,d3d11=n,b;${WINEDLLOVERRIDES:-}"
export D3D11_ENABLE=1
export D3DMETAL_ENABLE=1
export D3DMetal=1

if [[ -x "$WINE_RUNTIME_BIN" ]]; then
    RUNTIME_ROOT="$(cd "$(dirname "$WINE_RUNTIME_BIN")/.." && pwd)"
    export PATH="$RUNTIME_ROOT/bin:$PATH"
    export DYLD_FALLBACK_LIBRARY_PATH="$RUNTIME_ROOT/lib:$GPTK_ROOT/lib:/usr/lib"
    export DYLD_LIBRARY_PATH="$RUNTIME_ROOT/lib:$RUNTIME_ROOT/lib/external:$GPTK_ROOT/lib/external:${MOLTENVK_ENV:-$VERANTYX_VR_DRIVE/moltenvk_env}:${DYLD_LIBRARY_PATH:-}"
    version="$("$WINE_RUNTIME_BIN" --version 2>/dev/null || true)"
    echo "Using local Windows runtime: ${version:-$WINE_RUNTIME_BIN}"
    exec "$WINE_RUNTIME_BIN" "$@"
fi

if [[ ! -x "$WINE_WRAPPER" ]]; then
    echo "Local Wine 11 runtime not found: $WINE_RUNTIME_BIN" >&2
    echo "Custom Windows runtime wrapper not found or not executable: $WINE_WRAPPER" >&2
    exit 1
fi
if [[ ! -x "$GPTK_ROOT/bin/wine64" && ! -x "$GPTK_ROOT/bin/wine" ]]; then
    echo "Custom GPTK runtime not found under: $GPTK_ROOT/bin" >&2
    echo "Set WINE_RUNTIME_BIN or GPTK_ROOT to the project runtime." >&2
    exit 1
fi

export PATH="$GPTK_ROOT/bin:$PATH"
export DYLD_FALLBACK_LIBRARY_PATH="$GPTK_ROOT/lib:/usr/lib"
export DYLD_LIBRARY_PATH="$GPTK_ROOT/lib/external:${MOLTENVK_ENV:-$VERANTYX_VR_DRIVE/moltenvk_env}:${DYLD_LIBRARY_PATH:-}"
echo "Using legacy custom runtime wrapper: $WINE_WRAPPER" >&2
exec "$WINE_WRAPPER" "$@"
