#!/usr/bin/env python3
"""glovebox: build a game for Apple Vision Pro from the player's own copy, on their own Mac.

  glovebox doctor [--json]     what this Mac and headset still need, each with the one action that fixes it
  glovebox build [--json]      the game app, from the player's own game files, signed for their team
  glovebox install [--json]    the built app onto the paired headset
  glovebox collect [--json]    the headset's pipeline recording and hitch log, back on this Mac (and as a recipe)

build options (environment):
  GLOVEBOX_GAME   the depot's game/ folder if it is already on disk (else the Steam download step provides it)
  GLOVEBOX_KIT    the runtime kit, a folder or .zip (else kit/ or glovebox-kit.zip beside the engine, as the app
                  bundles it)
  GLOVEBOX_WORK   where everything for this package goes (default ~/Library/Application Support/Glovebox/alyx)
  GLOVEBOX_TEAM   the team to sign for (default: the paid team in Xcode's account settings, else the free one)
  GLOVEBOX_FREE_TEAM  0: refuse a free Personal Team. Default: allowed. Its build keeps the memory limit (Apple gives
                  free teams Increased Memory Limit) and drops Extended Virtual Addressing (paid teams only, and
                  not needed: a free build can reserve 64 GB at once on the headset); its profile lasts 7 days
  GLOVEBOX_SHARE_STUTTERS  1 / 0: the player's answer to sharing stutter reports (unset: the headset asks)
  GLOVEBOX_APP_PID  the Glovebox app that started this run: when it's gone, the run stops with everything it started
  --unsigned      build without signing (CI, and checking a build without touching an Apple account)

Every step can print JSON lines (--json) for the Glovebox app: {"event": ..., ...}. The app shows them as screens;
nothing here asks questions on the terminal."""
import datetime
import json
import os
import platform
import plistlib
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
MIN_XCODE = 26
MIN_FREE_GB = 15
FREE_TEAM = os.environ.get("GLOVEBOX_FREE_TEAM", "1") != "0"

JSON = "--json" in sys.argv


def emit(event, **kw):
    if JSON:
        print(json.dumps(dict(event=event, **kw)), flush=True)
    else:
        mark = {"ok": "✓", "missing": "✗", "warn": "!"}.get(kw.get("status"), "·")
        line = f"{mark} {kw.get('title', event)}"
        if kw.get("detail"):
            line += f": {kw['detail']}"
        print(line, flush=True)
        if kw.get("status") != "ok" and kw.get("fix"):
            print(f"    → {kw['fix'].get('label', '')} {kw['fix'].get('url', kw['fix'].get('command', ''))}")


def run(*cmd, timeout=60):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout, p.stderr
    except (OSError, subprocess.TimeoutExpired) as e:
        return 127, "", str(e)


# ---- doctor: each check returns (status, detail, fix) ----

def check_mac():
    if platform.machine() != "arm64":
        return "missing", "Glovebox needs a Mac with Apple silicon", None
    return "ok", f"Apple silicon, macOS {platform.mac_ver()[0]}", None


def check_disk():
    free = shutil.disk_usage(os.path.expanduser("~")).free / 1e9
    if free < MIN_FREE_GB:
        return "missing", f"{free:.0f} GB free, {MIN_FREE_GB} GB needed", {"kind": "open-url", "label": "Free up space",
                                                                          "url": "x-apple.systempreferences:com.apple.settings.Storage"}
    return "ok", f"{free:.0f} GB free", None


def use_xcode():
    """The Xcode every step uses: DEVELOPER_DIR when the Glovebox app passes it (its choice, MacTools.preflight), else the
    one the Mac already uses (xcode-select) when it is a full Xcode, else the newest release in /Applications, else the
    newest beta. Set for every command this engine runs, so nothing needs the Mac's default changed (which takes an
    administrator password). Returns the Xcode.app path, or None."""
    d = os.environ.get("DEVELOPER_DIR", "")
    if not d:
        rc, out, _ = run("xcode-select", "-p")
        d = out.strip() if rc == 0 else ""
    if d.endswith(".app/Contents/Developer") and os.path.isdir(d):
        os.environ["DEVELOPER_DIR"] = d
        return d[: -len("/Contents/Developer")]
    apps = sorted(os.path.join("/Applications", a) for a in os.listdir("/Applications")
                  if a.startswith("Xcode") and a.endswith(".app"))
    def version(app):
        try:
            v = plistlib.load(open(os.path.join(app, "Contents", "Info.plist"), "rb"))["CFBundleShortVersionString"]
            return tuple(int(x) for x in v.split(".") if x.isdigit())
        except (OSError, KeyError, ValueError):
            return ()
    apps.sort(key=lambda a: ("beta" not in a.lower(), version(a)))
    if not apps:
        return None
    os.environ["DEVELOPER_DIR"] = apps[-1] + "/Contents/Developer"
    return apps[-1]


# Xcode's own license and first-launch steps, with Apple's prompt naming Xcode: Glovebox never asks for the Mac's
# administrator password itself.
OPEN_XCODE = {"kind": "open-app", "label": "Open Xcode", "url": "xcode://"}


def check_xcode():
    app = use_xcode()
    if not app:
        return "missing", "Xcode is not installed", {"kind": "open-url", "label": "Get Xcode",
                                                     "url": "macappstore://apps.apple.com/app/xcode/id497799835"}
    rc, out, _ = run("xcodebuild", "-version")
    ver = out.split()[1] if rc == 0 and len(out.split()) > 1 else "?"
    try:
        major = int(ver.split(".")[0])
    except ValueError:
        major = 0
    if rc == 0 and major < MIN_XCODE:
        return "missing", f"Xcode {ver} is too old (needs {MIN_XCODE} or later)", {
            "kind": "open-url", "label": "Update Xcode", "url": "macappstore://apps.apple.com/app/xcode/id497799835"}
    rc_license, _, _ = run("xcodebuild", "-license", "check")
    rc_first, _, _ = run("xcodebuild", "-checkFirstLaunchStatus")
    if rc != 0 or rc_license != 0 or rc_first != 0:
        return "missing", "Xcode needs to finish setting up (its license, then a few parts it installs)", OPEN_XCODE
    return "ok", f"Xcode {ver}", None


def check_visionos_sdk():
    rc, out, _ = run("xcrun", "--sdk", "xros", "--show-sdk-version")
    if rc != 0:
        return "missing", "Xcode's visionOS support is not installed", {
            "kind": "command", "label": "Install visionOS support", "command": "xcodebuild -downloadPlatform visionOS"}
    return "ok", f"visionOS SDK {out.strip()}", None


def xcode_prefs():
    """Xcode's settings (`defaults export com.apple.dt.Xcode -`), {} when there are none."""
    rc, out, _ = run("defaults", "export", "com.apple.dt.Xcode", "-")
    if rc != 0:
        return {}
    try:
        return plistlib.loads(out.encode())
    except Exception:
        return {}


def xcode_account(prefs):
    """Whether Xcode's account list names someone. None when Xcode keeps no account list at all."""
    lists = prefs.get("DVTDeveloperAccountManagerAppleIDLists")
    if not isinstance(lists, dict):
        return None
    return any(isinstance(v, list) and any(map(is_account, v)) for v in lists.values())


def xcode_teams():
    """Teams Xcode's account settings know, without loading the account: [(team id, name, paid)]."""
    prefs = xcode_prefs()
    teams = []
    # Signing out of Xcode empties its account list but leaves the teams cached: no account, no team. An entry with
    # no name in it (Xcode 16 can leave those) is no account either.
    if xcode_account(prefs) is False:
        return teams
    for _account, lst in (prefs.get("IDEProvisioningTeamByIdentifier") or {}).items():
        for t in lst or []:
            teams.append((t.get("teamID"), t.get("teamName"), not t.get("isFreeProvisioningTeam", True)))
    return teams


def is_account(entry):
    """An entry in Xcode's account list that names someone: a non-empty identifier, or a record with one in it. A form
    this doesn't know counts as an account, as every entry did before."""
    if isinstance(entry, str):
        return bool(entry.strip())
    if isinstance(entry, dict):
        return any(map(is_account, entry.values()))
    if isinstance(entry, list):
        return any(map(is_account, entry))
    return not isinstance(entry, (bool, int, float, datetime.datetime))


def check_team():
    teams = xcode_teams()
    paid = [t for t in teams if t[2]]
    if paid:
        return "ok", "Apple Developer Program team in Xcode", None
    if teams and FREE_TEAM:
        return "ok", "free Apple account in Xcode (the app needs re-signing every 7 days)", None
    if teams:
        return "missing", "Xcode is signed in, but only with a free team (the game needs the paid program's memory limit)", {
            "kind": "open-url", "label": "Join the Apple Developer Program", "url": "https://developer.apple.com/programs/enroll/"}
    if xcode_account(xcode_prefs()):
        # Signed in, but Xcode hasn't saved the account's teams yet: it does once it asks Apple again (clicking the
        # account, Download Manual Profiles). Signing in again isn't what's needed.
        return "missing", "Xcode is signed in, but hasn't loaded the account's team yet", {
            "kind": "open-app", "label": "Load your team in Xcode's Accounts settings", "url": "xcode://"}
    return "missing", "Xcode is not signed in to an Apple account", {
        "kind": "open-app", "label": "Open Xcode's Accounts settings", "url": "xcode://"}


def headsets():
    fd, path = tempfile.mkstemp(suffix=".json")
    os.close(fd)
    try:
        rc, _, _ = run("xcrun", "devicectl", "list", "devices", "--json-output", path, timeout=30)
        if rc != 0:
            return []
        devs = json.load(open(path)).get("result", {}).get("devices", [])
    except (OSError, ValueError):
        return []
    finally:
        os.unlink(path)
    out = []
    for d in devs:
        hp, dp, cp = d.get("hardwareProperties", {}), d.get("deviceProperties", {}), d.get("connectionProperties", {})
        if hp.get("platform") != "visionOS" or cp.get("transportType") == "sameMachine":
            continue  # simulators show up as sameMachine
        h = {"name": dp.get("name"), "model": hp.get("productType"), "os": dp.get("osVersionNumber"),
             "udid": hp.get("udid"),
             "paired": cp.get("pairingState") == "paired", "developerMode": dp.get("developerModeStatus"),
             "reachable": cp.get("tunnelState") == "connected", "ddi": dp.get("ddiServicesAvailable") is True,
             "id": d.get("identifier")}
        if h["paired"] and not (h["reachable"] and h["developerMode"] == "enabled" and h["ddi"]):
            refresh(h)
        out.append(h)
    return out


def device_info(what, device, timeout=20):
    """`devicectl device info <what>` for one device, its result as a dict ({} when it could not connect)."""
    fd, path = tempfile.mkstemp(suffix=".json")
    os.close(fd)
    try:
        rc, _, _ = run("xcrun", "devicectl", "device", "info", what, "--device", device, "--timeout", str(timeout),
                       "--json-output", path, timeout=timeout + 15)
        return json.load(open(path)).get("result", {}) if rc == 0 else {}
    except (OSError, ValueError):
        return {}
    finally:
        os.unlink(path)


def refresh(h):
    """`list devices` reports what CoreDevice last heard from a headset until something connects to it: right after
    Developer Mode is turned on it still says off. Connecting (device info details) re-reads it, the way selecting the
    headset in Device Hub does; ddiServices then brings up the developer disk image services an install needs, and
    lockState says whether the headset has to be put on and unlocked first. All read-only."""
    r = device_info("details", h["id"], timeout=10)
    if r:
        dp, cp = r.get("deviceProperties", {}), r.get("connectionProperties", {})
        h["developerMode"] = dp.get("developerModeStatus", h["developerMode"])
        h["reachable"] = cp.get("tunnelState") == "connected" or h["reachable"]
        h["ddi"] = dp.get("ddiServicesAvailable") is True
    if h["developerMode"] == "enabled" and h["reachable"] and not h["ddi"]:
        h["ddi"] = bool(device_info("ddiServices", h["id"], timeout=60).get("ddiMetadata", {}).get("isUsable"))
    if r and not h["ddi"]:
        lock = device_info("lockState", h["id"], timeout=10)
        h["locked"] = bool(lock) and not lock.get("unlockedSinceBoot", True)
    return h


def check_headset():
    hs = headsets()
    if not hs:
        return "missing", "No Vision Pro paired with this Mac", {"kind": "guide", "label": "Pair your Vision Pro",
                                                                 "guide": "pair-headset"}
    h = next((x for x in hs if x["paired"] and x["developerMode"] == "enabled"), None)
    if not h:
        return "missing", f"{hs[0]['name']}: Developer Mode is off", {"kind": "guide", "label": "Turn on Developer Mode",
                                                                       "guide": "developer-mode"}
    if not h["reachable"] or not h.get("ddi", True):
        # Setup goes on; the install waits for it. The doctor runs every few seconds, so this clears by itself.
        why = "unlock it" if h.get("locked") else "put it on and unlock it (same Wi-Fi as this Mac)"
        return "warn", f"{h['name']} is ready but asleep: {why}", None
    return "ok", f"{h['name']} (visionOS {h['os']})", None


CHECKS = [("mac", "This Mac", check_mac), ("disk", "Space", check_disk), ("xcode", "Xcode", check_xcode),
          ("visionos", "visionOS support", check_visionos_sdk), ("team", "Apple developer account", check_team),
          ("headset", "Vision Pro", check_headset)]


def doctor():
    bad = 0
    for key, title, fn in CHECKS:
        status, detail, fix = fn()
        emit("check", check=key, title=title, status=status, detail=detail, fix=fix)
        bad += status == "missing"
        if status == "missing" and key in ("mac", "xcode"):
            break  # the later checks need these
    emit("done", step="doctor", ok=not bad)
    return 1 if bad else 0


def build(package="alyx", unsigned=False):
    from gb import build as b
    work = b.default_work(package)
    os.makedirs(work, exist_ok=True)
    team = os.environ.get("GLOVEBOX_TEAM")
    teams = xcode_teams()
    if not team and not unsigned:
        paid = [t for t in teams if t[2]]
        free = [t for t in teams if not t[2]] if FREE_TEAM else []
        if not paid and not free:
            emit("error", step="team", message="no paid Apple Developer Program team in Xcode")
            return 1
        team = (paid or free)[0][0]
    free_team = any(t[0] == team and not t[2] for t in teams)
    try:
        b.kit(os.environ.get("GLOVEBOX_KIT"), work, emit)
        b.runtime(package, work, emit)
        game_dir = b.game(work, os.environ.get("GLOVEBOX_GAME"), emit)
        foz_path = b.pipelines(package, work, game_dir, emit)
        demo = b.defaults(package, work, foz_path, emit)
        b.translate(work, emit)
        # Sign for the paired headset itself, so Xcode registers it on the team and puts it in the profile (a
        # generic visionOS build only installs on headsets the team already has).
        hs = [h for h in headsets() if h["paired"] and h["developerMode"] == "enabled" and h.get("udid")]
        app = b.app(package, work, demo, team or "UNSIGNED", emit, provisioning=not unsigned, unsigned=unsigned,
                    device=None if unsigned or not hs else hs[0]["udid"], free_team=free_team)
    except b.StepError as e:
        emit("error", message=str(e))
        return 1
    emit("done", step="build", ok=True, app=app)
    return 0


def install(package="alyx"):
    from gb import build as b
    app = os.path.join(b.default_work(package), "KleptonAlyx.app")
    if not os.path.isdir(app):
        emit("error", message="nothing built yet: run glovebox build")
        return 1
    hs = [h for h in headsets() if h["paired"] and h["developerMode"] == "enabled"]
    if not hs:
        emit("error", message="no paired Vision Pro with Developer Mode on")
        return 1
    try:
        b.install(app, hs[0]["id"], emit)
    except b.StepError as e:
        emit("error", message=str(e))
        return 1
    emit("done", step="install", ok=True)
    return 0


def collect(package="alyx"):
    from gb import build as b
    work = b.default_work(package)
    teams = sorted(xcode_teams(), key=lambda t: not t[2])  # the paid team first
    team = os.environ.get("GLOVEBOX_TEAM") or next((t[0] for t in teams if t[2] or FREE_TEAM), None)
    hs = [h for h in headsets() if h["paired"] and h["reachable"]]
    if not team or not hs:
        emit("error", message="needs a team in Xcode and the headset reachable")
        return 1
    g = os.path.join(work, "linux", "game")
    b.collect(package, work, hs[0]["id"], team, g if os.path.isdir(g) else None, emit)
    emit("done", step="collect", ok=True)
    return 0


def pipelines(game, package="alyx"):
    recipe_dir = os.path.join(ROOT, "packages", package, "pipelines")
    recipes = sorted(f for f in os.listdir(recipe_dir) if f.endswith(".recipe"))
    rc = 0
    for r in recipes:
        out = os.path.join(os.environ.get("GLOVEBOX_WORK", os.path.join(ROOT, "build")), package, r[:-7] + ".foz")
        os.makedirs(os.path.dirname(out), exist_ok=True)
        rc |= subprocess.call([sys.executable, os.path.join(HERE, "pipelines.py"), "build",
                               os.path.join(recipe_dir, r), game, out])
    return rc


def watch_app():
    """Started by the Glovebox app (GLOVEBOX_APP_PID): once the app is gone, even if it crashed, this run and everything
    it started (devicectl, xcodebuild) stop too. macOS 27 lists an app whose processes outlive it as running in the
    background, and notifies the player."""
    try:
        app = int(os.environ.get("GLOVEBOX_APP_PID", ""))
        app_group = os.getpgid(app)
    except (ValueError, OSError):
        return

    def alive():
        try:
            os.kill(app, 0)
            return True
        except PermissionError:
            return True
        except OSError:
            return False

    def watch():
        while alive():
            time.sleep(1)
        group = os.getpgrp()
        if group != app_group:  # the app starts the engine in a group of its own: this run and its tools
            os.killpg(group, signal.SIGTERM)
        os._exit(130)

    threading.Thread(target=watch, daemon=True).start()


def main(argv):
    watch_app()
    use_xcode()  # the same Xcode for every command, as the Glovebox app chose it (DEVELOPER_DIR) or the Mac's own
    args = [a for a in argv if not a.startswith("--")]
    if args[:1] == ["doctor"]:
        return doctor()
    if args[:1] == ["build"]:
        return build(unsigned="--unsigned" in argv)
    if args[:1] == ["install"]:
        return install()
    if args[:1] == ["collect"]:
        return collect()
    if args[:1] == ["pipelines"] and len(args) == 2:
        return pipelines(args[1])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
