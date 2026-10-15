# Where VXRuntime is going

**A compatibility runtime that runs Windows apps on a Mac, lets them mix with native ARM64 parts, and lets AI agents
observe and operate them through structure, with the user's permission.** Running old software is half of the value;
the other half, in a time of AI agents, is that existing software becomes something an agent can read and use.

It is not "every program converted to one instruction set". It is three layers, each useful on its own, built in
this order, each measured before it is claimed (the harness, the differential test, playtests).

## 1. Windows apps on Apple silicon, without Rosetta or Wine

VXRuntime already runs Windows VR games and SteamVR without Wine (README). Its own x86-64 → ARM64 translator
replaces Rosetta 2 for game code ([VXRuntime/TRANSLATOR.md](../VXRuntime/TRANSLATOR.md)); phase 2a matches Rosetta on
1.85 M random cases of 154 integer instruction forms.

## 2. x86-64 and ARM64 code in one process

The precedent is Microsoft's Arm64EC
([Arm64EC](https://learn.microsoft.com/en-us/windows/arm/arm64ec)): native and emulated x64 code call each other in
one process, so an app can be made native piece by piece. That works because Arm64EC fixes a calling convention,
stack and data layout, and loader behaviour for both sides. VXRuntime needs a mixed ABI of its own, and a function's C
type alone is not enough to bridge everything. The plan, narrowest first:

1. **C ABI function boundaries only**: integer, pointer and floating-point arguments and results, struct passing by
   the Windows x64 rules, called directly through thunks generated from declared signatures. This is how translated
   code already calls VXRuntime's own (native) Windows API.
2. Then, each with its own design and tests: callbacks (native code calling translated function pointers), exceptions
   and stack unwinding across the boundary, variadic functions, thread-local storage layout.
3. Then ARM64 Windows PEs loaded next to x86-64 ones, so a game's heavy DLL can be replaced by a native build.

The translator's calling convention (TRANSLATOR.md) is chosen with this in mind now, while it costs nothing.

## 3. AI agents that observe and operate Windows apps

VXRuntime sees every API call of the programs it runs, which no screenshot-driven agent can. But API calls alone do
not tell what a screen means or what a game's state is. Observation is several sources combined, and each tool says
which one an answer came from:

| What | Source | Coverage |
|---|---|---|
| Windows, controls, their text and state | the window and control objects VXRuntime implements (user32 / common controls), exposed like an accessibility tree | apps built from standard controls |
| What a custom-drawn screen or a game shows | the frame itself (VXRuntime has it before it reaches the display), with image understanding / OCR | any app, approximate |
| A game's state (position, health, objective, ...) | per-title integrations: documented interfaces, logs, or memory layouts a title's profile declares | only titles that have one |
| What the program did | API traces (files, network, registry, D3D calls), the harness's evidence | everything, but it is behaviour, not meaning |

Acting (input, menu commands, window operations) goes through the same runtime, so it is exact rather than clicks at
coordinates. It is allowed only with the user's permission, per app and per session, and logged; it never touches
anti-cheat-protected games. The existing MCP server (`tools/vxrt_mcp.py`) is where these tools go.

What makes this worth building is not running AI models on several operating systems (others already provide
cross-platform local inference); it is that Windows-only software becomes usable on a Mac **and** by an agent.

## An OS for apps, on top of macOS

Put together, VXRuntime is a runtime that is also a kind of operating system: not one that replaces macOS (the
kernel, drivers and hardware stay macOS's), but one that owns everything an app sees, its API, files, network and
devices, and so can run apps safely, let an AI work with them, and be repaired by an AI. Unlike an OS with one
assistant built in, the AI is the user's choice (Claude, Codex, Gemini, a local model), connected over MCP.

- **Safe to run.** With the translator, every instruction of an app passes through VXRuntime. Under Rosetta a
  hostile x86 program can still issue macOS system calls directly (the `syscall` instruction); translated code cannot,
  so access to files, network and devices is mediated at the API, without the weight of a virtual machine. Each app
  gets its own permissions (which folders, network or not, which devices), and its writes go to its own overlay
  (already how VXRuntime keeps game folders untouched) with snapshots to roll back to.
- **AI at the centre.** Agents observe and operate apps through structure (above), within the permissions.
- **Repaired by AI, never silently.** The loop exists today: a failure becomes evidence (harness), an agent changes
  VXRuntime, the differential test, harness runs and a playtest show the change works, and what cannot be fixed
  becomes an issue. A repair is applied only after its tests pass and the user approves it, and can be rolled back.
  Not breaking things comes before repairing them fast.
- **One place to start apps.** Windows and Mac apps launched from one place, with the AI at hand (VXRT Studio grown
  up).

## Order of work

1. Translator phases 2b–5 (TRANSLATOR.md), with the mixed-ABI choices above built in: it is also what makes complete
   mediation possible.
2. Safety: per-app permissions, snapshots and rollback of each app's overlay, a log of what apps and agents did.
3. Observation and operation through MCP: the control tree and frames first (they cover the most apps), then
   per-title game state for the games with profiles; AI repairs as a tested, approved, reversible flow.
4. One launcher for Windows and Mac apps with the AI at hand; ARM64 PEs next to x86-64 ones; macOS programs loading
   Windows DLLs through VXRuntime as a library.

Each step ships only with the measurements that show it (harness runs, differential tests, playtests), and states
what it does not cover.
