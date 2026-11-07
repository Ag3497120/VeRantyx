# Source attribution

PyroWave is authored by Hans-Kristian Arntzen / Themaister under MIT. The retained Metal source and its original notices are in `metal/`; `PROVENANCE.json` records the upstream revisions used by the contribution.

The CDF 5/3 Vulkan work also builds on the MIT-licensed Quest3-Pyrowave work by JMS1717. Its original transform/kernel contributions and notices are retained in the consolidated patch. This contribution adds/combines the matched Metal path, distortion/RDO corrections, fused full-RGBA output, packet batches and scratch-image cleanup; it does not claim sole authorship of the codec or the CDF transform.

Granite, Volk and Vulkan headers remain upstream dependencies with their own notices; copies of relevant notices are in `licenses/`. Android NDK, Xcode and Apple platform frameworks are external dependencies, not included SDKs.

Application-specific ABI/ELF hooks, binary-offset patchers, signing tools/keys, APKs, application binaries, game files, device identifiers and private settings are excluded. Measurement-environment attribution is recorded separately in `tests/RESULTS.md`.
