#!/usr/bin/env python3
"""Input through child windows, checked (make check): build/input_route.exe driven through Command Clips, which acts on
the top-level window only, as the Mac does: clicks land in the child window under the point (past one that answers
HTTRANSPARENT, in that child's coordinates, with the mouse captured on the press), keys, characters, Ctrl+V/Ctrl+C with
the clipboard, an input-method composition and the wheel go to the focus window. Made-up text only; the clipboard is a
pasteboard of the test's own (VXRT_PASTEBOARD), never the user's.

    python3 tests/input_route_check.py build/vxrt-arm64"""
import os, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
from vxrt_clip import Clip

failures = []
def check(cond, what):
    if not cond: failures.append(what); print("check failed:", what)

def main():
    runtime = sys.argv[1] if len(sys.argv) > 1 else "build/vxrt"
    sock = os.path.join(tempfile.mkdtemp(prefix="vxroute"), "c.sock")
    env = dict(os.environ, VXRT_CONTROL=sock, VXRT_PASTEBOARD="vxrt-input-route-check")
    p = subprocess.Popen([runtime, "build/input_route.exe"], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    t0 = time.time()
    try:
        c = Clip.wait_for(sock, 20)
        check(c.wait_idle(10000)["idle"], "idle after start")
        top = next(w["hwnd"] for w in c.state()["windows"] if w["class"] == "InputRoute")
        def do(req):
            r = c.ask(req); check(r.get("ok"), f"{req}: {r}"); c.wait_idle(5000); return r
        do(f"click {top} 20 30")                     # A, in A's coordinates
        do(f"click {top} 200 50")                    # B answers HTTRANSPARENT: C, beneath it
        do(f"click {top} 210 150")                   # C
        do(f"type {top} ab")                         # to A, which has the focus since its click
        do("clipboard-set made-up paste")
        do(f"key {top} 0x11+0x56")                   # Ctrl+V: A pastes
        do(f"key {top} 0x53")                        # S: A sends itself Z (SendInput); TranslateMessage makes 'z'
        do(f"ime {top} テスト")                      # A reads the result with ImmGetCompositionString
        do(f"wheel {top} 20 30 -120")                # to A, which leaves it to DefWindowProc: the parent gets it
        do(f"key {top} 0x11+0x43")                   # Ctrl+C: A copies made-up text
        r = c.ask("clipboard")
        check(r.get("text") == "made-up copy", f"the clipboard after Ctrl+C: {r}")
        do(f"key {top} 0x09")                        # Tab: the focus to C
        do(f"ime {top} 日本")                         # C leaves it to DefWindowProc: WM_IME_CHAR, then WM_CHAR
        check(c.ask(f"close {top}").get("ok"), "close")
        out, _ = p.communicate(timeout=20)
        lines = dict(l.split(": ", 1) for l in out.splitlines() if ": " in l and l.split(": ", 1)[0] in ("A", "B", "C", "top"))
        check(lines.get("A", "").startswith("down 20,30; up=1 ctrl+v=1 typed=[abmade-up paste] ime=[テスト] sendinput=3"), f"A: {lines.get('A')}")
        check(lines.get("B", "") == "", f"B (transparent) got: {lines.get('B')}")
        check(lines.get("C", "") == "down 50,50;down 60,150; typed=[日本]", f"C: {lines.get('C')}")
        check(lines.get("top", "") == "wheel -120 from A;", f"top: {lines.get('top')}")
        check(p.returncode == 0, f"exit {p.returncode}: {out.strip()[-400:]}")
    except Exception as e:
        failures.append(repr(e)); print("input_route:", repr(e))
        p.kill()
        print((p.communicate()[0] or "")[-1500:])
    took = (time.time() - t0) * 1e3
    print(f"input_route: clicks to the child under the point (past HTTRANSPARENT, captured), keys, characters, clipboard, "
          f"composition and wheel to the focus ({took:.0f} ms): {len(failures)} failed checks -> {'FAILED' if failures else 'ok'}")
    sys.exit(1 if failures else 0)

main()
