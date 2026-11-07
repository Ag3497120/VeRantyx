# Upstream integration validation

Validated on Apple Silicon/macOS with the Xcode toolchain while preparing this contribution. These are synthetic/build checks, not an end-to-end headset certification of the upstream branch.

- Full `make -C NativeARM check` passed, including the private-overlay sandbox tests, with a writable Swift module-cache path and permission to run the suite's own sandbox.
- `make -C NativeARM check-syntax` passed.
- Runtime cases include independent both-hand capacitive input, release-time pose/velocity, frame cadence/history and per-frame projection metadata without overwriting legacy IPD/haptic fields.
- Async pipeline tests passed, including required first-use lighting/depth success/failure and ordinary nonblocking materials.
- The synthetic MoltenVK GPU test verified 16 first-use depth pixels and warm disk-cache reuse with zero shader conversions.
- Both optional Fossilize and standard no-Fossilize hosts compiled from a fresh pinned Klepton checkout with the complete optional patch plus NativeARM refinements. The bridge, warp shaders and SceneKit loading library compiled.
- Synthetic installer/update and NotProton argument-forwarding tests passed; no-headset startup tests passed.
- All 15 preparation/cache/session tests passed. The no-code recipe check reported 9725 entries and no shader code.
- UI overlay tests passed; save-case migration passed on case-insensitive APFS (the distinct upper/lowercase collision case was skipped).
- Synthetic tracking-origin test passed. Offscreen stereo progress rendering checked cache-check, 50%, 100% and automatic dismissal with orange UI.

The released `native-moltenvr` fork was playtested by its maintainer on Quest 3. A fresh complete headset run of this upstream branch remains to be done, including ALVR transport regression testing. `make -C VXRuntime check` was attempted but cannot finish here because the MinGW C++ compiler is missing; no VXRuntime game/harness result is claimed.
