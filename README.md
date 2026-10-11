# VeRantyx

> **This is the `native` branch**: Valve's Linux ARM64 build on macOS, no Wine. The Wine version is on the [`wine`](https://github.com/Ag3497120/VeRantyx/tree/wine) branch; `main` has both.

**Play PC VR games from an Apple Silicon Mac in an Apple Vision Pro — with your hands and a pair of Joy-Con, no tracked VR controllers.**

VeRantyx renders Half-Life: Alyx on the Mac, streams every stereo frame to a
native visionOS app in hardware-encoded HEVC, and sends the headset's head and
hand poses back. Hands are tracked by the Vision Pro; buttons, sticks and
triggers come from Nintendo Switch Joy-Con (one in each hand), or the game is
played with bare hands. No tracked VR controllers are needed: the PS VR2 Sense
controllers that other Vision Pro ports of Alyx require are not sold in every
region (not in Japan at the time of writing).

There are **two versions**, on their own branches:

| Branch | How the game runs on the Mac | Status |
|---|---|---|
| [`native`](https://github.com/Ag3497120/VeRantyx/tree/native) | Valve's **Linux ARM64** build of Half-Life: Alyx (made for the Steam Frame) runs natively on macOS — no Wine, no Rosetta, no SteamVR — through a patched [Klepton](https://github.com/shinyquagsire23/Klepton) loader, a glibc compatibility layer and an OpenVR runtime of our own | New (2026-10-11). Menu at 60 fps, levels played for 20+ minutes on an M1 Max |
| [`wine`](https://github.com/Ag3497120/VeRantyx/tree/wine) | The Windows build with the real Windows SteamVR under Wine 11, DXVK → MoltenVK, and a SteamVR driver of our own | Stable since 2026-10-10; ~30 fps in game on an M1 Max |
| `main` | Both of the above | Development |

Both versions share the Mac HEVC encoder (`MacEncoder/`) and the Vision Pro
app (`VisionProClient/`).

<!-- VIDEO: after uploading docs/media/highlight.mp4 through the README editor
     on github.com, replace the poster link below with the generated
     https://github.com/user-attachments/... URL on its own line; GitHub then
     renders a player. -->
![Half-Life: Alyx in Apple Vision Pro, rendered on a MacBook Pro (54-second highlight; the last seconds show the room through the headset's passthrough)](docs/media/highlight.gif)

[![Watch the full recording on YouTube](docs/media/poster.jpg)](https://youtu.be/6xVGgdChDes)

*54-second highlight: [docs/media/highlight.mp4](docs/media/highlight.mp4) — the last seconds show the room through the Vision Pro's passthrough, where it was recorded. Full 6-minute unedited recording: https://youtu.be/6xVGgdChDes.*

MIT licensed. No proprietary code is included (see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)): Wine, DXVK and Steam are
downloaded from their publishers by the setup script, SteamVR and the game
come from your own Steam account, and Apple's Game Porting Toolkit is neither
used nor shipped.

## Why VeRantyx

- **No tracked VR controllers.** Vision Pro hand tracking gives the hands'
  position; Joy-Con gyro and accelerometer are fused in for fast motion and
  for when a hand leaves the cameras' view, and the grip pose is carried from
  the wrist while a Joy-Con hides the knuckles. Gravity gloves, grabbing,
  pinch-to-trigger and finger animation (native version) all work without
  PS VR2 Sense controllers.
- **Any Vision Pro.** The game renders on the Mac's GPU, so a first-generation
  (M2) Vision Pro plays the same as a new one; the headset only decodes and
  displays.
- **Your own copy, nothing redistributed.** The game, SteamVR (Wine version)
  and Steam come from your own Steam account; this repository contains no
  Valve or Apple code apart from Valve's BSD-licensed OpenVR headers.
- **Native ARM version, no emulation.** Valve's own ARM64 Linux build runs on
  Apple Silicon directly, with a small OpenVR runtime instead of SteamVR:
  the menu holds 60 fps where the Wine version managed ~30, shader compiles
  run off the render thread, and game audio plays through Core Audio.
- **Accurate image.** Each eye is drawn at the exact frustum the game rendered
  with, and every frame carries the head pose it was rendered for, so the
  headset reprojects correctly.
- **Open source, MIT.** Every part — Mac launcher, glibc layer, OpenVR runtime,
  encoder, visionOS app — is in this repository and can be read and rebuilt.

## Built on

| Project | Used for | License | In this repository? |
|---|---|---|---|
| [Klepton](https://github.com/shinyquagsire23/Klepton) by ShinyQuagsire | Native version: loading Valve's ARM64 ELF libraries on macOS, its bionic libc and Vulkan-on-MoltenVK runtime; our patch set adds glibc TLS, x18 and loader fixes, CPU topology, asynchronous pipelines (`NativeARM/klepton-patches/`) | MIT | No — fetched by `NativeARM/scripts/fetch_klepton.sh`, patched by `apply.py` |
| [MoltenVK](https://github.com/KhronosGroup/MoltenVK) | Vulkan → Metal (both versions; Wine version via DXVK) | Apache-2.0 | No (Klepton's build / DXVK) |
| Valve's Half-Life: Alyx (Linux ARM64 depot 546564, Windows build) | The game | Valve's terms | No — your own Steam account |
| Valve's [OpenVR SDK headers](https://github.com/ValveSoftware/openvr) | SteamVR driver, capture proxy, native OpenVR runtime | BSD-3-Clause | Yes (`VRDriver/include/`, `NativeARM/vrruntime/openvr/`) |
| [ALVR](https://github.com/alvr-org/ALVR) | OpenVR hand-skeleton bone poses for finger animation (native version) | MIT | Yes, unchanged data with its license (`NativeARM/vrruntime/hand_skeleton_alvr.inc`) |
| [Fossilize](https://github.com/ValveSoftware/Fossilize) | Optional shader-prewarm experiment (not used by the launcher) | MIT | No — fetched by `NativeARM/scripts/build_fossilize.sh` |
| Debian trixie arm64 packages | glib, libstdc++, ssl, png … the Linux libraries the game expects from the Steam Linux Runtime | various (DFSG) | No — fetched by `NativeARM/scripts/fetch_debian_libs.sh` |
| Wine Staging (Gcenx build), DXVK for macOS (Gcenx) | Wine version | LGPL-2.1, zlib | No — fetched by `scripts/setup.sh` |

Details: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

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

## Setup

Step by step in **[docs/NATIVE_SETUP.md](docs/NATIVE_SETUP.md)**: download the
Linux depot with your own Steam account, fetch Klepton and the Debian
libraries, build the launcher, sign in to macOS Steam, build the visionOS
app, then:

```bash
NativeARM/scripts/launch_native_vr.sh
```

Internals, findings and status: [NativeARM/README.md](NativeARM/README.md).

## How it works

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
skeleton data, both MIT (see "Built on").

## Measurements and limits

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

