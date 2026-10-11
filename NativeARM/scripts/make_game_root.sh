#!/usr/bin/env bash
# Builds a game root that combines the game content of an existing Half-Life:
# Alyx install (the Windows install under the Wine prefix — depot 546561 is
# shared by every platform) with the Linux ARM64 binaries of depot 546564,
# using symlinks only; nothing is copied or modified.
#
#   NativeARM/scripts/make_game_root.sh <depot_546564 dir> [alyx install dir] [out dir]
#
# The result mirrors Valve's layout:  <out>/game/bin/linuxsteamrtarm64/…,
# <out>/game/hlvr/{pak01_*.vpk, cfg, …, bin/linuxsteamrtarm64/…}.
set -euo pipefail
DEPOT="${1:?usage: make_game_root.sh <depot_546564 dir> [alyx install dir] [out dir]}"
VERANTYX_VR_DRIVE="${VERANTYX_VR_DRIVE:-$HOME/Verantyx_VR_Drive}"
WINEPREFIX="${WINEPREFIX:-$VERANTYX_VR_DRIVE/SteamVR_Prefix_Wine11_Test}"
INSTALL="${2:-$WINEPREFIX/drive_c/Program Files (x86)/Steam/steamapps/common/Half-Life Alyx}"
OUT="${3:-$VERANTYX_VR_DRIVE/alyx_linux}"
[[ -d "$INSTALL/game/hlvr" ]] || { echo "no Half-Life: Alyx content at $INSTALL" >&2; exit 1; }
[[ -d "$DEPOT/game/bin/linuxsteamrtarm64" ]] || { echo "no ARM64 binaries at $DEPOT" >&2; exit 1; }

rm -rf "$OUT"; mkdir -p "$OUT/game"
# Everything from the content install, by symlink, except the directories that
# must be merged with the Linux depot.
merge_dir() {   # merge_dir <relative dir>: real dir whose entries link to the install
    local rel="$1"
    local src="$INSTALL/$rel"
    local dst="$OUT/$rel"
    mkdir -p "$dst"
    [[ -d "$src" ]] || return 0
    for e in "$src"/* "$src"/.[!.]*; do
        [[ -e "$e" ]] || continue
        ln -sfn "$e" "$dst/$(basename "$e")"
    done
}
for e in "$INSTALL"/*; do
    b="$(basename "$e")"
    [[ "$b" == game ]] && continue
    ln -sfn "$e" "$OUT/$b"
done
for e in "$INSTALL"/game/*; do
    b="$(basename "$e")"
    [[ "$b" == bin || "$b" == hlvr ]] && continue
    ln -sfn "$e" "$OUT/game/$b"
done
merge_dir game/bin
merge_dir game/hlvr
merge_dir game/hlvr/bin
# Panorama's videos: an install whose panorama/videos was renamed (videos.bak,
# an old Wine-path workaround) still gets them here, or the in-game screens
# start players with an empty URL and libvideo crashes. The install is not
# touched; only this root sees "videos".
if [[ ! -d "$INSTALL/game/hlvr/panorama/videos" && -d "$INSTALL/game/hlvr/panorama/videos.bak" ]]; then
    rm -f "$OUT/game/hlvr/panorama"
    merge_dir game/hlvr/panorama
    ln -sfn "$INSTALL/game/hlvr/panorama/videos.bak" "$OUT/game/hlvr/panorama/videos"
fi
# The Linux ARM64 binaries, and the depot's own copies of anything else it ships.
ln -sfn "$DEPOT/game/bin/linuxsteamrtarm64" "$OUT/game/bin/linuxsteamrtarm64"
ln -sfn "$DEPOT/game/hlvr/bin/linuxsteamrtarm64" "$OUT/game/hlvr/bin/linuxsteamrtarm64"
for e in "$DEPOT"/game/*; do
    b="$(basename "$e")"
    [[ -e "$OUT/game/$b" ]] || ln -sfn "$e" "$OUT/game/$b"
done
# steam_appid.txt lets steam_api identify the game when it is not launched by Steam.
[[ -e "$OUT/game/bin/linuxsteamrtarm64/steam_appid.txt" ]] || echo 546560 > "$OUT/steam_appid.txt"
echo "Game root: $OUT"
ls "$OUT/game" | tr '\n' ' '; echo
ls "$OUT/game/hlvr" | head -8 | tr '\n' ' '; echo
