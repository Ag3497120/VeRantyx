#!/usr/bin/env bash
# Prints this Mac's hardware class and the VeRantyx defaults for it as shell
# assignments. launch_vision_pro_vr.sh evaluates the output, so any variable
# the user already set wins over the profile's value.
#
#   eval "$(./scripts/mac_profile.sh)"
#   ./scripts/mac_profile.sh --describe     # human-readable
#
# Tiers (from performance cores, GPU cores and memory):
#   high  Max/Ultra class: 8+ performance cores and 24+ GPU cores, 32+ GB
#   mid   Pro class, or base M3/M4 with 16+ GB
#   low   base M1/M2, under 16 GB, or fewer than 4 performance cores
# The game is CPU-bound under Wine on every Apple Silicon Mac tested, so the
# tiers mostly trade resolution and display rate for frame rate; the
# virtual display rate also sets how finely the game's frame rate quantizes.
set -euo pipefail

chip="$(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown)"
pcores="$(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || sysctl -n hw.physicalcpu 2>/dev/null || echo 4)"
ecores="$(sysctl -n hw.perflevel1.physicalcpu 2>/dev/null || echo 0)"
memory_gb=$(( $(sysctl -n hw.memsize 2>/dev/null || echo 17179869184) / 1073741824 ))
gpu_cores="$(system_profiler SPDisplaysDataType 2>/dev/null | awk -F': ' '/Total Number of Cores/ { print $2; exit }')"
gpu_cores="${gpu_cores:-8}"
generation="$(printf '%s' "$chip" | grep -oE 'M[0-9]+' | head -1 | tr -d M)"
generation="${generation:-1}"
macos="$(sw_vers -productVersion 2>/dev/null || echo 0)"

if [[ "$(uname -m)" != "arm64" ]]; then
    tier="unsupported"
elif (( pcores >= 8 && gpu_cores >= 24 && memory_gb >= 32 )); then
    tier="high"
elif (( memory_gb < 16 || pcores < 4 )); then
    tier="low"
elif (( pcores >= 6 || gpu_cores >= 14 || generation >= 3 )); then
    tier="mid"
else
    tier="low"
fi

case "$tier" in
    high)
        scale=0.55; hz=60; bitrate=20; width=1920; height=1080; fidelity=""; convert=auto ;;
    mid)
        scale=0.45; hz=60; bitrate=16; width=1728; height=972; fidelity=""; convert=auto ;;
    *)
        scale=0.35; hz=45; bitrate=12; width=1440; height=810; fidelity=2; convert=auto ;;
esac

if [[ "${1:-}" == "--describe" ]]; then
    echo "Chip: $chip ($pcores performance + $ecores efficiency cores, $gpu_cores GPU cores, ${memory_gb} GB, macOS $macos)"
    echo "Tier: $tier"
    echo "Defaults: SteamVR render scale $scale, virtual display $hz Hz, HEVC $bitrate Mbps, stream budget ${width}x${height}, frame conversion $convert${fidelity:+, Alyx fidelity level $fidelity}"
    exit 0
fi

printf 'VERANTYX_MAC_CHIP=%q\n' "$chip"
echo "VERANTYX_MAC_PCORES=$pcores"
echo "VERANTYX_MAC_ECORES=$ecores"
echo "VERANTYX_MAC_GPU_CORES=$gpu_cores"
echo "VERANTYX_MAC_MEMORY_GB=$memory_gb"
echo "VERANTYX_MAC_TIER=$tier"
echo ": \"\${VERANTYX_RENDER_TARGET_SCALE:=$scale}\""
echo ": \"\${VERANTYX_DISPLAY_HZ:=$hz}\""
echo ": \"\${VERANTYX_BITRATE_MBPS:=$bitrate}\""
echo ": \"\${VERANTYX_STREAM_WIDTH:=$width}\""
echo ": \"\${VERANTYX_STREAM_HEIGHT:=$height}\""
echo ": \"\${VERANTYX_GPU_CONVERT:=$convert}\""
[[ -z "$fidelity" ]] || echo ": \"\${HLA_FIDELITY_LEVEL:=$fidelity}\""
echo "export VERANTYX_MAC_TIER VERANTYX_RENDER_TARGET_SCALE VERANTYX_DISPLAY_HZ VERANTYX_BITRATE_MBPS VERANTYX_STREAM_WIDTH VERANTYX_STREAM_HEIGHT VERANTYX_GPU_CONVERT"
[[ -z "$fidelity" ]] || echo "export HLA_FIDELITY_LEVEL"
