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
# and registry, and Steam writes its files where it is installed. Paths are followed to where they really are
# (symbolic links resolved), so a Wine prefix reached through a link is refused too: copy Steam into the root, do not
# link it.
#
# What it does:
# - VXRT_ROOT is the root the folder is really in (<root>/drive_c/...), else $VXRT_WSTEAM_ROOT. A game started later
#   with the same VXRT_ROOT and without VXRT_STEAM_BRIDGE finds this Steam as on Windows: the registry
#   (HKCU\Software\Valve\Steam\ActiveProcess, written by steam.exe) and the named objects SteamStub looks for.
# - Steam's own options, checked on 2026-10-24 against the command line steam.exe gave steamwebhelper.exe:
#   -cef-disable-gpu (the web helper gets --disable-gpu --disable-gpu-compositing) and -cef-disable-sandbox
#   (--no-sandbox; Chromium's sandbox could not be in force here anyway, VXRuntime/README.md "Known risks").
#   -cef-single-process and -cef-in-process-gpu, named in older lists, add nothing in this client.
# - The web helper's own arguments, which no Steam option sets, come from VXRuntime (VXRT_EXTRA_ARGS,
#   VXRuntime/src/launchargs.c). By default the GPU stays on (no -cef-disable-gpu) and the web helper gets
#   --single-process (Chromium's child processes need handles passed between processes, not there yet) and
#   --disable-gpu-compositing: ANGLE on D3D11 rasterizes, GDI composites. This is what the Mac app and
#   launch_vxrt_nms_vr.sh start it with, and what ran signed in for hours with games (2026-10-25..28).
#   VXRT_WSTEAM_GPU=0 turns the GPU off (-cef-disable-gpu, and --single-process --disable-software-rasterizer;
#   without the latter Chromium keeps trying to make GL contexts for SwiftShader, logs "Failed to create shared
#   context for virtualization", draws nothing and ends itself). Only the sign-in window lives with that: once
#   signed in, the first raster/GL context Steam's UI asks for (some seconds after steam.exe runs
#   gldriverquery64.exe / vulkandriverquery64.exe, e.g. around a game launch) ends the web helper with
#   "FATAL:gles2_cmd_decoder.cc Validating command decoder is not supported" (int3 at libcef.dll+0x691ae98), and
#   Steam restarts it every ~10 s for ever (2026-10-28).
#   VXRT_WSTEAM_WEBHELPER_ARGS replaces the web helper arguments (empty: none).
# - No VXRT_STEAM_BRIDGE (the Mac Steam bridge would stand in for this Steam).
# - Refuses a Wine prefix: the folder or the root in one, any symbolic link down to three levels in either that leads
#   into one (that is where VXRuntime and Steam write), and a steam.exe that is itself a symbolic link.
# - Refuses while another steam.exe runs on VXRuntime: VXRuntime's named objects are the same for every root, so
#   two would take each other's.
# - What is written: the root, if it is not there yet; in it, VXRuntime's registry.txt, dpapi/ and its C: drive
#   (drive_c/windows, drive_c/users); in the Steam folder, what Steam itself writes there (its updates, logs, config,
#   web cache), as on Windows; and this script's log, ~/Library/Logs/VeRantyx/vxrt-windows-steam.log (the last
#   start's: .prev.log).
# - Steam restarting itself (after it has updated itself): the steam.exe it starts runs on, and the script waits for it.
# - VXRT_WSTEAM_DRY_RUN=1: only the checks; prints what it would start, starts and creates nothing.
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
if [[ ! -f "$STEAM_DIR/steam.exe" ]]; then
    echo "No Windows Steam in $STEAM_DIR." >&2
    echo "Install Windows Steam there first (its installer from Valve, run on VXRuntime with VXRT_ROOT=$WSTEAM_ROOT), then sign in in Steam's window." >&2
    exit 1
fi

# Where a path really is: every symbolic link followed, also for a path not there yet (the folder above it, where it
# really is, and the rest added; a link to something not there yet: where it points). Fails for links that go round in
# circles and for . or .. in the part not there yet.
physical() {
    local p="$1" n="${2:-0}" dir base target
    (( n <= 40 )) || return 1
    if [[ -e "$p" ]]; then realpath -- "$p"; return; fi
    dir="$(dirname -- "$p")"; base="$(basename -- "$p")"
    dir="$(physical "$dir" $((n + 1)))" || return 1
    [[ "$dir" == / ]] && dir=""
    if [[ -L "$dir/$base" ]]; then
        target="$(readlink -- "$dir/$base")" || return 1
        [[ "$target" == /* ]] || target="$dir/$target"
        physical "$target" $((n + 1)); return
    fi
    [[ "$base" != . && "$base" != .. ]] || return 1
    printf '%s/%s\n' "$dir" "$base"
}
# The Wine prefix a path (where it really is) is in: the nearest folder at or above it with Wine's registry files or
# dosdevices. None: fails.
wine_prefix_of() {
    local d="$1"
    while :; do
        if [[ -f "$d/system.reg" || -f "$d/user.reg" || -f "$d/userdef.reg" || -d "$d/dosdevices" ]]; then
            printf '%s\n' "$d"; return 0
        fi
        [[ "$d" == / || -z "$d" ]] && return 1
        d="$(dirname -- "$d")"
    done
}
# Ends the script when $2 ($1 says what it is) leads into a Wine prefix, or where it leads cannot be told.
refuse_wine() {
    local what="$1" path="$2" phys prefix
    if ! phys="$(physical "$path")"; then
        echo "$what $path: cannot tell where it really is (symbolic links in a circle, or . or .. in a part not there yet); refused." >&2
        exit 1
    fi
    if prefix="$(wine_prefix_of "$phys")"; then
        echo "$what $path is in a Wine prefix ($prefix; really $phys): VXRuntime does not run from or write into a Wine prefix." >&2
        echo "Install Windows Steam into a VXRuntime root of its own (VXRT_WSTEAM_ROOT, default $VERANTYX_VR_DRIVE/vxrt_wsteam), copied, not linked (on APFS, cp -c -R copies without taking more space)." >&2
        exit 1
    fi
}
# The same for a folder and every symbolic link in it down to three levels: where VXRuntime and Steam write
# (registry.txt, dpapi/, drive_c/windows/system32, drive_c/users/<name>; Steam's logs, config, steamapps/common/<game>).
# Steam's config/ and userdata/ are looked at as entries only, never inside.
refuse_wine_tree() {
    local what="$1" dir="$2" link
    refuse_wine "$what" "$dir"
    [[ -d "$dir" ]] || return 0
    while IFS= read -r -d '' link; do
        refuse_wine "A symbolic link in $what $dir:" "$dir/${link#./}"
    done < <(cd -P -- "$dir" && find . -maxdepth 3 \( -path ./config -o -path ./userdata \) ! -type l -prune -o -type l -print0)
}

if [[ -L "$STEAM_DIR/steam.exe" ]]; then
    echo "$STEAM_DIR/steam.exe is a symbolic link: start Windows Steam from the folder it really is in, a VXRuntime root's (copied, not linked)." >&2
    exit 1
fi
refuse_wine_tree "the Windows Steam folder" "$STEAM_DIR"
STEAM_DIR="$(physical "$STEAM_DIR")"
# 64-bit only (Steam has been 64-bit on Windows since December 2025; VXRuntime runs no 32-bit programs).
file -b "$STEAM_DIR/steam.exe" | grep -q 'PE32+' || { echo "$STEAM_DIR/steam.exe is not a 64-bit Windows program: an older Steam; VXRuntime runs 64-bit programs only." >&2; exit 1; }

# VXRT_ROOT: the root the folder is really in, else the Windows Steam root; never a Wine prefix either (VXRuntime writes
# its registry and C: drive there), and only a folder or nothing yet.
case "$STEAM_DIR" in
    */drive_c/*) ROOT="${STEAM_DIR%%/drive_c/*}" ;;
    *)           ROOT="$WSTEAM_ROOT" ;;
esac
refuse_wine_tree "VXRT_ROOT" "$ROOT"
ROOT="$(physical "$ROOT")"
[[ ! -e "$ROOT" || -d "$ROOT" ]] || { echo "VXRT_ROOT $ROOT is not a folder." >&2; exit 1; }

STEAM_ARGS=(-cef-disable-sandbox)
if [[ "${VXRT_WSTEAM_GPU:-1}" == 1 ]]; then DEFAULT_WEBHELPER_ARGS="--single-process --disable-gpu-compositing"
else STEAM_ARGS=(-cef-disable-gpu "${STEAM_ARGS[@]}"); DEFAULT_WEBHELPER_ARGS="--single-process --disable-software-rasterizer"; fi
[[ "${VXRT_WSTEAM_SILENT:-0}" == 1 ]] && STEAM_ARGS+=(-silent)
WEBHELPER_ARGS="${VXRT_WSTEAM_WEBHELPER_ARGS-$DEFAULT_WEBHELPER_ARGS}"
ENV=("VXRT_ROOT=$ROOT" "VXRT_DEBUG_OUTPUT=1" "VXRT_LOG_TIME=1")
[[ -n "$WEBHELPER_ARGS" ]] && ENV+=("VXRT_EXTRA_ARGS=steamwebhelper.exe=$WEBHELPER_ARGS${VXRT_EXTRA_ARGS:+;$VXRT_EXTRA_ARGS}")
if [[ -f "$VXRT_REDIST/manifest.json" ]]; then ENV+=("VXRT_DLL_PATH=$VXRT_REDIST")
else echo "Note: no Microsoft runtime DLLs in $VXRT_REDIST (tools/vxrt_redist.py collect); Steam brings most of its own" >&2; fi

if [[ "${VXRT_WSTEAM_DRY_RUN:-0}" == 1 ]]; then
    echo "Dry run, nothing started or created. Would start, in $STEAM_DIR:"
    printf ' '; printf ' %q' env -u VXRT_STEAM_BRIDGE "${ENV[@]}" "$VXRT" steam.exe "${STEAM_ARGS[@]}" "$@"; echo
    echo "VXRT_ROOT=$ROOT"
    exit 0
fi

[[ -x "$VXRT" ]] || { echo "No VXRuntime build at $VXRT: make -C VXRuntime" >&2; exit 1; }
STEAM_EXE_PATTERN='vxrt(-arm64)? (.*/)?[Ss][Tt][Ee][Aa][Mm]\.[Ee][Xx][Ee]( |$)'
running="$(pgrep -f "$STEAM_EXE_PATTERN" || true)"
if [[ -n "$running" ]]; then
    echo "Windows Steam is already running on VXRuntime (process $(echo $running)); quit it first." >&2
    exit 1
fi
[[ -f "$STEAM_DIR/bin/cef/cef.win64/steamwebhelper_real.exe" ]] && echo "Note: this folder has the Wine version's steamwebhelper shim (steamwebhelper_real.exe); VXRuntime does not need it" >&2

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
trap cleanup EXIT
trap 'exit 129' HUP; trap 'exit 130' INT; trap 'exit 143' TERM   # (the script ends there: cleanup on EXIT)
cd "$STEAM_DIR"
set -m
env -u VXRT_STEAM_BRIDGE "${ENV[@]}" "$VXRT" steam.exe "${STEAM_ARGS[@]}" "$@" >"$LOG" 2>&1 &
STEAM_PID=$!
set +m
status=0
wait "$STEAM_PID" || status=$?
echo "steam.exe exited ($status; log: $LOG)"
# Steam starts itself again and ends (after it has installed an update of itself, or when asked to restart): the
# steam.exe it started is in this process group, and this script now waits for that one, as long as there is one.
while next="$(pgrep -g "$STEAM_PID" -f "$STEAM_EXE_PATTERN" | head -n 1)" && [[ -n "$next" ]]; do
    echo "steam.exe started steam.exe again (process $next); waiting for that one"
    while kill -0 "$next" 2>/dev/null; do sleep 1; done
    echo "steam.exe (process $next) ended"
done
