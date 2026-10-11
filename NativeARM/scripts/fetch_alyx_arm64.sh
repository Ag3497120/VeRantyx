#!/usr/bin/env bash
# Downloads Half-Life: Alyx's Linux depot (546564, which contains the ARM64
# build) with your own Steam account through steamcmd. steamcmd asks for the
# password and Steam Guard interactively; nothing is stored by this script.
#
#   NativeARM/scripts/fetch_alyx_arm64.sh <steam-login-name>
#
# The depot lands under steamcmd's content directory, printed at the end.
set -euo pipefail
user="${1:?usage: fetch_alyx_arm64.sh <steam-login-name>}"
command -v steamcmd >/dev/null || { echo "steamcmd is required: brew install --cask steamcmd" >&2; exit 1; }
steamcmd +@sSteamCmdForcePlatformType linux +login "$user" +download_depot 546560 546564 +quit
base="$(dirname "$(readlink -f "$(command -v steamcmd)")")"
dir="$(find "$base" /opt/homebrew/Caskroom/steamcmd -type d -path '*content/app_546560/depot_546564' 2>/dev/null | head -1)"
echo "Depot directory: ${dir:-not found}"
[[ -n "$dir" ]] && ls "$dir/game/bin/linuxsteamrtarm64" | head -5
