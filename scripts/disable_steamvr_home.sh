#!/usr/bin/env bash
set -euo pipefail

VERANTYX_VR_DRIVE="${VERANTYX_VR_DRIVE:-$HOME/Verantyx_VR_Drive}"
WINEPREFIX="${WINEPREFIX:-$VERANTYX_VR_DRIVE/SteamVR_Prefix_Wine11_Test}"
STEAMVR_SETTINGS="${STEAMVR_SETTINGS:-$WINEPREFIX/drive_c/Program Files (x86)/Steam/config/steamvr.vrsettings}"
RENDER_TARGET_SCALE="${VERANTYX_RENDER_TARGET_SCALE:-0.5}"

if [[ "$(basename "$WINEPREFIX")" != "SteamVR_Prefix_Wine11_Test" ]]; then
    echo "Refusing to modify SteamVR settings outside the test prefix: $WINEPREFIX" >&2
    exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 is required to update SteamVR settings." >&2
    exit 1
fi

python3 - "$STEAMVR_SETTINGS" "$RENDER_TARGET_SCALE" <<'PY'
import json
import os
import shutil
import stat
import sys
import tempfile

path = sys.argv[1]
render_target_scale = float(sys.argv[2])
if not 0.35 <= render_target_scale <= 1.0:
    raise SystemExit("VERANTYX_RENDER_TARGET_SCALE must be between 0.35 and 1.0.")
backup = path + ".mbp-avp-vr-original"
os.makedirs(os.path.dirname(path), exist_ok=True)
if os.path.exists(path):
    with open(path, "r", encoding="utf-8") as source:
        settings = json.load(source)
    original_mode = stat.S_IMODE(os.stat(path).st_mode)
else:
    settings = {}
    original_mode = 0o644

steamvr = settings.setdefault("steamvr", {})
if not isinstance(steamvr, dict):
    raise SystemExit('SteamVR "steamvr" settings entry is not an object.')

changed = []
if steamvr.get("enableHomeApp") is not False:
    steamvr["enableHomeApp"] = False
    changed.append("SteamVR Home auto-start")
if steamvr.get("startDashboardFromAppLaunch") is not False:
    steamvr["startDashboardFromAppLaunch"] = False
    changed.append("dashboard startup during app launch")
if steamvr.get("renderTargetMultiplier") != render_target_scale:
    steamvr["renderTargetMultiplier"] = render_target_scale
    changed.append(f"SteamVR render resolution scale ({render_target_scale:g})")
if steamvr.get("motionSmoothing") is not False:
    steamvr["motionSmoothing"] = False
    changed.append("SteamVR motion smoothing")
dashboard = settings.setdefault("dashboard", {})
if not isinstance(dashboard, dict):
    raise SystemExit('SteamVR "dashboard" settings entry is not an object.')
if dashboard.get("enableDashboard") is not False:
    dashboard["enableDashboard"] = False
    changed.append("SteamVR dashboard rendering")
if not changed:
    print("SteamVR Home and dashboard auto-start are already disabled for app launches.")
    raise SystemExit(0)

if os.path.exists(path) and not os.path.exists(backup):
    shutil.copy2(path, backup)

fd, temporary_path = tempfile.mkstemp(prefix=".steamvr.vrsettings.", dir=os.path.dirname(path))
try:
    with os.fdopen(fd, "w", encoding="utf-8") as target:
        json.dump(settings, target, indent=3)
        target.write("\n")
    os.chmod(temporary_path, original_mode)
    os.replace(temporary_path, path)
finally:
    if os.path.exists(temporary_path):
        os.unlink(temporary_path)

print("Applied SteamVR settings: " + ", ".join(changed) + ".")
if os.path.exists(backup):
    print(f"Original SteamVR settings backup: {backup}")
PY
