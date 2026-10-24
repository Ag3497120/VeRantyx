#!/usr/bin/env python3
"""Command Clips, checked (make runtime-tests): build/clip.exe run by the runtime given, read and driven through its
control socket only (tools/vxrt_clip.py): the drawn text, clicks on it, keys, characters, waiting for idle, closing.

    python3 tests/clip_check.py build/vxrt-arm64"""
import os, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
from vxrt_clip import Clip

failures = []
def check(cond, what):
    if not cond: failures.append(what); print("check failed:", what)

def main():
    runtime = sys.argv[1] if len(sys.argv) > 1 else "build/vxrt"
    sock = os.path.join(tempfile.mkdtemp(prefix="vxclip"), "c.sock")
    p = subprocess.Popen([runtime, "build/clip.exe"], env=dict(os.environ, VXRT_CONTROL=sock), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    t0 = time.time()
    try:
        c = Clip.wait_for(sock, 20)
        idle = c.wait_idle(10000)
        check(idle["idle"], f"idle after start: {idle}")
        texts = c.texts()
        check("Count: 0" in texts and "[ Add ]" in texts, f"drawn text after start: {texts}")
        s = c.state()
        w = [w for w in s["windows"] if w["class"] == "ClipTest"]
        check(len(w) == 1 and w[0]["title"] == "Command Clips test", f"the window: {s['windows']}")
        steps = []
        for _ in range(3):
            t = time.time(); c.click_text("[ Add ]"); r = c.wait_idle(5000); steps.append((time.time() - t) * 1e3)
            check(r["idle"], f"idle after a click: {r}")
        check("Count: 3" in c.texts(), f"after three clicks: {c.texts()}")
        c.type("hi there"); c.wait_idle(5000)
        check("Typed: hi there" in c.texts(), f"after typing: {c.texts()}")
        c.key(0x52); c.wait_idle(5000)
        check("Count: 0" in c.texts(), f"after R: {c.texts()}")
        check(not c.click_text("nothing like this")["ok"], "click-text on text that is not there")
        c.close()
        out, _ = p.communicate(timeout=20)
        check(p.returncode == 0 and "count=0 typed=hi there pointer=3/3" in out, f"exit {p.returncode}: {out.strip()[-300:]}")
    except Exception as e:
        failures.append(repr(e)); print("clip:", repr(e))
        p.kill()
    took = (time.time() - t0) * 1e3
    steps_s = ", ".join(f"{m:.1f}" for m in steps) if "steps" in locals() else "-"
    print(f"clip: read, clicked, typed and closed through the control socket (click to idle: {steps_s} ms; all {took:.0f} ms): "
          f"{len(failures)} failed checks -> {'ok' if not failures else 'FAILED'}")
    sys.exit(1 if failures else 0)

if __name__ == "__main__":
    main()
