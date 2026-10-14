#!/usr/bin/env python3
"""VXRT Studio: a page in the browser listing the Windows VR games installed for VXRuntime, with buttons that copy
a prompt for an AI coding tool (Claude Code, Codex, Gemini CLI, Cursor...) to tune a game for this Mac or to make it
run, and the command that starts it.

    python3 tools/vxrt_studio.py [--port 8765] [--no-open]

The prompts are the ones tools/vxrt_doctor.py writes; paste one into your AI tool started in this repository (the
rules it follows are in AGENTS.md), or let the page start it: "AIに任せる" opens Terminal with Claude Code or Codex
(your own login) in this repository, given the prompt and the vxrt MCP server (tools/vxrt_mcp.py), with which the
agent runs and measures the game itself. The page is served on 127.0.0.1 only; starting an agent needs the page's
own token (other sites cannot), and the agent asks you before it runs anything.
"""
import argparse, glob, json, os, re, secrets, shlex, shutil, subprocess, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vxrt_doctor as doctor                                    # noqa: E402

REPO = doctor.REPO
TOKEN = secrets.token_urlsafe(18)                               # in the page: a request starting an agent must carry it
DRIVE = os.path.expanduser(os.environ.get("VERANTYX_VR_DRIVE", "~/Verantyx_VR_Drive"))
PREFIX = os.environ.get("WINEPREFIX", os.path.join(DRIVE, "SteamVR_Prefix_Wine11_Test"))
STEAM_ROOT = os.path.join(PREFIX, "drive_c", "Program Files (x86)", "steam")
LIBRARY = os.path.join(DRIVE, "vxrt_library")                  # scripts/fetch_windows_vr_app.sh installs here
NOT_GAMES = {"228980", "250820"}                                # Steamworks Common Redistributables, SteamVR

# What has been seen to work (VXRuntime/README.md has the details). A game not listed here is untested.
KNOWN = {
    "546560": ("works", "VR で動作。3k プリセットで 60 fps（M1 Max）", "Works in VR. 60 fps with the 3k preset (M1 Max)"),
    "450390": ("works", "VR で動作（タイトル画面まで確認、53–58 fps）", "Works in VR (title scene checked, 53–58 fps)"),
    "348250": ("partial", "VR で地形とラベルが出る。空が白い", "Terrain and labels in VR; the sky is white"),
    "457550": ("untested", "未確認（足りない関数あり）", "Not tried yet (functions missing)"),
}

def windows_to_mac(path):
    """A Steam library path as Windows Steam in the prefix writes it."""
    p = path.replace("\\\\", "\\")
    if re.match(r"[A-Za-z]:\\", p):
        drive, rest = p[0].lower(), p[3:].replace("\\", "/")
        if drive == "c": return os.path.join(PREFIX, "drive_c", rest)
        if drive == "z": return "/" + rest
    return None

def libraries():
    libs = [STEAM_ROOT]
    vdf = os.path.join(STEAM_ROOT, "steamapps", "libraryfolders.vdf")
    if os.path.exists(vdf):
        for m in re.finditer(r'"path"\s+"([^"]+)"', open(vdf, errors="replace").read()):
            p = windows_to_mac(m.group(1))
            if p and os.path.isdir(p) and os.path.realpath(p).lower() not in {os.path.realpath(x).lower() for x in libs}:
                libs.append(p)
    libs += sorted(d for d in glob.glob(os.path.join(LIBRARY, "*")) if os.path.isdir(os.path.join(d, "steamapps")))
    return libs

def manifest(path):
    text = open(path, errors="replace").read()
    get = lambda key: (re.search(r'"%s"\s+"([^"]*)"' % key, text, re.I) or [None, ""])[1]
    return {"appid": get("appid"), "name": get("name"), "installdir": get("installdir"), "size": int(get("SizeOnDisk") or 0)}

_scan_cache = {}
def find_games():
    games = []
    for lib in libraries():
        for acf in sorted(glob.glob(os.path.join(lib, "steamapps", "appmanifest_*.acf"))):
            m = manifest(acf)
            if not m["appid"] or m["appid"] in NOT_GAMES:
                continue
            folder = os.path.join(lib, "steamapps", "common", m["installdir"])
            if not os.path.isdir(folder):
                folder = lib if os.path.dirname(lib) == LIBRARY else None   # (steamcmd's force_install_dir: the game is the library)
            if not folder:
                continue
            key = (folder, os.path.getmtime(acf))
            if key not in _scan_cache:
                exes, _, markers = doctor.scan_game(folder)
                _scan_cache[key] = (doctor.pick_exe(exes, m["name"] or m["installdir"]), sorted(markers))
            exe, markers = _scan_cache[key]
            if not exe:
                continue
            games.append(dict(m, folder=folder, exe=exe, markers=markers, library=lib))
    return games

def last_run(exe):
    """The newest log of the game (logs/vxrt-*.log) and what went wrong in it."""
    log = doctor.find_log(os.path.basename(exe))
    if not log:
        return None
    found = doctor.read_log(log, os.path.basename(exe))
    problems = [{"kind": k, "what": w, "count": n} for k, items in found.items() for w, n in sorted(items.items(), key=lambda kv: -kv[1])[:3]]
    return {"log": os.path.relpath(log, REPO), "when": time.strftime("%Y-%m-%d %H:%M", time.localtime(os.path.getmtime(log))), "problems": problems[:8]}

def game_info(g, lang):
    status, ja, en = KNOWN.get(g["appid"], ("untested", "未確認", "Not tried yet"))
    engine = [m.split(":", 1)[1] for m in g["markers"] if m.startswith(("engine:", "vr:"))]
    anticheat = [m.split(":", 1)[1] for m in g["markers"] if m.startswith("anticheat:")]
    return {"appid": g["appid"], "name": g["name"], "exe": os.path.relpath(g["exe"], g["folder"]), "folder": g["folder"],
            "size_gb": round(g["size"] / 2**30, 1), "engine": engine, "anticheat": sorted(set(anticheat)),
            "status": status, "note": ja if lang == "ja" else en, "last_run": last_run(g["exe"])}

def agents():
    return {"claude": bool(shutil.which("claude")), "codex": bool(shutil.which("codex"))}

def delegate(game, goal, lang, tool):
    """Writes the prompt and a Terminal command file under runs/prompts/ and opens it: the agent starts in the
    repository with the prompt as its first message and the vxrt MCP server, in its normal (asking) mode."""
    prompt = doctor.build_prompt(game["exe"], goal, lang)
    d = os.path.join(REPO, "runs", "prompts")
    os.makedirs(d, exist_ok=True)
    stem = os.path.join(d, time.strftime("%Y%m%d-%H%M%S") + f"-{game['appid']}-{goal}-{tool}")
    open(stem + ".md", "w").write(prompt)
    q = shlex.quote
    if tool == "claude":
        # (run_game takes minutes: Claude Code's MCP tool timeout raised to 15 min)
        line = f'MCP_TOOL_TIMEOUT=900000 claude --mcp-config .mcp.json "$(cat {q(stem + ".md")})"'
    else:
        server = os.path.join(REPO, "tools", "vxrt_mcp.py")
        line = ("codex -c 'mcp_servers.vxrt.command=\"python3\"' -c " + q(f'mcp_servers.vxrt.args=["{server}"]') +
                f" -c 'mcp_servers.vxrt.tool_timeout_sec=900' \"$(cat {q(stem + '.md')})\"")
    with open(stem + ".command", "w") as f:
        f.write(f"#!/bin/bash\n# VXRT Studio: {game['name']} ({goal}) with {tool}\ncd {q(REPO)} || exit 1\n{line}\n")
    os.chmod(stem + ".command", 0o755)
    if not os.environ.get("VXRT_STUDIO_NO_OPEN"):               # (tests: write the files, do not start anything)
        subprocess.run(["open", "-a", "Terminal", stem + ".command"], check=True)
    return os.path.relpath(stem + ".md", REPO)

# ---- the page -----------------------------------------------------------------------------------------------------------
PAGE = r"""<!doctype html>
<html lang="ja"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>VXRT Studio</title>
<style>
:root { --bg:#f6f7f9; --panel:#fff; --text:#16181d; --muted:#5d6470; --line:#e2e5ea; --accent:#2f6fde; --accent-text:#fff;
        --ok:#1f8a4c; --warn:#b26b00; --idle:#6b7280; --code:#f0f2f5; }
@media (prefers-color-scheme: dark) { :root { --bg:#121417; --panel:#1b1e23; --text:#e8eaee; --muted:#9aa1ad; --line:#2c3038;
        --accent:#5b8ff0; --accent-text:#0b0d10; --ok:#4cc07f; --warn:#e2a53d; --idle:#8b93a0; --code:#23272e; } }
* { box-sizing:border-box; }
body { margin:0; background:var(--bg); color:var(--text); font:14px/1.5 -apple-system, "Hiragino Sans", "Helvetica Neue", sans-serif; }
header { display:flex; align-items:center; gap:12px; padding:14px 20px; border-bottom:1px solid var(--line); background:var(--panel); }
header h1 { font-size:17px; margin:0; } header .sub { color:var(--muted); font-size:12px; }
header .lang { margin-left:auto; }
main { display:grid; grid-template-columns: minmax(240px, 320px) 1fr; min-height:calc(100vh - 56px); }
#list { border-right:1px solid var(--line); padding:10px; overflow:auto; }
.game { padding:10px 12px; border-radius:8px; cursor:pointer; border:1px solid transparent; margin-bottom:6px; background:var(--panel); }
.game:hover { border-color:var(--line); } .game.sel { border-color:var(--accent); }
.game .name { font-weight:600; } .game .meta { color:var(--muted); font-size:12px; }
.dot { display:inline-block; width:8px; height:8px; border-radius:50%; margin-right:6px; vertical-align:1px; }
.works { background:var(--ok); } .partial { background:var(--warn); } .untested { background:var(--idle); }
#detail { padding:20px 24px; overflow:auto; }
#detail h2 { margin:0 0 4px; font-size:20px; }
.chips span { display:inline-block; font-size:12px; border:1px solid var(--line); border-radius:99px; padding:1px 8px; margin:0 4px 4px 0; color:var(--muted); }
.box { background:var(--panel); border:1px solid var(--line); border-radius:10px; padding:14px 16px; margin:14px 0; }
.box h3 { margin:0 0 8px; font-size:14px; }
.row { display:flex; flex-wrap:wrap; gap:8px; align-items:center; }
button { font:inherit; border:1px solid var(--line); background:var(--panel); color:var(--text); border-radius:7px; padding:6px 12px; cursor:pointer; }
button.primary { background:var(--accent); border-color:var(--accent); color:var(--accent-text); }
button:disabled { opacity:.5; cursor:default; }
select { font:inherit; padding:5px 6px; border-radius:7px; border:1px solid var(--line); background:var(--panel); color:var(--text); }
textarea { width:100%; min-height:260px; font:12px/1.45 ui-monospace, Menlo, monospace; background:var(--code); color:var(--text);
           border:1px solid var(--line); border-radius:8px; padding:10px; resize:vertical; }
code.cmd { display:block; white-space:pre-wrap; word-break:break-all; background:var(--code); padding:8px 10px; border-radius:8px; font-size:12px; }
.muted { color:var(--muted); } .small { font-size:12px; } .path { word-break:break-all; }
.problems li { font-size:12px; } .warn { color:var(--warn); }
.toast { position:fixed; bottom:18px; left:50%; transform:translateX(-50%); background:var(--text); color:var(--bg); padding:7px 14px; border-radius:8px; opacity:0; transition:opacity .2s; }
.toast.on { opacity:.95; }
@media (max-width: 720px) { main { grid-template-columns:1fr; } #list { border-right:0; border-bottom:1px solid var(--line); max-height:40vh; } #detail { padding:16px; } }
</style></head>
<body>
<header><h1>VXRT Studio</h1><span class="sub" id="sub"></span>
  <select class="lang" id="lang" aria-label="language"><option value="ja">日本語</option><option value="en">English</option></select></header>
<main><div id="list"></div><div id="detail"></div></main>
<div class="toast" id="toast"></div>
<script>
const S = {
  ja: { sub: "Windows VR ゲームを VXRuntime で動かす・調整するためのプロンプトを作ります",
        empty: "インストール済みのゲームが見つかりません（scripts/fetch_windows_vr_app.sh で入れられます）",
        pick: "左からゲームを選んでください", size: "GB", exe: "実行ファイル", folder: "場所", ac: "アンチチートあり（回避はしません）: ",
        ai: "AI に頼む", aihint: "Claude Code・Codex などをこのリポジトリで開き、コピーしたプロンプトを貼り付けてください。AI は AGENTS.md の規則（DRM を回避しない、ゲームファイルを変えない、確かめたものだけコミット）に従います。",
        tune: "この Mac 向けに調整", tunehint: "設定だけを変えて fps と解像度を詰める", support: "動くようにする", supporthint: "足りない Windows API などを VXRuntime に実装する",
        make: "プロンプトを作る", making: "調べています…", copy: "コピー", copied: "コピーしました",
        run: "起動", quality: "画質", qdef: "標準", runhint: "ターミナルで実行します。SteamVR と一緒に起動し、Vision Pro アプリから接続します。Mac 版 Steam にサインインしておいてください。",
        copycmd: "コマンドをコピー", last: "前回の実行", nolog: "まだ実行記録がありません", problems: "ログで見つかったこと", none: "問題なし",
        delegate: "AIに任せる", delhint: "ターミナルが開き、選んだ AI がこのリポジトリで作業を始めます（あなたのログインのまま）。AI は vxrt のツールでゲームを自分で起動・計測します。コマンドの実行やファイルの変更は、AI ツールの確認画面で承認してください。プレイ中は計測を拒否します。",
        started: "ターミナルで開始しました", notool: "（未インストール）" },
  en: { sub: "Prompts for running and tuning Windows VR games on VXRuntime",
        empty: "No installed games found (scripts/fetch_windows_vr_app.sh installs them)",
        pick: "Pick a game on the left", size: "GB", exe: "Executable", folder: "Folder", ac: "Has anti-cheat (it will not be circumvented): ",
        ai: "Ask an AI coding tool", aihint: "Open Claude Code, Codex or another tool in this repository and paste the prompt. The AI follows AGENTS.md (no DRM circumvention, game files untouched, commits only what it verified).",
        tune: "Tune for this Mac", tunehint: "settings only: fps and resolution", support: "Make it run", supporthint: "implement missing Windows APIs and such in VXRuntime",
        make: "Make the prompt", making: "Looking at the game…", copy: "Copy", copied: "Copied",
        run: "Start", quality: "Quality", qdef: "default", runhint: "Run it in Terminal. It starts SteamVR too; connect from the Vision Pro app. Mac Steam must be signed in.",
        copycmd: "Copy the command", last: "Last run", nolog: "Not run yet", problems: "Found in the log", none: "nothing",
        delegate: "Let the AI do it", delhint: "Opens Terminal with the chosen agent in this repository (your own login). It runs and measures the game itself with the vxrt tools; approve its commands and edits in the agent as usual. Runs are refused while you are playing.",
        started: "Started in Terminal", notool: "(not installed)" },
};
const TOKEN = "__TOKEN__";
let lang = localStorageGet("lang") || "ja", games = [], sel = null, goal = "support", tools = {};
function localStorageGet(k) { try { return localStorage.getItem(k); } catch (e) { return null; } }
function localStorageSet(k, v) { try { localStorage.setItem(k, v); } catch (e) {} }
const $ = id => document.getElementById(id);
function el(tag, attrs, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) { if (k === "onclick") e.onclick = v; else if (k === "class") e.className = v; else e.setAttribute(k, v); }
  for (const k of kids) if (k != null) e.append(k instanceof Node ? k : document.createTextNode(String(k)));
  return e;
}
function toast(t) { const x = $("toast"); x.textContent = t; x.classList.add("on"); setTimeout(() => x.classList.remove("on"), 1400); }
async function copy(text) {
  try { await navigator.clipboard.writeText(text); }
  catch (e) { const t = el("textarea"); t.value = text; document.body.append(t); t.select(); document.execCommand("copy"); t.remove(); }
  toast(S[lang].copied);
}
async function load() {
  $("lang").value = lang; $("sub").textContent = S[lang].sub; document.documentElement.lang = lang;
  games = await (await fetch("/api/games?lang=" + lang)).json();
  tools = await (await fetch("/api/agents")).json();
  renderList();
  if (sel) renderDetail(games.find(g => g.appid === sel.appid)); else $("detail").replaceChildren(el("p", { class: "muted" }, games.length ? S[lang].pick : S[lang].empty));
}
function renderList() {
  $("list").replaceChildren(...games.map(g => el("div", { class: "game" + (sel && sel.appid === g.appid ? " sel" : ""), onclick: () => { sel = g; renderList(); renderDetail(g); } },
    el("div", { class: "name" }, el("span", { class: "dot " + g.status }), g.name),
    el("div", { class: "meta" }, [g.engine.join(", "), g.size_gb + " " + S[lang].size].filter(Boolean).join(" · ")))));
}
function renderDetail(g) {
  if (!g) return;
  const T = S[lang];
  const out = el("textarea", { readonly: "", placeholder: "" });
  const copyBtn = el("button", { disabled: "" }, T.copy);
  copyBtn.onclick = () => copy(out.value);
  const goalSel = el("div", { class: "row" },
    ...["support", "tune"].map(k => {
      const r = el("label", { class: "small" }, el("input", { type: "radio", name: "goal", value: k }), " ", T[k], el("span", { class: "muted" }, " — " + T[k + "hint"]));
      r.firstChild.checked = goal === k; r.firstChild.onchange = () => { goal = k; };
      return el("div", {}, r);
    }));
  const makeBtn = el("button", { class: "primary" }, T.make);
  makeBtn.onclick = async () => {
    makeBtn.disabled = true; out.value = T.making;
    const r = await fetch(`/api/prompt?appid=${g.appid}&goal=${goal}&lang=${lang}`);
    out.value = await r.text(); makeBtn.disabled = false; copyBtn.disabled = !r.ok;
  };
  const delegateRow = el("div", { class: "row", style: "margin-top:12px" }, el("strong", { class: "small" }, T.delegate),
    ...[["claude", "Claude Code"], ["codex", "Codex"]].map(([k, label]) => {
      const b = el("button", tools[k] ? {} : { disabled: "" }, label + (tools[k] ? "" : " " + T.notool));
      b.onclick = async () => {
        b.disabled = true;
        const r = await fetch("/api/delegate", { method: "POST", headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ token: TOKEN, appid: g.appid, goal, lang, tool: k }) });
        toast(r.ok ? T.started : await r.text()); b.disabled = false;
      };
      return b;
    }));
  const q = el("select", {}, el("option", { value: "" }, T.qdef), el("option", { value: "2.5k" }, "2.5k"), el("option", { value: "3k" }, "3k"));
  const cmd = el("code", { class: "cmd" });
  const refresh = async () => { cmd.textContent = await (await fetch(`/api/command?appid=${g.appid}&quality=${q.value}`)).text(); };
  q.onchange = refresh; refresh();
  const lr = g.last_run;
  const last = el("div", { class: "box" }, el("h3", {}, T.last),
    lr ? el("div", {}, el("div", { class: "small muted" }, lr.when + " · " + lr.log),
                       el("div", { class: "small" }, T.problems + ":"),
                       lr.problems.length ? el("ul", { class: "problems" }, ...lr.problems.map(p => el("li", {}, `${p.kind}: ${p.what} ×${p.count}`))) : el("div", { class: "small muted" }, T.none))
       : el("div", { class: "small muted" }, T.nolog));
  $("detail").replaceChildren(...[
    el("h2", {}, el("span", { class: "dot " + g.status }), g.name),
    el("div", { class: "muted small" }, g.note),
    el("div", { class: "chips" }, ...g.engine.map(e => el("span", {}, e))),
    g.anticheat.length ? el("div", { class: "warn small" }, T.ac + g.anticheat.join(", ")) : null,
    el("div", { class: "small muted" }, `${T.exe}: ${g.exe}`), el("div", { class: "small muted path" }, `${T.folder}: ${g.folder}`),
    el("div", { class: "box" }, el("h3", {}, T.ai), el("p", { class: "small muted" }, T.aihint), goalSel,
       el("div", { class: "row", style: "margin:10px 0" }, makeBtn, copyBtn), out,
       delegateRow, el("p", { class: "small muted" }, T.delhint)),
    el("div", { class: "box" }, el("h3", {}, T.run), el("p", { class: "small muted" }, T.runhint),
       el("div", { class: "row", style: "margin-bottom:8px" }, el("span", { class: "small" }, T.quality), q, el("button", { onclick: () => copy(cmd.textContent) }, T.copycmd)), cmd),
    last].filter(Boolean));
}
$("lang").onchange = e => { lang = e.target.value; localStorageSet("lang", lang); load(); };
load();
</script></body></html>
"""

class Handler(BaseHTTPRequestHandler):
    def send(self, code, body, ctype="text/plain; charset=utf-8"):
        data = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        # Only this machine's browser, addressing us by name: a page elsewhere cannot read the answers (DNS rebinding).
        if self.headers.get("Host", "").split(":")[0] not in ("127.0.0.1", "localhost"):
            return self.send(403, "forbidden")
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        lang = q.get("lang", "ja") if q.get("lang") in ("ja", "en") else "ja"
        if u.path == "/":
            return self.send(200, PAGE.replace("__TOKEN__", TOKEN), "text/html; charset=utf-8")
        if u.path == "/api/agents":
            return self.send(200, json.dumps(agents()), "application/json")
        if u.path == "/api/games":
            return self.send(200, json.dumps([game_info(g, lang) for g in find_games()], ensure_ascii=False), "application/json")
        game = next((g for g in find_games() if g["appid"] == q.get("appid")), None)
        if not game:
            return self.send(404, "no such game")
        if u.path == "/api/prompt":
            goal = q.get("goal") if q.get("goal") in ("tune", "support") else "support"
            try:
                return self.send(200, doctor.build_prompt(game["exe"], goal, lang))
            except Exception as e:                          # (shown in the page instead of a prompt)
                return self.send(500, "vxrt doctor: %s" % e)
        if u.path == "/api/command":
            quality = q.get("quality") if q.get("quality") in ("2.5k", "3k") else ""
            return self.send(200, doctor.launch_command(game["exe"], quality))
        self.send(404, "not found")

    def do_POST(self):
        # Starting an agent: only from this page (its token, its origin, JSON so other sites' forms cannot post).
        host = self.headers.get("Host", "")
        origin = self.headers.get("Origin", "")
        if host.split(":")[0] not in ("127.0.0.1", "localhost") or origin not in ("http://" + host,) or \
           not self.headers.get("Content-Type", "").startswith("application/json") or urlparse(self.path).path != "/api/delegate":
            return self.send(403, "forbidden")
        try:
            body = json.loads(self.rfile.read(min(int(self.headers.get("Content-Length", 0)), 10000)))
        except ValueError:
            return self.send(400, "bad request")
        if not secrets.compare_digest(str(body.get("token", "")), TOKEN):
            return self.send(403, "forbidden")
        tool, goal = body.get("tool"), body.get("goal") if body.get("goal") in ("tune", "support") else "support"
        lang = body.get("lang") if body.get("lang") in ("ja", "en") else "ja"
        game = next((g for g in find_games() if g["appid"] == body.get("appid")), None)
        if tool not in ("claude", "codex") or not agents().get(tool) or not game:
            return self.send(400, "no such game or agent")
        try:
            return self.send(200, json.dumps({"prompt": delegate(game, goal, lang, tool)}), "application/json")
        except Exception as e:
            return self.send(500, "could not start: %s" % e)

    def log_message(self, fmt, *args):
        pass

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--no-open", action="store_true", help="do not open the browser")
    a = ap.parse_args()
    for port in (a.port, 0):                                  # (another one running: any free port)
        try:
            server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
            break
        except OSError:
            continue
    url = "http://127.0.0.1:%d/" % server.server_address[1]
    print("VXRT Studio: %s (Ctrl-C to stop)" % url)
    if not a.no_open:
        threading.Timer(0.3, lambda: subprocess.run(["open", url])).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass

if __name__ == "__main__":
    main()
