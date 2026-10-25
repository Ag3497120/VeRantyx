<h1 align="center">VeRantyx</h1>

<p align="center"><b>Half-Life: Alyx and Elite Dangerous on Apple Vision Pro, from just a Mac.<br>
No gaming PC, no Wine, no CrossOver.</b><br>
Windows PC VR games run on our own Windows runtime, written from scratch for Apple silicon, and stream to Vision Pro
or an ALVR headset. Some also play on the Mac's own screen.</p>

<p align="center">
  <a href="https://github.com/Ag3497120/VeRantyx/releases/latest"><img alt="Download VeRantyx for Mac" src="https://img.shields.io/github/v/release/Ag3497120/VeRantyx?label=VeRantyx%20for%20Mac&logo=apple"></a>
  <img alt="Apple silicon, macOS 14.2+" src="https://img.shields.io/badge/macOS-14.2%2B%20%C2%B7%20Apple%20silicon-black">
  <img alt="Headsets" src="https://img.shields.io/badge/headsets-Vision%20Pro%20%C2%B7%20ALVR-blue">
  <a href="LICENSE"><img alt="MIT license" src="https://img.shields.io/github/license/Ag3497120/VeRantyx"></a>
</p>

<p align="center">
  <a href="https://youtu.be/qF09ETv1TpU"><img src="docs/media/vxrt-highlight.gif" width="720" alt="Half-Life: Alyx (Windows build) with the real Windows SteamVR on VXRuntime, on a MacBook Pro, played in Apple Vision Pro with hand tracking and Joy-Con"></a><br>
  <sub>Half-Life: Alyx, Windows build, on a MacBook Pro M1 Max, no Wine. 3K preset, played in Vision Pro with hands + Joy-Con. <a href="https://youtu.be/qF09ETv1TpU">Full 4-minute unedited recording</a>.</sub>
</p>

> **New in 0.2.0 (Oct 22):** Elite Dangerous, on the Mac's screen or in the headset, with keyboard, mouse and a
> PS5 DualSense. **[Download VeRantyx for Mac →](https://github.com/Ag3497120/VeRantyx/releases/latest)**

> **Want your game to run? Ask your AI assistant.** VeRantyx is built to be extended with Claude Code, Codex,
> Gemini CLI or Cursor: run `/vxrt-support <your game>` in this repository and the agent measures the game, finds
> what stops it and implements the missing Windows functions, with the test harness checking every claim
> ([AGENTS.md](AGENTS.md)). Send what you get as a pull request or a report.

**You need your own copy of each game.** Nothing from Valve, Microsoft or any game is in this repository or its
releases: games and SteamVR come from your own Steam account. No anti-cheat is ever worked around.

<p align="center">
  <a href="#get-started">Get started</a> ·
  <a href="#what-runs">What runs</a> ·
  <a href="#updates">Updates</a> ·
  <a href="#under-the-hood">Under the hood</a> ·
  <a href="#branches">Branches</a> ·
  <a href="docs/README_ja.md">日本語</a>
</p>

## Updates

| Date | What changed |
|---|---|
| Oct 23 | **VeRantyx for Mac 0.2.1**: Direct3D 11 games use far less CPU per draw (Alyx busy scene 42-47 → 60 fps, M1 Max); ALVR no longer stuck at "os error 11" after a reconnect (#6); one-click "it ran" and "missing functions" reports after a play; the Wine-derived unwinder replaced ([notice](THIRD_PARTY_NOTICES.md)) |
| Oct 22 | **VeRantyx for Mac 0.2.0**: Elite Dangerous with two buttons, **On this Mac** and **Headset**; the Mac shows its frame rate while you play |
| Oct 22 | **The Lab: controllers work** (any headset): SteamVR now sees The Lab's controller bindings (a Windows pipe call answered wrongly, #2) |
| Oct 22 | ALVR over Wi-Fi: socket buffers fixed at 4 MB, which ended 270-800 ms frame delay spikes (#4); a new headset's first connection no longer shows double vision (#2, #4) |
| Oct 22 | **Vulkan, first steps** in our own `vulkan-1.dll`: a Windows program draws with depth test, culling, uniform buffers and push constants, and presents to a window. Next: what real games need (No Man's Sky, X-Plane 12) |
| Oct 22 | Elite Dangerous: PS5 DualSense in game; plays flat on the Mac's screen (about 60 fps in game, M1 Max) |
| Oct 21 | Elite Dangerous reaches the headset: official launcher (WPF on our own .NET runtime), sign-in, game, SteamVR (30-35 fps at 1700×1372 per eye) |
| Oct 18 | **VeRantyx for Mac 0.1.0**: setup and play without Terminal, signed and notarized, 11 languages |
| Oct 18 | Direct3D 12 steps 1-5: DXIL read without LLVM, translated to Metal, compute, graphics and swap chains |
| Oct 17 | The Lab runs on our own x86-64 → ARM64 translator (the path that replaces Rosetta 2), 52-56 fps |
| Oct 12 | VXRuntime, the Windows runtime, starts from scratch |

## Help wanted: does it run for you?

The fastest way to grow the list of games and headsets is your report, whether it works or not:
**[Report a game or headset →](https://github.com/Ag3497120/VeRantyx/issues/new/choose)** (in the app: Help >
Report a problem; it attaches the logs with your user name and Steam ID taken out). Quest and Pico are tested by
players only so far.

## Get started

1. **[Download VeRantyx for Mac](https://github.com/Ag3497120/VeRantyx/releases/latest)** — signed and notarized by
   Apple. Drag it to Applications. The runtime is inside: you need only macOS and Steam.
2. **Get started** in the app: it checks the Mac, gets SteamVR and your games' Windows builds with *your own* Steam
   account (or copies them from a Steam library you already have), and gathers the Microsoft runtime files from
   their own installers.
3. **Pick your headset and press Play.**

That's it. Details, languages (11) and what is checked: [MacApp/README.md](MacApp/README.md).
Prefer Terminal? `scripts/setup_vxrt.sh` then `scripts/launch_vxrt_alyx.sh` — see [the vxrt guide](docs/VXRT.md#quick-start).

<p align="center">
  <img src="docs/media/app-setup.png" width="32%" alt="VeRantyx for Mac: Get started, every step checked">
  <img src="docs/media/app-play.png" width="32%" alt="VeRantyx for Mac: Play, the installed games with what is known about each">
  <img src="docs/media/app-headset.png" width="32%" alt="VeRantyx for Mac: Headset, Vision Pro with hands and Joy-Con, or ALVR headsets">
</p>

## What runs

| Game | Status on VXRuntime | Measured |
|---|---|---|
| **Half-Life: Alyx** | ✅ Playable — played in the headset across level changes and saves | 60 fps at the 3K preset (1440×1620 per eye), MacBook Pro M1 Max |
| **Google Earth VR** | 🟡 Runs in VR, playtest pending | 56.7 fps, M1 Max |
| **Elite Dangerous** | 🟡 Flies on the Mac's screen and in the headset (official launcher; keyboard, mouse, DualSense) | flat: about 52-122 fps in game; VR: 30-35 fps at 1700×1372 per eye; M1 Max, Low |
| **No Man's Sky** | 🟡 Played on the Mac's screen (terrain drawn); in VR, picture and sound checked with both the Vision Pro's VeRantyx app and the ALVR app (hand input through ALVR too). Started by your own Windows Steam on VXRuntime (its DRM wrapper needs that Steam; you sign in yourself) | headset: 25-40 fps so far (not a harness measurement) |
| **The Lab** | 🟡 Runs in VR (title scene), playtest pending; its controller bindings load since 0.2.0 | 54.0 fps, M1 Max |
| Other SteamVR games | ⚪ Most still need Windows functions VXRuntime lacks | [games/](games/) has what is known |

Checked on a MacBook Pro M1 Max and a MacBook Air M5. Numbers come from harness runs with their run ids in
[games/](games/). Not supported: Direct3D 12 games (in progress, below), Vulkan, tessellation, anti-cheat (never
worked around).

| Headset | How | Status |
|---|---|---|
| **Apple Vision Pro** (M2 or M5) | VeRantyx app: hand tracking + Joy-Con, or bare hands — **no VR controllers needed** | ✅ Checked |
| **Apple Vision Pro** | "ALVR" app from the App Store | ✅ Checked (Alyx, Earth VR) |
| **Meta Quest, Pico** | ALVR 20.x, Wi-Fi or USB cable | ❔ Not tested yet — [we'd love a report](https://github.com/Ag3497120/VeRantyx/issues/new?template=alvr-headset.yml) |

## Under the hood

Everything below is in this repository, MIT licensed, and was written for this project (with the exceptions listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).

- **VXRuntime — a Windows runtime written from scratch for Apple silicon.** No Wine, no wineserver: a PE loader and
  the Windows API (kernel32, exceptions, files and registry, windows on AppKit, COM, audio on Core Audio) as native
  code that Windows programs call directly. It uses no Wine code, except about 120 lines of the x64 exception
  unwinder in versions from 2026-10-12 to 2026-10-23 (mac-v0.1.0, mac-v0.2.0), which were derived from Wine and have
  since been replaced ([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)). [VXRuntime/](VXRuntime/README.md)
- **Direct3D 11 → Metal in one step.** Our own DXBC → Metal Shading Language translator; geometry shaders and
  stream output run as compute. Alyx went from ~30 fps under Wine (DXVK → MoltenVK) to 60 fps on the same Mac.
- **Direct3D 12 → Metal, from scratch (early).** A DXIL reader with no LLVM, a DXIL → MSL translator and `d3d12.dll`
  on Metal: compute, root signatures, descriptor heaps, graphics pipelines, draws, swap chains, heaps, placed
  resources and GPU queries pass their tests. No D3D12 game runs yet.
- **The real Windows SteamVR on the Mac.** vrserver and vrcompositor run on VXRuntime with our driver; textures cross
  processes as IOSurfaces. Games talk to the **Mac** Steam client through a bridge — no Windows Steam, no Chromium.
- **HEVC streaming.** VideoToolbox HEVC (10-bit at the 3K preset) to our visionOS app, each frame at the exact pose and
  frustum it was rendered for — or through ALVR's own streaming core to ALVR clients, over Wi-Fi or a USB cable.
- **A notarized SwiftUI Mac app** that does the whole setup without Terminal, in 11 languages.
- **Our own x86-64 → ARM64 translator (in progress)** for when Rosetta 2 goes away: Google Earth VR has run on it;
  not used for games by default. [TRANSLATOR.md](VXRuntime/TRANSLATOR.md)
- **Built for AI coding agents.** `/vxrt-support <game>` and `/vxrt-tune <game>` in Claude Code, Codex, Gemini CLI and
  Cursor; a harness that measures runs into evidence (observed values with file hashes, sealed in a hash-chained
  ledger); an MCP server; [Command Clips](VXRuntime/CLIPS.md) to operate Windows programs without screenshots.
  [AGENTS.md](AGENTS.md)

## Branches

| Branch | What it is | For | Status |
|---|---|---|---|
| **[`main`](https://github.com/Ag3497120/VeRantyx)** = [`vxrt`](https://github.com/Ag3497120/VeRantyx/tree/vxrt) | Windows games + real SteamVR on **VXRuntime** | Players (via the app) and developers | ⭐ Current, developed daily — [full guide](docs/VXRT.md) |
| [`native`](https://github.com/Ag3497120/VeRantyx/tree/native) | Valve's **Linux ARM64** Alyx runs natively on macOS (patched Klepton + our OpenVR runtime) | Alyx only; the fastest path, no CPU translation | Kept as of 2026-10-11 |
| [`wine`](https://github.com/Ag3497120/VeRantyx/tree/wine) | Windows build + real SteamVR under **Wine 11**, DXVK → MoltenVK | Reference / history | Kept as of 2026-10-10, ~30 fps |

All Mac-side versions share the HEVC encoder (`MacEncoder/`) and the Vision Pro app (`VisionProClient/`).

## Your games, your copy

No Valve, Microsoft or game files are in this repository or its releases. SteamVR and the games come from your own
Steam account; Microsoft's runtime DLLs are taken from the redistributable installers your own installs carry, on your
Mac only. Licenses: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Not affiliated with Valve, Apple, Microsoft,
Meta, Nintendo, Sony, ALVR, Glovebox or Klepton.

**Help wanted:** a Quest or Pico report (works or not), and game profiles with harness runs as evidence
([games/README.md](games/README.md)). MIT © 2026 Ag3497120 (earlier versions contained about 120 lines of
Wine-derived LGPL code: see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).
