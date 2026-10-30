#!/bin/sh
# xros_device_run.sh [test...]: VX Runtime's tests on the headset, one launch each (one Windows program per process),
# and their logs back into $OUT (default ~/Verantyx_VR_Drive/wt/runs/avp-port/device). The headset must be worn and
# unlocked for a launch (devicectl waits otherwise). Tests: none xros_clear d3d12 suite hello_crt seh_try unwind cxx_exceptions
# xros_child xros_input xros_vr_depth, arena_* (relocated into the arena), fw_* (frameworks), fwarena_suite; <test>@flat
# for the window only (the default is the immersive space). WAIT=<s> per test.
# VISIONOS_PORT.md has what each one shows.
DEV=${DEV:-717AC0D2-8713-5537-8D5F-A611235E7D35}
APP=marimokodai.VXRuntimeHost
OUT=${OUT:-$HOME/Verantyx_VR_Drive/wt/runs/avp-port/device}
WAIT=${WAIT:-40}
mkdir -p "$OUT"
[ $# -gt 0 ] || set -- none xros_clear hello_crt seh_try unwind suite cxx_exceptions d3d12
for arg in "$@"; do
    # (<test>@flat: the window only; otherwise the swap chain is shown in the immersive space, the app's default)
    t=${arg%@*}; pres=vr; [ "$t" != "$arg" ] && pres=${arg##*@}
    echo "== $t ($pres)"
    if ! xcrun devicectl --timeout 120 device process launch --device "$DEV" --terminate-existing "$APP" -- -test "$t" -present "$pres" > "$OUT/$t.launch.txt" 2>&1; then
        echo "launch failed (worn and unlocked?): $OUT/$t.launch.txt"; tail -3 "$OUT/$t.launch.txt"; exit 1
    fi
    sleep "$WAIT"
    xcrun devicectl --timeout 60 device copy from --device "$DEV" --domain-type appDataContainer --domain-identifier "$APP" \
        --source Documents/vxrt.log --destination "$OUT/$t.$pres.vxrt.log" > /dev/null 2>&1
    xcrun devicectl --timeout 60 device copy from --device "$DEV" --domain-type appDataContainer --domain-identifier "$APP" \
        --source Documents/host_log.jsonl --destination "$OUT/host_log.jsonl" > /dev/null 2>&1
    grep -v '"mem"' "$OUT/host_log.jsonl" 2>/dev/null | grep "\"test\":\"$t\"" | tail -8 | cut -c1-220
    grep -v "XInput" "$OUT/$t.$pres.vxrt.log" 2>/dev/null | tail -6 | cut -c1-220
done
