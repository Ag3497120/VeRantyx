# Game profiles

One file per Windows VR game, `games/<Steam app id>.json`, checked against [profile.schema.json](profile.schema.json)
by `tools/vxrt_games.py check` (part of `make -C VXRuntime check`). The harness (`tools/vxrt_run.py`), the MCP server
(`tools/vxrt_mcp.py`), VXRT Studio and the doctor's prompts read them.

A profile says how the game starts (`launcher`, `scenarios` with their arguments and length), the settings it needs
(`settings`, only AGENTS.md's tunables), what a run must show (`baselines`, per scenario or `scenario@quality`), its
state (`status`, with the `level` reached: starts, connects, draws, runs, controls from harness runs; playable only
from a person's playtest, `tools/vxrt_playtest.py`, recorded in `playtests`), and what is known (`known_issues` with
their GitHub issues, `notes`). `works` means a playtest found it playable.

Adding a game or changing what is known about one is a pull request anyone can make, usually with an AI coding tool:

1. `tools/vxrt_run.py <app id>` (an installed game without a profile runs with a default one), until it runs.
2. Write or update `games/<app id>.json`: the scenarios you measured, `status` with the date and your Mac, and a
   baseline from your runs (`source`: the run ids). Do not lower a baseline without runs that show why.
3. `make -C VXRuntime check`, then open the pull request with the run ids (the template asks for them).

If the game does not run and you cannot fix it: `tools/vxrt_issue.py draft <app id> --tried "..."`, then post it with
`tools/vxrt_issue.py submit` (it asks you first).
