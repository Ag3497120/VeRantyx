# OpenVR shim (libopenvr_api replacement)

Half-Life: Alyx's Linux ARM64 build talks to the headset through OpenVR: its
`libvr.so` calls `VR_InitInternal2` and `VR_GetGenericInterface` on the
`libopenvr_api.so` it ships. On Linux that library finds SteamVR. On Vision Pro
there is no SteamVR, so Klepton replaces the library with this one (the same
move it makes for `libOVRPlugin.so` and the OpenXR loader) and the game gets
head tracking, frames and PSVR2 Sense input from visionOS instead.

```
 Alyx (libvr.so, libclient.so ...)           Linux ARM64, relinked by klepton-ld
        |  VR_GetGenericInterface("IVRSystem_0xx") -> C++ vtable calls
        v
 this shim   src/        IVRSystem  IVRCompositor  IVRInput  (+ chaperone, settings, render models)
        |  klvr_platform.h (plain C seam)
        v
 platform/klvr_platform_klepton.c   ->  kl_ovrp_* (poses, buttons, haptics, frame records)
                                        kl_vulkan_* (eye images, frame fence)
        v
 Klepton's visionOS frontend: CompositorServices, ARKit, GameController (Sense)
```

## What each part does

- **Vtables for every OpenVR version.** We cannot see which interface
  versions Alyx asks for until it boots, so `tools/gen_vtables.py` walks all
  64 tagged OpenVR SDK releases and builds all 123 interface versions from
  Valve's own `openvr_api.json`, cross-checked against `openvr.h`. A method we
  implement lands in its slot in every version that has it. A method we do not
  implement lands on a stub that logs `NOT IMPLEMENTED: IVRxxx::Name` once and
  returns zero, or an error code when the method returns one.
  `KLVR_STRICT=1` aborts on the first one instead.
- **IVRSystem** (`klvr_system.cpp`): render target size, projection, eye
  offsets and IPD from CompositorServices' view geometry; device classes and
  properties; poses in the standing and seated universes; events; the legacy
  controller-state API.
- **IVRCompositor** (`klvr_compositor.cpp`, `klvr_vk_submit.cpp`):
  `WaitGetPoses` blocks on Klepton's frame pacer and pins the head pose for
  the frame. `Submit` blits each eye from the game's `VkImage` into Klepton's
  eye image on the game's own queue, which handles side-by-side bounds, flips
  and format changes. The second eye closes the frame with the pose it was
  rendered from, so reprojection corrects against the real pose.
- **IVRInput** (`klvr_input.cpp`): reads the game's action manifest and the
  binding file it ships for our controller type, then evaluates those bindings
  against the Sense controllers every `UpdateActionState`. This covers
  boolean, vector1/2, pose, skeleton and vibration actions, the `button`,
  `trigger`, `joystick`, `dpad`, `grab`, `toggle_button` and `scalar_constant`
  modes, action sets and device restriction. Chords and `complex_button` are
  logged, not served.
- **Hand skeleton** (`klvr_skeleton.cpp`): the 31-bone SteamVR hand. The
  reference poses are Valve's own data, from the BSD-licensed SteamVR Unity
  plugin (`tools/gen_hand_poses.py`), because a skeleton that is off by one
  axis puts the fingers through the palm. Live poses blend Valve's open hand
  and fist per finger, using curls estimated from Sense contact: the thumb on
  the buttons or stick, the index finger on the trigger, the middle finger on
  the grip.
- **Haptics:** `TriggerHapticVibrationAction` and the legacy pulse go to the
  Sense controllers through Klepton's CoreHaptics path.

## Choices that may need revisiting on device

| Choice | Why | Knob |
|---|---|---|
| Controllers present as **Oculus Touch** | Alyx ships Touch bindings, and Sense has the same layout: a stick, two face buttons, a trigger, a grip and capacitive touch | `KLVR_CONTROLLER_TYPE`, `KLVR_BINDING_FILE` |
| Sense pose -> OpenXR grip: Klepton's pose pitched +37 deg about X | Klepton stores the Sense pose pitched -37 deg for OVRPlugin guests; kl_openxr undoes it the same way | `KL_XR_GRIP_PITCH` |
| Controller "raw" pose = SteamVR's Touch device origin | From the "openxr_grip" component of SteamVR's Quest 2 controller render models: the grip sits at (-/+0.007, -0.0018, 0.102) m, pitched 20.6 deg up, in the device's frame | `KLVR_RAW_FROM_GRIP="x,y,z,pitch"` |
| "Grip limit" skeleton pose = 80% of the fist | Valve's plugin does not ship that pose | `KLVR_GRIP_LIMIT` |
| No eye-tracked foveation | visionOS does not give apps gaze, so `GetEyeTrackedFoveationCenter` returns false and the game falls back to its fixed path. The foveation thread owns what replaces it | none |
| Render size = the platform's per-eye size | Taken from `kl_ovrp_eye_texture_size` | `KLVR_RENDER_SCALE` |
| No render models, no settings store, 2 m play area | Alyx draws its own hands, and visionOS draws its own boundary | `KLVR_PLAY_AREA_M` |
| Vulkan only | Source 2 on Linux renders with Vulkan | none |

## The Linux-to-Darwin ABI

The game is a Linux (AAPCS64) caller, and this shim is compiled for Apple's
arm64 ABI. The two differ on arguments narrower than 32 bits. Apple's ABI has
the caller zero- or sign-extend them and lets the callee rely on that, while
AAPCS64 leaves the upper bits undefined. Every OpenVR method with a `bool`,
`uint16_t` or similar argument (30 of them, such as `TriggerHapticPulse` and
`FadeToColor`) is therefore reached through a generated thunk that takes the
argument as `uint32_t` and truncates it explicitly. `tests/` calls one of
these slots with junk in the upper bits to prove it.

Struct layouts match: `openvr.h` packs the same way on `__linux__` and
`__APPLE__`. The interfaces have no virtual destructors, so slot N is the
Nth method on both sides.

## Building and testing (host)

```
make test VULKAN_INCLUDE=<Vulkan-Headers>/include
```

The test drives the shim through `VR_GetGenericInterface` and Valve's own C++
interface classes, so a slot out of place or an implementation that fell
through to its stub fails it. The platform in this build is
`platform/klvr_platform_null.c`, which a test can script: poses, buttons, fake
Vulkan entry points. It checks 198 things, including binding evaluation, action
sets, pose actions, the skeleton, compressed bone data, haptics routing, eye
blits with bounds and flips, and the narrow-argument thunk.

`make regen OPENVR_REPO=... STEAMVR_UNITY_REPO=...` regenerates
`src/generated/`, which is checked in so normal builds need neither clone.

## Integrating into Klepton

The Klepton fork belongs to the "Boot Alyx engine via Klepton" thread, so the
fork change ships here as a patch for that series:
`klepton/0009-m_alyx-bind-libopenvr_api.so-to-the-OpenVR-shim.patch`. It
applies on top of that thread's patches 0001 to 0008 and does three things:

1. **Build.** The Makefile compiles `src/*.cpp` and
   `platform/klvr_platform_klepton.c` into `m_alyx` when `KLVR_DIR` (default
   `runtime/openvr`) points at this directory. Without it the build is as
   before.
2. **Imports.** `m_alyx` treats `libopenvr_api.so` as served, so the game's
   copy is never mapped, and resolves `VR_*` names through
   `klvr_klepton_lookup` ahead of the glibc personality. `--gap` shows them
   resolved.
3. **Frame clock.** Nothing on the Mac installs a pacer, so the shim runs in
   host mode (below). The visionOS app will install
   `klvr_klepton_set_frame_pacer(guest_pace_wait)` where it installs the
   OpenXR one, once Alyx runs there.

On the Mac:

```
cd ~/alyx-depot/klepton
git am <project repo>/shim/openvr/klepton/0009-*.patch
ln -s <project repo>/shim/openvr runtime/openvr
make build/m_alyx
./build/m_alyx ~/alyx-depot/linux/game ~/alyx-depot/sysroot --run -console ...   # no -novr
```

**Host mode.** With no headset frontend, the shim keeps the game on its VR
path without pretending to show anything: frames run on a timer at the
display rate, the head is fixed (`KLVR_HOST_HEAD="x,y,z,yaw_deg"`, default
`0,1.6,0,0`), both controllers are connected and held still in front of the
body (`KLVR_HOST_HANDS=0` disconnects them), and `Submit` accepts each eye
without copying it. The game's own companion window is the picture.
`KLVR_HOST=0/1` overrides the automatic choice.

Optional, for better finger curls on device: have `KleptonControllers.swift`
publish the Sense controllers' capacitive trigger and stick touches in the
`touches` word. The shim infers them from the analog values until then.

## Not done yet

- **First contact (2026-09-27, Mac, host mode):** Alyx accepts the shim as
  SteamVR ("SteamVR initialized!", Apple Vision Pro, 72 Hz, zero-copy
  multiview) and binds `bindings_touch.json` (45 actions, 6 sets). It needs
  `-vr` on the command line. The run stalled at the first level load, which
  the next run checks against these fixes:
  - `FadeGrid` / `GetCurrentGridAlpha` now fade over time. The grid alpha was
    stuck at 0, and Source 2 fades to the grid before it starts a level load.
  - `IVRMailbox_001` is served for real. Its four slots are Register,
    Unregister, Send and Read, and it posts the `{"type":"ready"}` message the
    game waits for.
  - `VRControlPanel()` is exported, with slot 15 (dashboard toggle) and slot
    26 (external web root) answered.
  - IVROverlay has a real registry. `FindOverlay` returns `UnknownOverlay`
    for keys nobody created.
  - `IXrProto_001` returns InterfaceNotFound, as public SteamVR does.
  - When the platform has published no eye geometry, the IPD falls back to
    63 mm (`KLVR_IPD_M` changes it).
  - The HMD reports `HasDriverDirectModeComponent`.
- **Chapter 1 loads in VR mode (2026-09-28, Mac).** Serving
  `IVRIPCResourceManagerClient_004` switches libvr to "zero copy
  presentation", and then no frame reaches `Submit`. It is withheld
  (`KLVR_IPC_PROBE=1` serves the probe again), so libvr logs "Using copies!"
  and submits every frame. `IVRInputInternal_002` is still a logging probe.
- **On the headset (2026-09-28):** Chapter 1 starts on Vision Pro in
  device mode. `GetProjectionRaw` follows SteamVR's convention: "top" is
  minus the *down* tangent and "bottom" the *up* one, which is what makes
  `ComposeProjection` span the real frustum (OpenComposite measured it against
  SteamVR; a 04:28 change had it the other way, mirroring each eye's frustum
  vertically, and was reverted). Also, the first display
  query waits up to 5 s for CompositorServices' real per-eye frustums
  (`kl_ovrp_eye_frustum_ready`, weak), because Alyx reads the projection once
  at VR init.
- Debugging aids: `KLVR_TRACE_CALLS=1` counts calls per interface slot and
  prints them every 5 s (Apple arm64 only). `KLVR_TRACE_MAILBOX=1` logs
  mailbox reads. `KLVR_TRACE_FRAMES=1` prints, every 5 s, where frame time goes:
  blocked in `WaitGetPoses`, game work before the first `Submit`, between the
  eyes, and after the second `Submit`. `KLVR_TRACE_POSE=1` prints, once a
  second, the platform's head, eye geometry and controller state beside the
  poses the game receives. Every change in what `Submit` is handed (size,
  format, MSAA, bounds, layer) is logged.
- Submits with a pose (`Submit_TextureWithPose`, which Alyx uses) close the
  frame with the pose the game says it rendered from (`KLVR_SUBMIT_POSE=0`
  uses the handed-out one), and a depth projection attached to the submit is
  read back as tangents and logged beside the platform's ("MISMATCH" if the
  game rendered a different field of view). The frame is then closed with the
  frustum the game rendered with, so the compositor places the picture as it
  was drawn (`KLVR_RENDERED_FOV=0` always uses the platform's).
- Mac-side stand-ins for the headset: `KLVR_TAN="L,R,U,D"` hands the game
  that left-eye frustum (the right eye mirrored) in place of the platform's,
  and `KLVR_HOST_FPS=N` paces host frames at N per second while the display
  keeps reporting its own rate.
- `KLVR_ASYNC_FRAMES=1` (off by default, not yet run on device): the second
  `Submit` returns at once and a worker publishes the frame when the shim's
  copies finish, with `KL_VK_FRAME_SYNC=0` so Klepton does not also idle the
  game's queue. Without it a frame costs CPU + GPU time back to back.
- The platform follows the frontend's display clock (logged once as
  "clock: ..."), because Darwin's CLOCK_MONOTONIC and CACurrentMediaTime part
  ways after the device first sleeps.
- `GetFrameTiming` reports how many display intervals each frame took
  (`m_nNumFramePresents`, `m_nNumDroppedFrames`), for a game that advances its
  clock by the display period.
- Overlays are tracked, not drawn. If Alyx shows its loading screen or menus
  through one, `SetOverlayTexture` is where drawing starts.
- The Mac run renders nothing to a headset (host mode). The device path,
  Klepton's `kl_app.c` installing the frame pacer, comes with the visionOS
  target.
