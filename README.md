# VeRantyx

**Play Windows PC VR games on an Apple Silicon Mac, in an Apple Vision Pro.**

VeRantyx runs the real Windows SteamVR and a Windows game (Half-Life: Alyx)
under Wine on the Mac, captures every stereo frame the game submits, encodes it
in hardware, and streams it over Wi-Fi to a native visionOS app. The headset
sends its head and hand poses back; the game sees an Oculus Touch headset and
controllers. Hands are tracked by the Vision Pro; buttons, sticks and triggers
come from a pair of Nintendo Switch Joy-Con.

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

## Setup

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

## How it works

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

### Languages

C++ (SteamVR driver, OpenVR capture proxy, D3D11 proxy; MinGW-w64 cross
compiled to Windows x64), C (Steam WebHelper shim), Swift (macOS encoder,
visionOS app), Metal Shading Language (frame conversion, eye-image rendering),
Bash and Python (build, install and launch scripts). The only third-party
source in the tree is Valve's OpenVR SDK headers.

## Measurements

MacBook Pro (M1 Max, 64 GB), macOS 26, Vision Pro on the same Wi-Fi 6 network,
Half-Life: Alyx, 2026-10-10. Numbers come from the pipeline's own logs
(`logs/hardware-encoder.log` and the game-side capture log).

| Metric | Value | Conditions |
|---|---|---|
| Game render rate (Alyx, in game) | 29–35 fps (median 30) | 1115×900 per eye (SteamVR scale 0.7, Alyx's automatic top fidelity level), 60 Hz virtual display |
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

## Compared with MoltenVR

[MoltenVR](https://github.com/MoltenVR/MoltenVR-Preview) (first public
preview 2026-07-04, v1.0.0 2026-07-12) also plays PC VR games from an Apple
Silicon Mac, but takes a different route. From its README and this
repository:

| | VeRantyx | MoltenVR |
|---|---|---|
| SteamVR | Runs the real Windows SteamVR; a custom driver provides the HMD and controllers, and the game's `Submit` calls are hooked to capture frames | Does not run SteamVR; OpenComposite translates OpenVR calls to its own OpenXR runtime, which receives every frame directly |
| D3D11 on Metal | DXVK → MoltenVK (two layers) | DXMT (one layer) |
| Wine | Wine 11 Staging (Gcenx build) | WhiskyWine / GPTK bottles (Whisky, CrossOver, Wineskin) |
| Video | HEVC, custom UDP protocol, Wi-Fi 20 Mbps; frames carry the exact render pose for the headset's reprojection | H.264, ALVR protocol, USB (100 Mbps) or Wi-Fi, 72 fps claimed |
| Headset | Apple Vision Pro, own visionOS app (Compositor Services) | Any ALVR client (tested on Quest 3); Vision Pro not mentioned |
| Input | Vision Pro hand tracking for position, Joy-Con for buttons/sticks/triggers and inertial sensors; no haptics | VR controllers through ALVR: 6DoF, buttons, sticks, haptics |
| Audio | Core Audio process tap, no extra driver | BlackHole virtual audio device |
| Source | MIT, full source | Binaries only, 7-day trial then license key |

Running SteamVR keeps SteamVR Input (the game's own controller bindings),
chaperone and the dashboard working, at the cost of `vrserver`/`vrcompositor`
CPU time; MoltenVR's design avoids that cost but needs OpenComposite to cover
each game's OpenVR usage.

## Project history

The project was started in May 2026 with Gemini 3.1 Pro (initial commit
2026-05-31: a mock SteamVR driver that got Half-Life: Alyx through its
initialization), then left alone until September 2026, when work resumed
with GPT-6 Astra. On 2026-10-09 it was brought to a working stream with
Opus 5.5, and on 2026-10-10 finished with Fable 5.1: exact pose matching,
Vision Pro frustum handling, Joy-Con input with gravity-glove support, game
audio, GPU frame conversion and the one-command setup. All of it was built
from scratch by the author with these models; no code from other projects is
used apart from Valve's OpenVR headers.

## Status and limits

- Verified: SteamVR + Half-Life: Alyx on the Mac above, streaming to a Vision Pro,
  playing with Joy-Con. Other games and other Macs are untested.
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
