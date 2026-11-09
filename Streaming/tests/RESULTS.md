# Validation and historical measurements

## Source checks for this contribution

The CMake suite covers tangent-space geometry, alignment and inverse maps, wire and SEI framing, malformed descriptors, PyroWave envelope normalization, SGSR dimensions/cache keys, fence retirement and dependency-safe HEVC queue/reset handling. The VideoToolbox test uses fake property setters: it proves forwarding/error semantics, not hardware throughput. Synthetic Metal tests check pixels and producer/event/resource ordering.

## Historical test environment

These measurements were collected on the contributor's M3 Max / 36 GB Mac and Quest 3, over USB, using MoltenVR and paired experimental clients. They were not collected with VeRantyx's encoder/client and are not VXRuntime harness results. No optical motion-to-photon measurement was made. Raw personal device logs and binaries are excluded.

### Crop and foveated geometry

The retained four-mode test used the official MoltenVR beta: H.264, a 72 FPS synthetic source, 2048×2208 baseline per eye and an 800 Mbps configured budget. The static scene actually used only 13–20 Mbps, so this is not an 800 Mbps stress test.

| Mode | Encoded stereo size | Encoded pixels saved | Reported encode ms | Decode ms |
| --- | --- | ---: | ---: | ---: |
| Normal | 4096×2208 | 0% | stale / unusable | 6.25 |
| Crop | 3456×1984 | 24.2% | 60.75 | 6.07 |
| Foveated | 3296×1776 | 35.3% | 14.68 | 5.24 |
| Both | 2784×1600 | 50.7% | 11.28 | 4.43 |

The baseline encode counter froze; no encode-speedup percentage can be derived from it. End-to-end latency was unavailable because the unworn headset supplied no tracking. Repeated source frames inflate bridge counters; all modes retained approximately 72 unique source FPS. The exact dimensions/pixel savings are established; a doubled game FPS or halved optical latency is not. The official beta's implementation is independent of this extracted source contribution. See `evidence/geometry-four-modes.md` for the retained report.

### H.264 encoder policy

The shared no-game encode → USB → native decoded-image acquisition → USB ACK harness used 1664×1792 per eye, 72 Hz and a 350 Mbps target. Two modified runs reported 33.60 / 33.03 ms ACK-chain means at approximately 350 Mbps and 72 FPS. The single old run reported 99.22 ms at only 112.44 Mbps actual and 30.98 FPS. Because actual bitrate and output cadence differed, this is **not** an equal-quality A/B or a universal latency claim. See `evidence/h264-ack.json`.

On the contributor's hardware, default selection at larger stereo sizes could choose software AVC; explicit hardware selection at 4096×2208 succeeded. This is a capability observation, not a universal H.264 size limit. A later correction restored `DataRateLimits` passthrough; the historical throughput numbers predate that correction and are not a benchmark of the extracted helper in this PR.

### HEVC and receiver policy

Independent-frame and temporal/TSA modes require paired encoder/receiver behavior. The former trades image quality for queue freedom; the latter preserves base-layer references but was slower at higher resolutions in the retained experiments. No universal HEVC speedup is claimed. The portable queue tests establish dependency/credit/reset behavior, not a fresh hardware latency result.

## Not promoted

No production latency gain is claimed from diagnostic return-ACK changes, pacing observers, one-job decoder experiments, framed preflight experiments or UI bitrate unlocks alone. Those paths and binary adapters are not included. Default VeRantyx streaming remains unchanged; a controlled recipient-side A/B is needed before enabling any new profile by default.
