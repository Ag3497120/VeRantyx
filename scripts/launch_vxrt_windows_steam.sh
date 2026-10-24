#!/usr/bin/env bash
# The Windows Steam client on VXRuntime (research, VXRuntime/README.md "The Windows Steam client"): for games whose
# executable is wrapped in Steam's DRM (SteamStub), which a real Windows Steam you signed in to has to answer. You
# install Windows Steam into a VXRuntime root of its own and sign in yourself in Steam's own window; this script
# only starts it, until it exits or Ctrl-C.
#
#   scripts/launch_vxrt_windows_steam.sh [<Windows Steam folder>] [steam.exe arguments...]
#   VXRT_WSTEAM_SILENT=1 scripts/launch_vxrt_windows_steam.sh      # with -silent: Steam starts without its window
#
# The folder: the argument, else $VXRT_WSTEAM_DIR, else "$VXRT_WSTEAM_ROOT/drive_c/Program Files (x86)/Steam" with
# VXRT_WSTEAM_ROOT=~/Verantyx_VR_Drive/vxrt_wsteam. Never a Wine prefix (refused): VXRuntime keeps its own C: drive
# and registry, and Steam writes its files where it is installed.
#
# What it does:
# - VXRT_ROOT is the root the folder is in (<root>/drive_c/...), else $VXRT_WSTEAM_ROOT. A game started later with
#   the same VXRT_ROOT and without VXRT_STEAM_BRIDGE finds this Steam as on Windows: the registry
#   (HKCU\Software\Valve\Steam\ActiveProcess, written by steam.exe) and the named objects SteamStub looks for.
# - Steam's own options, checked on 2026-10-24 against the command line steam.exe gave steamwebhelper.exe:
#   -cef-disable-gpu (the web helper gets --disable-gpu --disable-gpu-compositing) and -cef-disable-sandbox
#   (--no-sandbox; Chromium's sandbox could not be in force here anyway, VXRuntime/README.md "Known risks").
#   -cef-single-process and -cef-in-process-gpu, named in older lists, add nothing in this client.
# - The web helper's --single-process, which no Steam option sets, comes from VXRuntime (VXRT_EXTRA_ARGS,
#   VXRuntime/src/launchargs.c): Chromium's child processes need handles passed between processes, not there yet.
#   VXRT_WSTEAM_WEBHELPER_ARGS replaces it (empty: none).
# - No VXRT_STEAM_BRIDGE (the Mac Steam bridge would stand in for this Steam).
# - Refuses while another steam.exe runs on VXRuntime: VXRuntime's named objects are the same for every root, so
#   two would take each other's.
# - Writes only its log: ~/Library/Logs/VeRantyx/vxrt-windows-steam.log (the last start's: .prev.log).
#
# "Steam Service Error" at start: the Steam Client Service (bin/SteamService.exe, a 32-bit Windows service) cannot
# be installed or run here; choose Cancel or close the window, and Steam goes on without it (VXRuntime/README.md).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
VERANTYX_VR_DRIVE="${VERANTYX_VR_DRIVE:-$HOME/Verantyx_VR_Drive}"
WSTEAM_ROOT="${VXRT_WSTEAM_ROOT:-$VERANTYX_VR_DRIVE/vxrt_wsteam}"
VXRT_REDIST="${VXRT_REDIST:-$VERANTYX_VR_DRIVE/vxrt_redist}"
LOG="${VXRT_WSTEAM_LOG:-$HOME/Library/Logs/VeRantyx/vxrt-windows-steam.log}"
VXRT="$REPO_ROOT/VXRuntime/build/vxrt"
[[ "${VXRT_ARM64:-0}" == 1 ]] && VXRT="$VXRT-arm64"

if [[ $# -ge 1 && "$1" != -* ]]; then STEAM_DIR="$1"; shift
else STEAM_DIR="${VXRT_WSTEAM_DIR:-$WSTEAM_ROOT/drive_c/Program Files (x86)/Steam}"; fi
[[ -x "$VXRT" ]] || { echo "No VXRuntime build at $VXRT: make -C VXRuntime" >&2; exit 1; }
if [[ ! -f "$STEAM_DIR/steam.exe" ]]; then
    echo "No Windows Steam in $STEAM_DIR." >&2
    echo "Install Windows Steam there first (its installer from Valve, run on VXRuntime with VXRT_ROOT=$WSTEAM_ROOT), then sign in in Steam's window." >&2
    exit 1
fi
STEAM_DIR="$(cd "$STEAM_DIR" && pwd)"

# Not a Wine prefix: a folder above it with Wine's registry files or dosdevices is one.
d="$STEAM_DIR"
while [[ "$d" != "/" ]]; do
    if [[ -f "$d/system.reg" || -f "$d/user.reg" || -d "$d/dosdevices" ]]; then
        echo "$STEAM_DIR is in a Wine prefix ($d): VXRuntime does not run from or write into a Wine prefix." >&2
        echo "Install Windows Steam into a VXRuntime root of its own (default: $WSTEAM_ROOT)." >&2
        exit 1
    fi
    d="$(dirname "$d")"
done
# 64-bit only (Steam has been 64-bit on Windows since December 2025; VXRuntime runs no 32-bit programs).
file -b "$STEAM_DIR/steam.exe" | grep -q 'PE32+' || { echo "$STEAM_DIR/steam.exe is not a 64-bit Windows program: an older Steam; VXRuntime runs 64-bit programs only." >&2; exit 1; }

# VXRT_ROOT: the root the folder is in, else the Windows Steam root.
case "$STEAM_DIR" in
    */drive_c/*) ROOT="${STEAM_DIR%%/drive_c/*}" ;;
    *)           ROOT="$WSTEAM_ROOT" ;;
esac

running="$(pgrep -f 'vxrt(-arm64)? (.*/)?[Ss][Tt][Ee][Aa][Mm]\.[Ee][Xx][Ee]( |$)' || true)"
if [[ -n "$running" ]]; then
    echo "Windows Steam is already running on VXRuntime (process $(echo $running)); quit it first." >&2
    exit 1
fi
[[ -f "$STEAM_DIR/bin/cef/cef.win64/steamwebhelper_real.exe" ]] && echo "Note: this folder has the Wine version's steamwebhelper shim (steamwebhelper_real.exe); VXRuntime does not need it" >&2

STEAM_ARGS=(-cef-disable-gpu -cef-disable-sandbox)
[[ "${VXRT_WSTEAM_SILENT:-0}" == 1 ]] && STEAM_ARGS+=(-silent)
WEBHELPER_ARGS="${VXRT_WSTEAM_WEBHELPER_ARGS-"--single-process"}"
ENV=("VXRT_ROOT=$ROOT" "VXRT_DEBUG_OUTPUT=1" "VXRT_LOG_TIME=1")
[[ -n "$WEBHELPER_ARGS" ]] && ENV+=("VXRT_EXTRA_ARGS=steamwebhelper.exe=$WEBHELPER_ARGS${VXRT_EXTRA_ARGS:+;$VXRT_EXTRA_ARGS}")
if [[ -f "$VXRT_REDIST/manifest.json" ]]; then ENV+=("VXRT_DLL_PATH=$VXRT_REDIST")
else echo "Note: no Microsoft runtime DLLs in $VXRT_REDIST (tools/vxrt_redist.py collect); Steam brings most of its own" >&2; fi

mkdir -p "$(dirname "$LOG")" "$ROOT"
[[ -s "$LOG" ]] && mv -f "$LOG" "${LOG%.log}.prev.log"   # (the last start's, kept)
echo "Windows Steam on VXRuntime: $STEAM_DIR (VXRT_ROOT=$ROOT, log: $LOG)"
echo "Games for this Steam: start them with VXRT_ROOT=$ROOT and without VXRT_STEAM_BRIDGE while it runs."

# steam.exe and everything it starts (the web helper, the crash reporter) in a process group of their own, stopped
# together when this script ends.
cleanup() {
    trap - EXIT HUP INT TERM
    if [[ -n "${STEAM_PID:-}" ]] && kill -0 -- "-$STEAM_PID" 2>/dev/null; then
        kill -TERM -- "-$STEAM_PID" 2>/dev/null || true
        for _ in 1 2 3 4 5; do kill -0 -- "-$STEAM_PID" 2>/dev/null || break; sleep 1; done
        kill -KILL -- "-$STEAM_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT HUP INT TERM
cd "$STEAM_DIR"
set -m
env -u VXRT_STEAM_BRIDGE "${ENV[@]}" "$VXRT" steam.exe "${STEAM_ARGS[@]}" "$@" >"$LOG" 2>&1 &
STEAM_PID=$!
set +m
status=0
wait "$STEAM_PID" || status=$?
echo "steam.exe exited ($status; log: $LOG)"
