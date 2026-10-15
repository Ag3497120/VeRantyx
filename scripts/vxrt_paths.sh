# Where SteamVR, the games and the Windows runtime DLLs are, for the launch_vxrt_*.sh scripts (sourced, not run).
#
#   STEAM_ROOT    the Steam library SteamVR runs from: ~/Verantyx_VR_Drive/vxrt_steam (tools/vxrt_steam.py fetches or
#                 imports SteamVR and games there; no Windows Steam, no Wine) or, if SteamVR is not there yet, the
#                 Wine version's prefix (read only, as before)
#   WINE_STEAM    the Wine version's Steam folder, a library games may still be in (read only); empty without it
#   VXRT_DLL_DIRS the Microsoft runtime DLLs: ~/Verantyx_VR_Drive/vxrt_redist (tools/vxrt_redist.py collects them from
#                 the redistributables your own installs carry) or, without it, the prefix's system32
#
# VXRT_STEAM, VXRT_REDIST and WINEPREFIX change the places; VXRT_NO_WINE=1 never uses the prefix (an error instead).
VERANTYX_VR_DRIVE="${VERANTYX_VR_DRIVE:-$HOME/Verantyx_VR_Drive}"
VXRT_STEAM="${VXRT_STEAM:-$VERANTYX_VR_DRIVE/vxrt_steam}"
VXRT_REDIST="${VXRT_REDIST:-$VERANTYX_VR_DRIVE/vxrt_redist}"
WINE_STEAM=""
if [[ "${VXRT_NO_WINE:-0}" != 1 ]]; then
    _prefix="${WINEPREFIX:-$VERANTYX_VR_DRIVE/SteamVR_Prefix_Wine11_Test}"
    [[ -d "$_prefix/drive_c/Program Files (x86)/steam/steamapps" ]] && WINE_STEAM="$_prefix/drive_c/Program Files (x86)/steam"
fi
if [[ -d "$VXRT_STEAM/steamapps/common/SteamVR/bin/win64" ]]; then
    STEAM_ROOT="$VXRT_STEAM"
elif [[ -n "$WINE_STEAM" && -d "$WINE_STEAM/steamapps/common/SteamVR/bin/win64" ]]; then
    STEAM_ROOT="$WINE_STEAM"
    echo "Note: SteamVR from the Wine prefix (read only); tools/vxrt_steam.py import or fetch puts it in $VXRT_STEAM" >&2
else
    echo "SteamVR not found: tools/vxrt_steam.py fetch <steam-login> 250820 (or import from a Steam library you have)" >&2
    exit 1
fi
if [[ -f "$VXRT_REDIST/manifest.json" ]]; then
    VXRT_DLL_DIRS="$VXRT_REDIST"
elif [[ -n "$WINE_STEAM" ]]; then
    VXRT_DLL_DIRS="$_prefix/drive_c/windows/system32"
    echo "Note: runtime DLLs from the Wine prefix; tools/vxrt_redist.py collect gathers them without it" >&2
else
    echo "No Microsoft runtime DLLs: tools/vxrt_redist.py collect (from the redistributables in SteamVR's _CommonRedist)" >&2
    exit 1
fi
# The library a game's executable is in (for its overlay): STEAM_ROOT, WINE_STEAM or none.
steam_library_of() {
    if [[ "$1" == "$STEAM_ROOT"/* ]]; then printf '%s' "$STEAM_ROOT"
    elif [[ -n "$WINE_STEAM" && "$1" == "$WINE_STEAM"/* ]]; then printf '%s' "$WINE_STEAM"
    fi
    return 0                                          # (none: the caller gives the game an overlay of its own)
}
