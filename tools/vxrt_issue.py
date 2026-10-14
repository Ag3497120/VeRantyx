#!/usr/bin/env python3
"""vxrt issue: when a game still does not run (or runs badly) after an AI agent has tried, this writes a GitHub issue
for the VeRantyx repository from the evidence: the harness's result, the first problem and the log around it, the
doctor's facts, the game profile, the machine, the commit, and what was tried. Personal details are taken out (home
folder, user and machine names, Steam names and ids, network addresses).

    tools/vxrt_issue.py draft <game> [--run runs/<id>] [--kind support|crash|performance|rendering] [--tried "..."|--tried-file f]
    tools/vxrt_issue.py submit runs/issues/<draft>.md      # the user, in Terminal: shows it, asks, posts with gh

A draft is only written to runs/issues/ (and printed). Posting needs the person at the keyboard: submit asks them to
type "yes" and refuses without a terminal, so an agent cannot post one on its own.
"""
import argparse, getpass, json, os, re, socket, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vxrt_doctor as doctor                                    # noqa: E402
import vxrt_games                                               # noqa: E402

REPO = doctor.REPO
RUNS = os.path.join(REPO, "runs")
DEFAULT_REPOSITORY = "Ag3497120/VeRantyx"
KINDS = {"support": "does not run", "crash": "crashes", "performance": "runs slowly", "rendering": "draws wrongly"}
MARK = "<!-- vxrt-issue v1 -->"

# ---- taking personal details out --------------------------------------------------------------------------------------------
def _steam_names(texts):
    names = set()
    for t in texts:
        names.update(re.findall(r"Client \d+ '([^']{2,40})'", t))
        names.update(re.findall(r"^\s*([A-Za-z0-9_.-]{3,40})\s+loopback:\d", t, re.M))
    return {n for n in names if n.lower() not in ("client", "server")}

def redactor(texts=()):
    home, user = os.path.expanduser("~"), getpass.getuser()
    host = socket.gethostname().split(".")[0]
    names = _steam_names(texts)
    rules = [
        (re.escape(home), "~"), (re.escape(home.replace("/", "\\")[1:]), "~"),          # /Users/x and Users\x (Z:\Users\x)
        (r"\b7656119\d{10}\b", "<steamid>"), (r"(user|user_keys)_\d{6,12}_", r"\1_<id>_"),
        (r"[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}", "<email>"),
        (r"\b(?!127\.0\.0\.1\b)(?:\d{1,3}\.){3}\d{1,3}\b", "<ip>"),
        (r"\bfe80::[0-9a-f:]+(%\w+)?", "<ip6>"), (r"\b(?:[0-9a-f]{2}:){5}[0-9a-f]{2}\b", "<mac>"),
    ]
    for word in [user, host] + sorted(names, key=len, reverse=True):
        if word and len(word) >= 3:
            rules.append((re.escape(word), "<user>" if word != host else "<host>"))
    compiled = [(re.compile(p, re.I), r) for p, r in rules]
    def redact(text):
        for p, r in compiled:
            text = p.sub(r, text)
        return text
    return redact

# ---- the evidence -----------------------------------------------------------------------------------------------------------------
def latest_run(key):
    if not os.path.isdir(RUNS):
        return None
    runs = sorted((d for d in os.listdir(RUNS) if f"-{key}-" in d and os.path.exists(os.path.join(RUNS, d, "result.json"))), reverse=True)
    return os.path.join(RUNS, runs[0]) if runs else None

def game_lines(path, exe):
    """The game's own lines of its log (not the SteamVR helpers it started), numbered."""
    out = []
    for n, line in enumerate(open(path, errors="replace"), 1):
        tag = re.match(r"\[vxrt ([^\]:]+):\d+\]", line)
        if tag and exe and tag.group(1).lower() != exe.lower():
            continue
        out.append((n, line.rstrip()[:300]))
    return out

def excerpt(lines, around, before=20, after=10):
    idx = next((i for i, (n, _) in enumerate(lines) if n >= around), len(lines) - 1)
    return "\n".join(f"{n}: {t}" for n, t in lines[max(0, idx - before): idx + after] if t.strip())

def build(game, run_dir=None, kind="support", tried="", repository=DEFAULT_REPOSITORY):
    """(title, body, facts) of a draft."""
    import vxrt_run
    key, profile, inst = vxrt_run.resolve(game)
    if not inst:
        raise ValueError(f"{game}: not installed or unknown")
    run_dir = run_dir or latest_run(key)
    result = json.load(open(os.path.join(run_dir, "result.json"))) if run_dir else None
    facts = doctor.analyze(inst["exe"])
    obs = {o["metric"]: o for o in (result or {}).get("observed", [])}
    exe = (result or {}).get("exe") or os.path.basename(inst["exe"])
    log = next((os.path.join(run_dir, "logs", f) for f in os.listdir(os.path.join(run_dir, "logs")) if f.startswith("vxrt-") and f not in ("vxrt-steamvr.log", "vxrt-steamvr-launch.log")), None) if run_dir else None
    lines = game_lines(log, exe) if log else []
    fp = (obs.get("first_problem") or {}).get("value")
    texts = [open(log, errors="replace").read(2_000_000)] if log else []
    redact = redactor(texts)
    head, dirty = vxrt_run.git_state()
    mi = facts["machine"]

    state = (result or {}).get("status", "no run")
    title = f"[{kind}] {profile['name']} ({profile['appid']}) {KINDS.get(kind, kind)} on VXRuntime"
    if fp: title += ": " + re.sub(r"^\[vxrt[^\]]*\]\s*[0-9:.]*\s*", "", fp["text"])[:90]
    B = [MARK, f"**Game:** {profile['name']} (Steam {profile['appid']}), `{os.path.relpath(inst['exe'], inst['folder'])}`, engine {', '.join(facts['engine']) or '?'}",
         f"**Machine:** {mi['chip']}, GPU {mi['gpu_cores']} cores, {mi['memory_gb']} GB, macOS {mi['macos']}",
         f"**VXRuntime:** commit {head}{' (with uncommitted changes)' if dirty else ''}",
         f"**Profile:** " + (f"`games/{profile['appid']}.json`: {profile['status']['state']} ({profile['status']['summary']['en']})" if os.path.exists(os.path.join(vxrt_games.GAMES_DIR, profile['appid'] + '.json')) else "none yet"),
         "", "## What happens"]
    if result:
        B.append(f"Harness run `{result['run_id']}` ({result['scenario']}{'@' + result['quality'] if result.get('quality') else ''}, {result.get('duration_s', '?')} s): **{state}**" +
                 (f" (refused: {', '.join(result.get('refused', []))})" if result.get("refused") else ""))
        for m in ("exited_early", "connected_after_s", "fps_median", "gpu_utilization_median", "frame_bright_fraction", "unhandled_exceptions",
                  "native_signals_outside_program", "unimplemented_calls", "managed_exceptions", "exit"):
            if m in obs: B.append(f"- {obs[m]['id']} `{m}` = `{json.dumps(obs[m]['value'], ensure_ascii=False)[:200]}`")
        for d in result.get("derived", []): B.append(f"- {d['id']} {d['claim']}: {'yes' if d['holds'] else '**no**'} ({d['rule']})")
        for i in result.get("inferred", []): B.append(f"- {i['id']} (suspicion, {i['confidence']}): {i['claim']}")
    else:
        B.append("No harness run (tools/vxrt_run.py) yet.")
    if fp:
        B += ["", "## The first problem", "```", fp["text"], "```", "", "<details><summary>The game's log around it</summary>", "", "```", excerpt(lines, fp["line"]), "```", "</details>"]
    elif lines:
        B += ["", "<details><summary>The end of the game's log</summary>", "", "```", "\n".join(f"{n}: {t}" for n, t in lines[-30:] if t.strip()), "```", "</details>"]
    missing = facts["missing_functions"]
    B += ["", "## Static facts (vxrt doctor)",
          f"- Graphics and others: {', '.join(facts['apis']) or 'none'}",
          f"- Windows functions VXRuntime lacks (imported; called or not unknown): {sum(len(v) for v in missing.values())}" +
          ("".join(f"\n  - {dll}: {', '.join(names[:25])}{' …' if len(names) > 25 else ''}" for dll, names in list(missing.items())[:6]) if kind in ("support", "crash") else ""),
          f"- DLLs not found: {', '.join(facts['missing_dlls']) or 'none'}"]
    if profile["known_issues"]:
        B += ["- Known issues in the profile:"] + [f"  - {k['summary']}" for k in profile["known_issues"]]
    B += ["", "## What was tried", tried.strip() or "_(nothing written: say what you or your AI tool changed and what each attempt showed)_",
          "", "## Not known", *[f"- {u['item']}: {u['reason']}" for u in (result or {}).get("unknown", [])][:6],
          "", f"_Written by `tools/vxrt_issue.py` from local evidence; personal details were taken out. Result files stay on the reporter's Mac ({os.path.relpath(run_dir, REPO) if run_dir else 'no run'})._"]
    return redact(title), redact("\n".join(B)), {"run": run_dir, "appid": profile["appid"], "repository": repository}

def similar(repository, appid, limit=8):
    """Open or closed issues mentioning the app id (gh, read only); [] when gh is missing or not signed in."""
    try:
        r = subprocess.run(["gh", "issue", "list", "--repo", repository, "--state", "all", "--search", f"{appid} in:title",
                            "--json", "number,title,state,url", "--limit", str(limit)], capture_output=True, text=True, timeout=20)
        return json.loads(r.stdout) if r.returncode == 0 else []
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return []

def draft(game, run_dir=None, kind="support", tried="", repository=DEFAULT_REPOSITORY):
    title, body, meta = build(game, run_dir, kind, tried, repository)
    os.makedirs(os.path.join(RUNS, "issues"), exist_ok=True)
    path = os.path.join(RUNS, "issues", time.strftime("%Y%m%d-%H%M%S") + f"-{meta['appid']}-{kind}.md")
    with open(path, "w") as f:
        f.write(f"<!-- repository: {repository} -->\n# {title}\n\n{body}\n")
    return {"draft": os.path.relpath(path, REPO), "title": title, "body": body, "similar": similar(repository, meta["appid"]),
            "submit": f"tools/vxrt_issue.py submit {os.path.relpath(path, REPO)}   (the user runs this in Terminal)"}

def submit(path):
    text = open(path).read()
    m = re.match(r"<!-- repository: (\S+) -->\n# (.+)\n\n", text)
    if not m or MARK not in text:
        sys.exit("not a draft written by tools/vxrt_issue.py")
    repository, title, body = m.group(1), m.group(2), text[m.end():]
    if not sys.stdin.isatty():
        sys.exit("Posting needs the user: run this yourself in Terminal (it shows the issue and asks first).")
    print(f"--- {repository} ---\n# {title}\n\n{body}\n---")
    for s in similar(repository, re.search(r"\((\d+)\)", title).group(1) if re.search(r"\((\d+)\)", title) else ""):
        print(f"similar: #{s['number']} [{s['state']}] {s['title']}  {s['url']}")
    if input(f"Post this issue to {repository} with your GitHub account? Type yes: ").strip().lower() != "yes":
        sys.exit("Not posted.")
    r = subprocess.run(["gh", "issue", "create", "--repo", repository, "--title", title, "--body", body], text=True, capture_output=True)
    print(r.stdout or r.stderr)
    sys.exit(r.returncode)

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("draft"); d.add_argument("game"); d.add_argument("--run"); d.add_argument("--kind", choices=list(KINDS), default="support")
    d.add_argument("--tried", default=""); d.add_argument("--tried-file"); d.add_argument("--repository", default=DEFAULT_REPOSITORY)
    d.add_argument("--json", action="store_true")
    s = sub.add_parser("submit"); s.add_argument("draft")
    a = ap.parse_args()
    if a.cmd == "submit":
        return submit(a.draft)
    tried = open(a.tried_file).read() if a.tried_file else a.tried
    r = draft(a.game, a.run, a.kind, tried, a.repository)
    if a.json:
        print(json.dumps(r, ensure_ascii=False, indent=2)); return
    print(f"# {r['title']}\n\n{r['body']}\n")
    for s_ in r["similar"]: print(f"similar: #{s_['number']} [{s_['state']}] {s_['title']}  {s_['url']}")
    print(f"\nDraft: {r['draft']}\nTo post it (you, in Terminal): {r['submit'].split('   ')[0]}")

if __name__ == "__main__":
    main()
