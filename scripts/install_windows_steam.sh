#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
VERANTYX_VR_DRIVE="${VERANTYX_VR_DRIVE:-$HOME/Verantyx_VR_Drive}"
WINEPREFIX="${WINEPREFIX:-$VERANTYX_VR_DRIVE/SteamVR_Prefix}"
WINE_BIN="${WINE_BIN:-$REPO_ROOT/VRDriver/wine_launcher.sh}"
STEAM_SETUP_EXE="${STEAM_SETUP_EXE:-$VERANTYX_VR_DRIVE/SteamSetup.exe}"
STEAM_ROOT="$WINEPREFIX/drive_c/Program Files (x86)/steam"
STEAM_EXE="${STEAM_EXE:-$STEAM_ROOT/steam.exe}"
HLA_DIR="$STEAM_ROOT/steamapps/common/half-life alyx"
HLA_EXE="${HLA_EXE:-$HLA_DIR/game/bin/win64/hlvr.exe}"
NESTED_MANIFEST="$HLA_DIR/steamapps/appmanifest_546560.acf"
STEAM_MANIFEST="$STEAM_ROOT/steamapps/appmanifest_546560.acf"
INSTALLER_ARGS=()

while (($#)); do
    case "$1" in
        --silent)
            INSTALLER_ARGS+=(/S)
            ;;
        --help|-h)
            cat <<'EOF'
Usage: scripts/install_windows_steam.sh [--silent]

Launch the Windows Steam installer with the repository's configured Windows
runtime. By default the installer is interactive. --silent passes NSIS's /S
switch for an unattended install using Steam's default prefix path.
EOF
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            echo "Use --help to see supported options." >&2
            exit 2
            ;;
    esac
    shift
done

if [[ ! -x "$WINE_BIN" ]]; then
    echo "Custom runtime launcher is not executable: $WINE_BIN" >&2
    exit 1
fi
if [[ ! -f "$STEAM_SETUP_EXE" ]]; then
    echo "Windows Steam installer was not found: $STEAM_SETUP_EXE" >&2
    echo "Set STEAM_SETUP_EXE to the official SteamSetup.exe file." >&2
    exit 1
fi
if [[ ! -d "$WINEPREFIX/drive_c" ]]; then
    echo "Windows prefix was not found: $WINEPREFIX" >&2
    exit 1
fi

# The recovered game has its manifest under the game directory. Steam expects
# it beside common/, so copy it there without changing the preserved original.
if [[ ! -f "$STEAM_MANIFEST" && -f "$NESTED_MANIFEST" && -f "$HLA_EXE" ]]; then
    cp -p "$NESTED_MANIFEST" "$STEAM_MANIFEST"
    echo "Registered the existing Half-Life: Alyx installation with Steam."
fi

if [[ -f "$STEAM_EXE" ]]; then
    echo "Windows Steam is already installed: $STEAM_EXE"
    exit 0
fi

export WINEPREFIX
echo "Starting SteamSetup.exe with the project runtime in prefix: $WINEPREFIX"
exec "$WINE_BIN" "$STEAM_SETUP_EXE" "${INSTALLER_ARGS[@]}"
