# M3, harder: a long list, look-alike names, a small font, the whole screen

The same comparison as `../m3-compare`, under harder conditions (2026-10-16, MacBook Pro M1 Max with a 4K display
beside it). `tests/shoplist.c --hard` starts with 18 items shown six to a page ("[ < ]" "Page 1 of 3" "[ > ]"), names
alike (milk, oat milk, soy milk, almond milk, milk chocolate, milk powder; rice, rice crackers, rice flour, brown
rice; oats), a small font, and asks "Remove ... from the list?" with "[ Cancel ]" first. The task: remove "oat milk"
and "rice crackers" and nothing else, add "eggs", save, close. Success is judged from the program's output and file:
the 16 other items untouched and in order, then eggs.

The screenshot agents got the whole screen at Retina scale (3024 x 1964, `tools/vxrt_screen.py --desktop`), the
program's windows where they are on it and a "Notes" window of another app that mentions oat milk, rice crackers,
eggs, "[ Remove ]" and "[ Save ]". The Command Clips agents had the same tool as before.

| | Command Clips | Screenshots |
|---|---|---|
| Done (program output and file) | 3 / 3 | 3 / 4 (one could not see the dialog: below) |
| Agent's time, s (done ones) | 91.7, 87.6, 77.9 (mean 85.7) | 198.0, 173.1, 201.6 (mean 190.9) |
| Agent's tool calls | 15, 16, 15 | 33, 30, 31 |
| Tokens (the subagent's total) | 57.1 k, 56.7 k, 57.6 k (mean 57.1 k) | 71.7 k, 68.8 k, 71.3 k (mean 70.6 k) |
| Screenshots taken | none | 14 to 16 each, plus crops and zooms |

- Harder, the gap grew: twice the time against 30 % more in M3's easy version (`../m3-compare`), twice the calls,
  a quarter more tokens (the images now count).
- No one removed a look-alike, and no one acted on the Notes window, either way.
- All three screenshot agents that finished took the field's text cursor for a letter ("eggsl"), pressed Backspace
  to find out and retyped the "s". The Command Clips agents read "Name: eggs|" and moved on.
- **The failed trial (vision3) found a runtime bug.** The confirmation window opened centered on the other display
  (above the built-in one, y = -790 in Windows coordinates), so a screenshot of one screen never showed it; the agent
  saw only "Answer the question first" and, rightly, did not click blindly. The Command Clips agents had the same
  windows (they reported the odd position) and were not affected: `state` lists every window wherever it is. Fixed in
  the runtime: a window with an owner, given no position, is now centered over its owner (`src/cocoa.m`), as dialogs
  are on Windows. `vision4` is the same trial again with that fix (it ran alone, after the others).
- A first round of screenshot trials was void and is not counted: the desktop image was laid out from screen (0, 0),
  so a window on the other display fell outside it (`vxrt_screen.py` now places the screen around the program's first
  window). They were stopped once that was found.

Folders: each trial's request log, the program's output and saved file; `vision4/first-screenshot.png` is what a
screenshot agent started from; `agents.txt` has tokens, tool calls and milliseconds per agent. vision3's program was
closed by hand after its agent stopped (that close is not in its log).
