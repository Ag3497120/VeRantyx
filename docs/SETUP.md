# Setup guide

VeRantyx runs Windows SteamVR and a Windows PC VR game on an Apple Silicon Mac
and streams the result to an Apple Vision Pro. This guide goes from an empty
Mac to playing Half-Life: Alyx.

## What you need

| Item | Notes |
|---|---|
| Apple Silicon Mac, macOS 14 or later | Tested on a MacBook Pro with M1 Max, macOS 26 |
| Xcode 26 | Builds the Mac encoder (`swiftc`) and the visionOS app |
| Apple Vision Pro, visionOS 26 | Paired with the Mac for development (Settings > General > Remote Devices) |
| Homebrew | `scripts/setup.sh` installs `mingw-w64` (Windows x64 cross compiler) |
| A Steam account that owns the game | SteamVR and the game are installed through Windows Steam inside Wine |
| Optional: a pair of Nintendo Switch Joy-Con | Buttons, sticks and inertial sensors; hand tracking supplies the position |
| ~80 GB of disk | Half-Life: Alyx alone is ~70 GB |

Nothing proprietary is in this repository. Wine, DXVK and the Steam installer
are downloaded from their publishers by the setup script; SteamVR and the game
come from your Steam account.

## 1. One command on the Mac

```bash
git clone https://github.com/Ag3497120/VeRantyx.git
cd VeRantyx
./scripts/setup.sh
```

The script:

1. checks the Mac, Xcode and Homebrew, and installs `mingw-w64`;
2. downloads [Wine Staging (Gcenx macOS build)](https://github.com/Gcenx/macOS_Wine_builds/releases) into `~/Verantyx_VR_Drive/Beer/wine/WineStaging.app`;
3. downloads [DXVK for macOS](https://github.com/Gcenx/DXVK-macOS/releases) (used by SteamVR's compositor);
4. downloads the Windows Steam installer and creates the Wine prefix `~/Verantyx_VR_Drive/SteamVR_Prefix_Wine11_Test`;
5. builds the Mac HEVC encoder and all Windows DLLs (SteamVR driver, OpenVR capture proxy, D3D11 proxy, Steam WebHelper shim);
6. registers the SteamVR driver folder and runs the Steam installer.

Environment variables change the locations and versions: `VERANTYX_VR_DRIVE`,
`WINEPREFIX`, `VERANTYX_WINE_VERSION`, `VERANTYX_WINE_URL`,
`VERANTYX_DXVK_RELEASE`, `VERANTYX_SKIP_STEAM_INSTALL=1`.

## 2. Install SteamVR and the game

Start Windows Steam once, sign in, and install **SteamVR** (App ID 250820) and
**Half-Life: Alyx** (App ID 546560) into the default library:

```bash
WINEPREFIX="$HOME/Verantyx_VR_Drive/SteamVR_Prefix_Wine11_Test" \
  ./VRDriver/wine_launcher.sh "$HOME/Verantyx_VR_Drive/SteamVR_Prefix_Wine11_Test/drive_c/Program Files (x86)/Steam/steam.exe" \
  -no-cef-sandbox -cef-single-process
```

If Steam's window stays blank, the CEF web helper failed to start; the launch
script in step 4 installs a shim that fixes this and retries.

## 3. Install the Vision Pro app

Open `VisionProClient/VisionSpatialTools/VisionSpatialTools.xcodeproj` in
Xcode, set your own team under *Signing & Capabilities*, select the Vision Pro
as the run destination and run once. The app needs the hand-tracking
capability, which the free provisioning profile supplies automatically.

From then on this installs and launches it from the terminal (the headset must
be paired and unlocked):

```bash
./scripts/deploy_vision_pro_app.sh
```

On first launch, allow local network access and hand tracking.

Optional: pair both Joy-Con in visionOS *Settings > Bluetooth* and leave
them separate: each then brings its own gyro and accelerometer, so both
hands get inertial sensing (orientation fused with hand tracking, wrist
flicks for the gravity gloves, dead reckoning while a hand is out of view).
Pressing L and R on both merges them into one controller, which also works
but exposes one sensor set (applied to the right hand). The app's
*Controller* row shows the controllers in use and their sensors. visionOS
presents a single Joy-Con as if held sideways; the app rotates its stick and
sensor axes back to the upright hold.

The merged Joy-Con pair can also be held in the left hand alone (the app's
"Hold the merged Joy-Con pair in the left hand" switch, on by default, with
the hold picker set to the way it is turned): the right Joy-Con's ZR and R
are the trigger and grip (the left one's ZL and L are ignored), the right
Joy-Con's stick moves/teleports and the left Joy-Con's stick turns, the
arrows are X/Y and A/B/X/Y are the right Touch buttons. The right hand is
free: its trigger and grip are gestures and it does the gravity gloves. The
hand holding the pad is not camera-tracked at all (a hand wrapped around a
controller tracks badly and only its buttons matter): it sits at a fixed
place beside and below the eyes, turned with the head, and the stick aims
its teleport arc. The app window shows what the cameras recognize for the
free hand (open, fist = grip, index pinch = trigger, other pinches).

A Nintendo Switch Pro Controller (or any other gamepad) works one-handed:
hold it in the left hand, turned sideways so the thumb reaches the left stick
and d-pad (pick the hold in the app window: upright, turned left or turned
right; the stick axes rotate with it). Its left half (ZL, L, left stick,
d-pad, −) is the left Touch controller; its right stick and A/B/X/Y drive the
right hand, whose trigger and grip stay gestures (index pinch, finger curl),
so the right hand is free to grab, shoot and use the gravity gloves. While
the left stick is pushed, the left hand's aim follows the stick: direction
from the stick angle (relative to where you face), distance from how far it
is pushed, so the teleport arc is aimed with the thumb. The arc's pitch
offset can be tuned with the `teleportAimPitch` default (degrees, 40).

Without Joy-Con, everything is done with the hands (both hands, per hand):

| Gesture | Touch input |
|---|---|
| Thumb–index pinch | Trigger (fire / use) |
| Curl middle, ring and little fingers | Grip (grab, gravity-glove lock) |
| Thumb–ring pinch | A / X (eject clip, health pen, laser sight) |
| Thumb–little pinch | B / Y (reload; left Y opens the game menu) |
| Thumb–middle pinch, then move the hand | Joystick (left: teleport aim, right: turn / jump / dash) |
| Left: thumb–middle pinch held 0.15 s | Stick forward (teleport aim; release to go) |
| Right: thumb–middle pinch held still 0.45 s | Stick click (weapon inventory) |
| Both fists raised above the head for 1.5 s | Recenter |

Gravity gloves without Joy-Con: aim the open hand at the item, curl the
fingers (grip) so it locks on, then flick the wrist sharply toward you and
keep the grip to catch it. The hand's velocity comes from hand tracking
alone, so make the flick quick and keep the hand inside the cameras' view.

## 4. Play

```bash
WINEPREFIX="$HOME/Verantyx_VR_Drive/SteamVR_Prefix_Wine11_Test" ./scripts/launch_vision_pro_vr.sh
```

The script rebuilds and installs the DLLs when they changed, starts the
encoder, Steam and SteamVR, waits for the Vision Pro app, then launches the
game through Steam. On the headset, open **VeRantyx** and tap
**ステレオVRを開始**. The window closes while the immersive view is open and
comes back when you press the Digital Crown.

Logs are in `logs/` (`hardware-encoder.log` has the pipeline statistics every
5 s, including the headset's own counters) and in the prefix
(`drive_c/vr_shared_frame.log` for the driver, `drive_c/vr_openvr_capture_proxy.log`
for the game-side capture).

## Mac profiles and tuning

`launch_vision_pro_vr.sh` first runs `scripts/mac_profile.sh`, which classes
the Mac by its performance cores, GPU cores and memory and sets defaults for
it (`./scripts/mac_profile.sh --describe` prints them):

| Tier | Macs | Render scale | Display | HEVC | Stream budget |
|---|---|---|---|---|---|
| high | Max/Ultra: 8+ performance cores, 24+ GPU cores, 32+ GB | 0.55 | 60 Hz | 20 Mbps | 1920x1080 |
| mid | Pro chips; base M3/M4 with 16+ GB | 0.45 | 60 Hz | 16 Mbps | 1728x972 |
| low | base M1/M2, under 16 GB | 0.35 | 45 Hz | 12 Mbps | 1440x810, Alyx fidelity level 2 |

Any variable you set yourself overrides the profile. The game is CPU-bound
under Wine on every tier, so the profile mainly trades resolution and
display rate for frame rate; the encoder then adapts on its own:

- frame conversion runs on the GPU (Metal) or the CPU (vImage), whichever
  timed faster on the first frames (`VERANTYX_GPU_CONVERT=auto`);
- the HEVC bitrate drops by 20 % whenever the link loses packets (the
  headset reports incomplete frames and gaps, the Mac counts send errors)
  and climbs back 10 % per clean 30 s up to the ceiling.

| Variable | Default | Effect |
|---|---|---|
| `VERANTYX_RENDER_TARGET_SCALE` | profile | SteamVR render resolution (area scale, like SteamVR's custom resolution). Applied at the next SteamVR start |
| `VERANTYX_DISPLAY_HZ` | profile | Virtual display refresh. The game's frame rate quantizes to whole display frames (60 Hz: 60, 30, 20, 15 fps), and the compositor costs CPU per display frame |
| `VERANTYX_BITRATE_MBPS` | profile | HEVC bitrate ceiling (2-300); `VERANTYX_BITRATE_MIN_MBPS` (40 % of it) is the floor, `VERANTYX_ADAPTIVE_BITRATE=0` fixes it. Lowered when the headset loses frames, and with ALVR also when they spend over 80 ms on the network |
| `VERANTYX_STREAM_WIDTH` x `VERANTYX_STREAM_HEIGHT` | profile | Pixel budget for both eyes; the aspect follows the headset's frustum |
| `VERANTYX_SEND_PACING_MBPS` | 60 | UDP burst pacing |
| `VERANTYX_KEYFRAME_INTERVAL` | 3 | Seconds between safety keyframes |
| `VERANTYX_GPU_CONVERT` | auto | 1 forces Metal, 0 forces vImage on the CPU |
| `VERANTYX_AUDIO` | 1 | Stream the Mac's audio to the headset (Mac speakers are muted meanwhile); `VERANTYX_AUDIO_KEEP_MAC_SPEAKERS=1` keeps them on |
| `VERANTYX_LOW_LATENCY_ENCODER` | 0 | VideoToolbox low-latency rate control (slower on M1 Max) |
| `HLA_FIDELITY_LEVEL` | profile | Adds Alyx's `vr_fidelity_level` console variables (0–8); unset lets Alyx choose |
| `VERANTYX_D3D_BACKEND` | dxvk | Direct3D backend. `dxmt`: the game renders through [DXMT](https://github.com/3Shain/dxmt) (Direct3D 11 → Metal in one layer, less CPU than DXVK → MoltenVK) while SteamVR's compositor keeps DXVK; the script downloads DXMT, installs it (system32 `d3d11.dll`, DXVK kept as `d3d11_dxvk.dll` for the compositor, `winemetal` into the Wine runtime) and restarts Steam. `d3dmetal` (with `VERANTYX_USE_DXVK=0`): Apple's D3DMetal for the whole prefix from your own Game Porting Toolkit copy at `GPTK_ROOT` (default `~/Verantyx_VR_Drive/gptk_wine_local`; nothing is downloaded or shipped, Apple's license does not allow redistribution). Alyx loads `d3d11.dll` from system32 explicitly, which is why both go there. Both are experimental; see "D3D backends" below |

### D3D backends

The game is CPU-bound under Wine, and part of that CPU is the Direct3D
translation itself. Three backends can be compared with the headset on
(`logs/hardware-encoder.log` shows the game's submit rate every 5 s):

```bash
./scripts/launch_vision_pro_vr.sh                                   # DXVK → MoltenVK (default)
VERANTYX_D3D_BACKEND=dxmt ./scripts/launch_vision_pro_vr.sh          # DXMT for the game
VERANTYX_D3D_BACKEND=d3dmetal VERANTYX_USE_DXVK=0 ./scripts/launch_vision_pro_vr.sh   # your own GPTK's D3DMetal
```

Switching restarts the Steam client (the game inherits its environment)
and, for `d3dmetal`, needs SteamVR stopped first since the compositor
changes backend too. Switching back to `dxvk` restores the DXVK DLLs.

Status (2026-10-11): `dxmt` starts Alyx and renders (confirmed in the
bench); its frame rate with the headset is still to be compared. `d3dmetal`
installs GPTK's D3D DLLs as the Wine runtime's builtins (they are Wine
builtins with a Unix half, not droppable native DLLs), but every GPTK
version checked (1.1 and 3.0) imports `ntdll.__wine_unix_call`, an entry
point that only Apple's and CodeWeavers' Wine 7-based builds export; on
this Wine 11 runtime the DLLs abort with "unimplemented function". Running
the whole pipeline on GPTK's own Wine 7.7 (how the project ran in June
2026) no longer works either: the current Steam client crashes at startup
there (`COMCTL32.dll failed to initialize`). D3DMetal therefore needs a
CrossOver-class Wine; the option stays in the scripts for anyone who has
one, and DXMT is the open-source way to a lighter D3D layer.

### Measuring on your Mac

`scripts/bench_alyx.sh` runs the whole pipeline without a headset (the driver
publishes a synthetic head that looks around and two hands), loads a map and
measures the game's submit rate, CPU per process and per game thread, and
GPU utilization for a minute:

```bash
./scripts/bench_alyx.sh baseline
./scripts/bench_alyx.sh hz90 VERANTYX_DISPLAY_HZ=90
./scripts/bench_alyx.sh scale07 VERANTYX_RENDER_TARGET_SCALE=0.7
```

Reports land in `logs/bench/`. `VERANTYX_BENCH_MAP` (default `a1_intro_world`),
`BENCH_SECONDS` (60) and `BENCH_WARMUP_SECONDS` (20) adjust the run. The
script restarts SteamVR for every run and stops it afterwards, since a
SteamVR started this way keeps publishing synthetic poses whenever the
headset pauses; `BENCH_REUSE_STEAMVR=1` and `BENCH_KEEP_STEAMVR=1` keep one
SteamVR across runs.

Known limits of the bench (2026-10-10): the first scene application after a
SteamVR start gets no scene focus from SteamVR in this setup (the second one
does, so reuse SteamVR for a second pass), and without a headset Alyx stays
in its "paused" state after the map's first frame, so the measurement covers
startup, loading and the first frame but not sustained gameplay yet. The
CPU/GPU split, per-process load and the startup path are measured as
intended; gameplay frame-rate comparisons still need the headset.

## Floor height and recentering

The Vision Pro's ARKit origin is on the floor, so the game's floor matches the
real floor and your eye height is your real one (standing or seated). The
horizontal position and yaw are recentered when the immersive view starts;
to recenter again while playing, hold both Joy-Con stick clicks for one
second, or, without Joy-Con, raise both fists above your head for 1.5 s. The launch script writes a SteamVR chaperone universe with an identity
standing transform and a `VERANTYX_PLAY_AREA_METERS` (default 3) square play
area, replacing the 1x1 m one that SteamVR's room setup wrote.

## Troubleshooting

- **"Not loading driver verantyx because it was blocked by a previous safe mode event"**: SteamVR entered safe mode after a crash. The launch script clears `steamvr.enableSafeMode` and the driver block in `steamvr.vrsettings` on every start.
- **Game audio permission**: the first run asks for *System Audio Recording* permission for your terminal. Allow it and run the script again.
- **The game starts at its highest resolution**: it was started by Steam before the script's launch options applied. The script stores the options in Steam's `localconfig.vdf` (while Steam is not running) and restarts such a game.
- **Alyx asks to confirm that the Mac is below its recommended specification**: DXVK reports an Apple GPU as "NVIDIA GeForce 6800", its placeholder for an unknown vendor. The launch script writes a `dxvk.conf` beside `hlvr.exe` that names a current GPU (`VERANTYX_DXVK_GPU_NAME`, default "NVIDIA GeForce RTX 3080"; `VERANTYX_DXVK_GPU_SPOOF=0` removes it). It takes effect at the next game start.
- **The Vision Pro app shows a green screen**: no video has arrived yet; it clears as soon as frames decode.
- **The game froze on a loading screen and quit a minute later**: SteamVR's compositor never accepts this game's `Submit` (the texture cannot be shared across Wine processes), so during a long level load it decided the game was not responding, took its scene focus away and answered `WaitGetPoses` with `DoNotHaveFocus`; Alyx then waited for focus that only an accepted `Submit` returns. The capture proxy now does two things: it answers those `WaitGetPoses` calls itself with the headset's poses (logged as "SteamVR reports no scene focus" in `drive_c/vr_openvr_capture_proxy.log`) so the game keeps rendering, and it gives SteamVR a stand-in shared handle for each submitted texture (the compositor-side D3D11 proxy turns it into a placeholder texture), so `Submit` is accepted and SteamVR sees the game rendering again. `VERANTYX_FAKE_SHARED_HANDLE=0` disables the second part. The game is also started with `+engine_no_focus_sleep 0` (`HLA_EXTRA_OPTIONS` replaces that).
- **Hand pose offset**: hand tracking places the virtual Touch controller where a hand holding one would be. If the game's hands are offset, adjust the `openxr_grip` rotation/offset in `VisionTrackingManager.swift` (`touchRawPose`).
