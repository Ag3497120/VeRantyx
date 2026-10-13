#!/usr/bin/env bash
# Downloads the Windows build of a Steam app (a VR title to run on VXRuntime) with your own Steam account through
# steamcmd. steamcmd asks for the password and Steam Guard itself; nothing is stored by this script. Free apps are
# added to the account by the download.
#
#   scripts/fetch_windows_vr_app.sh <steam-login-name> <appid> [install-dir]
#   scripts/fetch_windows_vr_app.sh me 450390      # The Lab
#   scripts/fetch_windows_vr_app.sh me 457550      # Bigscreen Beta
#
# The default install directory is ~/Verantyx_VR_Drive/vxrt_library/<appid> (outside any Wine prefix).
set -euo pipefail
user="${1:?usage: fetch_windows_vr_app.sh <steam-login-name> <appid> [install-dir]}"
appid="${2:?usage: fetch_windows_vr_app.sh <steam-login-name> <appid> [install-dir]}"
dir="${3:-$HOME/Verantyx_VR_Drive/vxrt_library/$appid}"
command -v steamcmd >/dev/null || { echo "steamcmd is required: brew install --cask steamcmd" >&2; exit 1; }
mkdir -p "$dir"
steamcmd +@sSteamCmdForcePlatformType windows +force_install_dir "$dir" +login "$user" +app_update "$appid" validate +quit
echo "Installed in: $dir"
find "$dir" -maxdepth 2 -iname "*.exe" | head -5
