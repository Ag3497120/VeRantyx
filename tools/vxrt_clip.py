#!/usr/bin/env python3
"""Command Clips: read and operate a program running on VXRuntime without looking at the screen.

The runtime opens a Unix socket when started with VXRT_CONTROL=<path> (VXRuntime/src/control.c); this talks to it.

    VXRT_CONTROL=/tmp/app.sock VXRuntime/build/vxrt-arm64 app.exe &
    tools/vxrt_clip.py --socket /tmp/app.sock state            # windows, the text drawn in them, idle or not
    tools/vxrt_clip.py --socket /tmp/app.sock click-text 0 Add  # a click on drawn text
    tools/vxrt_clip.py --socket /tmp/app.sock type 0 hello      # characters
    tools/vxrt_clip.py --socket /tmp/app.sock wait-idle 2000    # until what was asked is handled and painted
    tools/vxrt_clip.py --socket /tmp/app.sock texts             # just the drawn text, one per line
    tools/vxrt_clip.py --socket /tmp/app.sock do "click 0 150 62" "type 0 eggs" "key 0 13" wait-idle texts
                                                               # several requests in one go, each answer on a line

Requests: state, click <hwnd> <x> <y>, click-text <hwnd> <text>, key <hwnd> <vk>, type <hwnd> <text>, close <hwnd>,
wait-idle [ms]; hwnd 0 is the active window (else the first visible one). The notification area and popup menus:
tray, tray-click <n> [left|right|double|key|menu-key], tray-snapshot <n> <png>, menu, menu-pick <text>, menu-cancel.
Each answer is one line of JSON."""
import argparse, json, os, socket, sys, time


class Clip:
    """One connection to a program's control socket."""

    def __init__(self, path, timeout=30.0):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(timeout)
        self.s.connect(path)
        self.buf = b""

    @classmethod
    def wait_for(cls, path, seconds=20.0):
        """Connects once the program has opened its socket."""
        t0 = time.time()
        while True:
            try:
                return cls(path)
            except OSError:
                if time.time() - t0 > seconds:
                    raise
                time.sleep(0.05)

    def ask(self, line):
        self.s.sendall(line.encode() + b"\n")
        while b"\n" not in self.buf:
            chunk = self.s.recv(65536)
            if not chunk:
                raise ConnectionError("the program closed its control socket")
            self.buf += chunk
        out, self.buf = self.buf.split(b"\n", 1)
        return json.loads(out)

    def state(self): return self.ask("state")
    def wait_idle(self, ms=2000): return self.ask(f"wait-idle {ms}")
    def click(self, x, y, hwnd=0): return self.ask(f"click {hwnd} {x} {y}")
    def click_text(self, text, hwnd=0): return self.ask(f"click-text {hwnd} {text}")
    def key(self, vk, hwnd=0): return self.ask(f"key {hwnd} {vk}")
    def type(self, text, hwnd=0): return self.ask(f"type {hwnd} {text}")
    def close(self, hwnd=0): return self.ask(f"close {hwnd}")

    def texts(self):
        """Every text drawn in the program's windows, in the order drawn."""
        return [t["text"] for w in self.state()["windows"] for t in w["texts"]]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--socket", default=os.environ.get("VXRT_CONTROL"), help="the program's VXRT_CONTROL path")
    ap.add_argument("request", nargs="+", help="a request (see above), 'texts', or 'do' and one quoted request per argument")
    a = ap.parse_args()
    if not a.socket:
        sys.exit("vxrt_clip: no socket (--socket or VXRT_CONTROL)")
    c = Clip.wait_for(a.socket, 5)
    for req in (a.request[1:] if a.request[0] == "do" else [" ".join(a.request)]):
        if req == "texts":
            print("\n".join(c.texts()))
            continue
        r = c.ask(req)
        print(json.dumps(r, ensure_ascii=False))
        if req.startswith("close") and r.get("ok"):
            break                                               # (the program may be gone now)


if __name__ == "__main__":
    main()
