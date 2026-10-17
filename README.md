# VeRantyx

**Play PC VR games from an Apple Silicon Mac — in an Apple Vision Pro with your hands and a pair of Joy-Con, or in any ALVR headset.**

VeRantyx runs Windows SteamVR games such as Half-Life: Alyx on the Mac, streams
every stereo frame to the headset in hardware-encoded HEVC, and sends the
headset's head and hand poses back. Two ways to see it:

- **The VeRantyx app on Apple Vision Pro** (`VisionProClient/`): hands are
  tracked by the Vision Pro; buttons, sticks and triggers come from Nintendo
  Switch Joy-Con (one in each hand), or the game is played with bare hands. No
  tracked VR controllers are needed: the PS VR2 Sense controllers that other
  Vision Pro ports of Alyx require are not sold in every region (not in Japan at
  the time of writing).
- **ALVR's client apps** (`VERANTYX_ALVR=1`, vxrt version): the Vision Pro's
  "ALVR" app from the App Store, and the ALVR apps of Meta Quest and Pico, with
  their own controllers. Checked with the Vision Pro's ALVR app; **Quest and Pico
  are not tested yet** (we have none: reports are welcome).

The newest version, **vxrt**, needs no Wine: the Windows game and the real
Windows SteamVR run on [VXRuntime](#the-vxrt-version-vxruntime), a Windows
runtime written from scratch for Apple silicon in this repository.

| Branch | How the game runs on the Mac | Status |
|---|---|---|
| `main`, [`vxrt`](https://github.com/Ag3497120/VeRantyx/tree/vxrt) | The Windows build and the real Windows SteamVR on **VXRuntime** (vxrt): no Wine, Direct3D 11 on Metal by our own translator, Steam through the Mac Steam client (`main` and `vxrt` are the same) | Newest and developed every day. Played in the headset (Vision Pro app and ALVR) on a MacBook Pro M1 Max and a MacBook Air M5. Alyx 3K preset (1440×1620 per eye) at 60 fps on the M1 Max; The Lab and Google Earth VR run too |
| [`native`](https://github.com/Ag3497120/VeRantyx/tree/native) | Valve's **Linux ARM64** build of Half-Life: Alyx (made for the Steam Frame) runs natively on macOS — no Wine, no Rosetta, no SteamVR — through a patched [Klepton](https://github.com/shinyquagsire23/Klepton) loader, a glibc compatibility layer and an OpenVR runtime of our own | 2026-10-11. Menu at 60 fps, levels played for 20+ minutes on an M1 Max; Alyx only |
| [`wine`](https://github.com/Ag3497120/VeRantyx/tree/wine) | The Windows build with the real Windows SteamVR under Wine 11, DXVK → MoltenVK, and a SteamVR driver of our own | 2026-10-10, kept as it was; ~30 fps in game on an M1 Max |

All versions share the Mac HEVC encoder (`MacEncoder/`) and the Vision Pro
app (`VisionProClient/`).

### Recorded 2026-10-14 (vxrt version, Vision Pro app)

![vxrt version: the Windows build of Half-Life: Alyx and the Windows SteamVR running on VXRuntime (no Wine) on a MacBook Pro, played in Apple Vision Pro — main menu, the quarantine zone, then after a level change the pistol, hands and fingers, the wrist pocket and reloading](docs/media/vxrt-highlight.gif)

[![vxrt version — watch the full recording on YouTube](docs/media/vxrt-poster.jpg)](https://youtu.be/qF09ETv1TpU)

*vxrt version, 52-second highlight: [docs/media/vxrt-highlight.mp4](docs/media/vxrt-highlight.mp4).
The main menu, the quarantine zone (a2_quarantine_entrance), then, after the
level change into a2_pistol, the pistol, hands and fingers, an item put in the
wrist pocket and reloading. The Windows build of the game and the Windows
SteamVR, on VXRuntime instead of Wine; 3K preset (1440×1620 per eye), streamed
at 3404×1372 HEVC Main10, 40 Mbps, on an M1 Max. Hands are Vision Pro hand
tracking, buttons and sticks are Joy-Con. The surfaces lit green in places
(fences, plants) were fixed on 2026-10-17. Full 4-minute unedited recording:
https://youtu.be/qF09ETv1TpU. Recordings of the native and Wine versions are
under [Other versions](#other-versions).*

MIT licensed. No proprietary code is included (see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)): SteamVR and the games come
from your own Steam account; the Microsoft runtime DLLs the games need are taken
from the redistributable installers your own SteamVR and games carry, on your
Mac only; ALVR's streaming core (MIT) is fetched when it is built. The vxrt
version uses no Wine, DXVK or MoltenVK at all. The Wine version's setup
downloads Wine, DXVK and Steam from their publishers; Apple's Game Porting
Toolkit is neither shipped nor needed (an optional, experimental Wine-version
backend, `VERANTYX_D3D_BACKEND=d3dmetal`, uses D3DMetal only from a copy you
obtained from Apple yourself, [docs/SETUP.md](docs/SETUP.md)).

## Quick start (vxrt version)

You need an Apple silicon Mac with Rosetta 2, Xcode (or its command line tools),
[Homebrew](https://brew.sh), the Mac Steam client signed in to your account with
the games you own, and a headset: an Apple Vision Pro, or an ALVR 20.x client.

```bash
git clone https://github.com/Ag3497120/VeRantyx.git && cd VeRantyx
scripts/setup_vxrt.sh <steam-login> 546560 348250    # builds; SteamVR, Half-Life: Alyx, Google Earth VR (steamcmd)
scripts/setup_vxrt.sh --import "<a Steam library>"   # or: apps you already have, as APFS clones (no download)
```

SteamVR and the games go to `~/Verantyx_VR_Drive/vxrt_steam`, a Steam library
of its own (`tools/vxrt_steam.py fetch|import|status`). The Microsoft runtime
DLLs (msvcp140, vcruntime140, msvcp100, ...) are taken out of the
redistributable installers your own SteamVR and games carry
(`_CommonRedist/vcredist/*/VC_redist.x64.exe`, DirectX cabinets), opened without
running them, into `~/Verantyx_VR_Drive/vxrt_redist` (`tools/vxrt_redist.py`).
Nothing of Valve's or Microsoft's is in this repository or downloaded by it.
A Mac set up for the Wine version keeps working: its prefix is read (never
written) only while `vxrt_steam` has no SteamVR; `VXRT_NO_WINE=1` forbids it.
Then, with Steam running on the Mac and signed in:

```bash
scripts/launch_vxrt_alyx.sh                      # Vision Pro app: SteamVR on vxrt + encoder + Half-Life: Alyx
VXRT_VR_QUALITY=3k scripts/launch_vxrt_alyx.sh   # 1440×1620 per eye, 2880×1620 10-bit HEVC at 40 Mbps, 60 fps
VXRT_VR_QUALITY=2.5k scripts/launch_vxrt_alyx.sh # 2560×2880 per eye rendered (2× MSAA), same stream: ~50 fps in the menu, 30–37 in chapter 1
VXRT_VR_QUALITY=max scripts/launch_vxrt_alyx.sh  # 3200×3600 per eye (a Vision Pro eye's pixels), 3840×2160 stream, 16–21 fps
VERANTYX_ALVR=1 scripts/launch_vxrt_alyx.sh      # ALVR client apps instead (needs Rust, https://rustup.rs, the first time)
```

The first start compiles the game's shaders (a few minutes). The 10-bit stream
needs the current Vision Pro app (`scripts/deploy_vision_pro_app.sh`). The
quality presets are made for the Vision Pro app; with ALVR, leave them out at
first (see [Headsets](#headsets)).

Other SteamVR games: `scripts/launch_vxrt_game.sh "<game .exe>"` (The Lab and
Google Earth VR run; most games still need some Windows functions VXRuntime lacks).
What is known about each game is in [games/](games/).

## Headsets

| | VeRantyx app (Apple Vision Pro) | ALVR client apps (`VERANTYX_ALVR=1`) |
|---|---|---|
| Headsets | Apple Vision Pro (M2 or M5) | ALVR 20.x: the Vision Pro's "ALVR" (App Store), Meta Quest, Pico |
| Checked | Yes: Alyx, The Lab, Earth VR | The Vision Pro's ALVR app: Alyx and Earth VR (2026-10-17, MacBook Pro M1 Max and MacBook Air M5). **Quest, Pico: not tested yet** |
| Controls | Vision Pro hand tracking + Joy-Con (or bare hands) | The headset's controllers or hand tracking, through ALVR |
| Stream | HEVC (Main10 with the 3K preset), VeRantyx's own UDP protocol, each frame drawn at the game's exact frustum and pose | HEVC through ALVR's own streaming core (unchanged, v20.14.1), each frame stamped with the ALVR tracking sample it was rendered for |
| Audio | Core Audio process tap → the app | The same tap, given to ALVR as a device (the Mac's speakers mute while a headset listens) |
| Vibration | — | Passed on to the controllers (the game's pulses reach ALVR); felt in a controller: not checked yet (no Quest) |

With ALVR the headset shows its own rate (90 Hz on the Vision Pro) and stretches
the last frame to the head's turn until the next comes: when the game runs far
below that rate, turning the head warps the picture. Alyx at the 3K preset on a
MacBook Air M5 did; with ALVR's defaults (no preset) it did not. Details,
settings and what was checked: [ALVRBridge/README.md](ALVRBridge/README.md).

### Testing with a Quest or Pico (ALVR)

Not tested on either yet – we have neither. If you do, a report helps a lot, whether it works or not.

1. Set up the Mac as in [Quick start](#quick-start-vxrt-version), and install Rust (https://rustup.rs) for the ALVR
   part, which is built at the first start.
2. Install ALVR's client, version 20.x, on the headset (from ALVR's GitHub releases or SideQuest).
3. With Steam running on the Mac, start the game for ALVR:
   ```bash
   VERANTYX_ALVR=1 scripts/launch_vxrt_alyx.sh                      # Air and base chips: ALVR's defaults
   VERANTYX_ALVR=1 VXRT_VR_QUALITY=3k scripts/launch_vxrt_alyx.sh   # Pro/Max chips (24+ GPU cores)
   ```
4. Open ALVR on the headset on the same Wi-Fi (5 GHz, 80 MHz channel if you can). It connects by itself; the first
   time it connects once, applies the headset's display settings and connects again.
5. Report with the issue form "A headset through ALVR": what worked (picture, head, controllers, buttons, hand
   tracking, audio, vibration), how smooth it was, and, if something failed,
   `~/Library/Application Support/VeRantyx/ALVR/session_log.txt` and `logs/hardware-encoder.log`.

USB (wired) streaming is not wired up yet: use Wi-Fi.

## Status and limits (vxrt version)

2026-10-17, MacBook Pro (M1 Max) and MacBook Air (M5):

- **Half-Life: Alyx** plays in the headset with hands and Joy-Con, through
  level changes and saves (the 2026-10-14 recording goes from
  a2_quarantine_entrance into a2_pistol), and through the Vision Pro's ALVR
  app with game audio and hand tracking. Harness runs on the M1 Max: chapter 1
  at 60.0 fps with the 3K preset (20261017T022746Z-alyx-chapter-3k).
- **MacBook Air M5** (10 GPU cores, 24 GB): Alyx through ALVR at its defaults,
  27 fps median in game (20–34) in the session of 2026-10-17 (with
  `VXRT_FLASH_CAPTURE` on); the 3K preset is too much for it (see
  [Headsets](#headsets)).
- **Fixed on 2026-10-17:** fences, plants, fog and bottles that lit in one
  strong colour (red, green or blue) for single frames. A shader read beyond a
  constant buffer the game binds smaller than the shader declares; Direct3D
  reads 0 there, the translation read other draws' constants. Found with
  `VXRT_FLASH_CAPTURE` (a capture of the frames around such a flash).
- **Wi-Fi:** the stream needs a fast link. On 2.4 GHz it stutters and smears;
  5 GHz with an 80 MHz channel is much better.
- **max preset:** 16–21 fps on the M1 Max (3K holds 60).
- **First start** compiles thousands of shaders (a few minutes); Metal caches them.
- **Saves** made before 2026-10-14 (commit 8d10cfb) have no level state; start
  the chapter again.
- **Setup** without Wine (`scripts/setup_vxrt.sh`): checked on 2026-10-15 with
  `VXRT_NO_WINE=1` (the prefix never used) in harness runs: Alyx chapter 1 at
  3K 60.0 fps (20261015T005456Z-alyx-chapter-3k), Google Earth VR 57.4 fps
  (20261015T005918Z-earth-start), The Lab 54.8 fps (20261015T010214Z-thelab-title),
  with SteamVR and the games imported and the DLLs from SteamVR's redistributables.
  The steamcmd download path itself has not been run here (it needs your sign-in).
- Not there: Direct3D 12, Vulkan, tessellation, anti-cheat (never worked around).

## Why VeRantyx

- **No tracked VR controllers needed (Vision Pro app).** Vision Pro hand tracking gives the hands'
  position; Joy-Con gyro and accelerometer are fused in for fast motion and
  for when a hand leaves the cameras' view, and the grip pose is carried from
  the wrist while a Joy-Con hides the knuckles. Gravity gloves, grabbing,
  pinch-to-trigger and finger animation (native version) all work without
  PS VR2 Sense controllers.
- **Any Vision Pro, and ALVR headsets.** The game renders on the Mac's GPU, so
  a first-generation (M2) Vision Pro plays the same as a new one; the headset
  only decodes and displays. With `VERANTYX_ALVR=1` the vxrt version streams to
  ALVR's client apps instead (the Vision Pro's "ALVR", Meta Quest, Pico).
- **Your own copy, nothing redistributed.** The game, SteamVR (Wine and vxrt
  versions) and Steam come from your own Steam account; this repository contains no
  Valve or Apple code apart from Valve's BSD-licensed OpenVR headers.
- **Windows VR games without Wine.** VXRuntime, a Windows runtime written for
  Apple silicon in this repository, runs the Windows build of the game and the
  real Windows SteamVR with Direct3D 11 translated straight to Metal: Alyx at
  60 fps where Wine managed ~30 on the same Mac, and other Windows SteamVR
  games (The Lab, Google Earth VR) as well.
- **Native ARM version, no emulation.** Valve's own ARM64 Linux build runs on
  Apple Silicon directly, with a small OpenVR runtime instead of SteamVR:
  the menu holds 60 fps where the Wine version managed ~30, shader compiles
  run off the render thread, and game audio plays through Core Audio.
- **Accurate image.** Each eye is drawn at the exact frustum the game rendered
  with, and every frame carries the head pose it was rendered for, so the
  headset reprojects correctly.
- **Open source, MIT.** Every part — Windows runtime, Mac launcher, glibc layer,
  OpenVR runtime, encoder, visionOS app — is in this repository and can be read
  and rebuilt.


## Compared with MoltenVR

[MoltenVR](https://github.com/MoltenVR/MoltenVR-Preview) (first public
preview 2026-07-04, v1.0.0 2026-07-12) also plays PC VR games from an Apple
Silicon Mac, but takes a different route. From its README and this
repository (vxrt version):

| | VeRantyx (vxrt) | MoltenVR |
|---|---|---|
| Windows layer | VXRuntime, written for this project: no Wine | Wine bottles (WhiskyWine / GPTK: Whisky, CrossOver, Wineskin) |
| SteamVR | Runs the real Windows SteamVR; our driver provides the HMD and controllers, and the game's `Submit` calls are hooked to capture frames | Does not run SteamVR; OpenComposite translates OpenVR calls to its own OpenXR runtime, which receives every frame directly |
| D3D11 on Metal | Our own translator (DXBC → Metal Shading Language), one layer | DXMT (one layer) |
| Video | HEVC: to the Vision Pro app (own UDP protocol, exact render pose per frame) or through ALVR's streaming core | H.264, ALVR protocol, USB (100 Mbps) or Wi-Fi, 72 fps claimed |
| Headset | Apple Vision Pro (own visionOS app), and ALVR clients: the Vision Pro's ALVR app checked, Quest and Pico not tested yet | Any ALVR client (tested on Quest 3) |
| Input | Vision Pro app: hand tracking + Joy-Con (buttons, sticks, triggers, inertial sensors); ALVR: the headset's controllers or hands, vibration passed on (not felt yet: no Quest) | VR controllers through ALVR: 6DoF, buttons, sticks, haptics |
| Audio | Core Audio process tap, no extra driver (also to ALVR) | BlackHole virtual audio device |
| Games checked | Half-Life: Alyx, The Lab, Google Earth VR | Half-Life: Alyx, Beat Saber, Minecraft (Vivecraft) and more, per its author |
| Source | MIT, full source | Binaries only, 7-day trial then license key |

Running SteamVR keeps SteamVR Input (the game's own controller bindings),
chaperone and the dashboard working, at the cost of `vrserver`/`vrcompositor`
CPU time; MoltenVR's design avoids that cost but needs OpenComposite to cover
each game's OpenVR usage. Wine runs many more Windows programs than VXRuntime
does today; VXRuntime is what lets this project change every layer down to the
shader translation (the fix above is one example).

## The vxrt version: VXRuntime

### What vxrt is

VXRuntime ("vxrt", in [`VXRuntime/`](VXRuntime/)) is a Windows compatibility
layer written from scratch for Apple silicon Macs. It loads Windows programs
(PE files) and gives them the Windows API they call, implemented directly on
macOS: files, the registry, threads and synchronization, exceptions, windows
and input on AppKit, sockets, COM, audio on Core Audio, and Direct3D 11 / DXGI
on Metal. With it, the Windows build of Half-Life: Alyx and the real Windows
SteamVR (vrserver, vrcompositor and the VeRantyx driver) run on the Mac without
Wine, and the game reaches the Steam client you already run on macOS.

It was started on 2026-10-12 because Wine was the bottleneck of the Wine
version (the Mac was CPU-bound at ~30 fps, with wineserver, Steam's Chromium
and two layers of graphics translation all on the CPU) and because Wine's own
design blocked Apple's D3DMetal. vxrt runs the Windows build, so it is not
tied to a game having a Linux ARM64 build (Alyx is the only SteamVR title with
one, made for the Steam Frame).

### How it is built

```
 One macOS x86-64 process per Windows process (Rosetta 2 translates both together)
 ┌────────────────────────────────────────────────────────────────────────────┐
 │ hlvr.exe, vrcompositor.exe, ... (unchanged Windows PE code)                │
 │   │ calls Windows functions directly (ms_abi), no Unix/PE boundary         │
 │   ▼                                                                        │
 │ vxrt: PE loader · kernel32/ntdll · SEH and C++ exceptions · files/registry │
 │       user32/gdi on AppKit and Core Graphics · ws2_32 · COM · winmm/WASAPI │
 │       Direct3D 11/DXGI on Metal (DXBC → Metal Shading Language)            │
 │       shared textures (IOSurface + keyed mutex) across processes           │
 │       Steam bridge → the Mac Steam client's steamclient.dylib              │
 └────────────────────────────────────────────────────────────────────────────┘
```

- **No Unix half.** In Wine a Windows API call crosses from PE DLLs into a
  separate Unix side, and kernel objects shared between processes go through
  wineserver. In vxrt the Windows API is native Mach-O code with the Windows
  calling convention, called by the program directly; kernel objects live in
  the process, and only named objects other processes need are put in shared
  memory.
- **Thread state as macOS has it.** `gs` stays macOS's thread-specific data;
  the TEB pointer and TLS array are written into two of its slots, so Windows
  code, the runtime and Metal share a thread without switching anything.
- **Graphics straight to Metal.** Direct3D 11 calls become Metal calls, and
  shaders are translated from DXBC to Metal Shading Language once (in the
  background, cached by Metal). The Wine version went D3D11 → Vulkan (DXVK) →
  Metal (MoltenVK). Geometry shaders run as compute kernels, stream output
  (Unity's skinning) as well; textures shared
  between the game, the compositor and the driver are IOSurfaces with keyed
  mutexes, so SteamVR's compositor accepts the game's frames directly (under
  Wine it could not open them).
- **Steam without Windows Steam.** The game's `steamclient64.dll` is a
  built-in that forwards every Steam interface to the Mac Steam client you are
  signed in to (static ms_abi → System V stubs per interface method), so no
  Windows Steam and no Chromium run.
- **No code made at run time.** Method tables and call stubs are generated
  ahead of time or written in assembly; Valve's and Microsoft's files are used
  from your own installation and never shipped.
- **Built milestone by milestone**, each a small Windows program that has to
  run unchanged (`make check`, about 25 test programs): PE loader and kernel32 →
  threads and exceptions → files and DLLs → windows on AppKit → COM, I/O
  completion ports, MSVC C++ exceptions → Direct3D 11 on Metal → compute,
  input, audio → processes and shared textures (SteamVR) → the Steam bridge
  (Alyx) → geometry shaders → the 3K/10-bit work → Unity games (The Lab) →
  play in the headset (saves, level changes, multisampling). About 31,000 lines
  of C and Objective-C, written in three days (2026-10-12 to 14) with Claude
  Opus 5.5 in Claude Code. Design notes, every milestone and its test:
  [VXRuntime/README.md](VXRuntime/README.md).

### Compared with the Wine and native versions

Half-Life: Alyx, MacBook Pro M1 Max, measured with each version's own logs.

| | Wine version | Native version | vxrt version |
|---|---|---|---|
| Game build | Windows (x86-64) | Linux ARM64 (Steam Frame build) | Windows (x86-64) |
| CPU code | Rosetta 2 + Wine | Native ARM64 (no translation) | Rosetta 2, no Wine |
| VR runtime | Real SteamVR under Wine | Our own OpenVR runtime | Real SteamVR on vxrt |
| Graphics | D3D11 → DXVK → Vulkan → MoltenVK → Metal | Vulkan → MoltenVK → Metal | D3D11 → Metal (own translator) |
| Steam | Windows Steam + Chromium under Wine | Mac Steam client | Mac Steam client (bridge) |
| In game, default size | ~30 fps (CPU-bound, SteamVR scale 0.7) | 60 fps, 1298×1056 per eye | 45–57 fps, ~840×940 per eye |
| Larger sizes | — | 1982×1536 per eye ~39 fps; ~3K per eye ~20 fps | 3K preset: 1440×1620 per eye at 60 fps, streamed at 60 fps (HEVC Main10, ~4.7 megapixels); 2.5K preset: 2560×2880 per eye (2× MSAA), 50–55 fps in the menu but 30–37 in chapter 1 (GPU-bound; 2026-10-17); max preset: 3200×3600 per eye at 16–21 fps |
| Other Windows VR games | Possible (Wine) | No: needs a Linux ARM64 build | The Lab (title scene 53–58 fps) and Google Earth VR run; Bigscreen does not start yet |
| Played in the headset | Yes | Yes (20+ min sessions) | Yes (2026-10-14, across level changes, 3K) |

In short: the native version is the fastest (no CPU translation) but exists
only because Valve made an ARM64 Alyx; the Wine version runs any Windows game
but pays for Wine and double graphics translation; vxrt keeps the Windows build
and the real SteamVR while removing Wine, and is the one we can optimize down
to every call. It is a research project: Direct3D 12, Vulkan and tessellation
are not there yet, and it has been tested with four games on two Macs.

Where it is going: [docs/VISION.md](docs/VISION.md): Windows apps on a Mac without Rosetta or Wine, x86-64 and
ARM64 code mixed in one process, and AI agents that observe and operate the apps through structure (with the
user's permission).

### Beyond games

- **Its own x86-64 → ARM64 translator** (in progress,
  [VXRuntime/TRANSLATOR.md](VXRuntime/TRANSLATOR.md)): VXRuntime as a native
  ARM64 program that translates only the program's code, for when Rosetta 2 is
  gone. Its test programs and benchmarks run; games do not use it yet.
- **Command Clips** ([VXRuntime/CLIPS.md](VXRuntime/CLIPS.md)): a program on
  VXRuntime can be read and operated through a socket (its windows, the text
  drawn in them, clicks and keys as Windows messages, and when the result is
  painted), so an AI agent works with it as with a command-line tool instead of
  screenshots.

### Tuning a game or making one run, with an AI coding tool

Open the repository in your AI coding tool and use its commands; they are the
same in Claude Code, Codex, Gemini CLI and Cursor:

```
/vxrt-games              # what is installed, what is known to work, the latest runs
/vxrt-support thelab     # make a game run: measure, fix VXRuntime, prove it with a run
/vxrt-tune alyx          # settings only, for this Mac: alternating runs, a launch command
/vxrt-playtest alyx      # you play it in the headset; it records where it goes wrong, then reads it back
/vxrt-issue earth        # still failing: an issue draft from the evidence (only you post it)
```

**When does a game "work"?** Every harness run reports the level it reached:
`starts` → `connects` (reaches SteamVR) → `draws` (frames not black) → `runs`
(the whole scenario) → `controls` (SteamVR loaded bindings for its
controllers). The last level, `playable`, only a person gives: a playtest,
`tools/vxrt_playtest.py <game>`, starts the game as you would and records head
and hand tracking, triggers, grips, buttons, sticks and the frame rate every
second while you play with your controllers; click both sticks for a second
(or press Enter in its Terminal) where something goes wrong, and it keeps the
frame and the game's log at that moment. At the end it asks a short checklist
(picture, head, hands, buttons, movement, menus, sound). Playable: those five
yes, controller input seen, no crash, ten minutes or more. A game's profile
says `works` only after such a playtest (`--apply`); its problems are what
`/vxrt-support` fixes next.

A game is named by its profile key, Steam app id, or folder/.exe. Each tool
gets the commands, the vxrt MCP server and the rules ([AGENTS.md](AGENTS.md))
in the form it reads (`.claude/`, `.codex/config.toml`, `.gemini/` + `GEMINI.md`,
`.cursor/`), all written by `tools/vxrt_agents.py` from one source
(`.agents/commands/`, `.mcp.json`, AGENTS.md); workflows are in `.agents/skills/`.
Codex reads `.codex/config.toml` once you trust the project.

The commands start from a brief written by `tools/vxrt_doctor.py`, which looks
at the game without running it (executables and DLLs, engine, graphics API,
anti-cheat, the Windows functions VXRuntime does not have yet) and at its last
log. The brief says the goal, when the task is done (checkable with the
harness), the facts as JSON, the tools to use first (worded for the tool:
`--agent claude|codex|gemini|cursor`), the rules, when to stop and what to
report. Nothing is sent anywhere by the script. To paste it yourself:

```bash
tools/vxrt_doctor.py thelab --goal support --agent codex --copy   # make it run: changes VXRuntime
tools/vxrt_doctor.py alyx --goal tune --copy                      # settings only, for this Mac
tools/vxrt_doctor.py earth --facts                                # only the facts, as JSON
```

Briefs are in Japanese by default (`--lang en` for English). Games with
anti-cheat are flagged and not worked around.

**Auto-tuning.** `tools/vxrt_tune.py <game>` finds the best picture a game
holds at the display rate on your Mac, without a headset: it runs the harness
up a ladder of settings from the fewest pixels per eye to the most (default,
3K, 3K at render scale 2.4, 2.5K, max; a game's profile can have its own), stops
where a setting no longer holds 95 % of the display rate, and confirms the
highest one that does in alternating runs. `--plan` shows the ladder and the
time first (half an hour or more: you cannot play meanwhile); `--apply` writes
the confirmed choice into the game's profile for your Mac, and
`tools/vxrt_tune.py --show <game>` (or VXRT Studio's quality menu, "tuned for
this Mac") gives its launch command. `/vxrt-tune <game>` has your AI tool run
it (MCP `tune_game` / `read_tune`) and report it with the run ids.

The same in a page: `python3 tools/vxrt_studio.py` opens VXRT Studio in the
browser, a list of the games installed for VXRuntime (`~/Verantyx_VR_Drive/vxrt_steam`,
`vxrt_library`, and the Wine version's libraries if there are any) with what is known to work, the
last run's problems, and buttons that copy a prompt (tune or make it run) and
the launch command, or hand the job to an agent: "Let the AI do it" opens Terminal
with Claude Code or Codex (Gemini CLI and Cursor's agent too, when installed; your own login) in this repository, given the prompt
and the vxrt MCP server, and the agent runs and measures the game itself. The
page is served on 127.0.0.1 only; starting an agent needs the page's own token.

The agents measure with a harness, `tools/vxrt_run.py` (also an MCP server,
`tools/vxrt_mcp.py`, registered for Claude Code in `.mcp.json`): it runs a game
in roots of its own with a simulated headset, refuses runs that would not be
fair (you are playing, SteamVR is up, VR is off...), and writes what it saw as
evidence (observed values with their files' hashes, claims derived against
frozen baselines, suspicions, unknowns) into `runs/`, sealed in a hash-chained
ledger. `make -C VXRuntime vr-check` runs every game with a baseline. Workflows
for agents are in `.agents/skills/`.

What is known about each game is in `games/<Steam app id>.json` (how it starts,
the scenarios measured, baselines, known issues; see [games/README.md](games/README.md)):
anyone can add or update one in a pull request with harness runs as evidence.
When a game still does not run, `tools/vxrt_issue.py draft <game>` (MCP:
`prepare_issue`) writes an issue for this repository from the evidence, with
personal details taken out; `tools/vxrt_issue.py submit` shows it and asks
before posting it.

## Compared with Glovebox

[Glovebox](https://glovebox.vision/) (Udi Wertheimer, October 2026) also runs
Valve's Linux ARM64 build of Alyx through Klepton, but on the Vision Pro
itself. Both are open source and use your own copy of the game.

| | VeRantyx (native branch) | Glovebox |
|---|---|---|
| Where the game runs | On the Mac; streamed to the headset | On the Vision Pro (no Mac while playing) |
| Controllers | Hands + Joy-Con, or hands only | PS VR2 Sense controllers required |
| Headset | Any Vision Pro (M2 or M5) | Reported on an M5 Vision Pro |
| Image | Limited by the Mac's GPU and the Wi-Fi stream (1298×1056 per eye at 60 fps on an M1 Max) | ~3K per eye at 60 fps reported, no streaming |
| VR layer | Our own OpenVR runtime | — |

Not affiliated with Valve, Apple, Nintendo, Sony, Glovebox or Klepton.

## Other versions

The native and Wine versions, as they were when they were made (their own
branches have them).

### Native version (Linux ARM64 build, no Wine) — recorded 2026-10-11

![Native version: Half-Life: Alyx (Linux ARM64 build) running natively on a MacBook Pro, played in Apple Vision Pro — the Joy-Con, hands and fingers, gravity gloves, reloading a pistol](docs/media/native-highlight.gif)

[![Native version — watch the full recording on YouTube](docs/media/native-poster.jpg)](https://youtu.be/2z-GyAqAtpA)

*Native version, 48-second highlight: [docs/media/native-highlight.mp4](docs/media/native-highlight.mp4).
The Joy-Con it is played with (seen through the passthrough), hands and fingers
in Russell's lab, gravity gloves (pulling and catching a helmet), then
reloading the pistol. 60 fps on an M1 Max. Full 6-minute unedited recording of the native version: https://youtu.be/2z-GyAqAtpA.*

### Wine version (Windows build + SteamVR under Wine) — recorded 2026-10-10

<!-- VIDEO: after uploading docs/media/highlight.mp4 through the README editor
     on github.com, replace the poster link below with the generated
     https://github.com/user-attachments/... URL on its own line; GitHub then
     renders a player. -->
![Wine version: Half-Life: Alyx in Apple Vision Pro, rendered on a MacBook Pro (54-second highlight; the last seconds show the room through the headset's passthrough)](docs/media/highlight.gif)

[![Wine version — watch the full recording on YouTube](docs/media/poster.jpg)](https://youtu.be/6xVGgdChDes)

*Wine version, 54-second highlight: [docs/media/highlight.mp4](docs/media/highlight.mp4) — the last seconds show the room through the Vision Pro's passthrough, where it was recorded. Full 6-minute unedited recording of the Wine version: https://youtu.be/6xVGgdChDes.*

### Setup — native version

Step by step in **[docs/NATIVE_SETUP.md](docs/NATIVE_SETUP.md)**: download the
Linux depot with your own Steam account, fetch Klepton and the Debian
libraries, build the launcher, sign in to macOS Steam, build the visionOS
app, then:

```bash
NativeARM/scripts/launch_native_vr.sh
```

Internals, findings and status: [NativeARM/README.md](NativeARM/README.md).

### Setup — Wine version

```bash
git clone https://github.com/Ag3497120/VeRantyx.git
cd VeRantyx
./scripts/setup.sh
```

Then install SteamVR and the game through Windows Steam, build the visionOS
app once in Xcode with your team, and play:

```bash
WINEPREFIX="$HOME/Verantyx_VR_Drive/SteamVR_Prefix_Wine11_Test" ./scripts/launch_vision_pro_vr.sh
```

The full walkthrough, requirements, tuning variables and troubleshooting are
in **[docs/SETUP.md](docs/SETUP.md)**. A Japanese write-up of the internals is
in [docs/README_ja.md](docs/README_ja.md).

### How the native version works

```
 Mac (Apple Silicon)                                         Apple Vision Pro
 ┌───────────────────────────────────────────────┐           ┌────────────────────────┐
 │ alyx_host (NativeARM/host)                    │           │ VeRantyx visionOS app  │
 │  ├─ Klepton loader (patched): Valve's ARM64    │   Wi-Fi   │  (same as the Wine     │
 │  │   libengine2, libclient, … as guest code    │  HEVC UDP │   version)             │
 │  ├─ glibc layer: TLS, locale, sockets, SysV,   │ ────────▶ │                        │
 │  │   epoll/eventfd, CPU topology, …            │           │                        │
 │  ├─ SDL audio → Core Audio, Steam API → macOS  │  poses +  │                        │
 │  │   Steam client                              │  inputs   │                        │
 │  ├─ OpenVR runtime (NativeARM/vrruntime):      │ ◀──────── │                        │
 │  │   poses, input bindings, finger skeleton,   │           │                        │
 │  │   Vulkan readback → shared frame file       │           │                        │
 │  └─ MoltenVK: Vulkan → Metal, async pipelines  │           │                        │
 │ HardwareEncoder: HEVC + game audio → UDP       │           │                        │
 └───────────────────────────────────────────────┘           └────────────────────────┘
```

### How the Wine version works

```
 Mac (Apple Silicon)                                        Apple Vision Pro
 ┌──────────────────────────────────────────────┐           ┌──────────────────────────────┐
 │ Wine 11 (Staging)                            │   Wi-Fi   │ VeRantyx visionOS app        │
 │  ├─ Windows Steam + SteamVR (vrserver,       │  HEVC UDP │  ├─ UDP reassembly + HEVC    │
 │  │   vrcompositor via DXVK/MoltenVK)         │ ────────▶ │  │   decode (VideoToolbox)    │
 │  │   └─ driver_verantyx.dll  (virtual HMD +  │           │  ├─ Compositor Services:      │
 │  │       2 Touch controllers, pose thread)   │  poses +  │  │   each eye drawn at the    │
 │  └─ Half-Life: Alyx (hlvr.exe)               │  inputs   │  │   game's frustum, exact    │
 │      └─ openvr_api.dll proxy: hooks          │ ◀──────── │  │   source-pose reprojection │
 │          IVRCompositor::Submit, reads both   │  (90 Hz)  │  ├─ ARKit head + hands,      │
 │          eye textures → shared memory        │           │  │   Joy-Con via GameControl. │
 │ HardwareEncoder (Swift): Metal scale/convert │           │  └─ AVAudioEngine playback    │
 │  → VideoToolbox HEVC → UDP; Core Audio tap   │  PCM UDP  │                              │
 │  → PCM; poses → shared memory for the driver │ ────────▶ │                              │
 └──────────────────────────────────────────────┘           └──────────────────────────────┘
```

- **Driver** (`VRDriver/src/driver_verantyx.cpp`, C++): a SteamVR server
  driver exposing a virtual HMD with the Vision Pro's measured eye frustums
  and two Oculus Touch controllers. A dedicated thread forwards each headset
  pose the moment it arrives, records the pose IDs it handed to SteamVR, and
  publishes Joy-Con/hand-derived velocities intended to give Alyx's gravity
  gloves a real wrist flick (not yet verified in game).
- **Capture proxy** (`VRDriver/src/openvr_capture_proxy.cpp`): replaces
  `openvr_api.dll` beside the game, patches the `IVRCompositor` vtable, copies
  the left and right eye rectangles of every `Submit` with a non-blocking
  staging ring, and matches the game's render pose against the driver's ring
  so every frame carries the exact pose it was rendered with. During a level
  load the SteamVR compositor's own output (the loading screen) is captured
  instead through a D3D11 proxy inside `vrcompositor`.
- **Encoder** (`MacEncoder/HardwareEncoder.swift`): reads the shared frame,
  scales and converts it on the GPU (Metal), encodes HEVC with VideoToolbox,
  fragments it into MTU-sized datagrams with pacing, and taps the Mac's audio
  output (Core Audio process tap, no virtual audio driver) into 5 ms PCM
  packets.
- **visionOS app** (`VisionProClient/VisionSpatialTools`, Swift): a
  `CompositorLayer` renderer that places each eye image at the angles the
  game rendered with and gives visionOS the matching device anchor for
  late reprojection; a pose sampler running once per displayed frame that
  predicts the head to the expected display time and converts hand joints to
  Touch controller poses; Joy-Con input with gyro/accelerometer fusion; a
  jitter-buffered audio player.
- **Scripts** (Bash/Python): reproducible DLL builds (byte-identical from the
  same source), atomic installs, Steam launch-option management, and the
  launch supervisor.

#### Languages

C++ (SteamVR driver, OpenVR capture proxy, D3D11 proxy; MinGW-w64 cross
compiled to Windows x64), C (Steam WebHelper shim), Swift (macOS encoder,
visionOS app), Metal Shading Language (frame conversion, eye-image rendering),
Bash and Python (build, install and launch scripts). The only third-party
source in the tree is Valve's OpenVR SDK headers.

### Measurements (Wine version)

MacBook Pro (M1 Max, 64 GB), macOS 26, Vision Pro on the same Wi-Fi 6 network,
Half-Life: Alyx, 2026-10-10. Numbers come from the pipeline's own logs
(`logs/hardware-encoder.log` and the game-side capture log).

| Metric | Value | Conditions |
|---|---|---|
| Game render rate (Alyx, in game) | 29–35 fps (median 30) | 1115×900 per eye (SteamVR scale 0.7, Alyx's automatic top fidelity level), 60 Hz virtual display; the default scale is now 0.55 |
| Game render rate, before resolution/compositor changes | 13–24 fps | 1636×1319 per eye, 90 Hz virtual display |
| Game render rate, main menu | 31–72 fps depending on the menu scene | |
| Frames received by the headset | up to 60 fps; 43–46 new frames/s in game | HEVC 20 Mbps, 2276×910 for both eyes |
| Capture → network latency (encoder) | 14.2 ms p50 (GPU convert) vs 18.5 ms (CPU) | synthetic 30 fps source, loopback |
| Pose sent → frame displayed | ~130–140 ms in game | includes game render time at ~30 fps |
| Mac CPU | ~93 % of 10 cores total: the game ~2.6 cores, `vrcompositor` ~0.5, `vrserver` ~0.25, Steam CEF ~0.25, encoder ~0.1 | the bottleneck is CPU, not GPU |

What dominates is the game itself under Wine plus SteamVR's own processes;
the streaming pipeline adds little. Lowering the virtual display from 90 Hz to
60 Hz and the SteamVR resolution scale raised the in-game rate from ~18 to
~30 fps.

### Native version: measurements and limits

MacBook Pro (M1 Max), 2026-10-11, from the runtime's own logs:

| | Value |
|---|---|
| Main menu | 60 fps at the 60 Hz virtual display (game thread ~4.5 ms/frame) |
| Per-eye image | 1298×1056 (default); `VERANTYX_QUALITY=high` 1982×1536 at ~39 fps |
| First-time shader compile | 250–500 ms per pipeline, now off the render thread; ~2 ms once cached |
| Play session | 20+ minutes in the first levels |

Limits: the Mac's GPU caps resolution (`ultra`, ~3K per eye, runs at ~20 fps
on an M1 Max); level-transition screens are not drawn; the game pauses while
the headset reports no user.

### Status and limits (Wine version)

- Verified: SteamVR + Half-Life: Alyx on the Mac above, streaming to a Vision Pro,
  playing with Joy-Con. Other games and other Macs are untested; the Mac
  profiles are derived from this machine's measurements.
- Gravity gloves: the wrist-flick velocity path is implemented (Joy-Con gyro and
  accelerometer fused into the controller velocity) but not yet verified in
  game.
- The Mac is CPU-bound at ~30 fps in game; the Vision Pro reprojects every
  frame to the current head pose, so head movement stays smooth, but hands and
  the world update at the game's rate.
- visionOS still opens Home when you look at your palm and pinch; this cannot
  be disabled by an app. With Joy-Con you rarely pinch.
- Hand tracking holds the hand's last head-relative position while it is out
  of the cameras' view; Joy-Con gyro/accelerometer move it meanwhile (right
  hand only: a merged pair exposes one sensor set).

### Other Macs (Wine version)

`scripts/mac_profile.sh` classes the Mac (performance cores, GPU cores,
memory) into a tier and the launch script takes that tier's defaults for the
SteamVR render scale, virtual display rate, bitrate and stream size; the
encoder picks GPU or CPU frame conversion by timing both and lowers the
bitrate while the Wi-Fi link loses packets. `scripts/bench_alyx.sh` measures
the game's frame rate and the CPU/GPU load on any Mac without a headset, so
settings can be compared before playing. See
[docs/SETUP.md](docs/SETUP.md#mac-profiles-and-tuning).

## Built on

| Project | Used for | License | In this repository? |
|---|---|---|---|
| [Klepton](https://github.com/shinyquagsire23/Klepton) by ShinyQuagsire | Native version: loading Valve's ARM64 ELF libraries on macOS, its bionic libc and Vulkan-on-MoltenVK runtime; our patch set adds glibc TLS, x18 and loader fixes, CPU topology, asynchronous pipelines (`NativeARM/klepton-patches/`) | MIT | No — fetched by `NativeARM/scripts/fetch_klepton.sh`, patched by `apply.py` |
| [MoltenVK](https://github.com/KhronosGroup/MoltenVK) | Vulkan → Metal (both versions; Wine version via DXVK) | Apache-2.0 | No (Klepton's build / DXVK) |
| Valve's Half-Life: Alyx (Linux ARM64 depot 546564, Windows build) | The game | Valve's terms | No — your own Steam account |
| Valve's [OpenVR SDK headers](https://github.com/ValveSoftware/openvr) | SteamVR driver, capture proxy, native OpenVR runtime | BSD-3-Clause | Yes (`VRDriver/include/`, `NativeARM/vrruntime/openvr/`) |
| [ALVR](https://github.com/alvr-org/ALVR) | vxrt version with `VERANTYX_ALVR=1`: its streaming core `alvr_server_core` v20.14.1, unchanged, behind `ALVRBridge/`; native version: OpenVR hand-skeleton bone poses for finger animation | MIT | Core: no — fetched by Cargo when `ALVRBridge` is built; hand data: yes, unchanged with its license (`NativeARM/vrruntime/hand_skeleton_alvr.inc`) |
| [Fossilize](https://github.com/ValveSoftware/Fossilize) | Optional shader-prewarm experiment (not used by the launcher) | MIT | No — fetched by `NativeARM/scripts/build_fossilize.sh` |
| Debian trixie arm64 packages | glib, libstdc++, ssl, png … the Linux libraries the game expects from the Steam Linux Runtime | various (DFSG) | No — fetched by `NativeARM/scripts/fetch_debian_libs.sh` |
| Wine Staging (Gcenx build), DXVK for macOS (Gcenx) | Wine version | LGPL-2.1, zlib | No — fetched by `scripts/setup.sh` |
| Windows SteamVR, Microsoft Visual C++ / DirectX runtime DLLs | vxrt version: SteamVR from your own Steam account (steamcmd); the DLLs taken from the redistributable installers your SteamVR and games carry, on your Mac only | Valve's and Microsoft's terms | No |

Details: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Project history

The project was started in May 2026 with Gemini 3.1 Pro (initial commit
2026-05-31: a mock SteamVR driver that got Half-Life: Alyx through its
initialization), then left alone until September 2026, when work resumed
with GPT-6 Astra. On 2026-10-09 it was brought to a working stream with
Opus 5.5, and on 2026-10-10 finished with Fable 5.1: exact pose matching,
Vision Pro frustum handling, Joy-Con input with gravity-glove support, game
audio, GPU frame conversion and the one-command setup. All of it was built
from scratch by the author with these models. On 2026-10-11 the native ARM
version was started and brought to playable levels the same day; it builds
on Klepton (fetched and patched, not included) and includes ALVR's hand
skeleton data, both MIT (see "Built on"). On 2026-10-12 and 13 VXRuntime was
written from scratch with Opus 5.5 in Claude Code, and the Windows build of
Alyx with the real SteamVR ran on it without Wine (the `vxrt` branch). On
2026-10-14 The Lab (Unity) ran on it as well, the harness, game profiles and
VXRT Studio let an AI coding tool measure and extend it, and Alyx on vxrt was
played in the headset through level changes. From 2026-10-15 to 17 came
VXRuntime's own x86-64 → ARM64 translator
([VXRuntime/TRANSLATOR.md](VXRuntime/TRANSLATOR.md), not yet used for games),
Command Clips (programs on VXRuntime read and operated through a socket instead
of screenshots, [VXRuntime/CLIPS.md](VXRuntime/CLIPS.md)), and ALVR: the frames
go to ALVR's client apps through ALVR's own streaming core, with game audio,
hand tracking and controllers. Alyx was played through the Vision Pro's ALVR
app on a MacBook Pro and on a MacBook Air M5 (its SteamVR and game cloned from
an existing library), and the last known rendering problem (surfaces lit in
one strong colour for single frames) was found with a frame capture and fixed.
