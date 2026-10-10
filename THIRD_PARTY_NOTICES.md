# Third-party notices

| Component | Where | License |
|---|---|---|
| Klepton, by Max Thomas ([shinyquagsire23/Klepton](https://github.com/shinyquagsire23/Klepton)) | `klepton/` | MIT (`klepton/LICENSE`) |
| OpenVR SDK headers and API description (Valve) | `klepton/runtime/openvr/third_party/openvr/` | BSD-3-Clause (`LICENSE`) |
| SteamVR Unity plugin hand-pose data (Valve), used by the shim's skeleton | `klepton/runtime/openvr/` | BSD-3-Clause (`third_party/steamvr_unity_plugin.LICENSE`) |
| GameNetworkingSockets headers (Valve) | `klepton/runtime/linux/steam/` | BSD-3-Clause (`LICENSE-GameNetworkingSockets`) |
| Steam Audio's default HRTF (CIPIC subject 124), from Valve's open-source steam-audio | `klepton/runtime/data/phonon_hrtf_cipic_124.sofa` | Apache-2.0 |
| DejaVu Sans 2.37, the in-game menu font | `klepton/runtime/data/kl_menu_font.ttf` | Bitstream Vera and Arev font licenses; DejaVu changes public domain (`licenses/DejaVu-Sans.txt`) |
| LZMA SDK decoder (Igor Pavlov) | `launcher/Sources/CSteamSupport/lzma/` | Public domain |
| Zstandard decoder (Meta) | `launcher/Sources/CSteamSupport/zstd/` | BSD-3-Clause (`zstd/LICENSE`) |
| MoltenVK (Khronos Group) | downloaded at build time | Apache-2.0 (`licenses/MoltenVK.txt`) |
| Fossilize (Valve) | downloaded at build time | MIT |
| SDL 3 and sdl2-compat | downloaded at build time | zlib |
| LuaJIT | downloaded at build time | MIT |
| Sparkle, the installer's updater | downloaded at build time (`installer/`) | MIT (`licenses/Sparkle.txt`) |
| Debian's arm64 runtime libraries (libstdc++, libgcc, OpenSSL, GLib, ...) | downloaded at install time (`packages/alyx/runtime-debs.json`) | each package's own license (GPL-3.0 with the GCC Runtime Library Exception, Apache-2.0, LGPL-2.1, ...) |

The game itself is not part of Glovebox. Each player downloads their own copy from Steam with their own account.

The apps show every text above (app menu › Licenses… on the Mac, the account menu's Licenses on the headset), and the runtime kit carries them in `LICENSES/`; `tools/licenses.py` builds all three from one list.
