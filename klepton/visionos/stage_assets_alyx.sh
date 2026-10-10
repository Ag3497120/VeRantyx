#!/bin/bash
# Stage Half-Life: Alyx's game folder into the app's container on a Vision Pro:
# Documents/alyx/assets = the depot's game/ folder (kl_app.c points
# KL_ALYX_GAME there). Code is in the bundle; this is data only.
#
#   ./stage_assets_alyx.sh --dry-run        # build the local tree, print its size
#   ./stage_assets_alyx.sh [device]         # ...and upload it (devicectl; ~50 GB, once)
#
# What goes: everything the engine reads, minus what the demo does not need —
#   - hlvr/maps: only startup (the menu) and ALYX_MAPS (default: Chapter 1,
#     a1_intro_world + a1_intro_world_2). All maps: ALYX_MAPS=all.
#   - no x86-64 binaries (bin/linuxsteamrt64, hlvr/bin/linuxsteamrt64);
#   - the soname symlinks, as empty files of the same name (required; devicectl drops links);
#   - no ARM64 ELF contents: their signed translations are in the bundle. Each
#     ELF is staged as an EMPTY file of the same name, because Source 2
#     resolves its own module and executable paths (realpath, readlink of
#     /proc/self/exe) and needs them to exist; loads resolve to the bundle's
#     translation by name and never read these.
# The local tree (ALYX_STAGE, default ~/alyx-depot/stage/alyx-assets) is made
# of APFS clones (cp -c), so it costs no disk space.
set -euo pipefail
cd "$(dirname "$0")"
DEPOT="${ALYX_DEPOT:-$HOME/alyx-depot}"
GAME="$DEPOT/linux/game"
STAGE="${ALYX_STAGE:-$DEPOT/stage/alyx-assets}"
MAPS="${ALYX_MAPS:-a1_intro_world a1_intro_world_2}"
DRY=0; DEVICE=""
for a in "$@"; do case "$a" in --dry-run) DRY=1 ;; *) DEVICE="$a" ;; esac; done
BUNDLE_ID=$(python3 targets.py alyx bundle)

keep_map() {   # <path relative to hlvr/maps>
  [ "$MAPS" = all ] && return 0
  local top=${1%%/*}; top=${top%.vpk}; top=${top%_commentary}
  [ "$top" = startup ] && return 0
  for m in $MAPS; do [ "$top" = "$m" ] && return 0; done
  return 1
}

echo "[stage-alyx] building $STAGE (maps: ${MAPS})"
rm -rf "$STAGE"; mkdir -p "$STAGE"
n=0
( cd "$GAME" && find . -type d ) | while read -r d; do mkdir -p "$STAGE/$d"; done
( cd "$GAME" && find . -type f ) | while read -r f; do
  f=${f#./}
  case "$f" in
    bin/linuxsteamrt64/*|hlvr/bin/linuxsteamrt64/*) continue ;;
    hlvr/maps/*) keep_map "${f#hlvr/maps/}" || continue ;;
  esac
  case "$f" in
    bin/linuxsteamrtarm64/*|hlvr/bin/linuxsteamrtarm64/*)
      if head -c4 "$GAME/$f" | grep -q ELF; then : > "$STAGE/$f"; continue; fi ;;
  esac
  cp -c "$GAME/$f" "$STAGE/$f"
done
# The soname links hlvr.sh creates (libavcodec.so.58 -> ..., libSDL2-2.0.so.0
# -> libSDL2.so) must EXIST under their names — without them the engine crashes
# early in its module loading — but `devicectl device copy to` silently drops
# symlinks (measured: 27 of 27 missing on the headset). Every one points at an
# ELF, which is an empty placeholder anyway, so stage each as an empty file.
( cd "$GAME" && find . -type l ) | while read -r l; do
  case "$l" in ./bin/linuxsteamrt64/*|./hlvr/bin/linuxsteamrt64/*) continue ;; esac
  : > "$STAGE/${l#./}"
done
rm -rf "$STAGE/bin/linuxsteamrt64" "$STAGE/hlvr/bin/linuxsteamrt64"
SIZE=$(du -sh "$STAGE" | cut -f1)
echo "[stage-alyx] local tree: $SIZE ($(find "$STAGE" -type f | wc -l | tr -d ' ') files)"
[ "$DRY" = 1 ] && exit 0

TARGET="${DEVICE:-$(xcrun devicectl list devices 2>/dev/null | awk '/available|connected/ && /Vision/ {print $3; exit}')}"
[ -n "$TARGET" ] || { echo "!! no paired Vision Pro found (xcrun devicectl list devices)"; exit 1; }
echo "[stage-alyx] uploading $SIZE to $TARGET:$BUNDLE_ID Documents/alyx/assets — this is the long part"
xcrun devicectl device copy to --device "$TARGET" \
  --domain-type appDataContainer --domain-identifier "$BUNDLE_ID" \
  --source "$STAGE" --destination "Documents/alyx/assets"
echo "[stage-alyx] done"
