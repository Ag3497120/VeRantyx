#!/usr/bin/env python3
"""vxrt tune: finds the best picture a game holds at the display rate on this Mac, by running it with the harness
(tools/vxrt_run.py: own roots, simulated headset, evidence) up a ladder of settings, from the fewest pixels per eye to
the most, and confirming the choice in alternating runs.

    tools/vxrt_tune.py alyx                     # the profile's tune scenario and ladder (or the default ladder)
    tools/vxrt_tune.py thelab --plan            # what it would run, and about how long it takes; runs nothing
    tools/vxrt_tune.py earth --max-runs 6 --apply   # write the choice into games/<appid>.json ("tuned", this Mac)
    tools/vxrt_tune.py --show alyx              # the launch command tuned for this Mac, if there is one

A candidate holds when every run of it is healthy (the game reached SteamVR, ran the whole scenario, drew something)
and the median of its runs' fps_median is at least 95 % of the display rate. The tuner climbs while candidates hold;
the highest one that holds is run again, alternating with the next one up when that one came within 10 % (B A A B),
until each has --repeats runs; a candidate that stops holding sends it one step down. It stops when the choice is
confirmed, at --max-runs, or when the harness refuses a run (the user started playing, SteamVR is up, ...): it never
works around a refusal. Everything is written to runs/<tune id>/tune.json (each run's id and the observation the fps
comes from), sealed into runs/ledger.jsonl.

What it does not know: how the stream looks and feels in the headset over the user's Wi-Fi (the harness has no
headset), and picture quality beyond pixel counts. The game may also pick its own size (Half-Life: Alyx's fidelity
levels): the per-eye size is what the driver asks for.

Exit status: 0 a confirmed choice, 1 none holds, 2 not confirmed (runs ran out), 3 refused, 4 error.
"""
import argparse, json, os, statistics, subprocess, sys, time
from datetime import datetime, timezone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vxrt_doctor as doctor                                    # noqa: E402
import vxrt_games as profiles_db                                # noqa: E402
import vxrt_ledger as ledger                                    # noqa: E402

REPO = doctor.REPO
DRIVER_EYE = (800, 900)                 # the driver's default per-eye size (driver_verantyx.cpp); presets scale it
PRESET_SCALE = {"": 1.0, "3k": 1.8, "2.5k": 3.2, "max": 4.0}    # VXRT_VR_RENDER_SCALE each preset sets (launch_vxrt_steamvr.sh)
HOLD, NEAR = 0.95, 0.90                 # of the display rate: holds; close enough to deserve a second look
DEFAULT_LADDER = [
    {"quality": ""},
    {"quality": "3k"},
    {"quality": "3k", "settings": {"VXRT_VR_RENDER_SCALE": "2.4"}},
    {"quality": "2.5k"},
    {"quality": "max"},
]
OVERHEAD_S = 75                         # a run's start and stop around its scenario (SteamVR, the game, cleanup)

# ---- candidates -------------------------------------------------------------------------------------------------------------
def per_eye(c):
    s = float(c.get("settings", {}).get("VXRT_VR_RENDER_SCALE", PRESET_SCALE.get(c.get("quality", ""), 1.0)))
    return round(DRIVER_EYE[0] * s), round(DRIVER_EYE[1] * s)

def display_hz(c):
    return float(c.get("settings", {}).get("VERANTYX_DISPLAY_HZ", 60))

def label(c):
    w, h = per_eye(c)
    extra = " ".join(f"{k}={v}" for k, v in sorted(c.get("settings", {}).items()))
    return f"{c.get('quality') or 'default'} {w}x{h}" + (f" ({extra})" if extra else "")

def ladder_for(profile):
    """The profile's ladder (games/<appid>.json "tune"), or the default; sorted by pixels per eye."""
    lad = (profile or {}).get("tune", {}).get("ladder") or DEFAULT_LADDER
    return sorted(lad, key=lambda c: per_eye(c)[0] * per_eye(c)[1])

# ---- what a run says ----------------------------------------------------------------------------------------------------------
def run_facts(result):
    """{run_id, status, fps, fps_ref, healthy, why} from a harness result. Healthy: every derived claim but the fps
    baseline holds (the baseline belongs to another setting)."""
    obs = {o["metric"]: o for o in result.get("observed", [])}
    fps = obs.get("fps_median", {}).get("value")
    bad = [d["claim"] for d in result.get("derived", []) if d["claim"] != "fps_meets_baseline" and not d["holds"]]
    return {"run_id": result["run_id"], "status": result["status"], "fps": fps, "fps_ref": obs.get("fps_median", {}).get("id"),
            "healthy": result["status"] != "refused" and not bad and fps is not None, "why": bad or ([] if fps is not None else ["no fps"])}

def verdict(c, runs):
    """untested / holds / fails, and the median fps of the candidate's runs."""
    if not runs:
        return "untested", None
    fpss = [r["fps"] for r in runs if r["fps"] is not None]
    med = statistics.median(fpss) if fpss else None
    ok = all(r["healthy"] for r in runs) and med is not None and med >= HOLD * display_hz(c)
    return ("holds" if ok else "fails"), med

def near(c, runs):
    v, med = verdict(c, runs)
    return v == "fails" and all(r["healthy"] for r in runs) and med is not None and med >= NEAR * display_hz(c)

def next_step(ladder, runs, repeats):
    """The index of the candidate to run next, or None when done. runs: {index: [run facts]}."""
    held = [i for i in range(len(ladder)) if verdict(ladder[i], runs.get(i))[0] == "holds"]
    if not held:
        r0 = runs.get(0, [])
        if not r0 or (near(ladder[0], r0) and len(r0) < repeats):
            return 0
        return None
    h = held[-1]
    up = h + 1
    if up < len(ladder) and not runs.get(up):
        return up                                            # climb
    if len(runs[h]) < repeats:
        return h                                             # confirm the choice
    if up < len(ladder) and near(ladder[up], runs[up]) and len(runs[up]) < repeats:
        return up                                            # one more look at the one just out of reach
    return None

def choice(ladder, runs, repeats):
    """(index or None, confirmed)."""
    held = [i for i in range(len(ladder)) if verdict(ladder[i], runs.get(i))[0] == "holds"]
    if not held:
        return None, True
    h = held[-1]
    return h, len(runs[h]) >= repeats

def command_for(key, c):
    """The player's launch command for a candidate."""
    import vxrt_run
    _, _, inst = vxrt_run.resolve(key)
    env = " ".join(f"{k}={json.dumps(v) if ' ' in v or '{' in v else v}" for k, v in sorted(c.get("settings", {}).items()))
    cmd = doctor.launch_command(inst["exe"], c.get("quality", "")) if inst else f"tools/vxrt_run.py {key}"
    return cmd.replace(" && ", f" && {env} ", 1) if env else cmd

def machine_key(mi=None):
    mi = mi or doctor.machine_info()
    return f"{mi['chip']}, {mi['gpu_cores']}-core GPU"

# ---- the tune -------------------------------------------------------------------------------------------------------------------
def tune(game, scenario=None, max_runs=8, repeats=2, seconds=None, runner=None, tune_id=None, say=print, write=True):
    """Runs the ladder; returns the record (also written to runs/<tune id>/tune.json after every run)."""
    import vxrt_run
    key, profile, inst = vxrt_run.resolve(game)
    if not profile:
        raise ValueError(f"{game}: unknown game (tools/vxrt_run.py --list)")
    scenario = scenario or profile.get("tune", {}).get("scenario") or profile["default_scenario"]
    ladder = ladder_for(profile)
    runner = runner or (lambda c: vxrt_run.run(key, scenario, c.get("quality", ""), seconds, settings=c.get("settings"), quiet=True))
    tune_id = tune_id or datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + f"-tune-{key}"
    path = os.path.join(REPO, "runs", tune_id, "tune.json")
    rec = {"tune_id": tune_id, "game": key, "appid": profile["appid"], "scenario": scenario, "machine": machine_key(),
           "rule": {"holds": f"every run healthy and median fps_median >= {HOLD} x display rate", "near": f">= {NEAR} x display rate",
                    "repeats": repeats, "max_runs": max_runs},
           "ladder": [{"candidate": label(c), **c, "per_eye": per_eye(c), "display_hz": display_hz(c)} for c in ladder],
           "runs": [], "status": "running", "started": datetime.now(timezone.utc).isoformat()}
    runs = {}

    def save():
        if write:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            json.dump(rec, open(path, "w"), ensure_ascii=False, indent=2)

    save()
    while len(rec["runs"]) < max_runs:
        i = next_step(ladder, runs, repeats)
        if i is None:
            break
        say(f"tune {tune_id}: run {len(rec['runs']) + 1}/{max_runs}: {label(ladder[i])}")
        try:
            result = runner(ladder[i])
        except Exception as e:                                # (the harness itself failed)
            rec["status"], rec["error"] = "error", str(e); save(); return rec
        f = run_facts(result)
        rec["runs"].append({"candidate": i, **f})
        save()
        if f["status"] == "refused":
            rec["status"], rec["refused"] = "refused", result.get("refused", [])
            say(f"tune {tune_id}: refused ({', '.join(rec['refused'])}); stopped")
            break
        runs.setdefault(i, []).append(f)
        v, med = verdict(ladder[i], runs[i])
        say(f"  {f['run_id']}: fps {f['fps']} ({'healthy' if f['healthy'] else 'not healthy: ' + ', '.join(f['why'])}) -> {v} (median {med})")

    rec["candidates"] = []
    for i, c in enumerate(ladder):
        v, med = verdict(c, runs.get(i))
        rec["candidates"].append({"candidate": label(c), "verdict": v, "median_fps": med, "runs": [r["run_id"] for r in runs.get(i, [])]})
    if rec["status"] == "running" and not rec["runs"]:
        rec["status"] = "no_runs"                             # (--max-runs 0)
    if rec["status"] == "running":
        best, confirmed = choice(ladder, runs, repeats)
        if best is None:
            rec["status"] = "none_holds"
            fastest = max(((i, verdict(ladder[i], runs[i])[1]) for i in runs if all(r["healthy"] for r in runs[i])), key=lambda x: x[1] or 0, default=None)
            if fastest:                                       # (not a choice: what came closest)
                rec["closest"] = {"candidate": label(ladder[fastest[0]]), "median_fps": fastest[1], "target_fps": HOLD * display_hz(ladder[fastest[0]])}
        else:
            c = ladder[best]
            rec["status"] = "confirmed" if confirmed else "unconfirmed"
            rec["choice"] = {"candidate": label(c), "quality": c.get("quality", ""), "settings": c.get("settings", {}),
                             "per_eye": per_eye(c), "display_hz": display_hz(c), "median_fps": verdict(c, runs[best])[1],
                             "runs": [{"run_id": r["run_id"], "fps": r["fps"], "observation": r["fps_ref"]} for r in runs[best]],
                             "command": command_for(key, c)}
    rec["unknown"] = ["the picture and its smoothness in the headset over the user's Wi-Fi (no headset in the harness)",
                      "picture quality beyond pixels per eye; the game may choose its own size (per_eye is what the driver asks for)",
                      "other scenes of the game than the scenario measured"]
    rec["ended"] = datetime.now(timezone.utc).isoformat()
    save()
    if write:
        row = ledger.append({"tune_id": tune_id, "game": key, "status": rec["status"], "runs": [r["run_id"] for r in rec["runs"]],
                             "result": os.path.relpath(path, REPO), "result_sha256": ledger.sha256_file(path)})
        rec["ledger"] = {"row": row["i"], "sha": row["sha"]}    # (returned only: the sealed file stays as sealed)
    return rec

def apply(rec):
    """Writes a confirmed choice into the game's profile under "tuned" for this machine."""
    if rec.get("status") != "confirmed":
        raise ValueError("only a confirmed choice is written to the profile")
    f = os.path.join(REPO, "games", f"{rec['appid']}.json")
    p = json.load(open(f))
    ch = rec["choice"]
    p.setdefault("tuned", {})[rec["machine"]] = {
        "quality": ch["quality"], "settings": ch["settings"], "scenario": rec["scenario"], "median_fps": ch["median_fps"],
        "per_eye": ch["per_eye"], "display_hz": ch["display_hz"], "checked": rec["ended"][:10],
        "source": f"tools/vxrt_tune.py {rec['tune_id']} (runs {', '.join(r['run_id'] for r in ch['runs'])})"}
    problems = profiles_db.problems(p, f)
    if problems:
        raise ValueError("; ".join(problems))
    open(f, "w").write(json.dumps(p, indent=2, ensure_ascii=False) + "\n")
    return f

def tuned_for(game, machine=None):
    """(profile key, the "tuned" entry for this machine) or (key, None)."""
    p = profiles_db.by_key(str(game))
    if not p:
        return game, None
    return p["key"], p.get("tuned", {}).get(machine or machine_key())

# ---- a self-test of the climbing and choosing (make -C VXRuntime check) ----------------------------------------------------------
def selftest():
    def fake(fps_by_quality, healthy=True):
        n = [0]
        def runner(c):
            n[0] += 1
            fps = fps_by_quality[label(c)]
            fps = fps.pop(0) if isinstance(fps, list) else fps
            return {"run_id": f"r{n[0]}", "status": "no_baseline", "observed": [{"id": "o1", "metric": "fps_median", "value": fps}],
                    "derived": [{"claim": "game_connected_to_steamvr", "holds": healthy}]}
        return runner
    ladder = ladder_for(None)
    L = [label(c) for c in ladder]
    def go(fps, **kw):
        import vxrt_run
        real = vxrt_run.resolve
        vxrt_run.resolve = lambda g: ("t", {"appid": "1", "default_scenario": "s", "key": "t"}, None)
        try:
            return tune("t", runner=fake(fps, **{k: v for k, v in kw.items() if k == "healthy"}), say=lambda *a: None, write=False,
                        **{k: v for k, v in kw.items() if k != "healthy"})
        finally:
            vxrt_run.resolve = real
    checks = []
    # holds up to 2.5k, max far below: climbs, stops at max, confirms 2.5k (B A then B)
    r = go({L[0]: 60, L[1]: 60, L[2]: 59, L[3]: 58, L[4]: 20})
    checks.append(("climbs and confirms", r["status"] == "confirmed" and r["choice"]["quality"] == "2.5k" and len(r["runs"]) == 6))
    # 2.5k just out of reach (55 >= 54): looked at twice, alternating with the choice (B A B A)
    r = go({L[0]: 60, L[1]: 60, L[2]: 59, L[3]: [55, 55], L[4]: 10})
    checks.append(("near one gets a second look", r["choice"]["candidate"] == L[2] and [x["candidate"] for x in r["runs"]][-3:] == [3, 2, 3]))
    # the choice does not hold on its second run: one step down
    r = go({L[0]: 60, L[1]: 60, L[2]: [59, 40], L[3]: 30, L[4]: 10})
    checks.append(("steps down", r["choice"]["candidate"] == L[1] and r["status"] == "confirmed"))
    # nothing holds
    r = go({L[0]: 30, L[1]: 20, L[2]: 10, L[3]: 5, L[4]: 1})
    checks.append(("none holds", r["status"] == "none_holds" and len(r["runs"]) == 1))
    # unhealthy runs never hold
    r = go({x: 60 for x in L}, healthy=False)
    checks.append(("unhealthy", r["status"] == "none_holds"))
    # run budget
    r = go({x: 60 for x in L}, max_runs=3)
    checks.append(("budget", r["status"] == "unconfirmed" and len(r["runs"]) == 3))
    bad = [n for n, ok in checks if not ok]
    print(f"vxrt tune self-test: {len(checks) - len(bad)}/{len(checks)} -> {'ok' if not bad else 'FAILED: ' + ', '.join(bad)}")
    return not bad

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game", nargs="?", help="a profile key or Steam app id (tools/vxrt_run.py --list)")
    ap.add_argument("scenario", nargs="?", help="default: the profile's tune scenario, else its default scenario")
    ap.add_argument("--max-runs", type=int, default=8)
    ap.add_argument("--repeats", type=int, default=2, help="runs the choice needs (default 2)")
    ap.add_argument("--seconds", type=float, help="per run (default: the scenario's)")
    ap.add_argument("--plan", action="store_true", help="show the ladder and the time it may take; run nothing")
    ap.add_argument("--apply", action="store_true", help="write a confirmed choice into the game's profile (\"tuned\", this Mac)")
    ap.add_argument("--show", action="store_true", help="the tuned launch command for this Mac, from the profile")
    ap.add_argument("--id", help="the tune id (default: time and game)")
    ap.add_argument("--json", action="store_true", help="the record on stdout")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        sys.exit(0 if selftest() else 1)
    if not a.game:
        ap.error("which game? (tools/vxrt_run.py --list)")
    if a.show:
        key, t = tuned_for(a.game)
        if not t:
            sys.exit(f"{a.game}: not tuned for {machine_key()} yet (tools/vxrt_tune.py {a.game})")
        print(command_for(key, t)); print(f"# {t['median_fps']} fps at {t['per_eye'][0]}x{t['per_eye'][1]} per eye; {t['source']}", file=sys.stderr)
        return
    p = profiles_db.by_key(a.game) or next((x for x in profiles_db.load_all().values() if x["appid"] == a.game), None)
    if a.plan:
        if not p:
            sys.exit(f"{a.game}: no profile")
        sc = a.scenario or p.get("tune", {}).get("scenario") or p["default_scenario"]
        secs = a.seconds or p["scenarios"][sc]["seconds"]
        print(f"{p['name']}, scenario {sc}: up to {a.max_runs} runs of about {round((secs + OVERHEAD_S) / 60, 1)} min "
              f"(at most {round(a.max_runs * (secs + OVERHEAD_S) / 60)} min); the ladder:")
        for c in ladder_for(p):
            print(f"  {label(c)}  target {HOLD * display_hz(c):.0f} fps")
        return
    try:
        rec = tune(a.game, a.scenario, a.max_runs, a.repeats, a.seconds, tune_id=a.id, say=(lambda *x: print(*x, file=sys.stderr)))
    except ValueError as e:
        sys.exit(str(e))
    if a.apply and rec["status"] == "confirmed":
        print(f"written: {os.path.relpath(apply(rec), REPO)}", file=sys.stderr)
    if a.json:
        print(json.dumps(rec, ensure_ascii=False, indent=2))
    else:
        for c in rec.get("candidates", []):
            print(f"  {c['candidate']:42} {c['verdict']:9} median {c['median_fps']}  runs {', '.join(c['runs'])}")
        print(f"{rec['tune_id']}: {rec['status']}" + (f" -> {rec['choice']['candidate']}\n  {rec['choice']['command']}" if rec.get("choice") else ""))
        print(f"  record: runs/{rec['tune_id']}/tune.json")
    sys.exit({"confirmed": 0, "none_holds": 1, "unconfirmed": 2, "refused": 3}.get(rec["status"], 4))

if __name__ == "__main__":
    main()
