# Contributing to VeRantyx

Thank you for trying VeRantyx. The most useful help is what only you can give: **trying games and headsets the
author does not have, and telling what happened.** Reports from players are what fixed The Lab's controllers and the
Wi-Fi delay spikes with Quest headsets.

## Reports and data: always welcome

The one-click reports below come with the next version of VeRantyx for Mac (after 0.2.0); until then, the forms
are on GitHub: [it runs](https://github.com/Ag3497120/VeRantyx/issues/new?template=works.yml),
[missing functions](https://github.com/Ag3497120/VeRantyx/issues/new?template=missing-functions.yml).

| What | How | Effort |
|---|---|---|
| **It runs** (a game, your Mac, your headset, the frame rate) | In the app, when a game ends: **Report: it ran**. The form opens filled in; you only press Submit | one click |
| **Windows functions a game needs** | In the app, when a game stopped on a function VXRuntime lacks: **Send the missing functions** (names only, nothing from the game) | one click |
| **A problem** (does not start, crashes, slow, draws wrongly) | In the app: Help > Report a problem. It saves your logs on the Desktop with your Mac user name and Steam ID taken out | a few minutes |
| **A headset** (Quest, Pico, ...) | [Report a headset](https://github.com/Ag3497120/VeRantyx/issues/new?template=alvr-headset.yml) | a few minutes |

Each report goes into the [games/](games/) profiles, and the missing functions are implemented in the order most
games need them.

## Code

VeRantyx is written by its author together with AI coding agents, and every change is checked by `make check` and
the game harness. To keep the runtime consistent, **code is implemented on the author's side**: a pull request may be
used as an idea and rewritten rather than merged. The best way to contribute a fix is an issue that says what you
found, with a harness run:

```bash
tools/vxrt_run.py <game>                                   # measures the game, headless
tools/vxrt_issue.py draft <game> --tried "what you changed and what each run showed"
```

You are welcome to work with your own AI agent: this repository is set up for it ([AGENTS.md](AGENTS.md),
`/vxrt-support <game>`). If you open a pull request anyway, keep it to one fix, include the harness run ids before and
after, and make sure `make -C VXRuntime check` passes; others are closed with thanks.

## Rules that never change

- **Your own copy only.** No game, Valve or Microsoft files in issues, pull requests or releases.
- **No anti-cheat.** Games with Easy Anti-Cheat, BattlEye and similar are out of scope; nothing works around them.
- **No modified game files.** VeRantyx writes to its own overlay and AppData, never into a game's folder.

Credit: reports and ideas that lead to a change are named in the [update log](README.md#updates).
