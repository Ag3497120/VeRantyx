# M3: the same task, by screenshots and by Command Clips

Six Claude subagents (the same model), three per way, ran at the same time on six copies of `tests/shoplist.c` on the
MacBook Pro M1 Max (2026-10-16). The task, word for word the same for all: make the list eggs, flour, sugar, butter;
add milk before butter first and delete it with the program's own delete; save; close. None was shown the program's
source or how it behaves.

- **Screenshots** (`tools/vxrt_screen.py`): an image of the program's windows where they sit on the screen, clicks at
  pixels of it, typing and keys to the window clicked last, `close`. No drawn text, no idle signal (they waited with
  `sleep`). Its request log shows only `windows` (places and sizes) and `snapshot`, so it saw no text.
- **Command Clips** (`tools/vxrt_clip.py`): `state` (the drawn text and its boxes), `click-text`, `wait-idle`, `do`.

Success is judged from the program, not from the agents: what it printed when it exited and shoplist.txt.

| | Command Clips | Screenshots |
|---|---|---|
| Done (program output and file) | 3 / 3 | 3 / 3 |
| Agent's time, s | 85.5, 57.9, 66.4 (mean 69.9) | 96.3, 95.6, 106.0 (mean 99.3) |
| Agent's tool calls | 13, 10, 12 | 19, 19, 19 |
| Requests the program got | 40, 35, 35 | 37, 37, 37 (9 snapshots each) |
| First to last request, s | 64.6, 34.5, 39.8 | 75.4, 69.2, 81.1 |
| Tokens (the subagent's total) | 52.7 k, 52.0 k, 53.1 k | 53.2 k, 53.2 k, 53.1 k |

What it shows and what it does not:

- With text and an idle signal, the same agent finished 30 % sooner (mean 69.9 s against 99.3 s) with a third fewer
  tool calls. The difference is in the agent's own turns: one `state` replaces looking at an image, `click-text`
  replaces measuring a button, and `wait-idle` replaces `sleep` and another screenshot.
- Both ways did the task every time: this task does not tell them apart on reliability.
- The token totals are nearly the same: the agents' fixed context dominates them, and these screenshots are small
  (500 x 468, the program's windows only).
- This is the easiest case for screenshots: the image holds only the program, at 1x, in a clear font, with no other
  windows, no menus, no scrolling. A real computer-use agent sees the whole screen at Retina size, finds the window
  first and cannot ask where a window is; the gap here is a lower bound.
- Three runs each, run side by side (the same load on both ways).

Each trial's request log (`requests.log`, the milliseconds since the socket opened), the program's output and the file
it saved are in the folders here; `vision1/s5.png` is one of the screenshots an agent worked from. `agents.txt`:
tokens, tool calls and milliseconds per agent.
