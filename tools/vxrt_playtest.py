#!/usr/bin/env python3
"""vxrt playtest: you play a game in the Vision Pro with your controllers (Joy-Con, hands), and this records where it
goes wrong. It is the only way a game becomes "playable" (the last of the levels, see AGENTS.md): a harness run sees
SteamVR connect, frames drawn and controller bindings loaded, but only a person can say the game can be played.

    tools/vxrt_playtest.py alyx                  # the player's launcher, with the quality tuned for this Mac if any
    tools/vxrt_playtest.py earth --quality 3k --minutes 5
    tools/vxrt_playtest.py --show alyx           # the playtests recorded for a game
    tools/vxrt_playtest.py --apply runs/<id>     # put a playtest's verdict into games/<appid>.json

While you play (your own roots and saves, as a normal start):
  - every second: head and hand tracking, triggers, grips, buttons and sticks as the Vision Pro sends them, and the
    frame rate (the game's Submits, or the encoder's captures);
  - a mark where something goes wrong: click both sticks together for about a second (or press Enter in this
    Terminal, with a note if you like). Each mark keeps the frame on screen and the last lines of the game's log.
When the game ends (or Ctrl-C), it asks a short checklist. The record: runs/<id>/playtest.json, sealed in
runs/ledger.jsonl.

Playable: picture, head, hands, buttons and movement all answered yes, the controllers were really used (input
seen), the game did not crash, and the session lasted --minutes (default 10).
"""
import argparse, json, mmap, os, re, select, signal, struct, subprocess, sys, threading, time
from datetime import datetime, timezone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vxrt_doctor as doctor                                    # noqa: E402
import vxrt_games as profiles_db                                # noqa: E402
import vxrt_ledger as ledger                                    # noqa: E402
import vxrt_run as harness                                      # noqa: E402

REPO = doctor.REPO
ROOT = os.environ.get("VXRT_VR_ROOT", os.path.join(harness.DRIVE, "vxrt_root"))   # the player's (a playtest is play)
H = harness.HANDS                                                # struct SharedHands (driver_verantyx.cpp) in the shared file
MARK_HOLD_S = 0.8
REQUIRED = ["picture", "head", "hands", "buttons", "movement"]
QUESTIONS = {
    "ja": [("picture", "映像がヘッドセットに正しく出ましたか"), ("head", "頭の動きに映像がついてきましたか"),
           ("hands", "ゲーム内に手（コントローラー）が見え、動きが合っていましたか"),
           ("buttons", "トリガー・グリップ（撃つ・掴む）が効きましたか"), ("movement", "移動や向きの変更（スティック・テレポート）ができましたか"),
           ("menu", "メニューや画面上の操作ができましたか"), ("audio", "音は出ましたか")],
    "en": [("picture", "Did the picture show correctly in the headset"), ("head", "Did the picture follow your head"),
           ("hands", "Were your hands (controllers) in the game, moving with yours"),
           ("buttons", "Did trigger and grip (shoot, grab) work"), ("movement", "Could you move and turn (stick, teleport)"),
           ("menu", "Could you use menus and on-screen controls"), ("audio", "Was there sound")],
}
FREE = {"ja": [("reached", "どこまで遊べましたか（場面・章）"), ("notes", "ほかに気づいた問題（なければ空のまま Enter）")],
        "en": [("reached", "How far did you get (scene, chapter)"), ("notes", "Anything else that went wrong (Enter: none)")]}

def read_hands(m):
    """The input block, read between two even sequence numbers (the encoder writes it while we read)."""
    for _ in range(5):
        s1 = struct.unpack_from("<i", m, 20)[0]
        if s1 % 2:
            time.sleep(0.001); continue
        b = bytes(m[H:H + 300])
        if struct.unpack_from("<i", m, 20)[0] == s1:
            break
    lt, rt, ltrig, rtrig, rbtn, lbtn = struct.unpack_from("<BBBBII", b, 192)
    rsx, rsy, lsx, lsy = struct.unpack_from("<4f", b, 204)
    vision_ts = struct.unpack_from("<d", b, 244)[0]
    lgrip, rgrip, head = struct.unpack_from("<BBB", b, 284)
    return {"head": head, "left": lt, "right": rt, "trigger": max(ltrig, rtrig), "grip": max(lgrip, rgrip),
            "buttons": (rbtn & 0x1F) | (lbtn & 0x1F00), "stick": max(abs(rsx), abs(rsy), abs(lsx), abs(lsy)), "vision_ts": vision_ts,
            "both_sticks_clicked": bool(rbtn & 4) and bool(lbtn & (4 << 8))}

class Recorder:
    def __init__(self, shared, run_dir, game_log):
        self.shared, self.run_dir, self.game_log = shared, run_dir, game_log
        self.samples, self.marks, self.stop = [], [], threading.Event()
        self.t0 = time.time()

    def mark(self, how, note=""):
        t = round(time.time() - self.t0, 1)
        png = os.path.join(self.run_dir, f"mark_{len(self.marks) + 1}.png")
        frame = harness.grab_frame(self.shared, png) if os.path.exists(self.shared) else None
        tail = open(self.game_log, errors="replace").read().splitlines()[-25:] if os.path.exists(self.game_log) else []
        self.marks.append({"at_s": t, "how": how, "note": note, "frame": os.path.relpath(png, REPO) if frame else None,
                           "game_log_tail": [line[:300] for line in tail]})
        print(f"  mark {len(self.marks)} at {t:.0f} s{(': ' + note) if note else ''}", file=sys.stderr)

    def watch_input(self):
        while not self.stop.is_set() and (not os.path.exists(self.shared) or os.path.getsize(self.shared) < H + 300):
            self.stop.wait(1)
        if self.stop.is_set():
            return
        with open(self.shared, "rb") as f:
            m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            held, last = None, None
            while not self.stop.is_set():
                s = read_hands(m)
                now = time.time()
                if s["both_sticks_clicked"]:
                    held = held or now
                    if now - held >= MARK_HOLD_S:
                        self.mark("both sticks"); held = float("inf")   # (once per hold)
                else:
                    held = None
                if last is None or now - last >= 1.0:
                    s.pop("both_sticks_clicked")
                    self.samples.append(dict(s, t=round(now - self.t0, 1)))
                    last = now
                self.stop.wait(0.05)
            m.close()

    def watch_terminal(self):
        if not sys.stdin.isatty():
            return
        while not self.stop.is_set():
            r, _, _ = select.select([sys.stdin], [], [], 0.3)
            if r:
                self.mark("Enter", sys.stdin.readline().strip())

def summarise_input(samples):
    """Per 10-second window: tracking and how much of each input was used; and the totals."""
    windows = []
    for i in range(0, len(samples), 10):
        w = samples[i:i + 10]
        if not w: break
        live = sum(1 for a, b in zip(w, w[1:]) if b["vision_ts"] != a["vision_ts"])
        windows.append({"from_s": w[0]["t"], "headset_streaming": live >= len(w) // 2,
                        "head_tracked": round(sum(s["head"] for s in w) / len(w), 2),
                        "hands_tracked": round(sum((s["left"] + s["right"]) / 2 for s in w) / len(w), 2),
                        "trigger": sum(s["trigger"] > 128 for s in w), "grip": sum(s["grip"] > 128 for s in w),
                        "buttons": sum(s["buttons"] != 0 for s in w), "stick": sum(s["stick"] > 0.5 for s in w)})
    used = {k: sum(w[k] for w in windows) for k in ("trigger", "grip", "buttons", "stick")}
    return windows, used

def fps_series(logs, root_c, since):
    proxy = harness.proxy_lines(root_c, since)
    submit = [float(x) for line in proxy for x in re.findall(r"submit_fps=([\d.]+)", line)]
    return (submit, "the game's Submits (capture proxy)") if submit else \
           (harness.encoder_fps(os.path.join(logs, "hardware-encoder.log")), "the encoder's captures")

def ask(lang):
    answers = {}
    if not sys.stdin.isatty():
        return None
    print("\n" + ("プレイテストのチェック（y / n / s=分からない）:" if lang == "ja" else "Playtest checklist (y / n / s = not sure):"))
    for key, q in QUESTIONS[lang]:
        while True:
            a = input(f"  {q}? [y/n/s] ").strip().lower()[:1]
            if a in ("y", "n", "s"):
                answers[key] = {"y": "yes", "n": "no", "s": "unsure"}[a]; break
    for key, q in FREE[lang]:
        answers[key] = input(f"  {q}: ").strip()
    return answers

def playtest(game, quality=None, minutes=10.0, lang="ja"):
    key, spec, inst = harness.resolve(game)
    if not spec or not inst:
        sys.exit(f"{game}: not installed or unknown (tools/vxrt_run.py --list)")
    busy = harness.pgrep("[b]uild/vxrt") + harness.pgrep("[w]ine.*(vrserver|hlvr)")
    if busy:
        sys.exit("A game or SteamVR is already running: end it first")
    if quality is None:                                          # (what tools/vxrt_tune.py confirmed for this Mac)
        import vxrt_tune
        t = spec.get("tuned", {}).get(vxrt_tune.machine_key())
        quality, settings = (t["quality"], t["settings"]) if t else ("", {})
    else:
        settings = {}
    pid = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + f"-playtest-{key}"
    run_dir = os.path.join(REPO, "runs", pid)
    logs = os.path.join(run_dir, "logs")
    os.makedirs(logs, exist_ok=True)
    env = dict(os.environ, VXRT_VR_LOG_DIR=logs, **{k: str(v) for k, v in settings.items()})
    env.pop("VXRT_VR_QUALITY", None)
    if quality: env["VXRT_VR_QUALITY"] = quality
    if spec["launcher"] == "alyx":
        cmd, game_log, exe = [os.path.join(REPO, "scripts", "launch_vxrt_alyx.sh")], os.path.join(logs, "vxrt-alyx.log"), "hlvr.exe"
    else:
        exe = os.path.basename(inst["exe"])
        cmd, game_log = [os.path.join(REPO, "scripts", "launch_vxrt_game.sh"), inst["exe"]], os.path.join(logs, "vxrt-%s.log" % os.path.splitext(exe)[0])
    shared = os.path.join(ROOT, "drive_c", "vr_shared_frame.dat")
    rec = Recorder(shared, run_dir, game_log)
    print(f"playtest {pid}: {spec['name']}{', ' + quality if quality else ''}. Put on the headset and play.\n"
          f"  Something wrong? Click both sticks for a second (or press Enter here, with a note). End: quit the game or Ctrl-C.",
          file=sys.stderr)
    with open(os.path.join(logs, "launcher.log"), "w") as out:
        p = subprocess.Popen(cmd, env=env, stdout=out, stderr=subprocess.STDOUT, start_new_session=True)
    threads = [threading.Thread(target=rec.watch_input, daemon=True), threading.Thread(target=rec.watch_terminal, daemon=True)]
    for t in threads: t.start()
    ended_by = "game"
    try:
        p.wait()
    except KeyboardInterrupt:
        ended_by = "Ctrl-C"
        try: os.killpg(p.pid, signal.SIGTERM); p.wait(30)
        except (ProcessLookupError, subprocess.TimeoutExpired): pass
    rec.stop.set()
    for t in threads: t.join(2)
    duration = time.time() - rec.t0

    facts = harness.game_log_facts(game_log, exe)
    fps, fps_source = fps_series(logs, os.path.join(ROOT, "drive_c"), rec.t0)
    windows, used = summarise_input(rec.samples)
    hz = float(settings.get("VERANTYX_DISPLAY_HZ", 60))
    slow = [i * 5 for i, f in enumerate(fps) if f < 0.75 * hz]
    answers = ask(lang)
    # (quitting from the game's menu ends with ExitProcess(0); anything else that ended it by itself is a crash)
    crashed = bool(facts["unhandled"] or facts["signals"] or (facts["exit"] and not str(facts["exit"]).startswith("ExitProcess(0)")))
    problems = [{"what": "mark", **{k: m[k] for k in ("at_s", "how", "note", "frame")}} for m in rec.marks]
    problems += [{"what": f"answered no: {k}"} for k, v in (answers or {}).items() if v == "no"]
    if facts["first_problem"]: problems.append({"what": "first problem in the game's log", "line": facts["first_problem"]})
    if crashed: problems.append({"what": "the game crashed or quit by itself", "exit": facts["exit"]})
    if facts["unimplemented"]: problems.append({"what": "unimplemented Windows functions called", "calls": facts["unimplemented"]})
    if slow: problems.append({"what": f"frame rate under {0.75 * hz:.0f} fps", "at_s": slow[:20], "windows": len(slow)})
    lost = [w["from_s"] for w in windows if w["headset_streaming"] and w["head_tracked"] < 0.5]
    if lost: problems.append({"what": "head tracking lost while the headset was streaming", "at_s": lost[:20]})
    input_seen = any(used.values())
    verdict = {"answered": answers is not None,
               "required_yes": answers is not None and all(answers.get(k) == "yes" for k in REQUIRED),
               "input_seen": input_seen, "no_crash": not crashed, "long_enough": duration >= minutes * 60}
    verdict["playable"] = all(verdict.values())
    record = {"playtest_id": pid, "game": key, "appid": spec["appid"], "quality": quality or None, "settings": settings,
              "machine": doctor.machine_info(), "started": datetime.fromtimestamp(rec.t0, timezone.utc).isoformat(),
              "minutes": round(duration / 60, 1), "ended_by": ended_by, "commit": harness.git_state()[0],
              "answers": answers, "verdict": verdict, "problems": problems, "marks": rec.marks,
              "fps": {"source": fps_source, "every_s": 5, "series": fps}, "input": {"used_seconds": used, "per_10s": windows},
              "game_log": {"exit": facts["exit"], "first_problem": facts["first_problem"], "unimplemented_calls": facts["unimplemented"]},
              "rule": f"playable: {', '.join(REQUIRED)} answered yes, controller input seen, no crash, at least {minutes:g} minutes"}
    path = os.path.join(run_dir, "playtest.json")
    json.dump(record, open(path, "w"), ensure_ascii=False, indent=2)
    ledger.append({"playtest_id": pid, "game": key, "playable": verdict["playable"], "result": os.path.relpath(path, REPO),
                   "result_sha256": ledger.sha256_file(path)})
    return record

def apply(run_dir):
    """Adds a playtest to its game's profile ("playtests"), and its level: playable -> status works."""
    rec = json.load(open(os.path.join(run_dir, "playtest.json")))
    f = os.path.join(REPO, "games", f"{rec['appid']}.json")
    p = json.load(open(f))
    entry = {"date": rec["started"][:10], "machine": f"{rec['machine']['chip']}, {rec['machine']['gpu_cores']}-core GPU",
             "quality": rec["quality"] or "", "minutes": rec["minutes"], "playable": rec["verdict"]["playable"],
             "reached": (rec["answers"] or {}).get("reached", ""), "problems": len(rec["problems"]), "source": f"tools/vxrt_playtest.py {rec['playtest_id']}"}
    p.setdefault("playtests", []).append(entry)
    if rec["verdict"]["playable"]:
        p["status"].update({"state": "works", "level": "playable", "checked": entry["date"], "machine": entry["machine"]})
    problems = profiles_db.problems(p, f)
    if problems:
        raise ValueError("; ".join(problems))
    open(f, "w").write(json.dumps(p, indent=2, ensure_ascii=False) + "\n")
    return f

def show(game):
    p = profiles_db.by_key(game) or next((x for x in profiles_db.load_all().values() if x["appid"] == game), None)
    if not p: sys.exit(f"{game}: no profile")
    tests = sorted(d for d in os.listdir(os.path.join(REPO, "runs")) if d.endswith(f"-playtest-{p['key']}")) if os.path.isdir(os.path.join(REPO, "runs")) else []
    for d in tests:
        r = json.load(open(os.path.join(REPO, "runs", d, "playtest.json")))
        print(f"{d}: {'playable' if r['verdict']['playable'] else 'not playable'}, {r['minutes']} min, {len(r['problems'])} problems"
              + (f", reached: {r['answers'].get('reached')}" if r.get("answers") else ""))
        for pr in r["problems"][:8]:
            print(f"   - {pr['what']}" + (f" at {pr['at_s']} s" if isinstance(pr.get('at_s'), (int, float)) else "") + (f": {pr['note']}" if pr.get("note") else ""))
    if not tests: print("no playtests yet")

def selftest():
    """The input recorder against a stand-in shared file: samples, a both-sticks mark, the summary."""
    import tempfile
    d = tempfile.mkdtemp()
    shared = os.path.join(d, "vr_shared_frame.dat")
    with open(shared, "wb") as f: f.write(b"\0" * (H + 400))
    rec = Recorder(shared, d, os.path.join(d, "game.log"))
    t = threading.Thread(target=rec.watch_input, daemon=True); t.start()
    with open(shared, "r+b") as f:
        m = mmap.mmap(f.fileno(), 0)
        def put(head, trig, rb, lb, ts):
            struct.pack_into("<i", m, 20, struct.unpack_from("<i", m, 20)[0] + 1)
            m[H + 286] = head; m[H + 192] = m[H + 193] = 1; m[H + 195] = trig
            struct.pack_into("<II", m, H + 196, rb, lb); struct.pack_into("<d", m, H + 244, ts)
            struct.pack_into("<i", m, 20, struct.unpack_from("<i", m, 20)[0] + 1)
        for i in range(30):                                   # 1.5 s: trigger held, then both sticks for 1.2 s
            put(1, 255 if i < 10 else 0, 4 if i >= 6 else 0, (4 << 8) if i >= 6 else 0, float(i)); time.sleep(0.05)
        for i in range(30):
            put(1, 0, 0, 0, 100.0 + i); time.sleep(0.05)
        m.close()
    rec.stop.set(); t.join(2)
    windows, used = summarise_input(rec.samples)
    checks = [("samples", len(rec.samples) >= 2), ("one mark", len(rec.marks) == 1 and rec.marks[0]["how"] == "both sticks"),
              ("input seen", used["trigger"] >= 1 or used["buttons"] >= 1), ("windows", bool(windows))]
    bad = [n for n, ok in checks if not ok]
    print(f"vxrt playtest self-test: {len(checks) - len(bad)}/{len(checks)} -> {'ok' if not bad else 'FAILED: ' + ', '.join(bad)}")
    return not bad

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game", nargs="?")
    ap.add_argument("--quality", choices=["", "2.5k", "3k", "max"], help="default: tuned for this Mac (tools/vxrt_tune.py), else none")
    ap.add_argument("--minutes", type=float, default=10.0, help="the shortest session that can count as playable")
    ap.add_argument("--lang", choices=["ja", "en"], default="ja")
    ap.add_argument("--show", action="store_true"); ap.add_argument("--apply", metavar="RUN_DIR")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        sys.exit(0 if selftest() else 1)
    if a.apply:
        print(f"written: {os.path.relpath(apply(a.apply), REPO)}"); return
    if not a.game:
        ap.error("which game? (tools/vxrt_run.py --list)")
    if a.show:
        show(a.game); return
    r = playtest(a.game, a.quality, a.minutes, a.lang)
    v = r["verdict"]
    print(f"\n{r['playtest_id']}: {'PLAYABLE' if v['playable'] else 'not playable yet'} ({r['minutes']} min)")
    for k, ok in v.items():
        if k != "playable": print(f"  {k}: {'yes' if ok else 'NO'}")
    for pr in r["problems"]:
        print(f"  problem: {pr['what']}" + (f" at {pr['at_s']} s" if isinstance(pr.get('at_s'), (int, float)) else ""))
    print(f"  record: runs/{r['playtest_id']}/playtest.json  (tools/vxrt_playtest.py --apply runs/{r['playtest_id']} puts it in the profile)")

if __name__ == "__main__":
    main()
