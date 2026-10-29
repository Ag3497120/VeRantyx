#!/bin/sh
# aot_build.sh <pe> [outdir]: route-B module for <pe> into outdir (default build/aot): vxrt-aot gen, then each part's
# assembly assembled in parallel into <name>-<hash>[.p<k>].dylib. The .s files go to build/aot_s (large; local only).
# Translated code is the program's own: build/aot and build/aot_s are never committed or shared (RECOMP.md).
set -e
cd "$(dirname "$0")/.."
pe=$1; out=${2:-build/aot}
mkdir -p "$out" build/aot_s
name=$(build/vxrt-aot name "$pe"); stem=${name%.dylib}
b=$(basename "$pe"); s=build/aot_s/$b.s
rm -f build/aot_s/$b.s build/aot_s/$b.p*.s "$out/$stem".dylib "$out/$stem".p*.dylib
build/vxrt-aot gen "$pe" "$s" > build/aot_s/$b.list
jobs=${AOT_JOBS:-$(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || echo 8)}
# (one part: <stem>.dylib; parts: <stem>.p<k>.dylib from <b>.p<k>.s)
while read -r f; do
    case "$f" in *.p[0-9]*.s) k=${f##*.p}; k=${k%.s}; echo "$f $out/$stem.p$k.dylib" ;; *) echo "$f $out/$stem.dylib" ;; esac
done < build/aot_s/$b.list | xargs -P "$jobs" -n 2 sh -c 'clang -arch arm64 -c -o "${0%.s}.o" "$0" && clang -arch arm64 -dynamiclib -o "$1" "${0%.s}.o"'
ls -l "$out/$stem"*.dylib
