# Video streaming quality and latency components

Source-level contributions for VeRantyx's streaming pipeline: hardware encoder policy, bounded receiver work, crop/FOV geometry and GPU foveated packing. These components are intentionally separate from a particular launcher or private client binary. They do not change the existing encoder defaults or claim an end-to-end VeRantyx speedup.

## Components

- `videotoolbox/`: direct H.264/HEVC session helpers, hardware-only selection/proof, opt-in throughput properties, experimental negotiated HEVC temporal layers and transparent byte/window rate-limit forwarding. There are no process-wide interposers. An unsupported property is an error to handle, not a silently successful setting.
- `geometry/`: per-eye crop derived in tangent space, alignment-aware image sizes and a separable piecewise-linear foveated map. Crop removes temporal edges while preserving nasal edges. Foveated packing keeps the center at full source density and reduces peripheral samples.
- `macos/` and `shaders/`: Metal packer with bounded buffers, explicit producer events and source/destination lifetime retention. It does not require CPU pixel readback. The caller must honor the completion/event contract before consuming the packed buffer.
- `protocol/` and `receiver/geometry_types.h`: portable descriptor examples and codec-standard SEI / paired PyroWave envelope helpers. These are **not** an existing VeRantyx or ALVR wire format. A matching authenticated handshake and frame/epoch validation are still required before use.
- `receiver/`: fence-safe image retirement, a size-keyed SGSR policy and portable SEI parsing. Original generation/cookie ownership is preserved until GPU completion. These are independent of a decoder's Android ABI.
- `hevc/`: a bounded two-credit receiver queue for an explicitly paired Apple HEVC two-sublayer stream. It retains T0 dependencies, discards only proven TSA T1 pictures, rejects unknown VCL, and recovers at IDR. Do not apply it to arbitrary HEVC streams or drop dependent P pictures.

## Source integration

1. At session creation, require hardware and probe the actual codec/dimensions. H.264 uses the explicit Apple AVC encoder selector; check hardware proof after creation and report unsupported sizes. A dimension that worked on one Mac is not a universal hardware limit.
2. Keep the current encoder profile as baseline. The opt-in throughput policy (`RealTime=false`, low-latency rate-control selector disabled, no frame reordering; H.264 CAVLC) is an alternative to benchmark on the recipient Mac. Keep caller bitrate/rate-window limits intact across session reset. Upstream already has several low-latency settings; these helpers do not claim those settings as new work.
3. Resolve one immutable render/video geometry before starting the game. For crop, supply both the cropped FOV and reduced render size to the source. Foveated video alone does not reduce game render work.
4. Pack after source production and before encoding. Feed packed dimensions consistently into surfaces, encoder and frame metadata. Preserve original render pose and timestamps. The optional `MetalPacker` uses producer GPU events rather than CPU waits.
5. The receiver validates matching capabilities, descriptor, dimensions, PTS and epoch, then samples the inverse map. Restore a uniform source grid before SGSR; invalidate incompatible cached resources on a geometry change. Do not transform UI/lobby layers or silently enable a mapping on an old client.
6. Stage changes to the native Vision Pro and ALVR paths independently. This PR does not replace ALVR's existing protocol/FFR implementation. No client is automatically opted into a new wire format.

The 232-byte paired descriptor and separate 324-byte portable wire example are distinct contracts, not interchangeable formats. Neither authenticates itself. The examples are tools for integration, not a replacement for the transport's authentication.

## Tests

```sh
cmake -S Streaming -B build/streaming -DSTREAMING_UBSAN=ON
cmake --build build/streaming -j
ctest --test-dir build/streaming --output-on-failure
```

On macOS add `-DSTREAMING_METAL_TESTS=ON` for the synthetic GPU pixel/event test. It does not start a game, create a video compression session or connect to a headset. Historical hardware/USB results and their limits are in [tests/RESULTS.md](tests/RESULTS.md); they are not VeRantyx harness results.

The separate PyroWave contribution supplies the modified codec/backend work. This directory's carrier is only a transport example, not a bundled PyroWave implementation.
