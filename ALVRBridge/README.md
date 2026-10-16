# ALVRBridge

VeRantyx's frames to the ALVR client apps (Meta Quest, Pico, Apple Vision Pro's "ALVR" from the App Store) through
ALVR's own streaming core, `alvr_server_core` (https://github.com/alvr-org/ALVR, MIT), used unchanged at tag
v20.14.1. ALVR's server is made for Windows and Linux; its core builds on macOS (Apple silicon) as it is. Clients
of ALVR 20.x connect (the protocol is the major version).

Status: a probe. `alvr_probe` runs the core alone, waits for a client, logs its views, poses and buttons, and streams
an HEVC test pattern stamped with the newest head pose. The next step is VeRantyx's own frames (MacEncoder's HEVC)
and poses (VRDriver) through it.

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
  client asks for. What the picture looked like in the headset: still to be reported.
- A Quest: only through someone who has one.
