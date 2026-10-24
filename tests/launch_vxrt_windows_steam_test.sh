#!/usr/bin/env bash
# scripts/launch_vxrt_windows_steam.sh and a Wine prefix: refused however it is reached (the Steam folder, drive_c, the
# root or a folder above it linked in from one; VXRT_WSTEAM_ROOT one, linked to one, not there yet below a link into
# one, or a link to nothing in one; a symbolic link into one where VXRuntime or Steam write; steam.exe a link), and
# nothing written when refused. A clean root passes, also reached through a link, with the folder and VXRT_ROOT where
# they really are. Made-up folders and made-up PE headers only; nothing is started (VXRT_WSTEAM_DRY_RUN=1).
set -euo pipefail

SCRIPT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/scripts/launch_vxrt_windows_steam.sh"
T="$(mktemp -d "${TMPDIR:-/tmp}/wsteam-test.XXXXXX")"
trap 'rm -rf "$T"' EXIT
T="$(realpath "$T")"            # (macOS: /var is /private/var)
fail() { echo "launch_vxrt_windows_steam test failed: $*" >&2; exit 1; }

# A PE header and nothing else: $2 the machine, $3 the optional header's magic (made up, enough for file(1)).
pe() {
    head -c 512 /dev/zero >"$1"
    printf 'MZ' | dd of="$1" bs=1 seek=0 conv=notrunc 2>/dev/null
    printf '\x40\x00\x00\x00' | dd of="$1" bs=1 seek=60 conv=notrunc 2>/dev/null
    printf "PE\x00\x00$2\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xf0\x00\x22\x00$3" |
        dd of="$1" bs=1 seek=64 conv=notrunc 2>/dev/null
}
steam() { mkdir -p "$1"; pe "$1/steam.exe" '\x64\x86' '\x0b\x02'; }       # a 64-bit "steam.exe"
SUB="drive_c/Program Files (x86)/Steam"

W="$T/wine"                                                                # a Wine prefix
mkdir -p "$W/dosdevices" "$W/drive_c/windows/system32" "$W/$SUB/steamapps/common/Game" "$W/drive_c/settings"
echo 'WINE REGISTRY Version 2' >"$W/system.reg"; echo 'WINE REGISTRY Version 2' >"$W/user.reg"
steam "$W/$SUB"
steam "$T/standalone/Steam"                                                # a Steam folder in no root
mkdir -p "$T/library"                                                      # a folder in no Wine prefix
mkdir -p "$T/home" "$T/drive"

snapshot() { find "$T" -print0 | xargs -0 stat -f '%N %HT %z %m' | LC_ALL=C sort; }
# run [NAME=value ...] -- [arguments]: OUT, RC (in $T/home, with nothing from the caller's environment)
run() {
    local envs=()
    while [[ "$1" != -- ]]; do envs+=("$1"); shift; done; shift
    set +e
    OUT="$(cd "$T/home" && env -i PATH="$PATH" HOME="$T/home" VERANTYX_VR_DRIVE="$T/drive" VXRT_REDIST="$T/redist" \
        VXRT_WSTEAM_LOG="$T/home/logs/wsteam.log" VXRT_WSTEAM_DRY_RUN=1 ${envs[@]+"${envs[@]}"} bash "$SCRIPT" "$@" 2>&1)"
    RC=$?
    set -e
}
# refused <what> <reason in the output> [NAME=value ...] -- [arguments]
refused() {
    local what="$1" reason="$2" before after; shift 2
    before="$(snapshot)"; run "$@"; after="$(snapshot)"
    [[ $RC -ne 0 ]] || fail "$what: not refused: $OUT"
    [[ "$OUT" == *"$reason"* ]] || fail "$what: refused for another reason: $OUT"
    [[ "$before" == "$after" ]] || fail "$what: wrote: $(diff <(echo "$before") <(echo "$after") || true)"
}
# passes <what> <Steam folder, really> <VXRT_ROOT, really> [NAME=value ...] -- [arguments]
passes() {
    local what="$1" dir="$2" root="$3" before after; shift 3
    before="$(snapshot)"; run "$@"; after="$(snapshot)"
    [[ $RC -eq 0 ]] || fail "$what: refused: $OUT"
    [[ "$OUT" == *"Would start, in $dir:"* ]] || fail "$what: not the Steam folder $dir: $OUT"
    [[ "$OUT" == *$'\n'"VXRT_ROOT=$root"* ]] || fail "$what: not VXRT_ROOT=$root: $OUT"
    [[ "$before" == "$after" ]] || fail "$what: dry run wrote: $(diff <(echo "$before") <(echo "$after") || true)"
}
P="is in a Wine prefix"

# The Steam folder.
refused "Steam folder in a Wine prefix" "$P" -- "$W/$SUB"
mkdir -p "$T/vx1/drive_c"; ln -s "$W/$SUB" "$T/vx1/drive_c/Steam"          # (the review's case)
refused "Steam folder linked from a Wine prefix" "$P" -- "$T/vx1/drive_c/Steam"
mkdir -p "$T/vx2"; ln -s "$W/drive_c" "$T/vx2/drive_c"
refused "drive_c linked from a Wine prefix" "$P" -- "$T/vx2/$SUB"
ln -s "$W" "$T/alias"
refused "root linked to a Wine prefix" "$P" -- "$T/alias/$SUB"
refused "default folder, VXRT_WSTEAM_ROOT linked to a Wine prefix" "$P" VXRT_WSTEAM_ROOT="$T/alias" --
refused "Steam folder named by VXRT_WSTEAM_DIR through a link" "$P" VXRT_WSTEAM_DIR="$T/vx1/drive_c/Steam" --

# VXRT_ROOT, for a Steam folder in no root.
refused "VXRT_WSTEAM_ROOT a Wine prefix" "VXRT_ROOT $W $P" VXRT_WSTEAM_ROOT="$W" -- "$T/standalone/Steam"
refused "VXRT_WSTEAM_ROOT linked to a Wine prefix" "$P" VXRT_WSTEAM_ROOT="$T/alias" -- "$T/standalone/Steam"
refused "VXRT_WSTEAM_ROOT not there yet, below a link into a Wine prefix" "$P" \
    VXRT_WSTEAM_ROOT="$T/alias/new/root" -- "$T/standalone/Steam"
ln -s "$W/newroot" "$T/dangling"
refused "VXRT_WSTEAM_ROOT a link to nothing in a Wine prefix" "$P" VXRT_WSTEAM_ROOT="$T/dangling" -- "$T/standalone/Steam"
refused "VXRT_WSTEAM_ROOT below a link to nothing in a Wine prefix" "$P" \
    VXRT_WSTEAM_ROOT="$T/dangling/sub" -- "$T/standalone/Steam"
ln -s "$T/loop2" "$T/loop1"; ln -s "$T/loop1" "$T/loop2"
refused "VXRT_WSTEAM_ROOT links in a circle" "cannot tell where it really is" \
    VXRT_WSTEAM_ROOT="$T/loop1/root" -- "$T/standalone/Steam"

# Links in a clean root or Steam folder into a Wine prefix, where VXRuntime or Steam write.
steam "$T/r1/$SUB"; ln -s "$W/system.reg" "$T/r1/registry.txt"
refused "registry.txt linked into a Wine prefix" "symbolic link" -- "$T/r1/$SUB"
steam "$T/r2/$SUB"; ln -s "$W/drive_c/windows" "$T/r2/drive_c/windows"
refused "drive_c/windows linked into a Wine prefix" "symbolic link" -- "$T/r2/$SUB"
steam "$T/r3/$SUB"; mkdir -p "$T/r3/$SUB/steamapps/common"; ln -s "$W/$SUB/steamapps/common/Game" "$T/r3/$SUB/steamapps/common/Game"
refused "game folder linked from a Wine prefix" "symbolic link" -- "$T/r3/$SUB"
steam "$T/r4/$SUB"; ln -s "$W/drive_c/newlogs" "$T/r4/$SUB/logs"
refused "Steam's logs a link to nothing in a Wine prefix" "symbolic link" -- "$T/r4/$SUB"
steam "$T/r5/$SUB"; ln -s "$W/drive_c/settings" "$T/r5/$SUB/config"
refused "Steam's config linked into a Wine prefix" "symbolic link" -- "$T/r5/$SUB"
mkdir -p "$T/r6/$SUB"; ln -s "$W/$SUB/steam.exe" "$T/r6/$SUB/steam.exe"
refused "steam.exe a link into a Wine prefix" "is a symbolic link" -- "$T/r6/$SUB"
mkdir -p "$T/r7/$SUB"; pe "$T/r7/$SUB/steam.exe" '\x4c\x01' '\x0b\x01'
refused "32-bit steam.exe" "not a 64-bit Windows program" -- "$T/r7/$SUB"
[[ ! -e "$W/newroot" && ! -e "$W/new" && ! -e "$W/drive_c/newlogs" ]] || fail "something was created in the Wine prefix"

# A clean root: the folder and VXRT_ROOT where they really are, nothing created by a dry run.
steam "$T/vx/$SUB"
passes "clean root" "$T/vx/$SUB" "$T/vx" -- "$T/vx/$SUB"
ln -s "$T/vx" "$T/vxalias"
passes "clean root through a link" "$T/vx/$SUB" "$T/vx" -- "$T/vxalias/$SUB"
passes "default folder in a clean root through a link" "$T/vx/$SUB" "$T/vx" VXRT_WSTEAM_ROOT="$T/vxalias" --
passes "Steam folder in no root, VXRT_WSTEAM_ROOT not there yet" "$T/standalone/Steam" "$T/newroot" \
    VXRT_WSTEAM_ROOT="$T/newroot" -- "$T/standalone/Steam"
passes "relative VXRT_WSTEAM_ROOT, from where the script was started" "$T/standalone/Steam" "$T/home/rel" \
    VXRT_WSTEAM_ROOT="rel" -- "$T/standalone/Steam"
ln -s "$T/library" "$T/vx/$SUB/steamapps"
passes "a link to a folder in no Wine prefix" "$T/vx/$SUB" "$T/vx" -- "$T/vx/$SUB"
[[ ! -e "$T/newroot" && ! -e "$T/home/rel" && ! -e "$T/home/logs" ]] || fail "a dry run created the root or the log"

echo "launch_vxrt_windows_steam: Wine prefix refused through links, roots not there yet and links where VXRuntime and Steam write; clean roots pass where they really are: ok"
