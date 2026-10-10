#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
STEAM_ROOT="${STEAM_ROOT:-${WINEPREFIX:-$HOME/Verantyx_VR_Drive/SteamVR_Prefix}/drive_c/Program Files (x86)/Steam}"
CEF_DIR="$STEAM_ROOT/bin/cef/cef.win64"
TARGET="$CEF_DIR/steamwebhelper.exe"
REAL="$CEF_DIR/steamwebhelper_real.exe"
STATE="$CEF_DIR/.verantyx-steamwebhelper-shim.sha256"
SHIM="${STEAMWEBHELPER_SHIM:-$REPO_ROOT/build/steamwebhelper_shim.exe}"

if [[ ! -x "$SHIM" ]]; then
    "$SCRIPT_DIR/build_steamwebhelper_shim.sh"
fi
if [[ ! -f "$TARGET" ]]; then
    echo "Windows Steam WebHelper was not found: $TARGET" >&2
    exit 1
fi

shim_hash="$(shasum -a 256 "$SHIM" | awk '{print $1}')"
current_hash="$(shasum -a 256 "$TARGET" | awk '{print $1}')"
previous_shim_hash=""
if [[ -f "$STATE" ]]; then
    previous_shim_hash="$(cat "$STATE")"
fi

if [[ "$current_hash" == "$shim_hash" ]]; then
    if [[ ! -f "$REAL" ]]; then
        echo "The project shim is installed but its original WebHelper backup is missing: $REAL" >&2
        exit 1
    fi
    printf '%s\n' "$shim_hash" > "$STATE"
    echo "Windows Steam WebHelper shim is already installed."
    exit 0
fi

if ! file -b "$TARGET" | grep -Eq '^PE32\+ executable.*x86-64'; then
    echo "Refusing to replace a non-x86-64 Steam WebHelper: $TARGET" >&2
    exit 1
fi

if [[ -f "$REAL" ]]; then
    if [[ -z "$previous_shim_hash" ]]; then
        echo "An untracked steamwebhelper_real.exe already exists; refusing to overwrite it." >&2
        exit 1
    fi
    if [[ "$current_hash" != "$previous_shim_hash" ]]; then
        # Steam replaced the managed shim with a new official helper. Preserve that
        # update as the real executable before reinstalling the project shim.
        temporary_real="$REAL.tmp.$$"
        cp -p "$TARGET" "$temporary_real"
        mv -f "$temporary_real" "$REAL"
    fi
else
    if [[ -n "$previous_shim_hash" ]]; then
        echo "The tracked original WebHelper backup is missing: $REAL" >&2
        exit 1
    fi
    cp -p "$TARGET" "$REAL"
fi

temporary_target="$TARGET.tmp.$$"
cp "$SHIM" "$temporary_target"
chmod 755 "$temporary_target"
mv -f "$temporary_target" "$TARGET"
printf '%s\n' "$shim_hash" > "$STATE"
echo "Installed the project WebHelper shim. Original helper is preserved at: $REAL"
