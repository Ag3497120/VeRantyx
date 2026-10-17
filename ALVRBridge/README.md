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

Game audio: macOS cannot record an output device (ALVR records the speakers on Windows), so the encoder makes a
device ALVR can: an aggregate of the Mac's output and a tap of the Mac's sound, "VeRantyx ALVR game audio", private
to its process (ALVR runs in it), and names it to the session. The tap mutes the Mac's speakers while a headset
listens (`VERANTYX_AUDIO_KEEP_MAC_SPEAKERS=1`: not); `VERANTYX_ALVR_AUDIO=0`: no game audio. The headset buffers
250 ms of it (`VERANTYX_ALVR_AUDIO_BUFFER_MS`; ALVR's 50 ms, and 150, broke up with the Vision Pro's ALVR app). The first time, macOS
asks to allow the terminal under Screen & System Audio Recording (the tap needs it).

The distance between the headset's eyes (its view transforms) goes to SteamVR as the user's IPD
(Prop_UserIpdMeters): the encoder writes it after SharedHands in the map's tail ("VIPD"), the driver sets it when it
differs by half a millimetre. 63 mm until a headset reports one, and with the Vision Pro app.

Vibration: the driver's controllers have a haptic component; the game's vibrations (VREvent_Input_HapticVibration)
go per hand into the map's tail (SharedHaptic at 336: seq, duration, frequency, amplitude), the encoder passes each on
(`vx_alvr_haptics`) and ALVR sends it to the headset's controller, scaled by its haptics settings. Whether a
controller then shakes needs a headset with vibrating controllers (Quest, Pico): not checked yet.

## Build and run

```bash
cd ALVRBridge
cargo build --release                                  # fetches ALVR v20.14.1 and its crates (Rust 1.82 or later)
swiftc -O tools/hevc_testpattern.swift -o /tmp/hevc_testpattern
/tmp/hevc_testpattern /tmp/test.h265 2144 2048 180     # both eyes side by side, VideoToolbox HEVC, Annex-B
target/release/alvr_probe --hevc /tmp/test.h265 --seconds 300
```

Then open an ALVR 20.x client on the same network. The session (`~/Library/Application Support/VeRantyx/ALVR/`) is
set for the Mac on every start: clients trusted as they come (there is no dashboard), game audio only from the
encoder's device (above; the probe has none), HEVC, no server-side foveation, TCP (the visionOS client wants it). The first connection
of a new client ends once ("Cannot restart SteamVR": the settings it brought need a restart there is no dashboard
for); it connects again by itself.

## Over a USB cable

`VERANTYX_ALVR_WIRED=1` puts ALVR's wired client (`client.wired`, trusted) into the session, as its dashboard's Wired
Connection switch does; without it the entry is taken out again. ALVR's server core then, every second, looks for a
device with `adb devices -l` (adb from PATH, else the platform-tools it downloads from Google into the session's
folder), forwards ports 9943 and 9944 to it (`adb forward`), finds its ALVR app (`alvr.client` from the store or
`alvr.client.stable` from GitHub), starts it if it is not running, and once it runs in front connects to it at
127.0.0.1, streaming over TCP. Wi-Fi discovery goes on beside it.

## Checked (2026-10-17)

- `alvr_server_core` and `alvr_client_mock` build on a MacBook Pro M1 Max with Rust 1.99.1, ALVR unchanged (with its
  `openvr` submodule).
- `alvr_client_mock` on another Mac (the MacBook Air, the same network; one Mac cannot run both: client and server
  both take port 9943) found the probe, connected, sent its views and 4677 tracking samples in 23 s, and received the
  test stream at 72 frames a second with no network drops. (It does not decode, so it keeps asking for IDRs.)
- The wired path, with no headset: an `adb` on PATH that answers for a Quest 3 and does `adb forward` as an ssh tunnel
  to the Air, where `alvr_client_mock` ran; Wi-Fi discovery off and the Air's Wi-Fi entries removed, so only the
  wired one could connect. The probe listed the device, forwarded 9943 and 9944, found `alvr.client.stable` running
  and resumed, connected to 127.0.0.1 (the session's `client.wired`, current_ip 127.0.0.1) and got 4534 tracking
  samples in 25 s.
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
- Earth VR in the Vision Pro's ALVR app (2026-10-17, the user): it plays; picture, head tracking and hands work.
  Every frame was stamped with an ALVR sample's exact time (`VeRantyx:` lines in session_log.txt, every 5 s) and the
  game rendered each with the exact pose recorded (the capture proxy: render_pose_exact for all). A distortion when
  turning seen in the first session was gone in the next: the eyes' field of view had been latched before the client
  sent it (it sends +-inf for its first seconds, now kept out). Stream size against frame rate at 90 Hz: 2270x914
  88 fps; 3416x1366 59 fps reaching ALVR (the encoder falls behind); 2736x1096 at 1.6x rendering 78 fps (the game's
  GPU time), none dropped by the encoder: hence ALVR mode's defaults (2880x1040 budget, 1.4x, 40 Mbps). Flying and
  zooming in still stutter at times (frames 100-200 ms apart while Earth loads map data); whether that is Earth's
  own is to be seen with another game.
- Game audio in the Vision Pro's ALVR app (2026-10-17, the user, Half-Life: Alyx): the game's sound plays in the
  headset. With ALVR's 50 ms buffer, and 150, it broke up (the game's own output had no gaps: no `gaps of silence`
  in its log); with 250 ms it did not. The device and the handshake (sample rate) were first checked with the mock.
- Hands in the Vision Pro's ALVR app (2026-10-17, the user, on the Air): they vanished whenever the app had gone
  over to hand tracking (the Joy-Con untouched for a while: ALVR then drops the controller's motion); with the
  palm's pose in its place they stay.
- IPD (2026-10-17): ALVR's client_mock, changed locally to report eyes 61.2 mm apart, connected from the Air to a
  harness run (20261017T080322Z-alyx-chapter, 60.0 fps): the bridge logged the eyes at +-0.0306 m and the driver
  "IPD 61.2 mm (the headset's)" a second later.
- Vibration (2026-10-17): Half-Life: Alyx in the harness with client_mock connected from the Air
  (20261017T080757Z-alyx-chapter, 60.0 fps): the driver got the game's first vibration at 17:08:42.841 (right hand,
  amplitude 0.20) and over 1000 in two minutes; the bridge sent them to ALVR from the same moment.
- A Quest: only through someone who has one.
