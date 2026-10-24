#!/usr/bin/env python3
"""GDI to the screen, checked (make runtime-tests): build/present.exe draws made-up gradients through every path of
src/present.c and checks what GDI drew; this script asks the runtime given, through Command Clips only
(tools/vxrt_clip.py), what its Mac windows show (snapshot-view: the committed pictures as Core Animation has them,
at the window's backing scale) and what the client bitmaps hold (snapshot), checks the pixels, and closes the window.

    python3 tests/present_check.py build/vxrt-arm64"""
import os, struct, subprocess, sys, tempfile, time, zlib
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
from vxrt_clip import Clip

failures = []
def check(cond, what):
    if not cond: failures.append(what); print("check failed:", what)

def read_png(path):
    """(width, height, rows of (r, g, b, a)) of an 8-bit RGB or RGBA PNG."""
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos, idat, w = 8, b"", 0
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR": w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
        elif kind == b"IDAT": idat += body
        pos += 12 + n
    assert depth == 8 and ctype in (2, 6), f"PNG depth {depth} type {ctype}"
    bpp = 4 if ctype == 6 else 3
    raw, stride, rows, prev = zlib.decompress(idat), w * bpp, [], bytearray(w * bpp)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0; b = prev[i]; c = prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append([tuple(line[x * bpp:x * bpp + bpp]) + (() if bpp == 4 else (255,)) for x in range(w)])
        prev = line
    return w, h, rows

def grad(x, y): return ((x * 4) & 255, (y * 4) & 255, 0x40)
# Points of the main window's client area and the color each shows (GDI's pixels; the child is in front of the
# parent only on the screen, so the bitmap and the view differ there).
EXPECT = [
    ((7, 5), grad(7, 5)), ((63, 63), grad(63, 63)),                                 # A: top-down DIB section
    ((64 + 7, 5), grad(7, 5)), ((64 + 3, 61), grad(3, 61)),                         # B: bottom-up DIB section
    ((128 + 7, 5), grad(6, 4)), ((128 + 63, 63), grad(62, 62)),                     # C: StretchBlt 2x
    ((192 + 7, 5), grad(7, 5)), ((192 + 9, 40), grad(9, 40)),                       # D: SetDIBitsToDevice, two bands
    ((256 + 7, 5), grad(6, 4)), ((256 + 40, 63), grad(40, 62)),                     # E: StretchDIBits
    ((9, 64 + 33), grad(9, 33)),                                                    # F: DIB section over a mapping
    ((64 + 20, 64 + 20), (128, 128, 255)),                                          # G: AlphaBlend
    ((128 + 20, 64 + 20), (0, 255, 0)), ((128 + 4, 64 + 4), (255, 0, 0)),          # H: TransparentBlt
    ((192 + 10, 64 + 10), (0, 255, 255)), ((192 + 40, 64 + 10), (128, 128, 128)),  # I: viewport origin, clip
    ((256 + 7, 64 + 5), grad(7, 5)), ((256 + 7, 64 + 40), (227, 95, 0xbf)),        # J: SetDIBits, DSTINVERT
    ((40, 140), (255, 255, 255)), ((40, 200), (0, 100, 0)), ((120, 150), (0, 100, 0)), ((120, 220), (255, 255, 255)),   # WM_PAINT
    ((200, 130), (36, 0x80, 0x10)), ((300, 223), (222, 0x80, 0x10)), ((300, 230), (0, 0, 0)),                           # ScrollWindowEx
]
CHILD_PARENT = [((20, 214), (0, 100, 0)), ((20, 230), (0, 100, 0))]          # under the child, in the parent's bitmap
CHILD = [((20, 214), (255, 255, 0)), ((20, 230), (255, 128, 0))]                # the child (yellow over orange) on the screen

def near(got, want, tol):
    return all(abs(a - b) <= tol for a, b in zip(got, want))

def at(rows, scale, x, y):
    s = int(scale)
    return rows[y * s + s // 2][x * s + s // 2]

def main():
    runtime = sys.argv[1] if len(sys.argv) > 1 else "build/vxrt"
    tmp = tempfile.mkdtemp(prefix="vxpresent")
    sock = os.path.join(tmp, "c.sock")
    p = subprocess.Popen([runtime, "build/present.exe"], env=dict(os.environ, VXRT_CONTROL=sock), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    t0 = time.time()
    try:
        c = Clip.wait_for(sock, 20)
        idle = c.wait_idle(20000)
        check(idle["idle"], f"idle after drawing: {idle}")
        wins = {w["title"]: w for w in c.state()["windows"]}
        main_w, alpha_w, key_w, perf_w = (wins.get(t) for t in ("VX present test", "VX layered alpha", "VX layered key", "VX present perf"))
        check(all((main_w, alpha_w, key_w, perf_w)), f"the windows: {sorted(wins)}")
        # what GDI drew: the client bitmap
        png = os.path.join(tmp, "main_bitmap.png")
        check(c.ask(f"snapshot {main_w['hwnd']} {png}")["ok"], "snapshot")
        w, h, rows = read_png(png)
        check((w, h) == (320, 240), f"bitmap size {w}x{h}")
        for (x, y), want in EXPECT + CHILD_PARENT:
            got = rows[y][x][:3]
            check(got == want, f"bitmap ({x}, {y}): {got}, not {want}")
        # What the Mac windows show. (The window server shows a picture a frame or two after it was handed over; a busy
        # Mac may lag more, so a capture that does not match yet is taken again, twice at most.)
        def main_view():
            png, bad = os.path.join(tmp, "main_view.png"), []
            r = c.ask(f"snapshot-view {main_w['hwnd']} {png}")
            if not r.get("ok"): return r, [f"snapshot-view: {r}"]
            scale, source = r.get("scale", 1), r.get("source")
            if source not in ("window-server", "layers"): bad.append(f"snapshot-view source: {r}")
            if not (r.get("width") == 320 * scale and r.get("height") == 240 * scale and r.get("opaque") is True): bad.append(f"view size: {r}")
            w, h, rows = read_png(png)
            if (w, h) != (320 * scale, 240 * scale): return r, bad + [f"view PNG {w}x{h} at scale {scale}"]
            # (the window server's pixels went to the display's 8-bit color space and back to sRGB: a few values off,
            # more for saturated colors near the gamut's edges)
            tol = 8 if source == "window-server" else 0
            for (x, y), want in EXPECT + CHILD:                    # the committed picture with the child in front
                got = at(rows, scale, x, y)
                if not (near(got[:3], want, tol) and got[3] == 255): bad.append(f"view ({x}, {y}): {got}, not {want}")
            # tile A, every pixel: each GDI pixel is a block of scale x scale device pixels of one color (the window's
            # scale, nothing interpolated), within the color tolerance, and not moved: the mean red of each column is
            # 4x, the mean green of each row 4y (a pixel's shift would move them by 4)
            sc = int(scale)
            block = lambda x, y: [rows[y * sc + dy][x * sc + dx] for dy in range(sc) for dx in range(sc)]
            mixed = sum(1 for y in range(64) for x in range(64) if len(set(block(x, y))) != 1)
            far = sum(1 for y in range(64) for x in range(64) if not near(block(x, y)[0][:3], grad(x, y), tol))
            cols = [abs(sum(block(x, y)[0][0] for y in range(64)) / 64 - 4 * x) for x in range(64)]
            lines = [abs(sum(block(x, y)[0][1] for x in range(64)) / 64 - 4 * y) for y in range(64)]
            if not (mixed == 0 and far == 0 and max(cols) <= 2 and max(lines) <= 2):
                bad.append(f"tile A on the screen: {mixed} mixed blocks, {far} pixels off, columns off by up to {max(cols):.2f}, rows by {max(lines):.2f}")
            return r, bad
        def alpha_view():
            png = os.path.join(tmp, "alpha.png")
            r = c.ask(f"snapshot-view {alpha_w['hwnd']} {png}")         # per-pixel alpha (UpdateLayeredWindow)
            if not (r.get("ok") and r.get("opaque") is False and abs(r.get("window_alpha", 0) - 1) < 1e-3): return r, [f"layered alpha window: {r}"]
            tol, scale = (8 if r["source"] == "window-server" else 0), r["scale"]
            w, h, rows = read_png(png)
            left, right = at(rows, scale, 10, 30), at(rows, scale, 50, 30)
            return r, ([] if near(left, (255, 0, 0, 255), tol) else [f"opaque half: {left}"]) + \
                      ([] if abs(right[3] - 128) <= 2 and right[1] >= 245 and right[0] <= 4 * tol else [f"half-alpha half: {right}"])   # (unpremultiplied: errors doubled)
        def key_view():
            png = os.path.join(tmp, "key.png")
            r = c.ask(f"snapshot-view {key_w['hwnd']} {png}")           # color key and constant alpha (SetLayeredWindowAttributes)
            if not (r.get("ok") and r.get("opaque") is False and abs(r.get("window_alpha", 0) - 128 / 255) < 1e-2): return r, [f"color-key window: {r}"]
            ws, scale = r["source"] == "window-server", r["scale"]
            w, h, rows = read_png(png)
            # (the window server's picture carries the window's own alpha, 128: Core Animation's layers do not)
            blue, key, a = at(rows, scale, 4, 4), at(rows, scale, 24, 24), 128 if ws else 255
            return r, ([] if near(blue[:3], (0, 0, 255), 8 if ws else 0) and abs(blue[3] - a) <= 2 else [f"color-key window, blue: {blue}"]) + \
                      ([] if key[3] == 0 else [f"color-key window, the key: {key}"])
        def perf_view():
            png = os.path.join(tmp, "perf.png")
            r = c.ask(f"snapshot-view {perf_w['hwnd']} {png}")          # the last of 120 frames is what the screen shows
            if not r.get("ok"): return r, [f"perf window: {r}"]
            w, h, rows = read_png(png)
            got = at(rows, r["scale"], 500, 300)
            return r, [] if near(got[:3], (119, 0x20, 136), 8 if r["source"] == "window-server" else 0) else [f"perf window shows {got}"]
        for view in (main_view, alpha_view, key_view, perf_view):
            for attempt in range(3):
                r, bad = view()
                if not bad: break
                time.sleep(0.5)
            for b in bad: check(False, b)
            if view is main_view: scale, source = r.get("scale"), r.get("source")
        c.close(main_w["hwnd"])
        out, _ = p.communicate(timeout=30)
        last = out.strip().splitlines()[-1] if out.strip() else ""
        check(p.returncode == 0 and last.endswith("-> ok"), f"exit {p.returncode}: {out.strip()[-600:]}")
        print(last)
    except Exception as e:
        failures.append(repr(e)); print("present:", repr(e))
        p.kill()
        print(p.communicate()[0][-2000:])
    print(f"present_check: windows read through snapshot-view ({locals().get('source', '?')}, scale {locals().get('scale', '?')}) "
          f"({(time.time() - t0) * 1e3:.0f} ms): {len(failures)} failed checks -> {'ok' if not failures else 'FAILED'}")
    sys.exit(1 if failures else 0)

if __name__ == "__main__":
    main()
