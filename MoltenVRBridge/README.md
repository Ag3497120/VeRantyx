# Native ARM64 Alyx through MoltenVR

This optional path runs the owner's Linux ARM64 Half-Life: Alyx libraries on Apple Silicon using VeRantyx/Klepton, presents them through native OpenXR to MoltenVR and streams to Quest. It includes the bridge, installer, shader preparation/loading UI and optional NotProton Steam integration from [armax9/VeRantyx, native-moltenvr](https://github.com/armax9/VeRantyx/tree/native-moltenvr).

## Requirements

- Apple Silicon Mac, Xcode tools and native macOS Steam.
- An owned Alyx installation plus its Linux ARM64 Steam depot (`546564`). The installer checks the ELF architecture.
- MoltenVR installed/configured and its Quest app paired. OpenComposite and CrossOver are not used by this native path.
- NotProton is optional. It exposes macOS Steam's compatibility-tool selection so Alyx's normal Play button can invoke this launcher and forward launch options.

## Build and package

```sh
bash MoltenVRBridge/build-release.sh
```

The optional builder fetches pinned Klepton, Fossilize and OpenXR SDK sources and applies the complete runtime patch; use a clean dependency checkout. It builds `NativeARM/build/host-fossilize/alyx_host`, the bridge, warp shaders and loading library. The standard upstream host build keeps Fossilize disabled and uses its original output directory. See `RELEASE_PACKAGING.md` for the binary allowlist. The preparation and UI/save tools are shared with `NativeARM/` via source-tree symlinks; copy their contents when staging a standalone package.

The runtime archive layout expected by `install.py` is:

```text
package/
  install.py, fetch-debian.sh, start.command
  prepare_native_ui.py, normalize_save_names.py
  steam/                         # optional Steam compatibility tool
  runtime/
    alyx_host, bridge, libMoltenVK.dylib, libalyxloading.dylib
    warp.vert.spv, warp.frag.spv, vr_fidelity_levels_override.csv
    moltenvk-game/                # compatible game-side MoltenVK
    preparation/                 # NativeARM/preparation, including MIT notices
```

Copy `steam/` from the repository root into the package. No game/depot libraries, generated FOZ archives, Valve UI resources, saves, preferences, caches or logs belong in the archive.

## Install and play

From a staged runtime package:

```sh
python3 install.py "/path/to/Half-Life Alyx" "/path/to/depot_546564" --notproton
```

Omit `--notproton` for direct launch. The installer obtains required Debian libraries, links owner-local game/depot content, generates a private UI overlay and reconstructs the no-code shader recipe. It preserves managed settings, saves and caches during updates. Close Alyx first. An unmanaged `verantyx` directory is refused rather than overwritten.

Start native Steam and MoltenVR, then open the installed `start.command`. With NotProton, restart Steam and select **Alyx Native - MoltenVR** in Alyx's Compatibility properties; the normal Play button then uses the native runtime. Launch options are forwarded, for example:

```text
-novid +vr_fidelity_level_auto 0 +vr_fidelity_level 3
```

With no explicit fidelity override, automatic selection remains enabled. Resolution is obtained from MoltenVR at startup; an active/worn headset is not required to start preparation. Put the headset on when ready to play. The in-stream orange progress display distinguishes cache checking from compilation; normal loading messages and stage-room rendering use the owner's assets.

## Runtime behavior

- Quest controller poses, velocities, haptics and independent capacitive touches feed the native OpenVR runtime. Input release timing and historical poses are supplied by the separate NativeARM fixes.
- Frame-specific projection metadata follows the actual cropped pixels, including fidelity/allocation changes. The original ALVR IPD/haptic offsets remain unchanged; MoltenVR projection records use reserved tail space.
- Optional native-host Fossilize callbacks record encountered pipelines, reconstruct/replay owner-local recipes and flush recordings on exit. Preparation and gameplay share identity rules, persistent caches and a session lock. A recipe cannot guarantee that every later level material is warm.
- The supplied PC fidelity table uses 4x MSAA. The launcher sets a ragdoll limit of eight so Low does not discard almost every body; later explicit command-line arguments can override it.
- The Touch left-stick height binding is mapped to the explicit crouch toggle when the stock binding requires an inaccessible accessibility mode.
- Cooperative quit, private overlays and owner-local state remain part of the upstream runtime. The loading renderer is adapted from upstream VeRantyx rather than claimed as new engine functionality.

## Tests and scope

Synthetic runtime, installer/update, launch-option forwarding, no-headset startup, cache/session and first-use shadow tests are included. The released fork was playtested on Quest 3 + MoltenVR; this upstream integration branch still needs a fresh full headset run. CPU compositor timing does not measure GPU or end-to-end streaming latency.

This is optional integration code. It does not replace the existing ALVR/Vision Pro startup paths, and the project includes no entitlement bypass or game assets.
