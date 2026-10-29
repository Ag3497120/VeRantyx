# VisionOSProto

A visionOS test app (bundle id `marimokodai.VisionOSProto`, team 7YNW843VY6) that
proves the standalone presentation path without Cyberpunk: a synthetic heavy scene
rendered at a low, fixed rate. The app gives the compositor depth and uses foveation.
It does not replace the main app (`marimokodai.VisionSpatialTools`).

Build:

    cd VisionOSProto && xcodegen generate
    xcodebuild -project VisionOSProto.xcodeproj -scheme VisionOSProto -destination 'generic/platform=visionOS' -derivedDataPath build/device -allowProvisioningUpdates build
    xcodebuild -project VisionOSProto.xcodeproj -scheme VisionOSProto -destination 'generic/platform=visionOS Simulator' -derivedDataPath build/sim build

Install and launch on the headset (the headset must be worn and unlocked):

    D=$(xcrun devicectl list devices | grep -i 'Vision Pro' | grep -oE '[0-9A-F-]{36}' | head -1)
    xcrun devicectl device install app --device $D build/device/Build/Products/Debug-xros/VisionOSProto.app
    xcrun devicectl device process launch --device $D marimokodai.VisionOSProto -fps 22.5 -mode repeat -load 24
    xcrun devicectl device copy from --device $D --domain-type appDataContainer --domain-identifier marimokodai.VisionOSProto --source Documents/proto_log.jsonl --destination proto_log.jsonl

Launch arguments: `-fps 18|20|22.5|30|45|90`, `-mode repeat|hold`, `-load N` (fragment
iterations), `-depth NO`, `-foveation NO`, `-scale 0.4..1.0` (hold render scale with
MetalFX spatial), `-animate NO`, `-autoImmersive YES`. The environment variables
`VXPROTO_FPS` and `VXPROTO_MODE` override them. All of these can also be changed live in the window.

Modes:
- `repeat`: `LayerRenderer.minimumFrameRepeatCount`. visionOS shows each frame n+1 times and reprojects each repeat.
- `hold`: the game renders offscreen at exactly the set rate. A 90 Hz present pass copies the newest frame with its depth and pose. This is the planned product architecture.

See DESIGN.md for the SDK findings and the hosting and data-layout design.
