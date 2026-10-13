#!/usr/bin/env bash
# Downloads the Windows builds of Steam apps (VR titles to run on VXRuntime) with your own Steam account through
# steamcmd. steamcmd asks for the password and Steam Guard itself (once: it remembers the sign-in afterwards);
# nothing is stored by this script. Free apps get their free license first (app_license_request).
#
#   scripts/fetch_windows_vr_app.sh <steam-login-name> <appid> [<appid> ...]
#   scripts/fetch_windows_vr_app.sh me 450390 457550    # The Lab (~16.6 GB) and Bigscreen Beta (~5.0 GB)
#
# Each app goes to ~/Verantyx_VR_Drive/vxrt_library/<appid> (outside any Wine prefix); VXRT_LIBRARY changes the
# parent folder.
set -euo pipefail
user="${1:?usage: fetch_windows_vr_app.sh <steam-login-name> <appid> [<appid> ...]}"
shift
[[ $# -ge 1 ]] || { echo "usage: fetch_windows_vr_app.sh <steam-login-name> <appid> [<appid> ...]" >&2; exit 1; }
library="${VXRT_LIBRARY:-$HOME/Verantyx_VR_Drive/vxrt_library}"
command -v steamcmd >/dev/null || { echo "steamcmd is required: brew install --cask steamcmd" >&2; exit 1; }
for appid in "$@"; do                       # (steamcmd wants the folder before the sign-in; later runs reuse it)
    mkdir -p "$library/$appid"
    steamcmd +@sSteamCmdForcePlatformType windows +force_install_dir "$library/$appid" +login "$user" \
        +app_license_request "$appid" +app_update "$appid" validate +quit
done
for appid in "$@"; do
    echo "Installed in: $library/$appid"
    find "$library/$appid" -maxdepth 2 -iname "*.exe" | head -5
done
