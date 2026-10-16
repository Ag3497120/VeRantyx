# RemoteSpatialProbe

A small macOS app that renders stereo content on the Mac with Metal and shows it on an Apple Vision Pro through
Apple's **macOS spatial rendering** (macOS 26: SwiftUI `RemoteImmersiveSpace` + Compositor Services + ARKit on the
Mac). It is a feasibility and latency probe for VeRantyx: could SteamVR frames go to the headset through Apple's own
transport instead of our HEVC/UDP stream and visionOS app, and what does that cost in latency?

Nothing here has been run on a headset yet. The app builds against the macOS 26.0 SDK (Xcode 26.0).

## What it measures

Every frame goes into `~/Library/Logs/RemoteSpatialProbe/frames-<date>.csv` (times in seconds on the mach clock, the
same timebase as `CACurrentMediaTime()`):

| column | meaning |
| --- | --- |
| `frame_index` | `LayerRenderer.Frame.frameIndex` |
| `t_query`, `t_input` | when `queryNextFrame()` returned; when the wait for `optimalInputTime` ended |
| `optimal_input_time`, `rendering_deadline`, `predicted_presentation`, `trackable_anchor_time` | `frame.predictTiming()` |
| `drawable_presentation`, `presentation_frame_index`, `drawables` | `drawable.frameTiming.presentationTime`, `drawable.presentationFrameIndex`, `queryDrawables().count` |
| `pose_query_time`, `pose_delay_ms` | the time passed to `WorldTrackingProvider.queryDeviceAnchor(atTimestamp:)` (presentation − delay) |
| `anchor_present`, `anchor_tracked`, `anchor_timestamp`, `head_x/y/z` | the Vision Pro's `DeviceAnchor` from ARKit on the Mac |
| `t_commit`, `t_gpu_done`, `gpu_ms`, `late` | command buffer commit / completion, GPU time, finished after `renderingDeadline` |
| `flash`, `frame_repeat_count` | this frame shows the white flash square; `layerRenderer.minimumFrameRepeatCount` |

`session-<date>.txt` next to it has the capabilities the layer reported (foveation, layouts, formats, render
quality), the texture sizes, eye positions and IPD per drawable, ARKit events, every flash (press time, frame,
predicted presentation, press → presentation), and every spatial (pinch) event with its pose.

The main window shows the same live: frames rendered, late frames, skipped compositor frames, the median time from
frame query / input to predicted presentation, GPU time, whether device anchors arrive, pinch events, last flash.

In the headset (full immersion, all world-locked, nothing head-locked so reprojection is visible):
- a floor grid (red line = +X, blue line = forward −Z through the origin at your feet),
- cubes at 1, 2 and 3 m in front, one to the right, one behind you,
- a panel 2 m ahead: frame index (white) and predicted presentation time in s mod 10000 (green); the pose delay in
  ms (orange) when it is not zero,
- a 1 m grey square front-left that is white for exactly one frame on **Flash** / space bar (or every second with
  **Auto-flash**), for a camera or photodiode latency check,
- small yellow cubes at the pinching hand while you pinch (the only hand data the Mac gets, see below).

**Pose delay slider (reprojection test).** The frame is rendered with the head pose of *presentation − delay* and
that same pose is set as `drawable.deviceAnchor`. If the system reprojects from the drawable's anchor to the display
pose (as on visionOS), the world stays still when you turn your head even at 100–200 ms. If it swims, the remote
path does not reproject, and VeRantyx could not hand it frames rendered with an older pose. This is the most
important single result for VeRantyx.

## What the API is (verified in the SDK and Apple's docs)

- Scene: `RemoteImmersiveSpace(id:) { CompositorContent }` (SwiftUI, macOS 26 only), opened with
  `openImmersiveSpace(id:)`, closed with `dismissImmersiveSpace()`. Check `@Environment(\.supportsRemoteScenes)`.
  Opening prompts on the headset; only one Vision Pro at a time.
- Content: `CompositorLayer(configuration:) { layerRenderer in ... }` in a type conforming to `CompositorContent`.
- The device: `@Environment(\.remoteDeviceIdentifier)` (a `RemoteDeviceIdentifier?`, not stable across launches)
  → `ARKitSession(device:)` (from `_ARKit_SwiftUI`; C: `.cDevice` → `ar_session_create_with_device`).
- Poses: `WorldTrackingProvider` + `queryDeviceAnchor(atTimestamp:)` are available on macOS; set the result as
  `drawable.deviceAnchor`. World anchors, `anchorUpdates` and authorization queries are not.
- Hand tracking: **not available**. `HandTrackingProvider`, `HandAnchor`, `HandSkeleton` are
  `@available(macOS, unavailable)` and `hand_tracking.h` is `API_UNAVAILABLE(macos)` (so are plane detection, scene
  reconstruction, image/object tracking, camera frames). The Mac gets pinches through
  `LayerRenderer.onSpatialEvent`: kind `.indirectPinch`, `inputDevicePose.pose3D`, `selectionRay`, `chirality`.
- Immersion: `.full` and `.progressive` only (`.mixed` is unavailable on macOS; no passthrough). RealityKit is not
  supported; Metal 3 or Metal 4 rendering only.
- Frame loop (same as visionOS): `queryNextFrame()`, `startUpdate/endUpdate`, `predictTiming()` (optimalInputTime,
  renderingDeadline, presentationTime, trackableAnchorTime), `LayerRenderer.Clock().wait(until:)`,
  `queryDrawables()` (the singular `queryDrawable()` is unavailable on macOS), `encodePresent(commandBuffer:)`.
  `minimumFrameRepeatCount` and `renderQuality` exist on macOS too.
- Foveation: `capabilities.supportsFoveation`, `configuration.isFoveationEnabled`, `drawable.rasterizationRateMaps`
  exist on macOS; whether the remote device reports support is only known at run time (logged).
- Connection: Wi-Fi or USB through the Vision Pro Developer Strap (Apple engineer on the forums); no API tells the app
  which one is in use.
- Latency: Apple's docs and the WWDC25 session say nothing about it. That is what this probe is for.

Sources: WWDC25 session 294 "What's new in Metal rendering for immersive apps" (chapter "macOS spatial rendering"),
the `RemoteImmersiveSpace` documentation page, Apple developer forums threads 787528 and 818404, Xcode 26's
macOS "Spatial Rendering App" template (whose structure this app follows), and the macOS 26.0 SDK's
`SwiftUI`, `CompositorServices`, `_CompositorServices_SwiftUI`, `ARKit`, `_ARKit_SwiftUI` interfaces.

## Build

```bash
RemoteSpatialProto/scripts/build.sh              # xcodegen + xcodebuild, ad-hoc signed, does not launch
SIGN=dev RemoteSpatialProto/scripts/build.sh     # sign with the local "Apple Development" identity instead
```

The app: `RemoteSpatialProto/build/DerivedData/Build/Products/Release/RemoteSpatialProbe.app`.

## Run it (you, wearing the headset)

1. Mac on macOS 26 or later, Vision Pro on visionOS 26 or later, same Apple Account, same Wi-Fi network (or the
   Developer Strap's USB-C cable for the wired case). Close VeRantyx's own Vision Pro app and the stream first so
   they do not compete for the network.
2. Open the app (`open RemoteSpatialProto/build/DerivedData/Build/Products/Release/RemoteSpatialProbe.app`).
   "supportsRemoteScenes: yes" must show; if it says no, this Mac cannot do it.
3. Put the headset on, click **Open remote immersive space** on the Mac, pick the Vision Pro if asked, and accept
   the prompt on the headset.
4. Look around for ~30 s standing still, then ~30 s turning your head side to side, with the pose delay at 0.
5. Set the pose delay to 50, 100 and 200 ms (about 20 s each, turning your head) and note whether the cubes and
   grid stay still or swim/lag behind your head motion.
6. Press space (or **Flash**) a few times; optionally turn on **Auto-flash** and film the square through the lens
   together with the Mac screen for a camera latency check.
7. Pinch a few times (look at the panel and pinch) to see whether spatial events arrive.
8. Repeat steps 3–5 once over Wi-Fi and once over the Developer Strap if you have one. Then **Close**.

## What to send back

- The output of `RemoteSpatialProto/scripts/summarize.py` (newest run; or pass a CSV path) for each connection type.
- The `session-*.txt` file (capabilities, texture sizes, IPD, flashes, ARKit and spatial events).
- What you saw: does the world stay still at pose delay 0 / 50 / 100 / 200 ms? Any judder, black frames, stereo
  problems (cubes doubled or at the wrong depth)? How sharp is the panel text (render resolution)?
- Whether the device picker/prompt appeared and how long it took to connect; anything the window showed in red.

## Unknowns (cannot be checked without running it)

- Whether `presentationTime` / `optimalInputTime` on the Mac are already on the Mac's clock (the CSV shows it:
  query → presentation should be a positive few tens of ms) and how much transport latency they include.
- Whether the remote compositor reprojects with `drawable.deviceAnchor` (the pose delay test).
- Frame rate (90 / 96 / 100 Hz?), texture size per eye, whether foveation is supported remotely, and which layout
  (layered / dedicated) the remote device offers. The app chooses layered when offered and handles dedicated too.
- Whether `ARKitSession.run` succeeds without any authorization step on the Mac, and how fresh the device anchors are.
- Whether an ad-hoc signed app is accepted (forum reports of a device picker that never appears or a "Waiting"
  state exist even with Apple's template; try `SIGN=dev` if it stalls).
- Wired vs. wireless difference; whether the Mac window must stay frontmost for the space bar.
- `onSpatialEvent` pinch poses: the coordinate space they use on macOS (assumed the immersive space origin).
