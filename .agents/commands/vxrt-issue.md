---
description: Write a GitHub issue draft for a game that still fails on VXRuntime, from the harness evidence (never posts it)
argument-hint: <game> [what you tried]
---
Write an issue draft for VeRantyx about the game named in `{{args}}`, from the evidence. Never post it.

1. If no game was named, list the installed games (vxrt MCP `list_games`) and ask which one; stop until the user answers.
2. Use the newest harness run of the game in `runs/`; if there is none, run one first (vxrt MCP `run_game`, or `python3 tools/vxrt_run.py <game>`) and report a refusal's reason instead of working around it.
3. Write the draft with the vxrt MCP `prepare_issue` (or `python3 tools/vxrt_issue.py draft <game> --run runs/<run_id> --kind <kind> --tried "<what was tried and what each run showed>"`). Pick the kind (support, crash, performance, rendering) from the evidence.
4. Show the user the draft's path, its title and the similar issues it lists. Posting is theirs: `python3 tools/vxrt_issue.py submit runs/issues/<draft>.md`, in their own Terminal, asks them before it posts.
5. Talk to the user in their language.
