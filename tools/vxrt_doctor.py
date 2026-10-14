#!/usr/bin/env python3
"""vxrt doctor: looks at a Windows game (without running it) and at the last VXRuntime logs, and writes a prompt
for an AI coding tool (Claude Code, Codex, Gemini CLI, Cursor...) to tune the game for this Mac or to make it run.

    tools/vxrt_doctor.py "<game folder or .exe>" [--goal tune|support] [--lang ja|en] [--log logs/vxrt-alyx.log]
                         [--out prompt.md] [--copy]

The prompt is printed (or written to --out, or copied to the clipboard with --copy); paste it into your AI tool,
started in this repository. Nothing is sent anywhere by this script. The rules the AI follows are in AGENTS.md.
"""
import argparse, os, re, struct, subprocess, sys
from collections import defaultdict

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VXRT = os.path.join(REPO, "VXRuntime", "build", "vxrt")

# ---- PE imports -----------------------------------------------------------------------------------------------------
def pe_imports(path):
    """{dll: [function or #ordinal]} and the machine type of a PE file, or (None, None) if it is not one."""
    try:
        d = open(path, "rb").read()
    except OSError:
        return None, None
    if d[:2] != b"MZ" or len(d) < 0x40:
        return None, None
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    if d[pe:pe + 4] != b"PE\0\0":
        return None, None
    machine, nsec = struct.unpack_from("<HH", d, pe + 4)
    opt_size = struct.unpack_from("<H", d, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", d, opt)[0]
    dirs = opt + (112 if magic == 0x20B else 96)
    imp_rva, _ = struct.unpack_from("<II", d, dirs + 8)
    secs = []
    for i in range(nsec):
        o = opt + opt_size + 40 * i
        vs, va, rs, ro = struct.unpack_from("<IIII", d, o + 8)
        secs.append((va, max(vs, rs), ro))
    def off(rva):
        for va, size, ro in secs:
            if va <= rva < va + size:
                return rva - va + ro
        return None
    def cstr(o):
        e = d.find(b"\0", o)
        return d[o:e].decode("latin-1")
    out = defaultdict(list)
    o = off(imp_rva) if imp_rva else None
    wide = magic == 0x20B
    while o is not None and o + 20 <= len(d):
        ilt, _, _, name, iat = struct.unpack_from("<IIIII", d, o)
        if not name:
            break
        dll = cstr(off(name)).lower()
        t = off(ilt or iat)
        while t is not None:
            v = struct.unpack_from("<Q" if wide else "<I", d, t)[0]
            if not v:
                break
            if v >> (63 if wide else 31):
                out[dll].append("#%d" % (v & 0xFFFF))
            else:
                h = off(v & 0x7FFFFFFF)
                out[dll].append(cstr(h + 2) if h is not None else "?")
            t += 8 if wide else 4
        o += 20
    # Delay-loaded imports (Unity takes Direct3D this way): descriptors of 8 dwords, names as above.
    dl_rva = struct.unpack_from("<I", d, dirs + 13 * 8)[0] if dirs + 13 * 8 + 4 <= len(d) else 0
    o = off(dl_rva) if dl_rva else None
    while o is not None and o + 32 <= len(d):
        attrs, name, _, _, int_rva = struct.unpack_from("<IIIII", d, o)
        if not name:
            break
        base = 0 if attrs & 1 else struct.unpack_from("<Q" if wide else "<I", d, opt + 24)[0]   # (old descriptors hold VAs)
        nm = off(name - base)
        if nm is None:
            break
        dll = cstr(nm).lower()
        t = off(int_rva - base)
        while t is not None:
            v = struct.unpack_from("<Q" if wide else "<I", d, t)[0]
            if not v:
                break
            if v >> (63 if wide else 31):
                out[dll].append("#%d" % (v & 0xFFFF))
            else:
                h = off((v & 0x7FFFFFFF) - base)
                out[dll].append(cstr(h + 2) if h is not None else "?")
            t += 8 if wide else 4
        o += 32
    return dict(out), machine

def wine_builtin(path):
    """Wine's own DLLs (marked after the DOS header) need Wine: VXRuntime passes over them (loader.c wine_builtin)."""
    try:
        with open(path, "rb") as f: return f.read(0x60)[0x40:0x50] == b"Wine builtin DLL"
    except OSError:
        return False

def canonical(dll):
    """API sets resolve as VXRuntime's loader does (loader.c canonical_dll)."""
    if dll.startswith("api-ms-win-crt-"): return "ucrtbase.dll"
    if dll.startswith("api-ms-win-core-"): return "kernel32.dll"
    if dll.startswith(("api-ms-win-security-", "api-ms-win-eventing-")): return "advapi32.dll"
    if dll.startswith(("api-ms-win-ntuser-", "ext-ms-win-ntuser-")): return "user32.dll"
    if dll.startswith("api-ms-win-"): return "kernelbase.dll"
    return dll

def runtime_exports():
    if not os.path.exists(VXRT):
        subprocess.run(["make", "-C", os.path.join(REPO, "VXRuntime"), "build/vxrt"], stdout=subprocess.DEVNULL)
    r = subprocess.run([VXRT, "--list-exports"], capture_output=True, text=True)
    ex = defaultdict(set)
    for line in r.stdout.splitlines():
        if "\t" in line:
            dll, name = line.split("\t", 1)
            ex[dll.lower()].add(name)
    return ex

# ---- what the game is -----------------------------------------------------------------------------------------------
ANTI_CHEAT = {"easyanticheat": "Easy Anti-Cheat", "battleye": "BattlEye", "beservice": "BattlEye", "vgk": "Vanguard",
              "xigncode": "XIGNCODE3", "nprotect": "nProtect GameGuard", "gameguard": "nProtect GameGuard",
              "equ8": "EQU8", "anticheatexpert": "ACE", "denuvo": "Denuvo Anti-Cheat"}

def scan_game(root):
    exes, dlls, markers = [], {}, set()
    for dirpath, dirnames, files in os.walk(root):
        dirnames[:] = [x for x in dirnames if not x.startswith(".")]
        for f in files:
            low = f.lower()
            full = os.path.join(dirpath, f)
            for key, name in ANTI_CHEAT.items():
                if key in low or key in dirpath.lower():
                    markers.add("anticheat:" + name)
            if low == "unityplayer.dll": markers.add("engine:Unity")
            if low == "gameassembly.dll": markers.add("engine:Unity (IL2CPP)")
            if low.endswith("-win64-shipping.exe") or low == "ue4prereqsetup_x64.exe": markers.add("engine:Unreal Engine")
            if low == "engine2.dll": markers.add("engine:Source 2")
            if low == "openvr_api.dll": markers.add("vr:OpenVR")
            if low == "openxr_loader.dll": markers.add("vr:OpenXR")
            if low.endswith(".exe"):
                exes.append(full)
            elif low.endswith(".dll"):
                dlls.setdefault(low, full)
    return exes, dlls, markers

HELPERS = ("crashhandler", "crashpad", "crash_handler", "unitycrashhandler", "setup", "redist", "vcredist", "dxsetup",
           "uninst", "launcher_helper", "webhelper", "easyanticheat", "beservice", "prereq",
           "tb_streamer", "vconsole", "resourcecompiler", "injector")   # (Source 2's tools; VeRantyx's injector)

def steam_name(root):
    return steam_app(root)[1]

def steam_app(root):
    """(app id, name) from a Steam app manifest: in a Steam library above the game (.../steamapps/common/<folder>,
    matched by its install folder) or next to it (steamcmd's force_install_dir)."""
    ap = os.path.abspath(root)
    places = []
    if "/steamapps/common/" in ap:
        lib, rest = ap.split("/steamapps/common/", 1)
        places.append((lib, rest.split("/")[0]))
    places.append((ap, None))
    for lib, folder in places:
        sa = os.path.join(lib, "steamapps")
        if not os.path.isdir(sa):
            continue
        found = {}
        for f in os.listdir(sa):
            m = re.match(r"appmanifest_(\d+)\.acf$", f)
            if m and m.group(1) != "228980":                 # (Steamworks Common Redistributables)
                text = open(os.path.join(sa, f), errors="replace").read()
                n, d = re.search(r'"name"\s+"([^"]+)"', text), re.search(r'"installdir"\s+"([^"]+)"', text)
                found[m.group(1)] = (n.group(1) if n else None, d.group(1) if d else None)
        match = [i for i, (_, d) in found.items() if folder and d and d.lower() == folder.lower()]
        if match:
            return match[0], found[match[0]][0]
        if not folder and found:
            i = next(iter(found)); return i, found[i][0]
    return None, None

def pick_exe(exes, name):
    """The game's own executable: not a helper, the one named like the game if there is one, else the largest."""
    games = [e for e in exes if not any(h in os.path.basename(e).lower() for h in HELPERS)] or exes
    key = re.sub(r"[^a-z0-9]", "", name.lower())
    named = [e for e in games if re.sub(r"[^a-z0-9]", "", os.path.splitext(os.path.basename(e))[0].lower()) in (key, key + "64", key + "win64")]
    pool = named or games
    return max(pool, key=os.path.getsize) if pool else None

def closure(main_exe, game_dlls):
    """The main executable, the DLLs beside it and those it imports (recursively) from the game's folder: what the
    game loads, as far as static imports and its own folder tell (DLLs loaded later by path are not seen)."""
    todo = [main_exe] + [os.path.join(os.path.dirname(main_exe), f) for f in os.listdir(os.path.dirname(main_exe)) if f.lower().endswith(".dll")]
    seen, out = set(), []
    while todo:
        p = todo.pop()
        if p.lower() in seen: continue
        seen.add(p.lower()); out.append(p)
        imports, _ = pe_imports(p)
        for dll in (imports or {}):
            if dll in game_dlls: todo.append(game_dlls[dll])
    return out

def machine_info():
    def sh(*a):
        try: return subprocess.run(a, capture_output=True, text=True).stdout.strip()
        except OSError: return ""
    chip = sh("sysctl", "-n", "machdep.cpu.brand_string")
    mem = sh("sysctl", "-n", "hw.memsize")
    gpu = re.search(r"Total Number of Cores: (\d+)", sh("system_profiler", "SPDisplaysDataType"))
    os_ver = sh("sw_vers", "-productVersion")
    return {"chip": chip, "memory_gb": int(mem) // 2**30 if mem.isdigit() else "?",
            "gpu_cores": gpu.group(1) if gpu else "?", "macos": os_ver}

# ---- the last run's log -----------------------------------------------------------------------------------------------
LOG_PATTERNS = [
    ("unimplemented call", r"called an unimplemented function: (\S+)"),
    ("missing DLL", r"DLL (\S+) not found"),
    ("pipeline state failed", r"pipeline state failed: (.*)"),
    ("shader did not compile", r"(?:shader|variant).* did not compile: (.{0,160})"),
    ("unhandled exception", r"(unhandled exception.{0,160}|unwind did not reach.{0,80})"),
    ("message box", r"MessageBox: (.{0,200})"),
    ("exit", r"(ExitProcess\(\d+\) from \S+)"),
    ("audio gaps", r"audio: (\d+ gaps of silence.{0,60})"),
    ("not supported", r"(\S+ is not supported.{0,80}|not implemented: .{0,120})"),
]

def read_log(path, exe_name=None):
    """What went wrong in a log, by kind. Lines of other processes the game started (SteamVR's vrmonitor and
    vrwebhelper, crash handlers: "[vxrt <name>:<pid>]") are left out when exe_name is given."""
    if not path or not os.path.exists(path):
        return {}
    found = defaultdict(lambda: defaultdict(int))
    with open(path, errors="replace") as f:
        for line in f:
            other = re.match(r"\[vxrt ([^\]:]+):\d+\]", line)
            if other and exe_name and other.group(1).lower() != exe_name.lower():
                continue
            for label, pat in LOG_PATTERNS:
                m = re.search(pat, line)
                if m:
                    found[label][m.group(1).strip()[:200]] += 1
    return found

def proxy_fps(root_c):
    path = os.path.join(root_c, "vr_openvr_capture_proxy.log")
    if not os.path.exists(path):
        return None
    fps = [float(m.group(1)) for m in re.finditer(r"submit_fps=([\d.]+)", open(path, errors="replace").read()[-200000:])]
    return fps[-24:] if fps else None

# ---- the prompt ---------------------------------------------------------------------------------------------------------
TEXT = {
    "ja": {
        "intro_tune": "このリポジトリ（VeRantyx / VXRuntime）で、次の Windows VR ゲームをこの Mac 向けに最適化してください。設定の調整だけで、ランタイムのコードは変えません。",
        "intro_support": "このリポジトリ（VeRantyx / VXRuntime）で、次の Windows ゲームが VXRuntime で動くようにしてください。足りない Windows API の実装や Direct3D の変換の修正など、VXRuntime 側（VXRuntime/src）の変更が対象です。",
        "rules": "まず AGENTS.md を読み、その規則と手順に従ってください（DRM・アンチチートの回避、ゲームファイルの改変や再配布はしない／変更後は make -C VXRuntime check を通す／計測は AGENTS.md の方法で行い、数値で報告する）。",
        "facts": "## 調べた事実（vxrt doctor が自動で集めたもの）",
        "machine": "マシン", "game": "ゲーム", "engine": "エンジン・VR", "apis": "グラフィックスなど",
        "missing": "VXRuntime にまだない関数（ゲームの EXE/DLL が使うもの。呼ばれたときだけ止まるので、ログの「unimplemented call」が優先）",
        "nodll": "見つからない DLL（ゲームフォルダにもランタイムにもない）",
        "log": "前回の実行ログ", "fps": "直近の fps（ゲームの Submit 回数/秒）",
        "task_tune": "## お願いしたいこと\n1. 計測は tools/vxrt_run.py {harness}（または MCP ツール vxrt の run_game）で行う。ヘッドセットなしで別フォルダに起動し、観測・導出・推測・不明に分けた結果（runs/<run_id>/result.json）を返す。前提条件を満たさず拒否されたら、その理由を報告して止まる（ユーザーのゲームを止めない）。\n2. 変えてよい項目（AGENTS.md の「調整できる設定」）だけを使い、目標（表示 Hz に張り付く fps、なるべく高い解像度）に近づける。設定の比較は交互に複数回（A B B A）。\n3. 試した設定と結果を表にし、各数値に run_id と観測 ID（o#）を付ける。一番良い組み合わせを起動コマンド（環境変数・ゲームの引数）として示す。推測（i#）は推測として書く。",
        "task_support": "## お願いしたいこと\n1. まず tools/vxrt_run.py {harness}（または MCP ツール vxrt の run_game）で今の状態を測る。結果の first_problem・unimplemented_calls・例外数から、ゲームの起動や描画を止めているものを特定する（上の「ない関数」は静的な一覧で、呼ばれるとは限らない）。\n2. VXRuntime/src に実装・修正を加え、テストを足して make -C VXRuntime check を通す。\n3. もう一度 vxrt_run.py で測り、何が動いて何が残っているかを run_id と観測 ID（o#）・導出（d#）を付けて報告する。make -C VXRuntime vr-check で既存のゲームが壊れていないことも確かめる。コミットは確かめたものだけ。",
        "anticheat": "注意：このゲームにはアンチチート（{}）があります。回避は行わず、オフラインで遊べる部分があるかの確認にとどめてください。",
        "none": "なし", "others": "ほかの実行ファイル", "launch": "起動コマンド（SteamVR と一緒に起動する）",
    },
    "en": {
        "intro_tune": "In this repository (VeRantyx / VXRuntime), tune the following Windows VR game for this Mac. Settings only: do not change the runtime's code.",
        "intro_support": "In this repository (VeRantyx / VXRuntime), make the following Windows game run on VXRuntime: implementing missing Windows APIs, fixing the Direct3D translation and so on, in VXRuntime/src.",
        "rules": "Read AGENTS.md first and follow its rules and procedures (no DRM or anti-cheat circumvention, no modifying or redistributing game files; make -C VXRuntime check must pass after changes; measure as AGENTS.md describes and report numbers).",
        "facts": "## Facts (collected by vxrt doctor)",
        "machine": "Machine", "game": "Game", "engine": "Engine / VR", "apis": "Graphics and others",
        "missing": "Functions VXRuntime does not have yet (used by the game's EXE/DLLs; they only stop a program when called, so the log's 'unimplemented call' entries come first)",
        "nodll": "DLLs not found (neither in the game folder nor in the runtime)",
        "log": "Last run's log", "fps": "Recent fps (the game's Submits per second)",
        "task_tune": "## What to do\n1. Measure with tools/vxrt_run.py {harness} (or the vxrt MCP tool run_game): it starts the game without a headset in roots of its own and returns evidence split into observed / derived / inferred / unknown (runs/<run_id>/result.json). If it refuses, report the reason and stop (never stop the user's game).\n2. Using only the settings AGENTS.md lists as tunable, get as close as you can to the goal (fps at the display rate, as high a resolution as possible). Compare settings in alternating runs (A B B A).\n3. Report the settings tried as a table, each number with its run_id and observation id (o#), and give the best combination as a launch command. Mark inferences (i#) as such.",
        "task_support": "## What to do\n1. First measure the current state with tools/vxrt_run.py {harness} (or the vxrt MCP tool run_game). Use its first_problem, unimplemented_calls and exception counts to find what stops the game (the missing functions above are a static list: not all are called).\n2. Implement or fix it in VXRuntime/src, add tests, and get make -C VXRuntime check to pass.\n3. Measure again with vxrt_run.py and report what works and what is left, citing run_ids, observation ids (o#) and derived claims (d#). Check with make -C VXRuntime vr-check that the games that ran still do. Commit only what you have verified.",
        "anticheat": "Note: this game has anti-cheat ({}). Do not circumvent it; only check whether parts of it can be played offline.",
        "none": "none", "others": "other executables", "launch": "Launch command (starts SteamVR too)",
    },
}

GRAPHICS = {"d3d11.dll": "Direct3D 11", "d3d12.dll": "Direct3D 12 (VXRuntime: not supported)", "d3d9.dll": "Direct3D 9",
            "dxgi.dll": "DXGI", "opengl32.dll": "OpenGL", "vulkan-1.dll": "Vulkan (VXRuntime: not supported)",
            "xinput1_3.dll": "XInput", "xinput1_4.dll": "XInput", "xaudio2_9.dll": "XAudio2", "openvr_api.dll": "OpenVR",
            "wininet.dll": "WinINet", "winhttp.dll": "WinHTTP", "ws2_32.dll": "Winsock"}

DEFAULT_ROOT = os.path.expanduser("~/Verantyx_VR_Drive/vxrt_root/drive_c")
DEFAULT_DLL_PATH = os.path.expanduser("~/Verantyx_VR_Drive/SteamVR_Prefix_Wine11_Test/drive_c/windows/system32")
_exports = None

def find_log(exe_name):
    """The newest logs/vxrt-*.log that mentions the game's executable."""
    d = os.path.join(REPO, "logs")
    logs = sorted((os.path.join(d, f) for f in os.listdir(d) if f.startswith("vxrt-") and f.endswith(".log")), key=os.path.getmtime, reverse=True) if os.path.isdir(d) else []
    stem = re.escape(os.path.splitext(exe_name.lower())[0])
    return next((p for p in logs if re.search(r"\b%s\b" % stem, open(p, errors="replace").read(400000).lower())), None)

def launch_command(exe, quality=""):
    """How the game is started on VXRuntime (scripts/), from the repository; quality: a VXRT_VR_QUALITY preset."""
    env = f"VXRT_VR_QUALITY={quality} " if quality else ""
    if os.path.basename(exe).lower() == "hlvr.exe":            # Half-Life: Alyx has a launcher of its own
        return f'cd "{REPO}" && {env}scripts/launch_vxrt_alyx.sh'
    return f'cd "{REPO}" && {env}scripts/launch_vxrt_game.sh "{os.path.abspath(exe)}"'

def analyze(game, log=None, root_c=DEFAULT_ROOT, dll_path=DEFAULT_DLL_PATH):
    """The facts about a game (its folder or .exe) as a dict: what build_prompt() writes, and what the MCP server
    (tools/vxrt_mcp.py) returns. Raises ValueError when there is no executable."""
    global _exports
    root = game if os.path.isdir(game) else os.path.dirname(game)
    exes, game_dlls, markers = scan_game(root)
    appid, name = steam_app(root)
    if not name and "/common/" in os.path.abspath(root):    # a Steam library: steamapps/common/<game>/...
        name = os.path.abspath(root).split("/common/", 1)[1].split("/")[0]
    main_exe = game if game.lower().endswith(".exe") else pick_exe(exes, name or os.path.basename(os.path.normpath(root)))
    if not main_exe:
        raise ValueError("No .exe found in %s" % root)
    disk_dlls = {f.lower() for f in os.listdir(dll_path) if not wine_builtin(os.path.join(dll_path, f))} if os.path.isdir(dll_path) else set()
    if _exports is None:
        _exports = runtime_exports()
    exports = _exports

    missing, nodll, apis = defaultdict(set), set(), set()
    for path in closure(main_exe, game_dlls):
        imports, machine = pe_imports(path)
        if not imports or machine != 0x8664:          # (x86-64 only: 32-bit helpers and other files are left out)
            continue
        for dll, funcs in imports.items():
            if dll in GRAPHICS: apis.add(GRAPHICS[dll])
            c = canonical(dll)
            if dll in game_dlls:                         # shipped with the game: looked at as a file of its own
                continue
            if c in exports:
                for fn in funcs:
                    if fn not in exports[c] and not fn.startswith("#"):
                        missing[c].add(fn)
            elif dll in disk_dlls:                       # a real Windows DLL loaded from the DLL path
                continue
            else:
                nodll.add(dll)
    if any(m.startswith("engine:Unity") for m in markers) and not any(x.startswith("Direct3D") for x in apis):
        apis.add("Direct3D 11 (Unity's default on Windows, loaded at run time)")

    log = log or find_log(os.path.basename(main_exe))
    found = read_log(log, os.path.basename(main_exe))
    proxy = os.path.join(root_c, "vr_openvr_capture_proxy.log")
    fps = proxy_fps(root_c) if log and os.path.exists(proxy) and abs(os.path.getmtime(log) - os.path.getmtime(proxy)) < 600 else None
    others = [os.path.relpath(e, root) for e in exes if e != main_exe and not any(h in os.path.basename(e).lower() for h in HELPERS)]
    return {
        "name": name or os.path.basename(os.path.normpath(root)), "folder": root, "exe": main_exe,
        "other_exes": others, "machine": machine_info(),
        "engine": sorted(m.split(":", 1)[1] for m in markers if m.startswith(("engine:", "vr:"))),
        "anticheat": sorted({m.split(":", 1)[1] for m in markers if m.startswith("anticheat:")}),
        "appid": appid, "apis": sorted(apis), "launch": launch_command(main_exe),
        "harness": f"tools/vxrt_run.py {appid or '<game>'}",
        # Static: imported, so possibly called; a stub stops the program only when it is called.
        "missing_functions": {dll: sorted(v) for dll, v in sorted(missing.items(), key=lambda kv: -len(kv[1]))},
        "missing_dlls": sorted(nodll),
        "log": log, "log_problems": {k: dict(sorted(v.items(), key=lambda kv: -kv[1])[:8]) for k, v in found.items()},
        "recent_submit_fps": fps[-12:] if fps else None,
    }

def build_prompt(game, goal="support", lang="ja", log=None, root_c=DEFAULT_ROOT, dll_path=DEFAULT_DLL_PATH):
    """The prompt for a game (its folder or .exe), as main() prints it. vxrt_studio.py calls this too."""
    T = TEXT[lang]
    f = analyze(game, log, root_c, dll_path)
    L = [T["intro_" + goal], T["rules"]]
    if f["anticheat"]: L.append("\n" + T["anticheat"].format(", ".join(f["anticheat"])))
    L.append("\n" + T["facts"])
    mi = f["machine"]
    L.append(f"- {T['machine']}: {mi['chip']}, GPU {mi['gpu_cores']} cores, {mi['memory_gb']} GB, macOS {mi['macos']}")
    L.append(f"- {T['game']}: {f['name']} — {os.path.relpath(f['exe'], f['folder'])}" + (f" ({T['others']}: {', '.join(f['other_exes'][:6])})" if f["other_exes"] else ""))
    L.append(f"- {T['engine']}: {', '.join(f['engine']) or T['none']}")
    L.append(f"- {T['apis']}: {', '.join(f['apis']) or T['none']}")
    L.append(f"- {T['launch']}: {f['launch']}")
    L.append(f"- {T['missing']}: {sum(len(v) for v in f['missing_functions'].values())}")
    for dll, names in f["missing_functions"].items() if goal == "support" else ():
        L.append(f"  - {dll} ({len(names)}): {', '.join(names[:40])}{' …' if len(names) > 40 else ''}")
    L.append(f"- {T['nodll']}: {', '.join(f['missing_dlls']) or T['none']}")
    log = f["log"]
    if log:
        L.append(f"- {T['log']} ({os.path.relpath(log, REPO) if log.startswith(REPO) else log}):")
        if not f["log_problems"]: L.append(f"  - {T['none']}")
        for label, items in f["log_problems"].items():
            L.append(f"  - {label}: " + "; ".join(f"{k} ×{n}" for k, n in items.items()))
    if f["recent_submit_fps"]:
        L.append(f"- {T['fps']}: {', '.join(f'{x:.0f}' for x in f['recent_submit_fps'])}")
    L.append("\n" + T["task_" + goal].format(harness=f["harness"].split(" ", 1)[1]))
    return "\n".join(L) + "\n"

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game", help="the game's folder or its .exe")
    ap.add_argument("--goal", choices=["tune", "support"], default="support")
    ap.add_argument("--lang", choices=["ja", "en"], default="ja")
    ap.add_argument("--log", help="a VXRuntime log of the game (default: the newest logs/vxrt-*.log)")
    ap.add_argument("--root", default=DEFAULT_ROOT, help="VXRT_ROOT/drive_c of the last run")
    ap.add_argument("--dll-path", default=DEFAULT_DLL_PATH, help="where Windows DLLs the runtime loads from disk live (VXRT_DLL_PATH)")
    ap.add_argument("--out"); ap.add_argument("--copy", action="store_true")
    a = ap.parse_args()
    try:
        prompt = build_prompt(a.game, a.goal, a.lang, a.log, a.root, a.dll_path)
    except ValueError as e:
        sys.exit(str(e))
    if a.out:
        open(a.out, "w").write(prompt)
    if a.copy:
        subprocess.run(["pbcopy"], input=prompt.encode())
    if not a.out:
        sys.stdout.write(prompt)
    if a.copy:
        print("\n(copied to the clipboard)", file=sys.stderr)

if __name__ == "__main__":
    main()
