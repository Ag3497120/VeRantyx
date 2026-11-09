# Optional PyroWave quality and latency contribution

A separate source contribution for evaluating PyroWave as an optional VeRantyx stream codec, especially for high-bitrate USB use. This provides a modified standalone Metal implementation and a consolidated Vulkan-backend patch, with the associated scalar test evidence. It does not change VeRantyx's current codec, client or stream defaults.

## Why evaluate it

The historical tests in [tests/RESULTS.md](tests/RESULTS.md) include successful approximately 2000 Mbps / 72 Hz streams with roughly 4–5 ms sender encoding and 7–10 ms receiver decoding. These are useful low codec-stage costs at high bitrate. They are not a promise of 10–20 ms motion-to-photon or a universal speed/quality advantage over H.264/HEVC. A separate overloaded case is retained too.

## Changes included

- Paired CDF 5/3 Metal encoder/decoder support and its transform/shader source, retaining CDF 9/7 compatibility. CDF 5/3 is an existing upstream/donor technique; this contribution does not claim to invent the transform.
- Correct weighted-discard distortion accounting and CDF 5/3 luma initial-error rate/distortion accounting. Ordinary CDF 9/7 remains available.
- Fused final CDF 5/3 inverse synthesis and colour conversion into ordinary full-size RGBA, avoiding an extra presentation conversion while preserving existing uniform-image consumers.
- The CDF 9/7 fragment-to-RGBA optimization and omission of four unconsumed scratch images. The latter saves logical image storage; no committed-VRAM/FPS gain is inferred just from allocation counts.
- Validated exact-copy CPU packet batching with original-parser fallback. The separate framed/preflight experiment is not enabled.
- Explicit source-level controls instead of application-ID detection or binary-offset hooks. Historical ABI shims, renderer trampolines, APK builders and app patchers are excluded.

The Vulkan patch retains upstream/donor alternative transform support needed to reconstruct the reviewed backend. Only CDF 5/3 and CDF 9/7 profiles are proposed here; the additional legacy modes are not new recommended presets.

## Build and test

```sh
cmake -S StreamingPyroWave -B build/pyrowave-tests
cmake --build build/pyrowave-tests -j
ctest --test-dir build/pyrowave-tests --output-on-failure

# Optional standalone macOS Metal library (does not install or start a stream)
cmake -S StreamingPyroWave/metal -B build/pyrowave-metal \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build/pyrowave-metal -j
```

The receiver patch applies directly to PyroWave commit `d2997ac172bdc00e29c58e3f2938acb7e94580bf`:

```sh
git -C /path/to/pyrowave checkout d2997ac172bdc00e29c58e3f2938acb7e94580bf
git -C /path/to/pyrowave apply --check /path/to/StreamingPyroWave/patches/vulkan-backend.patch
git -C /path/to/pyrowave apply /path/to/StreamingPyroWave/patches/vulkan-backend.patch
```

The matching Granite source pin and original upstream references are in `PROVENANCE.json`. A fresh Android/visionOS receiver build is still required; this PR does not bundle an SDK or claim that build has been performed.

## Integration contract

- Negotiate codec version, wavelet/chroma/precision and presentation layout before creating either endpoint. Do not send CDF 5/3 to an unmodified CDF 9/7 receiver. The extracted Metal library deliberately defaults to CDF 9/7; set `PYROWAVE_WAVELET=53` only for a matched pair. This safer opt-in differs from the historical standalone test build's default.
- The reviewed fused CDF 5/3 backend uses `pyrowave_decoder_set_cdf53v2(decoder, 8)` with its required FP16/precision/chroma support. Check the return value. Consume the full-size RGBA output; do not assume packed-YUV plane layouts from older modes.
- `PYROWAVE_V9_PACKET_BATCH=1` explicitly selects the measured backend batch path. Unknown packet layouts fall back to the ordinary parser. No package name automatically enables a profile.
- Preserve render timestamps, frame IDs and GPU image ownership. Keep the tested two-job decoder baseline; higher bitrate is not a reason to remove backpressure or fence handling.
- A new optional codec requires both a sender connection and a compatible VeRantyx headset client. Existing ALVR clients do not become compatible merely because the library builds. Native Vision Pro presentation and codec negotiation remain integration work.
- Report actual bitrate separately from the menu target. Treat 2000 Mbps as a tested configuration on one USB setup, not a universal Wi-Fi, codec or headset limit.

Source build checks and synthetic parser tests do not replace a new recipient-side A/B. Attribution and licenses are retained; no private application or game files are included.
