# Native loading interstitials

The MoltenVR bridge renders Alyx mailbox loading messages and StageOverride requests with an asynchronous SceneKit worker. The room, holographic map, route geometry, DDS textures and fonts come from the owner's installed game. The renderer is adapted from upstream VeRantyx421b682 under MIT. No game resources are shipped.

The producer continues submitting OpenXR frames while game submits stall. Each cached stereo frame keeps the head pose and frustum it was rendered with; the existing rotational warp uses those values. Loading ends automatically on the mailbox end event. Missing room assets fall back to map/text; an unavailable renderer is not advertised to the guest. Normal gameplay keeps the existing frame path.

Build the optional `libalyxloading.dylib` using build-release.sh and ship it next to bridge. start.command configures the session-private event paths. LoadingTests.swift and BridgeLoadingTest.cpp exercise actual owner assets, stage-ready handshake, image output, C ABI, pose metadata and end-of-load handoff without running Alyx or requiring a headset.
