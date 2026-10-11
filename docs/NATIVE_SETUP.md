# Native version — setup

Valve's Linux ARM64 build of Half-Life: Alyx running directly on an Apple
Silicon Mac, streamed to an Apple Vision Pro. Everything is built from source
on your Mac; the game comes from your own Steam account. Nothing from Valve
is redistributed.

Tested on a MacBook Pro (M1 Max, 64 GB), macOS 26, Vision Pro (visionOS 26).

## You need

- An Apple Silicon Mac with Xcode, Homebrew and Python 3
- An Apple Vision Pro on the same Wi-Fi network
- Half-Life: Alyx in your Steam library
- [steamcmd](https://developer.valvesoftware.com/wiki/SteamCMD):
  `brew install --cask steamcmd`
- The macOS Steam client (Steam.app), signed in with the account that owns the
  game — the game checks Steam at start-up
- Optional: two Nintendo Switch Joy-Con paired with the Vision Pro (the game
  also plays with hands only)

## 1. Get the code

```bash
git clone -b native https://github.com/Ag3497120/VeRantyx.git
cd VeRantyx
```

## 2. Download the game (your account)

The Linux depot (546564) holds the ARM64 binaries; the content (~72 GB) is the
same for every platform. steamcmd asks for your password and Steam Guard code
itself; no script stores them.

```bash
# The ARM64 binaries only (260 MB):
NativeARM/scripts/fetch_alyx_arm64.sh <your-steam-login>
# The whole Linux install, including the content (~72 GB), into a folder of
# your choice:
steamcmd +@sSteamCmdForcePlatformType linux +force_install_dir "$HOME/Verantyx_VR_Drive/alyx_steam_linux" \
         +login <your-steam-login> +app_update 546560 validate +quit
```

(If you already have the Windows version installed, for example from the
`wine` branch, its content folder can be used instead of the full download;
that is how the native version was developed. The full Linux download path
above has not been tested end to end yet.)

## 3. Fetch and build the runtime pieces

```bash
NativeARM/scripts/fetch_debian_libs.sh          # Linux libraries the game expects (Debian arm64)
NativeARM/scripts/fetch_klepton.sh              # Klepton + MoltenVK, pinned commit
python3 NativeARM/klepton-patches/apply.py NativeARM/build/Klepton
make -C NativeARM/host                          # builds NativeARM/build/host/alyx_host
```

## 4. Lay out the game

```bash
DEPOT=<the depot_546564 folder printed by fetch_alyx_arm64.sh, or your full Linux install>
INSTALL=<the folder that contains game/hlvr with the content>
NativeARM/scripts/make_guest_dir.sh "$DEPOT"
NativeARM/scripts/make_game_root.sh "$DEPOT" "$INSTALL"
```

This builds `~/Verantyx_VR_Drive/alyx_linux` out of symlinks; nothing is
copied or changed in your install.

## 5. Vision Pro app

Open `VisionProClient/VisionSpatialTools/VisionSpatialTools.xcodeproj` in
Xcode, choose your team (a free Apple Account works for your own device) and
run it on the Vision Pro once.

## 6. Play

Start Steam.app and sign in, open the VeRantyx app on the Vision Pro, then:

```bash
NativeARM/scripts/launch_native_vr.sh
```

The script builds the encoder, starts the game and prints the frame rate every
5 seconds. Stop with Ctrl+C (it gives the game up to 30 s to exit cleanly).
Logs: `logs/native/alyx_host.log`, `logs/native/hardware-encoder.log`.

### Options

| Variable | Effect |
|---|---|
| `VERANTYX_QUALITY=high` / `ultra` | Higher render and stream resolution (see the README for measured frame rates) |
| `VERANTYX_DISPLAY_HZ=90` | 90 Hz virtual display (default 60) |
| `VERANTYX_ASYNC_PIPELINES=0` | Compile shaders on the render thread (hitches) instead of in the background |
| `VERANTYX_BENCH=1` | Run without a headset (synthetic head pose) |
| `ALYX_MAP=<map>` | Start map (default `startup`, the main menu) |

## Troubleshooting

- *"macOS Steam is not running"*: start Steam.app and sign in first.
- *"The Wine version's encoder is running"*: stop the Wine version with
  `scripts/stop_wine_vr.sh` (on `main` / `wine`).
- A short stall the first time a new material appears: the shader is being
  compiled (in the background since the async pipelines); the second time it
  is cached by macOS.
- The game exits with "Timed out waiting for response from Mongoose": update
  to the current runtime (the stand-in for SteamVR's level-transition service
  answers it).
