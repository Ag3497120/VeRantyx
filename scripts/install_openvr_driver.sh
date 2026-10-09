#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SOURCE_DLL="${SOURCE_DLL:-$REPO_ROOT/VRDriver/driver_verantyx.dll}"
WINEPREFIX="${WINEPREFIX:-$HOME/Verantyx_VR_Drive/SteamVR_Prefix}"
STEAM_ROOT="${STEAM_ROOT:-$WINEPREFIX/drive_c/Program Files (x86)/Steam}"
STEAMVR_DIR="${STEAMVR_DIR:-$STEAM_ROOT/steamapps/common/SteamVR}"
DRIVER_DIR="${VR_DRIVER_DIR:-$WINEPREFIX/drive_c/vr_driver/verantyx/bin/win64}"
STEAMVR_DRIVER_DIR="${STEAMVR_DRIVER_DIR:-$STEAMVR_DIR/drivers/verantyx/bin/win64}"

if [[ ! -f "$SOURCE_DLL" ]]; then
    echo "Built driver DLL was not found: $SOURCE_DLL" >&2
    echo "Run ./scripts/build_openvr_driver.sh first." >&2
    exit 1
fi
if [[ ! -f "$DRIVER_DIR/../driver.vrdrivermanifest" && ! -f "$DRIVER_DIR/../../driver.vrdrivermanifest" ]]; then
    echo "SteamVR driver manifest was not found next to the configured driver: $DRIVER_DIR" >&2
    echo "Set VR_DRIVER_DIR to the bin/win64 directory registered with SteamVR." >&2
    exit 1
fi

install_driver() {
    local driver_dir="$1"
    local destination="$driver_dir/driver_verantyx.dll"
    local backup="$destination.mbp-avp-vr-original"

    mkdir -p "$driver_dir"
    if [[ -f "$destination" ]] && ! cmp -s "$SOURCE_DLL" "$destination" && [[ ! -f "$backup" ]]; then
        cp -p "$destination" "$backup"
    fi
    cp "$SOURCE_DLL" "$destination"
    cmp -s "$SOURCE_DLL" "$destination"
    echo "Installed SteamVR driver: $destination"
    if [[ -f "$backup" ]]; then
        echo "Original driver backup: $backup"
    fi
}

install_driver "$DRIVER_DIR"

# SteamVR may prefer a same-named bundled driver over the registered external
# copy. Keep both copies identical so the runtime actually loads this build.
if [[ -n "$STEAMVR_DRIVER_DIR" && -f "$STEAMVR_DRIVER_DIR/driver_verantyx.dll" ]]; then
    if [[ "$STEAMVR_DRIVER_DIR" != "$DRIVER_DIR" ]]; then
        install_driver "$STEAMVR_DRIVER_DIR"
    fi
elif [[ -n "$STEAMVR_DRIVER_DIR" ]]; then
    echo "No bundled Verantyx driver was found at $STEAMVR_DRIVER_DIR; external driver remains installed."
fi
