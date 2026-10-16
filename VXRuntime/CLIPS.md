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
tools/vxrt_clip.py --socket /tmp/app.sock do "click 0 150 62" "type 0 eggs" "key 0 13" wait-idle texts   # several at once
```

`VXRT_CONTROL_LOG=<file>` appends every request with the milliseconds since the socket opened.

The MCP server (`tools/vxrt_mcp.py`) has the same as `clip_state`, `clip_act` (an action, then the state once idle)
and `clip_wait`.

## Requests

One per line on the socket; each answered with one line of JSON. `hwnd` 0 is the active window (else the first
visible one).

| Request | What it does |
|---|---|
| `state` | windows (hwnd, parent, class, title, visible, enabled, client size, origin on the screen), the text drawn in each with its box (client coordinates), the active window, the focus, `idle` |
| `windows` | the same without the text |
| `snapshot <hwnd> <path>` | the client area's pixels as a PNG file |
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

## Demo: an AI agent doing a task it was not shown how to do (M2)

`tests/shoplist.c` is a shopping list of the kind an AI would generate for VXRuntime, drawn with GDI: a name field
that takes typing only after a click on it, `[ Add ]`, a `[ x ]` per row that asks in a second window
(`[ Yes ]` / `[ No ]`), `[ Save ]` to shoplist.txt, a status line. An agent (Claude, a subagent with no view of the
screen and told not to read the program's source) got only the tool's usage and the task: make the list eggs, flour,
sugar, butter, adding milk before butter first and deleting it with the program's own delete, save, close.

It did, found on its way from `state` that the field loses its focus after each Add, that delete asks in a separate
window, and where each row's `[ x ]` is. Checked from outside the agent: the program printed
`items=eggs,flour,sugar,butter saved=1` and shoplist.txt holds the four lines (`demos/m2-shoplist/`). 48 requests
(15 state, 14 wait-idle, 7 click-text, 6 click, 5 type, 1 close) in 74 s, nearly all of it the agent's own thinking
(the requests within one of its calls came 40 ms apart, the start of the tool included). Its one stumble was its shell (zsh does not split a command kept in a
variable), hence `do` for several requests per call.

## Compared with screenshots (M3)

The same task by six agents, three with screenshots and coordinate clicks only (`tools/vxrt_screen.py`: no text, no
idle signal; requests `windows` and `snapshot`), three with Command Clips: all six did it (checked from the program's
output and file); with Command Clips in 30 % less time (mean 69.9 s against 99.3 s) and a third fewer tool calls, the
tokens about the same. The screenshots here were the easiest kind (the program's windows only, 1x); details and the
caveats in `demos/m3-compare/README.md`.

## Checked

`tests/clip.c` (a counter, a button drawn into a memory DC and blitted, typed text, R to reset), driven only through
the socket by `tests/clip_check.py`, in `make check` (x86 build under Rosetta) and `make check-arm64`; on the MacBook
Air M5 also with `VXRT_VCPU=1`. `tests/shoplist_check.py` does the shopping list as a fixed script (14 actions,
both windows, the saved file checked) in the same runs. From a click to idle with the new text: 2.6 ms (M1 Max),
3.1 ms (M5), most of it the 1 ms polling of `wait-idle`.
