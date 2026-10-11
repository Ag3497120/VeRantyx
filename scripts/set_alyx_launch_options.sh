#!/usr/bin/env bash
set -euo pipefail

# Stores Half-Life: Alyx launch options in the test prefix's Steam user config.
# Steam starts Alyx by itself (about 40 s after Steam starts and again after
# Alyx exits) and ignores a launch request while it does, so options passed on
# the -applaunch command line were often lost. Steam applies these stored
# options to every launch. Steam rewrites the file while it runs, so this only
# writes when the Windows Steam client is not running.

VERANTYX_VR_DRIVE="${VERANTYX_VR_DRIVE:-$HOME/Verantyx_VR_Drive}"
WINEPREFIX="${WINEPREFIX:-$VERANTYX_VR_DRIVE/SteamVR_Prefix_Wine11_Test}"
STEAM_ROOT="${STEAM_ROOT:-$WINEPREFIX/drive_c/Program Files (x86)/Steam}"
OPTIONS="${1:?usage: set_alyx_launch_options.sh \"<options>\"}"

if [[ "$(basename "$WINEPREFIX")" != "SteamVR_Prefix_Wine11_Test" ]]; then
    echo "Refusing to modify Steam settings outside the test prefix: $WINEPREFIX" >&2
    exit 1
fi
# Exit 3 while Steam runs and the stored values differ, so the caller can stop
# Steam and call again (Steam rewrites this file while it runs).
STEAM_RUNNING=0
if pgrep -f 'Steam[/\\]steam\.exe' >/dev/null 2>&1; then STEAM_RUNNING=1; fi

shopt -s nullglob
configs=("$STEAM_ROOT"/userdata/*/config/localconfig.vdf)
if (( ${#configs[@]} == 0 )); then
    echo "No Steam user config found under $STEAM_ROOT/userdata; Alyx launch options were not stored." >&2
    exit 0
fi

for config in "${configs[@]}"; do
    python3 - "$config" "$OPTIONS" "$STEAM_RUNNING" <<'PY'
import os
import re
import shutil
import sys
import tempfile

path, options, steam_running = sys.argv[1], sys.argv[2], sys.argv[3] == "1"
app_path = ["userlocalconfigstore", "software", "valve", "steam", "apps"]
app_id = "546560"
# The Steam overlay hooks the game's D3D11/DXGI; it has nothing to show in a
# VR game here and breaks the DXMT backend, so it stays off for Alyx.
wanted = {"LaunchOptions": options, "OverlayAppEnable": "0"}
with open(path, "r", encoding="utf-8") as source:
    lines = source.read().split("\n")

token = re.compile(r'^(\s*)"((?:[^"\\]|\\.)*)"(?:\s+"((?:[^"\\]|\\.)*)")?\s*$')
stack = []
pending_key = None
apps_close = None        # index of the "apps" block's closing brace
app_open = None          # index of the 546560 block's opening brace
app_close = None
option_lines = {}
for index, line in enumerate(lines):
    stripped = line.strip()
    if stripped == "{":
        stack.append(pending_key or "")
        pending_key = None
        if [key.lower() for key in stack] == app_path + [app_id]:
            app_open = index
        continue
    if stripped == "}":
        lowered = [key.lower() for key in stack]
        if lowered == app_path + [app_id]:
            app_close = index
        if lowered == app_path:
            apps_close = index
        if stack:
            stack.pop()
        continue
    match = token.match(line)
    if not match:
        continue
    key, value = match.group(2), match.group(3)
    if value is None:
        pending_key = key
    elif [k.lower() for k in stack] == app_path + [app_id]:
        for name in wanted:
            if key.lower() == name.lower():
                option_lines[name] = index

def escape(value):
    return value.replace("\\", "\\\\").replace('"', '\\"')

changed = False
new_entries = []
for name, value in wanted.items():
    line = f'"{name}"\t\t"{escape(value)}"'
    if name in option_lines:
        index = option_lines[name]
        indent = re.match(r"^(\s*)", lines[index]).group(1)
        if lines[index] != indent + line:
            lines[index] = indent + line
            changed = True
    else:
        new_entries.append(line)
if new_entries:
    changed = True
    if app_open is not None:
        indent = re.match(r"^(\s*)", lines[app_open]).group(1) + "\t"
        lines[app_open + 1:app_open + 1] = [indent + entry for entry in new_entries]
    elif apps_close is not None:
        indent = re.match(r"^(\s*)", lines[apps_close]).group(1) + "\t"
        lines[apps_close:apps_close] = [f'{indent}"{app_id}"', f"{indent}{{"] + \
            [f"{indent}\t{entry}" for entry in new_entries] + [f"{indent}}}"]
    else:
        print(f"No Steam apps section in {path}; Alyx launch options were not stored.", file=sys.stderr)
        raise SystemExit(0)
if not changed:
    print(f"Alyx launch options already stored in {path}")
    raise SystemExit(0)
if steam_running:
    print("Windows Steam is running and Alyx's stored launch options or overlay setting need a change; Steam has to be stopped first.")
    raise SystemExit(3)

backup = path + ".mbp-avp-vr-original"
if not os.path.exists(backup):
    shutil.copy2(path, backup)
mode = os.stat(path).st_mode & 0o777
fd, temporary = tempfile.mkstemp(prefix=".localconfig.", dir=os.path.dirname(path))
try:
    with os.fdopen(fd, "w", encoding="utf-8") as target:
        target.write("\n".join(lines))
    os.chmod(temporary, mode)
    os.replace(temporary, path)
finally:
    if os.path.exists(temporary):
        os.unlink(temporary)
print(f"Stored Alyx launch options in {path} (backup: {backup})")
PY
done
