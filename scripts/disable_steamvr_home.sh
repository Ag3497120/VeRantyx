#!/usr/bin/env bash
set -euo pipefail

VERANTYX_VR_DRIVE="${VERANTYX_VR_DRIVE:-$HOME/Verantyx_VR_Drive}"
WINEPREFIX="${WINEPREFIX:-$VERANTYX_VR_DRIVE/SteamVR_Prefix_Wine11_Test}"
STEAMVR_SETTINGS="${STEAMVR_SETTINGS:-$WINEPREFIX/drive_c/Program Files (x86)/Steam/config/steamvr.vrsettings}"
# SteamVR's per-eye size = driver size (949x759 for the Vision Pro frustum)
# x sqrt(scale). It ignored renderTargetMultiplier and used its own GPU
# estimate (1.5) instead, which gave 1156x932; Alyx's automatic fidelity then
# picked 1.4x that. 0.7 gives ~794x635, so Alyx's top level is ~1124x899.
RENDER_TARGET_SCALE="${VERANTYX_RENDER_TARGET_SCALE:-0.55}"
CHAPERONE_FILE="${CHAPERONE_FILE:-$(dirname "$STEAMVR_SETTINGS")/chaperone_info.vrchap}"
PLAY_AREA_METERS="${VERANTYX_PLAY_AREA_METERS:-3}"

if [[ "$(basename "$WINEPREFIX")" != SteamVR_Prefix_*_Test ]]; then
    echo "Refusing to modify SteamVR settings outside the test prefix: $WINEPREFIX" >&2
    exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 is required to update SteamVR settings." >&2
    exit 1
fi

# Room setup auto-launches on every start and writes a 1x1 m play area with a
# floor offset; the headset already supplies floor-level poses, so give
# SteamVR an identity standing transform and a roomy play area instead.
python3 - "$CHAPERONE_FILE" "$PLAY_AREA_METERS" <<'PY'
import json, os, shutil, sys, time
path, size = sys.argv[1], float(sys.argv[2])
half = size / 2
height = 2.43
corners = [(-half, -half), (-half, half), (half, half), (half, -half)]
bounds = []
for i in range(4):
    (x0, z0), (x1, z1) = corners[i], corners[(i + 1) % 4]
    bounds.append([[x0, 0, z0], [x0, height, z0], [x1, height, z1], [x1, 0, z1]])
wanted = {"collision_bounds": bounds, "play_area": [size, size],
          "standing": {"translation": [0, 0, 0], "yaw": 0},
          "time": time.strftime("%a %b %d %H:%M:%S %Y"), "universeID": "1"}
data = {"jsonid": "chaperone_info", "universes": [wanted], "version": 5}
if os.path.exists(path):
    try:
        current = json.load(open(path))
        universes = [u for u in current.get("universes", []) if str(u.get("universeID")) == "1"]
        if universes and universes[0].get("standing") == wanted["standing"] and universes[0].get("play_area") == wanted["play_area"]:
            raise SystemExit(0)
    except (ValueError, OSError):
        pass
    backup = path + ".mbp-avp-vr-original"
    if not os.path.exists(backup):
        shutil.copy2(path, backup)
os.makedirs(os.path.dirname(path), exist_ok=True)  # fresh prefix: Steam has not created config/ yet
with open(path, "w", encoding="utf-8") as target:
    json.dump(data, target, indent=3)
print(f"Wrote SteamVR chaperone universe 1: {size:g}x{size:g} m play area, floor at the headset's floor.")
PY

python3 - "$STEAMVR_SETTINGS" "$RENDER_TARGET_SCALE" <<'PY'
import json
import os
import shutil
import stat
import sys
import tempfile

path = sys.argv[1]
render_target_scale = float(sys.argv[2])
if not 0.2 <= render_target_scale <= 1.5:
    raise SystemExit("VERANTYX_RENDER_TARGET_SCALE must be between 0.2 and 1.5.")
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
if steamvr.get("supersampleManualOverride") is not True:
    steamvr["supersampleManualOverride"] = True
    changed.append("manual SteamVR resolution")
if steamvr.get("supersampleScale") != render_target_scale:
    steamvr["supersampleScale"] = render_target_scale
    changed.append(f"SteamVR render resolution scale ({render_target_scale:g})")
if "renderTargetMultiplier" in steamvr:
    del steamvr["renderTargetMultiplier"]
    changed.append("legacy renderTargetMultiplier removed")
if steamvr.get("motionSmoothing") is not False:
    steamvr["motionSmoothing"] = False
    changed.append("SteamVR motion smoothing")
# A crash under Wine leaves SteamVR in safe mode, which then refuses to load
# the Verantyx driver ("blocked by a previous safe mode event") and SteamVR
# starts without the headset.
# A fresh prefix: SteamVR must load only this driver, and must not wait for
# another headset or tracking system.
for key, value in (("forcedDriver", "verantyx"), ("activateMultipleDrivers", False),
                   ("requireHmd", False), ("showMirrorView", False)):
    if steamvr.get(key) != value:
        steamvr[key] = value
        changed.append(f"steamvr.{key}")
for section, enabled in (("verantyx", True), ("lighthouse", False), ("oculus", False)):
    entry = settings.setdefault(section, {})
    if isinstance(entry, dict) and entry.get("enable") is not enabled:
        entry["enable"] = enabled
        changed.append(f"{section}.enable")
if steamvr.get("enableSafeMode") is not False:
    steamvr["enableSafeMode"] = False
    changed.append("SteamVR safe mode")
driver_settings = settings.get("driver_verantyx")
if isinstance(driver_settings, dict) and driver_settings.get("blocked_by_safe_mode"):
    driver_settings["blocked_by_safe_mode"] = False
    changed.append("Verantyx driver safe-mode block")
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
