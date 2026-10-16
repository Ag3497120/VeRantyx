#!/usr/bin/env python3
"""A screenshot-and-coordinates way to operate a program on VXRuntime, as computer-use agents do: the baseline Command
Clips is compared with (CLIPS.md, M3). It gives an agent pixels only, never the drawn text, and no signal that the
program is done; it never touches the Mac's real screen or mouse.

    tools/vxrt_screen.py --socket S screenshot out.png   # every visible window at its place on the screen, one image
    tools/vxrt_screen.py --socket S click X Y            # a left click at a point of the last screenshot
    tools/vxrt_screen.py --socket S type TEXT            # characters, to the window clicked last
    tools/vxrt_screen.py --socket S key VK               # a key (virtual-key code), to the window clicked last
    tools/vxrt_screen.py --socket S close                # closes the window clicked last (no title bars are drawn)

From the program it asks only for the windows' places and sizes (`windows`, not `state`) and their pixels
(`snapshot`), so its request log shows that it saw no text."""
import argparse, json, os, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vxrt_clip import Clip
from PIL import Image

MARGIN = 20


def layout_path(sock): return sock + ".screen.json"
def load(sock):
    try: return json.load(open(layout_path(sock)))
    except (OSError, ValueError): return {}
def save(sock, d): json.dump(d, open(layout_path(sock), "w"))


def screenshot(c, sock, out):
    wins = [w for w in c.ask("windows")["windows"] if w["visible"] and w["client"][0] > 0 and w["client"][1] > 0]
    if not wins:
        sys.exit("no visible window")
    x0 = min(w["origin"][0] for w in wins) - MARGIN; y0 = min(w["origin"][1] for w in wins) - MARGIN
    x1 = max(w["origin"][0] + w["client"][0] for w in wins) + MARGIN; y1 = max(w["origin"][1] + w["client"][1] for w in wins) + MARGIN
    canvas = Image.new("RGB", (x1 - x0, y1 - y0), (64, 68, 76))      # (the desktop behind the windows)
    placed = []
    with tempfile.TemporaryDirectory() as d:
        for w in sorted(wins, key=lambda w: w["hwnd"]):                # (made later: on top)
            png = os.path.join(d, f"{w['hwnd']}.png")
            if not c.ask(f"snapshot {w['hwnd']} {png}").get("ok"):
                continue
            img = Image.open(png).convert("RGB")
            at = (w["origin"][0] - x0, w["origin"][1] - y0)
            canvas.paste(img, at)
            placed.append({"hwnd": w["hwnd"], "x": at[0], "y": at[1], "w": img.width, "h": img.height})
    canvas.save(out)
    d = load(sock); d["placed"] = placed; save(sock, d)
    print(json.dumps({"ok": True, "image": out, "size": [canvas.width, canvas.height]}))


def click(c, sock, x, y):
    d = load(sock)
    for p in reversed(d.get("placed", [])):                          # (the topmost window there)
        if p["x"] <= x < p["x"] + p["w"] and p["y"] <= y < p["y"] + p["h"]:
            r = c.ask(f"click {p['hwnd']} {x - p['x']} {y - p['y']}")
            d["focus"] = p["hwnd"]; save(sock, d)
            print(json.dumps({"ok": r.get("ok", False)}))
            return
    print(json.dumps({"ok": False, "error": "no window at that point of the last screenshot"}))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--socket", default=os.environ.get("VXRT_CONTROL"))
    ap.add_argument("action", choices=["screenshot", "click", "type", "key", "close"])
    ap.add_argument("args", nargs="*")
    a = ap.parse_args()
    c = Clip.wait_for(a.socket, 5)
    if a.action == "screenshot":
        screenshot(c, a.socket, a.args[0] if a.args else "screen.png")
    elif a.action == "click":
        click(c, a.socket, int(a.args[0]), int(a.args[1]))
    else:
        focus = load(a.socket).get("focus", 0)
        req = {"type": f"type {focus} {' '.join(a.args)}", "key": f"key {focus} {a.args[0] if a.args else ''}", "close": f"close {focus}"}[a.action]
        print(json.dumps({"ok": c.ask(req).get("ok", False)}))


if __name__ == "__main__":
    main()
