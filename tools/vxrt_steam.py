#!/usr/bin/env python3
"""vxrt steam: the Steam library VXRuntime runs SteamVR and games from, without Windows Steam or a Wine prefix:
~/Verantyx_VR_Drive/vxrt_steam (VXRT_STEAM changes it), laid out as a Steam library is (steamapps/common/<folder>,
steamapps/appmanifest_<id>.acf, config/), so SteamVR finds its folders as it would on Windows.

    tools/vxrt_steam.py status
    tools/vxrt_steam.py fetch <steam-login-name> 250820 546560   # SteamVR, Half-Life: Alyx: Windows builds, steamcmd
    tools/vxrt_steam.py import "<a Steam library>" [250820 546560 ...]   # apps you already have elsewhere

fetch downloads with your own Steam account through steamcmd, which asks for the password and Steam Guard itself
(nothing is stored here). import copies apps from another Steam library on this Mac (a Windows drive, the Wine
version's prefix, ...) as APFS clones: no download and no extra disk space, and the source is only read. Without
app ids it imports SteamVR and every game the source has a profile for (games/*.json).
"""
import glob, json, os, re, shutil, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vxrt_games                                               # noqa: E402

DRIVE = os.path.expanduser(os.environ.get("VERANTYX_VR_DRIVE", "~/Verantyx_VR_Drive"))
STEAM = os.environ.get("VXRT_STEAM", os.path.join(DRIVE, "vxrt_steam"))
STEAMVR = "250820"
REDIST = os.environ.get("VXRT_REDIST", os.path.join(DRIVE, "vxrt_redist"))

def manifest(path):
    text = open(path, errors="replace").read()
    get = lambda key: (re.search(r'"%s"\s+"([^"]*)"' % key, text, re.I) or [None, ""])[1]
    return {"appid": get("appid"), "name": get("name"), "installdir": get("installdir"), "path": path}

def apps(library):
    """{app id: manifest} of a Steam library (its steamapps/appmanifest_*.acf), and of steamcmd installs inside its
    steamapps/common (force_install_dir puts the manifest beside the app)."""
    found = {}
    for acf in glob.glob(os.path.join(library, "steamapps", "appmanifest_*.acf")) + \
               glob.glob(os.path.join(library, "steamapps", "common", "*", "steamapps", "appmanifest_*.acf")):
        m = manifest(acf)
        if m["appid"] and m["appid"] not in found:
            found[m["appid"]] = m
    return found

def app_dir(library, m):
    d = os.path.join(library, "steamapps", "common", m["installdir"])
    return d if os.path.isdir(d) else None

def status():
    have = apps(STEAM)
    out = {"library": STEAM, "steamvr": bool(have.get(STEAMVR) and app_dir(STEAM, have[STEAMVR])),
           "apps": {i: m["name"] for i, m in have.items() if app_dir(STEAM, m)},
           "redist": os.path.exists(os.path.join(REDIST, "manifest.json")),
           "redist_dlls": len(json.load(open(os.path.join(REDIST, "manifest.json")))["dlls"]) if os.path.exists(os.path.join(REDIST, "manifest.json")) else 0}
    out["ready_without_wine"] = out["steamvr"] and out["redist"]
    return out

def import_apps(source, ids):
    src = apps(source)
    if not src:
        sys.exit(f"{source}: no Steam apps there (no steamapps/appmanifest_*.acf)")
    if not ids:
        known = {p["appid"] for p in vxrt_games.load_all().values()}
        ids = [i for i in src if i == STEAMVR or i in known]
    os.makedirs(os.path.join(STEAM, "steamapps", "common"), exist_ok=True)
    for i in ids:
        m = src.get(i)
        if not m or not app_dir(source, m):
            print(f"{i}: not installed in {source}"); continue
        dest = os.path.join(STEAM, "steamapps", "common", m["installdir"])
        if os.path.exists(dest):
            print(f"{i} {m['name']}: already in {dest}"); continue
        # (cp -c: APFS clones, the copy shares the blocks until either side changes)
        r = subprocess.run(["cp", "-c", "-R", app_dir(source, m), dest])
        if r.returncode:
            shutil.rmtree(dest, ignore_errors=True)
            sys.exit(f"{i}: copying failed (another volume? cp -c clones only within one APFS volume)")
        shutil.copyfile(m["path"], os.path.join(STEAM, "steamapps", f"appmanifest_{i}.acf"))
        print(f"{i} {m['name']}: imported into {dest}")

def fetch(login, ids):
    if not shutil.which("steamcmd"):
        sys.exit("steamcmd is required: brew install --cask steamcmd")
    os.makedirs(os.path.join(STEAM, "steamapps", "common"), exist_ok=True)
    for i in ids:
        have = apps(STEAM).get(i)
        dest = app_dir(STEAM, have) if have else os.path.join(STEAM, "steamapps", "common", f"app_{i}")
        dest = dest or os.path.join(STEAM, "steamapps", "common", f"app_{i}")
        os.makedirs(dest, exist_ok=True)
        r = subprocess.run(["steamcmd", "+@sSteamCmdForcePlatformType", "windows", "+force_install_dir", dest, "+login", login,
                            "+app_license_request", i, "+app_update", i, "validate", "+quit"])
        acf = os.path.join(dest, "steamapps", f"appmanifest_{i}.acf")
        if r.returncode or not os.path.exists(acf):
            sys.exit(f"{i}: steamcmd did not finish (exit {r.returncode})")
        m = manifest(acf)
        final = os.path.join(STEAM, "steamapps", "common", m["installdir"])
        if os.path.abspath(dest) != os.path.abspath(final):       # (first download: named after the app's own folder)
            os.rename(dest, final)
        shutil.copyfile(os.path.join(final, "steamapps", f"appmanifest_{i}.acf"), os.path.join(STEAM, "steamapps", f"appmanifest_{i}.acf"))
        print(f"{i} {m['name']}: {final}")

def main():
    a = sys.argv[1:]
    if a[:1] == ["status"]:
        print(json.dumps(status(), ensure_ascii=False, indent=1))
    elif a[:1] == ["import"] and len(a) >= 2:
        import_apps(os.path.expanduser(a[1]), a[2:])
    elif a[:1] == ["fetch"] and len(a) >= 3:
        fetch(a[1], a[2:])
    else:
        sys.exit(__doc__)

if __name__ == "__main__":
    main()
