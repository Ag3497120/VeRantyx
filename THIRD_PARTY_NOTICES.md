# Third-party notices

All code in this repository was written for VeRantyx, with these exceptions:

- `VRDriver/include/openvr.h`, `VRDriver/include/openvr_driver.h` and
  `NativeARM/vrruntime/openvr/` are the OpenVR SDK headers, Copyright (c)
  Valve Corporation, used under the BSD-3-Clause license
  (https://github.com/ValveSoftware/openvr). They are needed to compile the
  SteamVR driver, the OpenVR capture proxy and the native OpenVR runtime.
- `NativeARM/vrruntime/hand_skeleton_alvr.inc` contains hand-skeleton bone
  poses taken unchanged from ALVR (https://github.com/alvr-org/ALVR,
  `alvr/server_openvr/cpp/alvr_server/Controller.cpp`), Copyright (c)
  2018-2019 polygraphene, 2020-2024 alvr-org, MIT license (full text in the
  file).
- `NativeARM/klepton-patches/` is a patch set against Klepton
  (https://github.com/shinyquagsire23/Klepton, MIT, by ShinyQuagsire); the
  files under `klepton-patches/runtime/` are new files added to it.

The following are **not** part of this repository. `scripts/setup.sh`
downloads them from their publishers, and you install the rest through your
own Steam account:

- Wine Staging, macOS build by Gcenx (LGPL-2.1) — https://github.com/Gcenx/macOS_Wine_builds
- DXVK for macOS, build by Gcenx (zlib) — https://github.com/Gcenx/DXVK-macOS
- Klepton (MIT) with its MoltenVK build (Apache-2.0) — fetched by
  `NativeARM/scripts/fetch_klepton.sh`
- Debian trixie arm64 packages (glib, libstdc++, OpenSSL, libpng, …; each under
  its own license) — fetched by `NativeARM/scripts/fetch_debian_libs.sh`
- Fossilize (MIT, Valve) — fetched by `NativeARM/scripts/build_fossilize.sh`
  (optional experiment)
- Steam, SteamVR and the game — Valve Corporation, under their own terms. The
  native version uses the Linux ARM64 depot of Half-Life: Alyx, downloaded
  with your own account; the native launcher patches a few instructions of
  the game in memory at load (byte-checked by hash, see
  `NativeARM/host/kl_guestfix.c`) and never writes to the game's files.

Apple's Game Porting Toolkit (D3DMetal) is not used, distributed or required.
