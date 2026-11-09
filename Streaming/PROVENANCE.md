# Provenance and license

The geometry, descriptor, Metal packer, encoder policy, bounded queue and ownership contributions are supplied from armax's local streaming work under the repository's MIT license. Existing notices in retained source files remain applicable.

The foveated map is an original three-segment separable linear mapping. ALVR's fixed-foveated transport and PSVR2Toolkit were inspected as prior art; their shader/code was not copied into these mapping functions. This does not claim ALVR protocol compatibility or identical reconstruction quality.

Apple VideoToolbox/CoreMedia/CoreVideo/Metal frameworks are external platform dependencies. The OpenXR interpretation of FOV angles follows the public specification; no loader or headset runtime is bundled.

No client/app binary, binary-offset patcher, game content, extracted proprietary shader, device identifier, pairing secret or signing key is included. Historical scalar measurements identify their actual test environment in `tests/RESULTS.md`.
