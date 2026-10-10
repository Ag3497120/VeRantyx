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

Optional: pair Joy-Con in visionOS *Settings > Bluetooth*, then press L and R
on both at the same time to merge them into one controller. The app's
*コントローラー* row shows the controller in use and which motion sensors
visionOS exposes.

## 4. Play

```bash
WINEPREFIX="$HOME/Verantyx_VR_Drive/SteamVR_Prefix_Wine11_Test" ./scripts/launch_vision_pro_vr.sh
```

The script rebuilds and installs the DLLs when they changed, starts the
encoder, Steam and SteamVR, waits for the Vision Pro app, then launches the
game through Steam. On the headset, open **MBP AVP VR** and tap
**ステレオVRを開始**. The window closes while the immersive view is open and
comes back when you press the Digital Crown.

Logs are in `logs/` (`hardware-encoder.log` has the pipeline statistics every
5 s, including the headset's own counters) and in the prefix
(`drive_c/vr_shared_frame.log` for the driver, `drive_c/vr_openvr_capture_proxy.log`
for the game-side capture).

## Tuning

All of these are environment variables for `launch_vision_pro_vr.sh`:

| Variable | Default | Effect |
|---|---|---|
| `VERANTYX_RENDER_TARGET_SCALE` | 0.7 | SteamVR render resolution (area scale, like SteamVR's custom resolution). Applied at the next SteamVR start |
| `VERANTYX_DISPLAY_HZ` | 60 | Virtual display refresh; lower is less compositor CPU |
| `VERANTYX_BITRATE_MBPS` | 20 | HEVC bitrate |
| `VERANTYX_STREAM_WIDTH` x `VERANTYX_STREAM_HEIGHT` | 1920x1080 | Pixel budget for both eyes; the aspect follows the headset's frustum |
| `VERANTYX_SEND_PACING_MBPS` | 60 | UDP burst pacing |
| `VERANTYX_KEYFRAME_INTERVAL` | 3 | Seconds between safety keyframes |
| `VERANTYX_GPU_CONVERT` | 1 | Scale/convert frames on the GPU (Metal); 0 uses vImage on the CPU |
| `VERANTYX_AUDIO` | 1 | Stream the Mac's audio to the headset (Mac speakers are muted meanwhile); `VERANTYX_AUDIO_KEEP_MAC_SPEAKERS=1` keeps them on |
| `VERANTYX_LOW_LATENCY_ENCODER` | 0 | VideoToolbox low-latency rate control (slower on M1 Max) |
| `HLA_FIDELITY_LEVEL` | unset | Adds Alyx's `vr_fidelity_level` console variables |

## Troubleshooting

- **"Not loading driver verantyx because it was blocked by a previous safe mode event"**: SteamVR entered safe mode after a crash. The launch script clears `steamvr.enableSafeMode` and the driver block in `steamvr.vrsettings` on every start.
- **Game audio permission**: the first run asks for *System Audio Recording* permission for your terminal. Allow it and run the script again.
- **The game starts at its highest resolution**: it was started by Steam before the script's launch options applied. The script stores the options in Steam's `localconfig.vdf` (while Steam is not running) and restarts such a game.
- **The Vision Pro app shows a green screen**: no video has arrived yet; it clears as soon as frames decode.
- **Hand pose offset**: hand tracking places the virtual Touch controller where a hand holding one would be. If the game's hands are offset, adjust the `openxr_grip` rotation/offset in `VisionTrackingManager.swift` (`touchRawPose`).
