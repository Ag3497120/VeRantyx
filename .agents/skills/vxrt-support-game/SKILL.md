---
name: vxrt-support-game
description: Make a Windows VR game run on VXRuntime (vxrt) — measure with the harness, find what stops it, implement the missing Windows behaviour, prove it with a run. Use when asked to "make <game> run", to support a new title, or when a game crashes, exits or draws nothing on vxrt.
---

# Making a game run on VXRuntime

Read AGENTS.md first; its rules always apply (no DRM or anti-cheat work, game files untouched, commit only what a
run shows).

## The loop

1. **Measure first.** `tools/vxrt_run.py <game or app id>` (MCP: `run_game`). It refuses unfair runs with a typed
   reason (`user_not_playing`, `steamvr_not_running`, `steam_running`, `roots_isolated`, `vr_mode`,
   `settings_allowed`): report the reason and stop; never stop the user's game, never point it at their roots.
2. **Find the first blocker** in the result, in this order: `first_problem` (with its log line), `unimplemented_calls`,
   `unhandled_exceptions`, `native_signals_outside_program`, `managed_exceptions`, `exit`; then
   `connected_after_s` (null: never reached SteamVR) and `frame_bright_fraction` (black: connects but draws nothing).
   Read around it: `tools/vxrt_run.py` keeps the logs in `runs/<run_id>/logs/` (MCP: `read_log` with
   `process: "game"` — the game's log also holds lines of SteamVR helpers it started, vrmonitor and vrwebhelper,
   which are noise).
3. **Fix it where it belongs** (AGENTS.md lists the files), the way Windows behaves (return values, last error,
   reference counts). Add a test in `VXRuntime/tests` when it is not trivial; `make -C VXRuntime check`.
4. **Measure again** and compare (`compare_runs`). Then `make -C VXRuntime vr-check` (or at least the games that
   worked) so nothing else broke. Report with run ids and observation ids; say what is still unknown.

5. **Record it** in the game's profile (`games/<appid>.json`): scenarios, status, a baseline from your runs, known
   issues. **Stuck?** After three attempts without progress, `tools/vxrt_issue.py draft` (MCP `prepare_issue`) with what
   you tried; show the user the draft and stop (only they post it).

`vxrt_doctor` / `analyze_game` lists functions the game imports that vxrt lacks: a static list. Most are never
called; implement what a run shows being called.

## Finding out more (there is no debugger: lldb does not attach to Rosetta processes here)

- **Who calls what:** `--trace` (VXRT_TRACE=1) logs loads and failures; `--set VXRT_TRACE_CALLS=N` logs the first N
  calls of every runtime function with arguments and the return address (`image+offset`). The last calls before a
  crash are usually the answer.
- **Native code at an offset:** `xcrun llvm-objdump -d --start-address=0x<imagebase+rva> --stop-address=... <dll>`
  (image base: `xcrun llvm-objdump -p`). Crash reports print `image+0xRVA` frames.
- **Where a message comes from:** find the string's address in `.rdata`, then the `lea reg, [rip+X]` that loads it
  in `.text` (search the disassembly for the address).
- **abort():** with VXRT_TRACE, a macOS abort (malloc's "pointer being freed was not allocated", libc++) prints a
  backtrace naming the program's frames.
- **Stack overflows** print the program's frames found on the stack.
- **REA** (github.com/morluto/rea, MCP, optional) decompiles DLLs (Ghidra) and reads .NET assemblies; it needs
  Node ≥ 22.19, JDK 21 and Ghidra — ask the user before downloading anything.

## Things learned the hard way

- **Unity (Mono):** the Boehm GC finds each thread's stack with VirtualQuery (walking down while pages are
  PAGE_READWRITE) and stops threads with SuspendThread/GetThreadContext; Mono's JIT reads TLS at gs:0x1480+8i.
  Wrong answers there show up far away: methods called on the wrong objects, NullReferenceExceptions every frame.
  Many managed exceptions in a run: look at the first one, then suspect the runtime before the game.
- **Unity plugins** load from `<Game>_Data/Plugins/x86_64` through SetDllDirectory. Unity initializes OpenVR twice:
  openvr_api unloads vrclient in between (FreeLibrary must really detach a DLL that only LoadLibrary brought in).
- **Unity skinning** uses stream output with vertex shader code (CreateGeometryShaderWithStreamOutput).
- **Source 2 (Alyx)** needs `-vr`; without it, it starts in desktop mode and fails in ways that look like
  regressions (the harness passes the right arguments; do not run hlvr.exe by hand).
- **SteamVR's vrserver** sometimes dies at start ("unwind did not reach its target frame"): run again before
  concluding anything.
- A game's own vrclient may start vrmonitor/vrwebhelper; their unimplemented functions do not matter.
