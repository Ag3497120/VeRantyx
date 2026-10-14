# Working on VeRantyx with an AI coding tool

This file is for AI coding agents (Claude Code, Codex, Gemini CLI, Cursor and others) asked to tune a Windows VR game
for a Mac, or to make one run on VXRuntime. A prompt for such a task usually comes from `tools/vxrt_doctor.py`,
which collects the facts about the game, the Mac and the last run. Read this file before you change anything.

## What this repository is

- **VXRuntime (`VXRuntime/`)**: a Windows runtime written from scratch for Apple silicon (no Wine). One macOS
  x86-64 process under Rosetta; Windows APIs are C/Objective-C functions (`VXRuntime/src`), Direct3D 11 is translated
  to Metal by our own DXBC→MSL translator (`dxbc.c`, `d3d11*.m`), OpenGL goes to macOS's OpenGL (`opengl.c`).
  `VXRuntime/README.md` has the design and the history of every milestone.
- **SteamVR on VXRuntime**: the real Windows SteamVR runs on VXRuntime with our driver (`VRDriver/`), which hands the
  frames to the Mac encoder (`MacEncoder/HardwareEncoder.swift`, HEVC) that streams them to the Vision Pro app
  (`VisionProClient/`).
- **Steam**: games reach the Mac Steam client through VXRuntime's Steam bridge (`VXRT_STEAM_BRIDGE=1`), signed in with
  the user's own account.

## Rules (always)

- Never circumvent DRM, anti-cheat or license checks, and never fake ownership. If a game has anti-cheat
  (Easy Anti-Cheat, BattlEye, ...), say so and stop; do not try to make it run.
- Never modify, patch or redistribute game files, Valve's or Microsoft's files. Writes go to overlays
  (`VXRT_OVERLAY`), never into the game folder or the Wine prefix.
- No machine code generated at run time: write C / Objective-C / generated C stubs.
- Ask the user before downloading anything, posting anything, or any action outside this repository.
- Do not run a game while the user is playing (check `pgrep -fl "build/vxrt"` first); never run two SteamVR
  instances at once.
- Commit only what you have verified (tests pass, and a run shows it works); say plainly what you did not verify.
- Code style: match the surrounding code (comment density, naming). Comments say why; numbers measured go in the
  commit message.

## Build and test

```bash
make -C VXRuntime            # build/vxrt
make -C VXRuntime check      # all runtime tests (must pass after every change)
VXRuntime/build/vxrt --list-exports | grep -i <function>   # is a Windows function implemented?
```

## Running a game

```bash
scripts/launch_vxrt_alyx.sh                                   # Half-Life: Alyx (own launcher)
scripts/launch_vxrt_game.sh "<game .exe>" [arguments...]      # any other SteamVR game
VXRT_VR_QUALITY=3k scripts/launch_vxrt_game.sh "<game .exe>"  # with a quality preset
```

Logs go to `logs/` (`vxrt-<exe>.log` for the game, `vxrt-steamvr.log`, `hardware-encoder.log`); frame metrics to
`~/Verantyx_VR_Drive/vxrt_root/drive_c/vr_openvr_capture_proxy.log` (lines `Capture metrics: submit_fps=...`).
For tests without the headset, use separate roots so the user's headset is not disturbed:
`VXRT_VR_ROOT=/tmp/x/root VXRT_VR_OVERLAY=/tmp/x/overlay VXRT_VR_LOG_DIR=/tmp/x/logs`, and feed a head pose into
`<root>/drive_c/vr_shared_frame.dat` (layout: `struct SharedHands` in `VRDriver/src/driver_verantyx.cpp`) or SteamVR will not give the game focus.

## Measuring (report numbers, not impressions)

| What | How |
| --- | --- |
| Frame rate | `submit_fps` in the capture proxy log; `capture=` / `encoded=` in `hardware-encoder.log` |
| GPU busy | `ioreg -r -d1 -c IOAccelerator \| grep "Device Utilization"` while running |
| GPU time per render pass | `VXRT_GPU_PROFILE=1` (the costliest passes every 4 s, in the game's log) |
| Render passes, memory traffic, what ends passes | `VXRT_STATS=1` |
| CPU hot spots | `VXRT_PROFILE=/path/file` |
| Audio gaps | the game's log: `audio: N gaps of silence` |
| Why it quit | the game's log: `ExitProcess(...) from ...`, `MessageBox:`, `called an unimplemented function` |
| Calls into the runtime | `VXRT_TRACE_CALLS=N` (N calls per function); `VXRT_TRACE=1` everything |
| Failed file opens / GL shaders | `VXRT_TRACE_FILES=1`, `VXRT_TRACE_GL=1` |

Other programs on the Mac (Photos' `mediaanalysisd`, other sessions) skew GPU numbers: check `uptime`/`top`, and
compare settings in alternating runs (A B B A), not one after another.

## Settings you may tune (a "tune" task changes only these)

| Setting | Meaning |
| --- | --- |
| `VXRT_VR_QUALITY=3k` / `2.5k` | presets: render size, stream size, display rate (see `scripts/launch_vxrt_steamvr.sh`) |
| `VXRT_VR_RENDER_SCALE` (0.5–4) | the driver's render size per eye relative to its default |
| `VERANTYX_DISPLAY_SCALE` | SteamVR compositor output size |
| `VERANTYX_DISPLAY_HZ` (60/72/90) | the virtual display's rate |
| `VERANTYX_STREAM_WIDTH/HEIGHT` | the stream's pixel budget (aspect follows the eyes); one HEVC session encodes ~60 fps of 2880x1620 |
| `VERANTYX_BITRATE_MBPS`, `VERANTYX_HEVC_QUALITY` (0–1) | stream bitrate and quality target |
| `VXRT_MSAA_MAX` (1/2/4), `VXRT_MAX_ANISOTROPY` | caps on multisampling and anisotropic filtering |
| `VXRT_VR_SETTINGS='{"steamvr": {...}}'` | SteamVR settings merged into the overlay's copy |
| game arguments | e.g. Source 2: `-noautofidelity +vr_fidelity_level N` |

Goal of a tune: frames at the display rate (`submit_fps` ≈ `VERANTYX_DISPLAY_HZ`), then the highest resolution
that keeps it. A render size larger than the stream's budget only supersamples.

## Making a game run (a "support" task)

1. Run `tools/vxrt_doctor.py "<game>" --goal support` and launch the game with `scripts/launch_vxrt_game.sh`.
2. Find the first thing that stops it in the log: `called an unimplemented function: DLL!Name` (the stub stops the
   program), `DLL x not found`, `pipeline state failed`, `did not compile`, an unhandled exception, `ExitProcess`.
   Functions listed as missing by the doctor only matter once they are called.
3. Implement it where its DLL lives (`grep -n "VX_EXPORT(<neighbour>)" VXRuntime/src/*.c`): kernel32 functions in
   `kernel32.c`/`files.c`/`sync.c`, user32/gdi32 in `desktop.c`/`user32.c`/`gdi.c`, small leftovers in `extras.c`,
   Direct3D in `d3d11*.m`/`dxgi.m`, the shader translator in `dxbc.c`. Behave like Windows (return values, last
   error); a stub that returns failure is fine when the game handles it.
4. Add a test under `VXRuntime/tests` when the behaviour is not trivial; `make -C VXRuntime check` must pass.
5. Run the game again; repeat until it reaches the headset. Report what works, what is left, with logs and numbers.

Known limits: no Direct3D 12, no Vulkan, no tessellation; stream output only with a vertex shader's code (not from a
geometry shader); geometry shaders run as compute (and split render passes). Each is a larger project; say so rather than starting it unasked.
