# PyroWave validation and historical measurements

## This source contribution

The standalone ARM64 Metal codec builds with CMake/Apple Clang. Five CPU/UBSan tests cover 1000 reference packet-batch/counter cases, atomic rejection of malformed tails, and explicit/default/invalid wavelet selection. The consolidated Vulkan patch was checked against a clean checkout of its exact upstream commit. A fresh Android receiver build and VeRantyx headset run have **not** been performed.

Historical transform/shader validation and paired-client tests preceded this extraction. The extracted library now defaults to CDF 9/7 and requires explicit CDF 5/3 selection; the private-client adapters were removed. Historical performance numbers therefore do not certify a newly integrated VeRantyx build.

## Historical environment and successful high-bitrate tests

M3 Max / 36 GB Mac, Quest 3 over USB, MoltenVR with original and paired modified clients, 26 October 2026. These are short approximately 20-second telemetry windows, not long-session certification. There is no optical motion-to-photon measurement. `evidence/high-bitrate-summary.json` contains derived values and hashes of the retained source records without raw personal device logs.

| Variant / per eye | Actual Mbps (mean) | Reported headset FPS | Encode ms | Decode ms | Reported headset E2E ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Modified / 2368×2560 | 2000.02 | 71.99 | 5.37 | 9.77 | 47.84 |
| Modified A / 2144×2304 | 1953.91 | 71.99 | 4.96 | 7.23 | 43.54 |
| Modified B / 2144×2304 | 1962.43 | 71.99 | 4.41 | 8.04 | 46.40 |
| Original / 2144×2304 | 1978.56 | 71.97 | 4.09 | 11.13 | 48.69 |
| Overload reference / dimensions not retained in this record | 1995.20 | 65.70 | 5.08 | 28.68 | 88.52 |

All rows requested 72 Hz and a 2000 Mbps budget. The two modified 2144 runs share one original baseline; they are not independent A/B pairs. Actual bitrate and image characteristics differ. Client/package identity was not captured in every live window. Do not turn these rows into an equal-quality causal speedup percentage. The overloaded case demonstrates that 2000 Mbps alone does not guarantee a fast receiver.

These observations support evaluating an optional codec with low sender/decoder cost at high bitrate. They do not prove that H.264 has a universal 1000 Mbps maximum. H.264 was tested at up to 1000 Mbps in the contributor's specific setup; that menu/test boundary is not a codec-standard limit. There is no controlled cross-codec equal-quality comparison here.

## Lower-resolution ACK-chain test

At 1664×1792 per eye, 72 Hz and a 600 Mbps target, the retained no-game encode → USB → native decoded-image acquisition → USB ACK test reported:

| Variant | Actual Mbps | ACK-chain mean ms | Reported fresh acquisition FPS |
| --- | ---: | ---: | ---: |
| Fast53 A | 599.11 | 29.80 | 71.98 |
| Fast53 B | 599.18 | 31.67 | 71.80 |
| Older path | 599.08 | 59.56 | 57.16 |

See `evidence/ack-600.json`. This test used an older retained encoder/decoder baseline rather than the later downloaded game baseline above. ACK-chain timing includes the diagnostic return path, is not game E2E and is not optical latency. CDF 5/3 versus CDF 9/7 is not an equal-quality test.

## Not inferred

No new 3–5 ms production win is attributed to ACK/TCP diagnostic changes. No bitrate UI unlock is counted as an encoder optimization. No long-session, Wi-Fi or VeRantyx game performance is certified by these tests. No VXRuntime harness results are claimed.
