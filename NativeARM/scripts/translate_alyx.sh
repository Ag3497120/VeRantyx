#!/usr/bin/env bash
# Translates every ARM64 library of the Alyx Linux depot (and the Debian
# runtime libraries) into Mach-O dylibs with klepton-ld.
#
#   NativeARM/scripts/translate_alyx.sh <depot_546564 dir> [out dir]
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPOT="${1:?usage: translate_alyx.sh <depot_546564 dir> [out dir]}"
OUT="${2:-$SCRIPT_DIR/../build/dylibs}"
LD="${KLEPTON_LD:-$SCRIPT_DIR/../build/Klepton/build/klepton-ld}"
DEBIAN="$SCRIPT_DIR/../build/debian/usr/lib/aarch64-linux-gnu"
[[ -x "$LD" ]] || { echo "klepton-ld not built; run fetch_klepton.sh" >&2; exit 1; }
mkdir -p "$OUT/game" "$OUT/hlvr" "$OUT/system"
translate() {
    local src="$1" dst="$2"
    [[ -f "$src" && ! -L "$src" ]] || return 0
    [[ "$dst" -nt "$src" ]] && return 0
    "$LD" "$src" -o "$dst" --platform macos --quiet 2>&1 | grep -E 'refused [1-9]|error' || true
}
for f in "$DEPOT"/game/bin/linuxsteamrtarm64/*.so*; do
    b="$(basename "$f")"; [[ "$b" == libvulkan_freedreno* ]] && continue
    translate "$f" "$OUT/game/${b%%.so*}.dylib"
done
for f in "$DEPOT"/game/hlvr/bin/linuxsteamrtarm64/*.so*; do
    translate "$f" "$OUT/hlvr/$(basename "${f%%.so*}").dylib"
done
if [[ -d "$DEBIAN" ]]; then
    for f in "$DEBIAN"/*.so*; do
        translate "$f" "$OUT/system/$(basename "${f%%.so*}").dylib"
    done
fi
echo "Translated: $(ls "$OUT"/game "$OUT"/hlvr "$OUT"/system 2>/dev/null | grep -c dylib) dylibs in $OUT"
