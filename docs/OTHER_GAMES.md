# Other SteamVR games (Wine version)

The native version runs Half-Life: Alyx only: it depends on Valve's Linux ARM64 build, which no other VR game has.
The Wine version runs the Windows SteamVR and the Windows game, so in principle other Windows SteamVR titles can
work. **Only Half-Life: Alyx has been confirmed so far.**

## Launching another title

```bash
VERANTYX_APP=earth scripts/launch_vision_pro_vr.sh                 # Google Earth VR (app 348250)
VERANTYX_APP=<steam app id> VERANTYX_APP_EXE="<the game's .exe in the prefix>" \
    VERANTYX_APP_NAME="<name for messages>" scripts/launch_vision_pro_vr.sh
```

Install the game first through the Windows Steam client in the Wine prefix. The script places the capture proxy
(`openvr_api.dll`) beside the given .exe, launches the title through Steam, and waits for its first captured eye
pair. The Alyx-only steps (launch options, fidelity, GPU-name `dxvk.conf`) are skipped for other titles.
`VERANTYX_BENCH=1` runs without a headset (synthetic poses).

Frames are captured from the game's `IVRCompositor::Submit`: Direct3D 11 textures (as Alyx) and, since
2026-10-12, OpenGL textures (read back with `glGetTexImage` on the game's own context; not forwarded to SteamVR,
whose OpenGL path needs `WGL_NV_DX_interop`, which Wine on macOS does not have).

## Status

| Title | API / VR | Status here |
|---|---|---|
| Half-Life: Alyx | D3D11 / OpenVR | Works (about 30 fps on an M1 Max) |
| Google Earth VR | **OpenGL** / OpenVR (IVRCompositor_021) | Starts, creates its GL context, connects to SteamVR, then shuts OpenVR down about a second later, before its first Submit, with no error output. Cause not found yet (2026-10-12). |

## Likely candidates (research, 2026-10-12, untested)

From ProtonDB, PCGamingWiki and CrossOver reports. Direct3D 11 + OpenVR titles take the same path as Alyx; the
main question for them is the frame rate (the x86-64 game runs through Rosetta 2).

| Likely | Maybe | Unlikely |
|---|---|---|
| SUPERHOT VR, Assetto Corsa, Skyrim VR, Fallout 4 VR, H3VR, BONEWORKS, Blade & Sorcery, Walkabout Mini Golf, Gorilla Tag | Beat Saber, Pavlov's older builds and BONELAB in OpenXR mode (need SteamVR's OpenXR runtime registered in the prefix), No Man's Sky (Vulkan textures, not captured yet) | VRChat and Pavlov (Easy Anti-Cheat has no macOS runtime under Wine), Microsoft Flight Simulator 2024 (D3D12 only), Rec Room (shut down 2026-06-01) |
