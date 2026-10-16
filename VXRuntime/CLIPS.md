# Command Clips

A program running on VXRuntime can be read and operated without looking at the screen: its windows, the text drawn
in them and where, and actions as Windows messages, through a socket the runtime opens. An AI agent (or a script)
then works with the program as with a command-line tool instead of through screenshots and clicks, and knows when
an action's result is there instead of guessing how long to wait.

The plan this is the first step of (generated apps that run on VXRuntime as Windows and Mac builds, operated and
checked this way): https://claude.ai/artifact/2huxsyiWjPBfhVAk5hsmUU

## Use

```bash
VXRT_CONTROL=/tmp/app.sock VXRuntime/build/vxrt-arm64 app.exe &
tools/vxrt_clip.py --socket /tmp/app.sock state              # JSON: windows, drawn text with boxes, idle
tools/vxrt_clip.py --socket /tmp/app.sock click-text 0 Add   # a click on the middle of drawn text
tools/vxrt_clip.py --socket /tmp/app.sock type 0 hello
tools/vxrt_clip.py --socket /tmp/app.sock wait-idle 2000
tools/vxrt_clip.py --socket /tmp/app.sock texts              # the drawn text alone, one per line
```

The MCP server (`tools/vxrt_mcp.py`) has the same as `clip_state`, `clip_act` (an action, then the state once idle)
and `clip_wait`.

## Requests

One per line on the socket; each answered with one line of JSON. `hwnd` 0 is the active window (else the first
visible one).

| Request | What it does |
|---|---|
| `state` | windows (hwnd, parent, class, title, visible, enabled, client size), the text drawn in each with its box (client coordinates), the active window, the focus, `idle` |
| `click <hwnd> <x> <y>` | WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP at client coordinates |
| `click-text <hwnd> <text>` | the same at the middle of the first drawn text containing `<text>` (hwnd 0: any window) |
| `key <hwnd> <vk>` | WM_KEYDOWN, WM_KEYUP (no WM_CHAR: use `type`) |
| `type <hwnd> <text>` | WM_CHAR for each UTF-16 unit of the text |
| `close <hwnd>` | WM_CLOSE |
| `wait-idle [ms]` | answers once the program is idle, or after ms (default 2000) with `"idle": false` |

## How it knows

- **Text**: GDI's TextOut, ExtTextOut and DrawText record what they draw and its box on the surface they draw on (a
  window's client area or a bitmap, `src/gdi.c`). Painting over a place (FillRect, PatBlt, StretchDIBits ...) forgets
  the text there; BitBlt and StretchBlt copy it along with the pixels, so double-buffered programs report their text
  too.
- **Idle**: every thread that owns a window waits in GetMessage, WaitMessage or MsgWaitForMultipleObjects with nothing
  queued, nothing sent to it and none of its windows left to paint (`vx_ui_idle`, `src/user32.c`), twice in a row a
  millisecond apart. A program that renders in a PeekMessage loop (most games) is never idle; `wait-idle` then
  times out.
- The socket is the user's only (mode 0600) and is removed when the program exits.

## Limits (now)

- Text drawn with Direct3D, OpenGL or DirectWrite is not seen; only GDI's.
- There are no standard controls (BUTTON, EDIT ...) in VXRuntime yet, so no control tree: windows and drawn text.
- Clicks go to the window given (not the child under the point).

## Checked

`tests/clip.c` (a counter, a button drawn into a memory DC and blitted, typed text, R to reset), driven only through
the socket by `tests/clip_check.py`, in `make check` (x86 build under Rosetta) and `make check-arm64`; on the MacBook
Air M5 also with `VXRT_VCPU=1`. From a click to idle with the new text: 2.6 ms (M1 Max), 3.1 ms (M5), most of it
the 1 ms polling of `wait-idle`.
