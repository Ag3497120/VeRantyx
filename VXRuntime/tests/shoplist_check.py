#!/usr/bin/env python3
"""The shopping list (tests/shoplist.c) operated through Command Clips only, a fixed script (make runtime-tests): type
into the field, add with the button and with Enter, delete with its confirmation window (No, then Yes), save, close;
the program's own output and shoplist.txt say whether the task was done.

    python3 tests/shoplist_check.py build/vxrt-arm64"""
import os, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
from vxrt_clip import Clip

failures = []
def check(cond, what):
    if not cond: failures.append(what); print("check failed:", what)

def window(c, cls):
    return next((w for w in c.state()["windows"] if w["class"] == cls), None)

def box_of(w, text):
    t = next(t for t in w["texts"] if t["text"] == text)
    return t["box"]

def main():
    runtime = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/vxrt")
    work = tempfile.mkdtemp(prefix="vxshop")
    sock = os.path.join(work, "c.sock")
    exe = os.path.abspath("build/shoplist.exe")
    env = dict(os.environ, VXRT_CONTROL=sock)
    if "VXRT_ROOT" in env: env["VXRT_ROOT"] = os.path.abspath(env["VXRT_ROOT"])
    p = subprocess.Popen([runtime, exe], cwd=work, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    actions = 0
    try:
        c = Clip.wait_for(sock, 20); c.wait_idle(10000)
        main_w = window(c, "ShopList")
        x, y, w, h = box_of(main_w, "Name: ")
        c.click(x + 5, y + h // 2); c.type("apples"); c.key(13); c.wait_idle(); actions += 3      # Enter adds
        c.type("bread"); c.click_text("[ Add ]"); c.wait_idle(); actions += 2
        check(any(t == "Name: " for t in c.texts()), f"a click elsewhere takes the focus from the field: {c.texts()}")
        c.click(x + 5, y + h // 2); c.type("milk"); c.key(13); c.wait_idle(); actions += 3      # (into the field again)
        texts = c.texts()
        check(["1. apples", "2. bread", "3. milk"] == [t for t in texts if t[:1].isdigit()], f"three rows: {texts}")
        row = box_of(window(c, "ShopList"), "2. bread")
        xs = [t["box"] for t in window(c, "ShopList")["texts"] if t["text"] == "[ x ]"]
        x_of_bread = next(b for b in xs if abs(b[1] - row[1]) < 8)
        c.click(x_of_bread[0] + 5, x_of_bread[1] + 5); c.wait_idle(); actions += 1
        ask = window(c, "ShopAsk")
        check(ask and any(t["text"] == "Delete bread?" for t in ask["texts"]), f"the question: {ask}")
        c.click_text("[ No ]"); c.wait_idle(); actions += 1
        check(window(c, "ShopAsk") is None and "Status: Kept it" in c.texts(), f"after No: {c.texts()}")
        c.click(x_of_bread[0] + 5, x_of_bread[1] + 5); c.wait_idle(); c.click_text("[ Yes ]"); c.wait_idle(); actions += 2
        check("Status: Deleted bread" in c.texts() and "2. milk" in c.texts(), f"after Yes: {c.texts()}")
        c.click_text("[ Save ]"); c.wait_idle(); actions += 1
        check("Status: Saved 2 items to shoplist.txt" in c.texts(), f"after Save: {c.texts()}")
        c.close(); actions += 1
        out, _ = p.communicate(timeout=20)
        check(p.returncode == 0 and "items=apples,milk saved=1" in out, f"exit {p.returncode}: {out.strip()[-300:]}")
        saved = open(os.path.join(work, "shoplist.txt")).read() if os.path.exists(os.path.join(work, "shoplist.txt")) else None
        check(saved == "apples\nmilk\n", f"shoplist.txt: {saved!r}")
    except Exception as e:
        failures.append(repr(e)); print("shoplist:", repr(e)); p.kill()
    print(f"shoplist: a task done through Command Clips alone ({actions} actions, two windows, a file saved): "
          f"{len(failures)} failed checks -> {'ok' if not failures else 'FAILED'}")
    sys.exit(1 if failures else 0)

if __name__ == "__main__":
    main()
