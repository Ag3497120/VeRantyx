#!/usr/bin/env bash
# Builds the flat guest library directory the loader searches: every ARM64
# library of the Alyx Linux depot (game and hlvr), plus the Debian runtime
# libraries, each reachable by its SONAME.
#
#   NativeARM/scripts/make_guest_dir.sh <depot_546564 dir> [out dir]
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPOT="${1:?usage: make_guest_dir.sh <depot_546564 dir> [out dir]}"
OUT="${2:-$SCRIPT_DIR/../build/guest/lib}"
DEBIAN="$SCRIPT_DIR/../build/debian/usr/lib/aarch64-linux-gnu"
# Replaced rather than loaded: the Adreno Vulkan driver (MoltenVK is the ICD
# here) and mimalloc (every malloc binds to the host allocator).
SKIP="libvulkan_freedreno_hlvr.so libmimalloc.so"
# Debian libraries the game expects from the Steam Linux Runtime.
DEBIAN_SONAMES="libstdc++.so.6 libgcc_s.so.1 libogg.so.0 libglib-2.0.so.0 libgobject-2.0.so.0 libgio-2.0.so.0
libgmodule-2.0.so.0 libgthread-2.0.so.0 libz.so.1 libuuid.so.1 libssl.so.3 libcrypto.so.3 libpng16.so.16
libfribidi.so.0 libexpat.so.1 libbz2.so.1.0 libbrotlidec.so.1 libbrotlicommon.so.1 libzstd.so.1
libatomic.so.1 libpcre2-8.so.0 libffi.so.8 libmount.so.1 libblkid.so.1 libselinux.so.1"

rm -rf "$OUT"; mkdir -p "$OUT"
link() {   # link <file>: by SONAME and by file name
    local f="$1" b soname
    b="$(basename "$f")"
    for s in $SKIP; do [[ "$b" == "$s" ]] && return 0; done
    head -c4 "$f" | grep -q ELF || return 0
    ln -sfn "$f" "$OUT/$b"
    soname="$(objdump -p "$f" 2>/dev/null | awk '/SONAME/ {print $2}')"
    [[ -n "$soname" && "$soname" != "$b" ]] && ln -sfn "$f" "$OUT/$soname"
    # The depot's unversioned libfoo.so files are empty; the game dlopens some
    # libraries by that name, so it points at the real file too.
    local bare="${b%%.so*}.so"
    if [[ "$bare" != "$b" && ! -e "$OUT/$bare" ]]; then ln -sfn "$f" "$OUT/$bare"; fi
    return 0
}
for f in "$DEPOT"/game/bin/linuxsteamrtarm64/*.so* "$DEPOT"/game/hlvr/bin/linuxsteamrtarm64/*.so*; do
    [[ -L "$f" ]] && continue
    link "$f"
done
for s in $DEBIAN_SONAMES; do
    [[ -e "$OUT/$s" ]] && continue          # the game's own copy wins
    real="$(readlink -f "$DEBIAN/$s" 2>/dev/null || true)"
    if [[ -n "$real" && -f "$real" ]]; then ln -sfn "$real" "$OUT/$s"; else echo "missing Debian library: $s" >&2; fi
done
echo "$(ls "$OUT" | wc -l | tr -d ' ') entries in $OUT"
