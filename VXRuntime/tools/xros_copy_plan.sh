#!/bin/sh
# xros_copy_plan.sh [--run]: the plan for putting Cyberpunk 2077 on the Vision Pro (VISIONOS_PORT.md, "The game on the
# device"). Without --run it only measures and prints: what goes where, the sizes, and a time estimate. With --run it
# does it - ONLY after the user's explicit yes (the game's files and its translated code go onto the user's own
# headset; nothing is committed or shared). Steam-less: no Steam client is copied or emulated; the game finds none.
#
#   1. the game's AOT modules, made again for the headset's arena (XROS_ARENA, 0x7000000000: every image relocated
#      there, since the DLLs' preferred bases are in system ranges on the device), as signed frameworks in the app
#      (tools/xros_aot.sh fw:<pe>@<base>), then the app built and installed (devicectl device install app)
#   2. the install (read-only use) into the app's container: Documents/root/drive_c/Program Files (x86)/Steam/
#      steamapps/common/Cyberpunk 2077 (devicectl device copy to; or Finder file sharing)
#   3. UserSettings.json (lowest settings) into Documents/root/drive_c/users/vxrt/AppData/Local/CD Projekt Red/
#      Cyberpunk 2077 - from the Mac's settings copy, never UserSettings.orig.json; saves and Steam config not copied
#
#   GAME=<install>  (default: the Windows Steam install under ~/Verantyx_VR_Drive/vxrt_wsteam)
#   SETTINGS=<UserSettings.json>  DEV=<device id>  RATE_MBS=<measured devicectl MB/s, default 40>
set -e
cd "$(dirname "$0")/.."
DEV=${DEV:-717AC0D2-8713-5537-8D5F-A611235E7D35}
APP=marimokodai.VXRuntimeHost
GAME=${GAME:-$HOME/Verantyx_VR_Drive/vxrt_wsteam/drive_c/Program Files (x86)/Steam/steamapps/common/Cyberpunk 2077}
SETTINGS=${SETTINGS:-}
ARENA=${XROS_ARENA:-0x7000000000}
RATE=${RATE_MBS:-40}
DST_GAME="Documents/root/drive_c/Program Files (x86)/Steam/steamapps/common/Cyberpunk 2077"
DST_SET="Documents/root/drive_c/users/vxrt/AppData/Local/CD Projekt Red/Cyberpunk 2077"
RUN=0; [ "$1" = "--run" ] && RUN=1

[ -d "$GAME" ] || { echo "no install at $GAME"; exit 1; }
mb() { du -sm "$1" 2>/dev/null | cut -f1; }
echo "== Cyberpunk 2077 on the Vision Pro: plan ($( [ $RUN = 1 ] && echo RUN || echo 'dry run: nothing is copied' ))"
total=$(mb "$GAME")
echo "install: $GAME"
for d in archive bin engine r6 red4ext tools; do [ -d "$GAME/$d" ] && printf '  %-8s %6s MB\n' "$d" "$(mb "$GAME/$d")"; done
echo "  total    $total MB"
# the images the game loads that are its own (exe and DLLs in bin/x64 and plugins): each needs a module
pes=$(find "$GAME/bin/x64" -maxdepth 2 \( -iname '*.exe' -o -iname '*.dll' \) | sort)
npe=$(printf '%s\n' "$pes" | grep -c . || true)
img=0
for p in $(printf '%s\n' "$pes" | tr ' ' '\037'); do
    p=$(printf '%s' "$p" | tr '\037' ' ')
    sz=$(VXRT_AOT_COPY=1 build/vxrt-aot size "$p" 2>/dev/null || true)
    [ -n "$sz" ] && img=$((img + sz))
done
[ "$img" -gt 0 ] && echo "images: $npe PE files, $((img >> 20)) MB of address space in the arena at $ARENA (XROS_ARENA_MB must hold it)" \
                 || echo "images: $npe PE files (sizes: run vxrt-aot analyze; the arena at $ARENA)"
mac_aot=$HOME/Verantyx_VR_Drive/wt/avp-preflight/VXRuntime/build/aot
[ -d "$mac_aot" ] && echo "AOT modules on the Mac today: $(ls "$mac_aot" | wc -l | tr -d ' ') dylibs, $(mb "$mac_aot") MB (made for the Mac's arena: made again for $ARENA)"
fwmb=$(( $( [ -d "$mac_aot" ] && mb "$mac_aot" || echo 1300 ) ))
echo "app with the modules as frameworks: about $((fwmb + 25)) MB (install: devicectl device install app)"
secs=$(( (total + fwmb) / RATE ))
echo "time at $RATE MB/s (devicectl over the network; measure with a 1 GB file first): about $((secs / 60)) min, plus the modules' generation (~20-40 min on the Mac) and signing"
echo "free space needed on the headset: about $(( (total + fwmb) / 1024 + 3 )) GB (plus the shader cache, ~1-2 GB)"
echo
echo "commands:"
echo "  tools/xros_aot.sh fw:<each PE>@auto (XROS_ARENA=$ARENA), then the app built (modules as frameworks, local only)"
echo "  xcrun devicectl device install app --device $DEV build/xros/derived/Build/Products/Debug-xros/VXRuntimeHost.app"
echo "  xcrun devicectl device copy to --device $DEV --domain-type appDataContainer --domain-identifier $APP --source \"$GAME\" --destination \"$DST_GAME\""
[ -n "$SETTINGS" ] && echo "  xcrun devicectl device copy to ... --source \"$SETTINGS\" --destination \"$DST_SET/UserSettings.json\""
echo "  launch: xcrun devicectl device process launch --device $DEV $APP -exe \"C:/Program Files (x86)/Steam/steamapps/common/Cyberpunk 2077/bin/x64/Cyberpunk2077.exe\" (not built yet: -exe)"
[ $RUN = 1 ] || { echo; echo "dry run only. --run needs the user's explicit yes (62 GB of game data and its translated code onto the headset)."; exit 0; }

# ---- --run (only with the user's yes) ----
args=""
for p in $(printf '%s\n' "$pes" | tr ' ' '\037'); do args="$args fw:$(printf '%s' "$p" | tr '\037' ' ' | sed 's/ /\\ /g')@auto"; done
echo "the modules: tools/xros_aot.sh with every image relocated into the arena (bases packed from $ARENA)"
XROS_ARENA=$ARENA XROS_ARENA_MB=$(( (img >> 20) + 64 )) eval tools/xros_aot.sh $args
cd xros/app && xcodegen generate --quiet && cd ../..
xcodebuild -project xros/app/VXRuntimeHost.xcodeproj -scheme VXRuntimeHost -destination 'generic/platform=visionOS' -derivedDataPath build/xros/derived \
    -allowProvisioningUpdates -quiet ARCHS=arm64 VXRT_LIBDIR="$PWD/build/xros" build
xcrun devicectl device install app --device "$DEV" build/xros/derived/Build/Products/Debug-xros/VXRuntimeHost.app
xcrun devicectl device copy to --device "$DEV" --domain-type appDataContainer --domain-identifier "$APP" --source "$GAME" --destination "$DST_GAME"
[ -n "$SETTINGS" ] && xcrun devicectl device copy to --device "$DEV" --domain-type appDataContainer --domain-identifier "$APP" \
    --source "$SETTINGS" --destination "$DST_SET/UserSettings.json"
echo "done"
