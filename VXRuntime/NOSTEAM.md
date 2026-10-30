# Cyberpunk 2077 (Steam version) without a Steam client

A VXRT_ROOT that is a Windows PC without Steam: no Steam client, no `HKCU\Software\Valve\Steam` values, no
`steamclient64.dll` anywhere steam_api64.dll looks. Nothing is faked and nothing in the game is changed:
steam_api64.dll simply finds no Steam, as on a PC where Steam is not installed. This is the layout the Vision Pro
app needs (no Steam client can run there).

## The Mac test (2026-10-30, avp-preflight e6bc240 arm64 build, AOT strict, NPU race on)

Root: `~/Verantyx_VR_Drive/vxrt_nosteam`, made by `tools/nosteam_root.sh <root> <game folder>` (checks: no Steam keys
in registry.txt, no steamclient*.dll in the root's system32 or the game's folders). The install is the existing one
(`vxrt_wsteam/.../steamapps/common/Cyberpunk 2077`), read through `VXRT_OVERLAY=<install>:<root>/overlay_game`: the
game's writes (its crash-info .txt files in bin/x64) land in the overlay; the install had no file newer than the runs.
AppData and Saved Games are the new root's own, fresh (no saves copied).

Environment (on top of the shell; `VXRT_STEAM_BRIDGE SteamAppId SteamGameId SteamClientLaunch SteamEnv SteamPath` unset):
`VXRT_ROOT VXRT_OVERLAY VXRT_DLL_PATH=vxrt_redist VXRT_DEBUG_OUTPUT=1 VXRT_LOG_TIME=1 VXRT_D3D12_FRAME_STATS=10
VXRT_D3D12_LENIENT_STATES=1 VXRT_PRESENT_SNAPSHOT(_EVERY=60) VXRT_NPU_INTERP=race VXRT_AOT_DIR VXRT_AOT_STRICT=1
VXRT_MEM_LIMITS=6144,8192`; args `--launcher-skip -skipStartScreen` (the normal launch's). Scripts:
`runs/avp/nosteam.sh` (menu), `runs/avp/ns_game.sh` (NEW GAME through the shared `runs/tools/to_game.sh` with
`CP_NOSTEAM=1` in `runs/tools/run_cp.sh`).

| | result |
|---|---|
| steam_api64 | `SteamAPI_IsSteamRunning() did not locate a running instance of Steam` / `Could not determine Steam client install directory` |
| GameServicesSteam | its `Initialize` cannot succeed after that (its `Failed to initialize Steam` line was not in the debug output: it goes through the engine's logger); no `FAILED - GameServices`, no dialog: the engine goes on |
| intro videos | play (Bink), "PRESS ... TO CONTINUE" screen |
| main menu | NEW GAME / LOAD GAME / SETTINGS / MODS / CREDITS / QUIT GAME, no CONTINUE (fresh profile), CD PROJEKT RED ACCOUNT button |
| NEW GAME (to_game.sh, fresh profile) | character creation, load, in game (The Streetkid, "Find Kirk on the second floor"); the game wrote its own AutoSave-0 in the new root |
| AOT strict | 0 fallbacks (2.29 billion entries) |
| memory (lowest settings, UserSettings.lowtex.json) | menu 5.5 GB; 6144 MB crossed 185 s after the first present, 8192 MB at 199 s (in the load after character creation); in game 9.2 GB (peak 10.2 GB, Metal 3.8 GB) |

So Steam is not a wall for the port: option A of the Steam study holds. What is lost without Steam: achievements,
rich presence, Steam Cloud, the encrypted app ticket (online features). Saves are local files.

`gameServicesWrapper "Null"`: not tried. The key is read through the engine's config system, but no shipped config
file (`engine/config`, `r6/config`, `launcher-configuration.json`) and no AppData file sets it, and it is not a
documented command-line option. Not needed either: without Steam the engine already continues.

Also seen: the game starts `REDEngineErrorReporter.exe` as a child process (it ran under vxrt on the Mac). The
headset has no posix_spawn for us: in-process thread, or confirm the game goes on when the spawn fails.

## The same layout on the Vision Pro (marimokodai.VXRuntimeHost)

The app (`xros/app/Sources/main.m`, avp-port) already sets `VXRT_ROOT=Documents/root` and never sets
`VXRT_STEAM_BRIDGE`, so `vx_steam_registry()` writes nothing and registry.txt gets only the defaults. Layout:

    Documents/root/registry.txt                         (made by the runtime at first start; no Valve keys)
    Documents/root/drive_c/windows/system32/            (empty: no steamclient*.dll)
    Documents/root/drive_c/Program Files (x86)/Steam/steamapps/common/Cyberpunk 2077/   (the install, copied once;
                                                                                         needs the user's yes)
    Documents/root/overlay_game/                        (the game's writes, so the copy stays as shipped)
    Documents/root/drive_c/users/vxrt/AppData/Local/CD Projekt Red/Cyberpunk 2077/UserSettings.json  (lowest settings)
    Documents/root/drive_c/users/vxrt/Saved Games/CD Projekt Red/Cyberpunk 2077/   (empty: fresh profile)

Changes for the Cyberpunk launch in the app (small; to be made on avp-port when that tree's work is committed):
1. `unsetenv` `VXRT_STEAM_BRIDGE SteamAppId SteamGameId` before `vx_loader_main` (a launch from Xcode can carry them).
2. `VXRT_OVERLAY=<install>:Documents/root/overlay_game` when the install is there.
3. cwd = `<install>/bin/x64`, args `Cyberpunk2077.exe --launcher-skip -skipStartScreen`.
4. Before the start, the same checks as `tools/nosteam_root.sh` (it takes any root: run it on a copy of the
   container pulled with `devicectl device copy from` to check the device's layout from the Mac).
