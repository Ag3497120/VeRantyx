#!/bin/sh
# xros_device_run.sh [test...]: VX Runtime's tests on the headset, one launch each (one Windows program per process),
# and their logs back into $OUT (default ~/Verantyx_VR_Drive/wt/runs/avp-port/device). The headset must be worn and
# unlocked for a launch (devicectl waits otherwise). Tests: none d3d12 suite hello_crt seh_try unwind cxx_exceptions.
# VISIONOS_PORT.md has what each one shows.
DEV=${DEV:-717AC0D2-8713-5537-8D5F-A611235E7D35}
APP=marimokodai.VXRuntimeHost
OUT=${OUT:-$HOME/Verantyx_VR_Drive/wt/runs/avp-port/device}
WAIT=${WAIT:-40}
mkdir -p "$OUT"
[ $# -gt 0 ] || set -- none hello_crt seh_try unwind suite cxx_exceptions d3d12
for t in "$@"; do
    echo "== $t"
    if ! xcrun devicectl --timeout 120 device process launch --device "$DEV" --terminate-existing "$APP" -- -test "$t" > "$OUT/$t.launch.txt" 2>&1; then
        echo "launch failed (worn and unlocked?): $OUT/$t.launch.txt"; tail -3 "$OUT/$t.launch.txt"; exit 1
    fi
    sleep "$WAIT"
    xcrun devicectl device copy from --device "$DEV" --domain-type appDataContainer --domain-identifier "$APP" \
        --source Documents/vxrt.log --destination "$OUT/$t.vxrt.log" > /dev/null 2>&1
    xcrun devicectl device copy from --device "$DEV" --domain-type appDataContainer --domain-identifier "$APP" \
        --source Documents/host_log.jsonl --destination "$OUT/host_log.jsonl" > /dev/null 2>&1
    grep -v '"mem"' "$OUT/host_log.jsonl" 2>/dev/null | grep "\"test\":\"$t\"" | tail -8 | cut -c1-220
    grep -v "XInput" "$OUT/$t.vxrt.log" 2>/dev/null | tail -6 | cut -c1-220
done
