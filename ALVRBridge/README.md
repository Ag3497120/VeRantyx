# ALVRBridge

VeRantyx's frames to the ALVR client apps (Meta Quest, Pico, Apple Vision Pro's "ALVR" from the App Store) through
ALVR's own streaming core, `alvr_server_core` (https://github.com/alvr-org/ALVR, MIT), used unchanged at tag
v20.14.1. ALVR's server is made for Windows and Linux; its core builds on macOS (Apple silicon) as it is. Clients
of ALVR 20.x connect (the protocol is the major version).

Status: wired into VeRantyx. `VERANTYX_ALVR=1 scripts/launch_vxrt_steamvr.sh` (and the game launchers on it) sends
the encoder's HEVC (MacEncoder/HardwareEncoder.swift) to ALVR's core through this library's C interface
(`src/lib.rs`, loaded at run time), and writes the ALVR client's head, hands or controllers, buttons and eye field
of view into the shared map's SharedHands, where the SteamVR driver reads the Vision Pro app's: the driver is
unchanged. Each frame carries the time of the ALVR tracking sample it was rendered for. `alvr_probe` still runs the
core alone with a test pattern.

A headset whose display settings differ from the last one's (another model, another rate) makes ALVR store them and
ask for its host to be restarted (ALVR's dashboard restarts SteamVR): here the encoder exits with 75 and the launch
script starts it again; it then connects with them. SteamVR's display rate follows the last ALVR headset's at the
next start (`VERANTYX_DISPLAY_HZ` overrides it).

Not yet: haptics (VeRantyx's driver has none), game audio to the headset, IPD from the client's view transforms.

## Build and run

```bash
cd ALVRBridge
cargo build --release                                  # fetches ALVR v20.14.1 and its crates (Rust 1.82 or later)
swiftc -O tools/hevc_testpattern.swift -o /tmp/hevc_testpattern
/tmp/hevc_testpattern /tmp/test.h265 2144 2048 180     # both eyes side by side, VideoToolbox HEVC, Annex-B
target/release/alvr_probe --hevc /tmp/test.h265 --seconds 300
```

Then open an ALVR 20.x client on the same network. The session (`~/Library/Application Support/VeRantyx/ALVR/`) is
set for the Mac on every start: clients trusted as they come (there is no dashboard), no game audio (macOS cannot
record an output device), HEVC, no server-side foveation, TCP (the visionOS client wants it). The first connection
of a new client ends once ("Cannot restart SteamVR": the settings it brought need a restart there is no dashboard
for); it connects again by itself.

## Checked (2026-10-17)

- `alvr_server_core` and `alvr_client_mock` build on a MacBook Pro M1 Max with Rust 1.99.1, ALVR unchanged (with its
  `openvr` submodule).
- `alvr_client_mock` on another Mac (the MacBook Air, the same network; one Mac cannot run both: client and server
  both take port 9943) found the probe, connected, sent its views and 4677 tracking samples in 23 s, and received the
  test stream at 72 frames a second with no network drops. (It does not decode, so it keeps asking for IDRs.)
- The Vision Pro's ALVR app (App Store, built on ALVR 20.14.1) connected over Wi-Fi: real head poses (seated, about
  1.03 m) and the left hand's, 6480 frames sent with 2 IDRs asked for (the mock asks hundreds: it does not decode), so
  the headset decoded the stream. It asked for 2144 x 2048 per eye at 90 Hz; the probe now streams at the rate the
  client asks for. In the headset the test pattern looked right (both eyes, the moving bar, the counter; no artifacts
  or stutter noticed), at 90 Hz.
- The encoder with ALVR, without SteamVR (a script writing frames into the shared map as the driver does): the mock
  on the Air connected, received the stream, and its head pose and ALVR's sample times reached SharedHands. A mock
  with other display settings than the stored ones: the encoder started again once and the mock connected.
- Earth VR with `VERANTYX_ALVR=1` through the harness: run 20261016T214636Z-earth-start, level controls, 70.8 fps at
  the 72 Hz the mock asked for, the game's frames (the capture proxy's) encoded at about 72 fps and received by the
  mock for two and a half minutes without a network drop. (The harness drives the game with simulated poses.)
- Not yet: a game in a real headset through ALVR; a Quest only through someone who has one.
