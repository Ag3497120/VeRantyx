---
name: vxrt-tune-game
description: Tune a Windows VR game on VXRuntime (vxrt) for this Mac — settings only, measured with the harness in alternating runs, reported with evidence. Use when asked to optimize, raise fps or resolution, or pick the best quality preset for a game.
---

# Tuning a game on VXRuntime (settings only)

Read AGENTS.md first. A tune changes settings, never the runtime's code.

## Measure

- `tools/vxrt_run.py <game> [scenario] --quality 3k --set NAME=VALUE ...` (MCP: `run_game` with `quality` and
  `settings`). Only the settings AGENTS.md lists are accepted; others are refused (`settings_allowed`).
- Each run's `fps_median` is over the last part of the run (after loading), from the game's own Submit calls where
  the capture proxy exists (Half-Life: Alyx), otherwise from the frames the encoder took from SteamVR.
  `gpu_utilization_median` tells GPU-bound (≥ 90 %) from paced or CPU-bound (< 60 %).
- Use the scenario that matters: Alyx `menu` is a light scene with a known 20 fps limit; `chapter` is gameplay.

## Compare fairly

- One run per setting is not a comparison: run A B B A (or more) and compare medians (`compare_runs`).
  Differences of a few fps are noise unless they repeat.
- Check `uptime`/`top` first: other programs (Photos' mediaanalysisd, other sessions) change GPU numbers.
- The goal: fps at the display rate (`VERANTYX_DISPLAY_HZ`, 60 by default for the presets), then the highest
  resolution that keeps it. A render size larger than the stream's budget (about 2880x1620 at 60 fps for one HEVC
  session) only supersamples.

## Report

A table: setting → fps (run ids, observation ids), GPU %, frame checked. The best combination as a launch
command (`scripts/launch_vxrt_alyx.sh` or `scripts/launch_vxrt_game.sh` with the environment). Mark suspicions
(`inferred`, i#) as such, and list what was not measured (frame pacing, the real headset, audio).
