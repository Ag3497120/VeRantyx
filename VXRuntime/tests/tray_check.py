#!/usr/bin/env python3
"""The notification area, checked (make runtime-tests): build/tray.exe (tests/tray.c) puts two icons in the menu bar
through Shell_NotifyIcon; this clicks them through its control socket only (Command Clips: tray, tray-click, menu,
menu-pick), as a person would click the status items and the menus, and reads back what the program received.

    python3 tests/tray_check.py build/vxrt-arm64"""
import os, shutil, struct, subprocess, sys, tempfile, time, zlib
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
from vxrt_clip import Clip

failures = []
def check(cond, what):
    if not cond: failures.append(what); print("check failed:", what)

def read_png(path):
    """Width, height and a pixel(x, y) -> (r, g, b, a) of an 8-bit RGBA PNG (what AppKit writes)."""
    d = open(path, "rb").read()
    pos, idat, w = 8, b"", 0
    while pos < len(d):
        n, kind = struct.unpack(">I4s", d[pos:pos + 8])
        body = d[pos + 8:pos + 8 + n]
        if kind == b"IHDR": w, h, depth, color = struct.unpack(">IIBB", body[:10])
        if kind == b"IDAT": idat += body
        pos += 12 + n
    if not w or depth != 8 or color != 6: return 0, 0, lambda x, y: (0, 0, 0, 0)
    raw, stride, rows, prev = zlib.decompress(idat), w * 4, [], bytearray(w * 4)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a, b, c = line[i - 4] if i >= 4 else 0, prev[i], prev[i - 4] if i >= 4 else 0
            p = a + b - c
            pred = [0, a, b, (a + b) // 2, a if abs(p - a) <= abs(p - b) and abs(p - a) <= abs(p - c) else b if abs(p - b) <= abs(p - c) else c][f]
            line[i] = (line[i] + pred) & 0xff
        rows.append(line); prev = line
    return w, h, lambda x, y: tuple(rows[y][x * 4:x * 4 + 4])

def main():
    runtime = sys.argv[1] if len(sys.argv) > 1 else "build/vxrt"
    sock = os.path.join(tempfile.mkdtemp(prefix="vxtray"), "c.sock")
    ico = "Z:" + os.path.abspath("build/tray.ico").replace("/", "\\")
    p = subprocess.Popen([runtime, "build/tray.exe", ico], env=dict(os.environ, VXRT_CONTROL=sock), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    t0 = time.time()
    out = ""
    try:
        c = Clip.wait_for(sock, 20)
        icons = []
        for k in range(400):                                      # (the program adds its icons after its own checks, and
            icons = c.ask("tray")["icons"]                        #  the menu bar places them in about half a second)
            if len(icons) == 2 and (all(i["shown"] for i in icons) or k >= 160): break
            time.sleep(0.025)
        check(len(icons) == 2, f"two icons: {icons}")
        first, second = icons[0], icons[1]
        main_hwnd = first["hwnd"]
        check(first["id"] == 7 and first["version"] == 4 and first["tip"] == "Tray test: ready" and first["image"] == [16, 16]
              and not first["hidden"] and first["callback"] == 0x8001, f"the first icon: {first}")
        check(second["id"] == 8 and second["version"] == 0 and second["tip"] == "Second icon" and second["image"] == [32, 32]
              and second.get("guid") == "12345678-9abc-def0-0102-030405060708" and second["callback"] == 0x8002, f"the second icon: {second}")
        if first["shown"]:                                        # (in the menu bar of one of the displays)
            rect = first["rect"]
            check(rect[2] > rect[0] and 0 < rect[3] - rect[1] < 100, f"the first icon in the menu bar: {rect}")
        else:                                                     # (a menu bar full up to the notch keeps new items off)
            print("tray: the menu bar did not show the icons (no room?); their places are not checked")

        # The status item's picture, drawn at twice its size: the file icon was loaded at 16x16 (blue) with its 32x32 (red)
        # and 48x48 (green, left half clear) kept, and at 2x AppKit draws a larger one. Every size has a clear corner.
        png = os.path.join(os.path.dirname(sock), "icon.png")
        check(c.ask(f"tray-snapshot 0 {png}")["ok"], "tray-snapshot")
        w, h, px = read_png(png)
        corner, right = px(0, 0), px(w * 3 // 4, h // 2)
        check(w >= 32 and corner[3] == 0 and right[3] == 255 and right[:3] != (0, 0, 255), f"the status item's picture: {w}x{h}, {corner} {right}")
        check(not c.ask("tray-snapshot 5 /dev/null")["ok"], "tray-snapshot of no icon")

        def act(request):
            r = c.ask(request)
            check(r.get("ok"), f"{request}: {r}")
            c.wait_idle(5000)
            return r
        act("tray-click 0 left")                                  # version 4: down, up, NIN_SELECT with the point
        act("tray-click 0 key")                                   # NIN_KEYSELECT at the icon's corner
        act("tray-click 0 right")                                 # down, up, WM_CONTEXTMENU: the program's menu opens
        menu = {}
        for _ in range(200):
            menu = c.ask("menu")
            if menu.get("open"): break
            time.sleep(0.01)
        check(menu.get("open") and menu.get("hwnd") == main_hwnd, f"the menu is open: {menu}")
        items = menu.get("items", [])
        texts = [i["text"] for i in items]
        check(texts == ["Open window", "Check me", "Disabled", "", "More", "Exit"], f"the menu's items: {texts}")
        if len(items) == 6:
            check(items[1]["checked"] and not items[2]["enabled"] and items[3]["separator"] and items[5]["default"]
                  and [i["text"] for i in items[4].get("items", [])] == ["Deep item"], f"the items' states: {items}")
        check(not c.ask("menu-pick Disabled")["ok"], "a disabled item cannot be chosen")
        act("menu-pick Deep")                                     # from the submenu
        act("tray-click 0 right")
        act("menu-pick Exit")                                     # the program deletes this icon
        icons = c.ask("tray")["icons"]
        check(len(icons) == 1 and icons[0]["id"] == 8, f"one icon left: {icons}")
        act("tray-click 0 left")                                  # version 0: down, up; a menu that answers with WM_COMMAND
        act("menu-pick Open")                                     # the program destroys the icon's window
        icons = c.ask("tray")["icons"]
        check(icons == [], f"no icon left after its window went: {icons}")
        check(not c.ask("tray-click 0 left")["ok"], "a click on no icon")
        c.ask(f"close {main_hwnd}")
        out, _ = p.communicate(timeout=20)
    except Exception as e:
        failures.append(repr(e)); print("tray:", repr(e))
        p.kill()
        out = p.communicate()[0] or ""
    lines = out.splitlines()
    events = [l for l in lines if l.startswith(("event", "picked", "track", "command"))]
    want = ["event icon=7 msg=0x201", "event icon=7 msg=0x202", "event icon=7 msg=0x400", "event icon=7 msg=0x401",
            "event icon=7 msg=0x204", "event icon=7 msg=0x205", "event icon=7 msg=0x7b", "picked=201",
            "event icon=7 msg=0x204", "event icon=7 msg=0x205", "event icon=7 msg=0x7b", "picked=104",
            "event icon=8 msg=0x201", "event icon=8 msg=0x202", "track=1", "command=101 window=second"]
    check([e.split(" at=")[0] for e in events] == want, "the messages the program received:\n  " + "\n  ".join(events))
    # Version 4's point: the same one for the three messages of a click (the icon's middle, or the mouse while the menu
    # bar has not placed the icon; icons move as others come and go and between displays, so no place is compared).
    pts = [tuple(map(int, e.split(" at=")[1].split(","))) for e in events if " at=" in e]
    check(len(pts) >= 7 and pts[0] == pts[1] == pts[2] and pts[4] == pts[5] == pts[6], f"the points: {pts}")
    check(p.returncode == 0 and "tray: failures=0" in out, f"exit {p.returncode}: {out.strip()[-1500:]}")
    shutil.rmtree(os.path.dirname(sock), ignore_errors=True)
    where = f"in the menu bar at {rect}" if "rect" in locals() else "not placed by the menu bar"
    print(f"tray: two icons added ({where}), clicked (left, keyboard, right twice, version 0) and removed through the control "
          f"socket, menus picked from (all {(time.time() - t0) * 1e3:.0f} ms): {len(failures)} failed checks -> {'ok' if not failures else 'FAILED'}")
    sys.exit(1 if failures else 0)

if __name__ == "__main__":
    main()
