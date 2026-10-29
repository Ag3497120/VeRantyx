#!/bin/sh
# aot_compare.sh <exe> [args]: the program under Rosetta (the reference), on the ARM64 runtime's translator, and on its
# route-B module with VXRT_AOT_STRICT=1 (build/aot; no fallback allowed); output lines compared (not "info:" / "time:").
cd "$(dirname "$0")/.."
export VXRT_ROOT=build/root VXRT_NO_ACTIVATE=1
O=build/recomp/out; mkdir -p $O
exe=$1; shift
n=$(basename "$exe" .exe)
ROSETTA_ADVERTISE_AVX=1 build/vxrt "$exe" "$@" > $O/$n.rosetta 2>&1
build/vxrt-arm64 "$exe" "$@" > $O/$n.jit 2>&1
VXRT_AOT_STRICT=1 VXRT_AOT_DIR=$PWD/build/aot build/vxrt-arm64 "$exe" "$@" > $O/$n.aotb 2>&1
status=0
for k in rosetta jit aotb; do grep -v -E "^(info:|time:|\[vxrt\])" $O/$n.$k > $O/$n.$k.cmp; done
for k in jit aotb; do
    if cmp -s $O/$n.rosetta.cmp $O/$n.$k.cmp; then echo "$n $k: identical to Rosetta"; else echo "$n $k: DIFFERS"; diff $O/$n.rosetta.cmp $O/$n.$k.cmp | head -10; status=1; fi
done
grep "aot:.*entries" $O/$n.aotb
exit $status
