#!/bin/sh
# xros_shadercache.sh [compile <pending dir> <out dir> | device]: the Metal shader cache of the visionOS app made on the
# Mac (VISIONOS_PORT.md, "shaders"). The device compiles no source offline; the runtime there writes each source it had
# to compile to Documents/shadercache/pending/<key>.metal with <key>.flags (src/d3d12.m). Here each is compiled with
# xcrun -sdk xros metal, the options it had there, into <key>.metallib: the key the runtime looks up.
#   compile <pending> <out>: just that (the Mac's own sources can be compiled the same way)
#   device (default): pending from the headset, compiled, the .metallib files back into Documents/shadercache
set -e
DEV=${DEV:-717AC0D2-8713-5537-8D5F-A611235E7D35}
APP=${APP:-marimokodai.VXRuntimeHost}
compile() {
    in=$1; out=$2; mkdir -p "$out"; n=0; t0=$(date +%s)
    for m in "$in"/*.metal; do
        [ -f "$m" ] || continue
        k=$(basename "$m" .metal); set -- $(cat "$in/$k.flags" 2>/dev/null || echo "0 0 0")
        args="-std=metal3.1"; [ "$1" = 1 ] && args="-std=metal3.2"
        [ "$2" = 1 ] && args="$args -DVXRT_HAS_MIN_LOD_CLAMP=1"
        [ "$3" = 1 ] && args="$args -fno-fast-math"
        xcrun -sdk xros metal $args -w -o "$out/$k.tmp.metallib" "$m" && mv "$out/$k.tmp.metallib" "$out/$k.metallib" && n=$((n + 1))
    done
    echo "xros_shadercache: $n libraries for visionOS in $(( $(date +%s) - t0 )) s -> $out"
}
case "${1:-device}" in
compile) compile "$2" "$3" ;;
device)
    w=${OUT:-$HOME/Verantyx_VR_Drive/wt/runs/avp-port/shadercache}; rm -rf "$w"; mkdir -p "$w"
    xcrun devicectl device copy from --device "$DEV" --domain-type appDataContainer --domain-identifier "$APP" \
        --source Documents/shadercache/pending --destination "$w/pending" > "$w/pull.txt" 2>&1 || { tail -3 "$w/pull.txt"; exit 1; }
    compile "$w/pending" "$w/out"
    for f in "$w"/out/*.metallib; do
        xcrun devicectl device copy to --device "$DEV" --domain-type appDataContainer --domain-identifier "$APP" \
            --source "$f" --destination "Documents/shadercache/$(basename "$f")" > /dev/null
    done
    echo "xros_shadercache: $(ls "$w"/out/*.metallib | wc -l | tr -d ' ') libraries copied into Documents/shadercache" ;;
esac
