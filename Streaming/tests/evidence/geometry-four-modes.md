# Official MoltenVR beta — four video-only modes

H.264 · 2048×2208 baseline/eye · 72 FPS source · 800 Mbps target · USB/TCP. 38 s per mode; first 15 s excluded. No games or private adapters.

| Mode | Encoded SBS | Source FPS | Reported encode ms | Decode ms | Actual Mbps | Encoded pixels saved |
|---|---|---:|---:|---:|---:|---:|
| normal | 4096×2208 | 71.99 | 65.88* | 6.25 | 19.62 | 0.0% |
| crop | 3456×1984 | 72.00 | 60.75 | 6.07 | 19.75 | 24.2% |
| foveated | 3296×1776 | 72.00 | 14.68 | 5.24 | 16.47 | 35.3% |
| both | 2784×1600 | 71.97 | 11.28 | 4.43 | 13.12 | 50.7% |

Important limits:
- No total-latency measurement: Quest was unworn and tracking=0; host-clock timestamps are unsuitable for native headset e2e timing.
- *Baseline t_enc stayed frozen through the measurement window. It is a reported stale value, not a validated steady-state baseline. Do not claim a measured end-to-end or encode speedup percentage from this column.
- Static video does not fill an 800 Mbps target. Actual Mbps above; this is not an 800 Mbps stress test.
- Native bridge repeated source frames: reported Mac/decoder rates ~76–100 FPS while the publisher stayed72. This is not unique-frame or gameFPS gain.
- All feature modes were verified in native logs, by capability negotiation and final encoder dimensions. Combined mode removes50.75% of encoded pixels, and crop reduces source render pixels24.18%.
- No visible-quality or game-performance validation in this test. Original app preferences restored.
