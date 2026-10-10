# Third-party notices

All code in this repository was written for VeRantyx, with one exception:

- `VRDriver/include/openvr.h` and `VRDriver/include/openvr_driver.h` are the
  OpenVR SDK headers, Copyright (c) Valve Corporation, used under the
  BSD-3-Clause license (https://github.com/ValveSoftware/openvr). They are
  needed to compile the SteamVR driver and the OpenVR capture proxy.

The following are **not** part of this repository. `scripts/setup.sh`
downloads them from their publishers, and you install the rest through your
own Steam account:

- Wine Staging, macOS build by Gcenx (LGPL-2.1) — https://github.com/Gcenx/macOS_Wine_builds
- DXVK for macOS, build by Gcenx (zlib) — https://github.com/Gcenx/DXVK-macOS
- Steam, SteamVR and the game — Valve Corporation, under their own terms

Apple's Game Porting Toolkit (D3DMetal) is not used, distributed or required.
