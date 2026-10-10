# How Glovebox works

Half-Life: Alyx was made for a gaming PC plugged into a SteamVR headset. Glovebox runs it on Apple Vision Pro, on the headset's own chip: no PC, no streaming. This page explains how.

## The game already speaks ARM

Steam ships Half-Life: Alyx with a native 64-bit ARM Linux build next to the Windows one. That build is 69 ARM64 libraries plus the game's data.

Vision Pro's M-series chip is ARM64 too, so no instruction-by-instruction emulation is needed. Three things stand in the way:

1. **visionOS doesn't run Linux programs.** The libraries expect glibc, Linux system calls, ELF loading and a SteamVR runtime.
2. **visionOS never runs code that wasn't signed into the app.** There is no just-in-time compiler and no downloading code later. Every instruction the game runs has to be inside the signed app when it installs.
3. **Apple reserves register x18.** The kernel clears it, but the GCC-built game uses it as an ordinary register in tens of thousands of places.

## Klepton: Linux libraries in, signed Vision Pro code out

[Klepton](https://github.com/shinyquagsire23/Klepton), by Max Thomas, was built to run Android VR games on Vision Pro. Ahead of time, it relinks ARM64 ELF libraries into Mach-O libraries that can be signed and loaded like any other app code. Every instruction that touches x18 is rewritten to use a per-thread slot instead. For Alyx, that's 65,586 sites, and none are left unhandled.

Glovebox extends Klepton for a desktop Linux game:

- **A glibc personality.** Klepton imitated Android's Bionic C library. Alyx needs glibc: ELF thread-local storage (TLSDESC), GNU hash tables, glibc's struct layouts, eventfd/epoll/futex behaviour faithful enough for Source 2's job system, SysV semaphores, and Linux signal numbers.
- **Engine scripting without a JIT.** The game's Lua scripts run on a LuaJIT built for visionOS with the JIT off. The Panorama UI's V8 runs in jitless mode.
- **The C++ runtime and a few system libraries** (libstdc++, OpenSSL, GLib and others) come from Debian's official ARM64 packages. They are downloaded from Debian, checked against pinned hashes and converted on your Mac with the game (`packages/alyx/runtime-debs.json`).

The conversion runs on your Mac at install time: 89 libraries, the game's own plus its runtime's. The whole build takes under 2 minutes on an M-series Mac Studio. Then Xcode signs the result with your own developer account. Nothing converted ever leaves your machines.

## Graphics: Vulkan on Metal, and Source 2's own renderer

Alyx renders with Vulkan. [MoltenVK](https://github.com/KhronosGroup/MoltenVK) translates it to Metal, Apple's graphics API. The game renders both eyes itself, at its own quality settings, and hands the frames to visionOS's Compositor Services, which puts them on the displays.

Source 2 creates GPU pipelines the first time a surface or effect appears, and on Metal each one can take up to half a second to compile. To keep those stalls out of play, Glovebox builds the pipelines ahead of time behind the loading screen, using Valve's open-source [Fossilize](https://github.com/ValveSoftware/Fossilize). It records any new pipeline it meets so the next launch already has it.

The pipeline list is built from Valve's shaders, so we don't ship it. Glovebox ships only a recipe saying where in the game's own shader files each piece is, with no shader code in it (`packages/alyx/pipelines/`). During install, your Mac rebuilds the full list from your copy of the game, which takes under two seconds (`engine/pipelines.py`).

## VR: answering SteamVR

The game talks to SteamVR through the OpenVR API. Glovebox replaces `libopenvr_api.so` with a shim that answers from visionOS:

- **Head tracking** from ARKit, with each eye's real field of view.
- **PS VR2 Sense controllers** through Apple's GameController framework (buttons, sticks, triggers) and ARKit's accessory tracking (position and rotation). The game's own SteamVR input actions and bindings are used as shipped.
- **Frame pacing.** The game draws at half the display rate: 45 fps when visionOS runs the displays at 90 Hz, and 60 fps at 120 Hz. Each frame is re-aimed to your latest head rotation for every display refresh.

## Memory

Alyx uses about 6 to 8 GB on the headset. Apps on visionOS get 5 GB by default. Apple's Increased Memory Limit capability raises that, and a free Apple Account's Personal Team gets it too. Extended Virtual Addressing is for paid accounts only, and the game doesn't need it, so a free team's build leaves it out. The difference is how long an install lasts: 7 days with a free account, a year with a paid Developer Program membership.

## Steam, from Swift

The headset app includes a Steam client written in Swift. It:

- signs in with your password and a Steam Guard approval (it never stores the password);
- checks that your account owns the game;
- downloads it from Steam's content servers with every chunk and file verified;
- syncs your Steam Cloud saves the way the Steam client does.

Steam itself refuses the download keys for an account that doesn't own the game.

## What Glovebox never does

It never hosts, shares or bundles any of Valve's files. Every game file comes from your own Steam account onto your own devices. The converted code is signed with your own developer account, so it runs only on your headset.
