# Standalone Vision Pro host: design note (draft, 2026-10-29)

Goal: Cyberpunk 2077 (Windows build) runs on Apple Vision Pro (M2) through
VXRuntime, with nothing streamed from a Mac. This note says how the pieces sit
inside one visionOS app, based on what the visionOS 26.0 SDK (Xcode 26.0,
17A324) actually exposes. `VisionOSProto/` is the presentation-path prototype.

## 1. What the SDK gives us (checked in the headers, not from memory)

| Need | visionOS 26.0 SDK | Consequence |
|---|---|---|
| Late reprojection to the display rate | CompositorServices: the drawable's `deviceAnchor` (the pose the pixels were made from) plus `depthTextures`; the compositor warps every displayed frame | Depth is mandatory: without it only rotation is corrected |
| Fixed low app rate | `LayerRenderer.minimumFrameRepeatCount` (C: `cp_layer_renderer_set_minimum_frame_repeat_count`, visionOS 1.0+). The rate is display Hz / (n+1): at 90 Hz that is 45, 30, 22.5 or 18 | 22.5 is native. Exact 20 needs our own pacing (prototype "hold" mode) |
| Foveation without gaze | `isFoveationEnabled` plus `drawable.rasterizationRateMaps` (a layered map covers both eyes). The app never sees gaze | Bind the map in every pass that writes the drawable |
| Render quality | `LayerRenderer.renderQuality` / `maxRenderQuality` (visionOS 26) | A second dial before raising the repeat count |
| MetalFX temporal scaler | **`API_UNAVAILABLE(visionos)`** (`MTLFXTemporalScaler`, `MTL4FXTemporalScaler`, the denoised scaler and `MTLFXFrameInterpolator`). The simulator SDK has no MetalFX at all | Temporal upscaling must be ours. Options: FSR2 compiled natively to Metal (MIT), or a TAAU on the ANE |
| MetalFX spatial scaler | Available (`MTLFXSpatialScalerDescriptor`, iOS 16 / visionOS 1 availability) | Usable after our own temporal pass, or alone |
| Core ML on the ANE | `MLComputeUnits.cpuAndNeuralEngine` and `MLComputeDevice.allComputeDevices` (`.neuralEngine`) are not marked unavailable | Interpolation and super-resolution models can target the ANE. The on-device probe confirms this |
| Memory limit | `com.apple.developer.kernel.increased-memory-limit` (Boolean, in `VisionOSProto.entitlements`). The Developer portal lists it for VISION_OS. Our dev profile for team 7YNW843VY6 granted it automatically | The real limit is read at run time with `os_proc_available_memory()` (`<os/proc.h>`, iOS 13 / visionOS availability). The probe logs it at launch |
| Address space | `com.apple.developer.kernel.extended-virtual-addressing` (Boolean, VISION_OS listed, paid teams only). It was also granted | Needed for the Windows address-space layout and large file mappings. The probe measures the largest PROT_NONE reservation |
| JIT | No `dynamic-codesigning` for third-party apps | All x86 code must be recompiled ahead of time (the static recompilation track). The probe tries MAP_JIT and reports the outcome. In the simulator it is allowed, which says nothing about the device |
| Large data | `BackgroundAssets` (visionOS 2.4+), app container, `Network.framework` | See section 4 |

## 2. Process and thread layout inside the app

One process (a visionOS app cannot spawn processes):

```
VXProto.app (later: VeRantyx Player.app)
├─ main thread: SwiftUI (launcher window, settings, diagnostics)
├─ "game" threads: VXRuntime ARM64 + recompiled game modules
│     Windows threads are pthreads. NT API, Win32 and D3D12 are the VXRuntime layers.
│     D3D12 → Metal translation writes into GameSlot textures (color, depth, motion vectors)
├─ "present" thread: the CompositorServices loop (the prototype's ProtoRenderer)
│     90 Hz: picks the newest finished GameSlot, runs the warp core, writes the drawable
└─ ANE/Core ML queue: interpolation and super-resolution requests
```

The prototype's **hold mode is this exact structure**, with a synthetic scene
standing in for the game:

1. The game renders at its own fixed rate (20 or 22.5 fps) into offscreen
   2-layer textures (`GameSlot`: color, depth and the pose it was rendered from).
   There are three slots, so the 90 Hz pass never reads a slot that is being written.
2. Upscaling runs on the game's command buffer. In the prototype that is
   MetalFX spatial. In the product it is our temporal pass (FSR2-native-Metal
   or ANE) followed by optional MetalFX spatial.
3. The present thread runs every compositor frame. It copies the newest slot
   into the foveated drawable (with the rate map bound), writes the slot's depth,
   and sets `deviceAnchor` to the slot's pose. The compositor then reprojects to
   the display pose.

Frame-repeat mode is the simpler alternative: the game writes straight into the
drawable and visionOS repeats it. It cannot do 20 fps exactly. It also ties the
game's frame to the compositor's frame budget, so a 45 ms game frame misses
deadlines. Hold mode decouples them, which is the point.

### Warp core in the present pass (planned)

The present pass grows into the warp core, in this order:
- **DIBR stereo**: the game renders one view (with depth). The present pass makes
  the second eye by depth-based reprojection. This roughly halves the game's GPU work.
- **NPU interpolation**: between game frames N-1 and N, using the game's own
  motion vectors (`VXRT_NPU_INTERP=race`, the same path as on the Mac). The ANE
  produces a mid frame and the present pass shows it at the right display slot.
  Object motion (the orbiting cubes in the prototype) then stops juddering at the game rate.
- Head motion is already handled by the compositor (deviceAnchor plus depth),
  so the warp core does not need to correct head pose.

GPU priority is the risk: a 40 ms game command buffer and the 1 ms present
command buffer share one GPU, and public Metal has no queue priority. The
prototype measures this as "late" drawables in hold mode at high load. If it is
bad, the game's frame is split into several command buffers (VXRuntime already
submits per D3D12 ExecuteCommandLists), so the present pass can slot in between.

## 3. Code hosting

| Component | Form inside the app | Notes |
|---|---|---|
| VXRuntime (ARM64) | Static library linked into the app executable (`libvxrt.a`), C ABI | Same sources as the Mac build. Platform-specific parts (`mach_vm`, file mapping, the Metal device) sit behind the existing HAL. No Hypervisor.framework on visionOS: user-mode only |
| Recompiled modules | One signed `.framework` per PE module (`Cyberpunk2077.framework`, `REDEngine… .framework`, the d3d/dxgi shims), in `Frameworks/` | Generated on the Mac by the recompiler and signed with the team at build time. VXRuntime's PE loader maps image sections from the original `.exe`/`.dll` (data and relocations), but resolves code addresses through the recompiled module's address table instead of executing x86. Original PE files are data only |
| Unrecompiled x86 | Not possible on the device (no JIT) | Coverage must be 100 %. Anything missed is a hard stop with a log entry. The Mac build keeps the Rosetta fallback for discovery |
| D3D12 → Metal | Inside VXRuntime | Shaders are translated DXIL→MSL **offline on the Mac** and shipped as `.metallib` (pipeline cache), with `MTLBinaryArchive` built on the device on first run |
| Warp core | Swift/Metal in the app (the prototype's present pass) plus the Core ML model | The model (`.mlmodelc`) is in the bundle and compiled for the ANE on first run |
| Temporal upscaler | Metal compute in VXRuntime (FSR2 compiled natively for Metal, MIT, not translated) or an ANE model | MetalFX temporal does not exist on visionOS |

Signing: everything executable is in the bundle and signed by team 7YNW843VY6.
The recompiled game code is derived from the user's own copy. It is built on the
user's Mac and installed to the user's own headset only (a development or
personal build). It is never distributed.

## 4. The 70 GB of game data on the device

Vision Pro storage is 256 GB, 512 GB or 1 TB. App bundles must stay small, so
the data lives in the app container:

```
<container>/Library/Application Support/VXRT/        (isExcludedFromBackup = true)
├─ drive_c/                     Windows view (VXRuntime NT path root)
│  ├─ Games/Cyberpunk 2077/     the user's own install, copied from the Mac
│  │  ├─ archive/pc/content/*.archive   ~60 GB, read-only, mmap'd on demand
│  │  ├─ archive/pc/ep1/*.archive       Phantom Liberty
│  │  ├─ bin/x64/*.exe *.dll            PE images, data only (code is in Frameworks/)
│  │  └─ engine/, r6/, tools/ …
│  └─ users/player/AppData/Local/CD Projekt Red/Cyberpunk 2077/UserSettings.json
├─ shadercache/<gpu-family>/*.metallib, *.binarchive   ~1–2 GB
├─ saves/   (the game's own save path, mapped from Saved Games)
└─ manifest.json   file list, sizes and SHA-256 per file, recompiler version, PE checksums
<container>/Library/Caches/VXRT/   rebuildable (binary archives, logs); the OS may purge it
```

- **Transfer**: development and personal use go Mac → headset over the local
  network, through a small receiver in the app (Network.framework, Bonjour,
  resumable, checked against `manifest.json`) or `devicectl device copy to` for
  development. The source is the user's own Steam install on the Mac. Nothing is
  downloaded from third parties, and the user does not sign in to Steam on the device.
  BackgroundAssets suits App Store-hosted assets, which do not apply here.
- **Access**: `.archive` files are memory-mapped read-only (no copy into RAM).
  Mapped file pages are clean memory, so the kernel can drop them under pressure
  and they count less against the app limit than dirty memory. That is a large
  part of reaching ≤ 3.5 GB: textures stream from the mapped archive into
  `MTLHeap`s that the D3D12 layer sizes, not into duplicate CPU copies.
- **Space check** before transfer: about 70 GB of game data plus about 2 GB of
  shader cache. Ask the user before starting.
- Never write into `drive_c/Games/...` at run time (the same rule as on the
  Mac). Settings go only to the AppData `UserSettings.json`.

## 5. Budget recap and what the prototype measures

- The prototype logs the compositor rate, app frames, the game rate, heavy-pass GPU ms,
  present-pass GPU ms, late drawables, `os_proc_available_memory` and the
  footprint every second to `Documents/proto_log.jsonl`.
- With the GPU-load stepper, the prototype can raise the synthetic heavy pass to
  about 40 ms (the Cyberpunk El Coyote budget on the M2 after a 2.5x cut). That
  shows whether hold mode still keeps 90 Hz presents.
