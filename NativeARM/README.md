# NativeARM — Valve's Linux ARM64 Half-Life: Alyx on macOS, no Wine

Started 2026-10-11. The goal is to run the ARM64 Linux build that Valve ships
for the Steam Frame directly on Apple Silicon — no Rosetta, no Direct3D
translation, no VM — with a small OpenVR runtime of our own feeding the
existing VeRantyx streaming pipeline and Joy-Con / hand input, so the game
can run faster than it does under Wine and still needs no tracked VR
controllers.

## What is known (spike of 2026-10-11)

- Steam depot **546564** ("linux") of app 546560 contains both
  `game/bin/linuxsteamrt64` (x86-64) and **`game/bin/linuxsteamrtarm64`** plus
  `game/hlvr/bin/linuxsteamrtarm64`: 86 ARM64 ELF files, 260 MB. The content
  depot (546561, 72 GB) is shared with the Windows install.
  `scripts/fetch_alyx_arm64.sh` downloads it with your own Steam account.
- `hlvr` (26 KB) only dlopens `libengine2.so` and calls `Source2Main`; our
  launcher does the same.
- The binaries are glibc (symbol versions 2.17–2.38) + libstdc++ (GLIBCXX up
  to 3.4.32). Relocations: RELATIVE / ABS64 / JUMP_SLOT / GLOB_DAT, plus 63
  `R_AARCH64_TLSDESC` and 3 `TLS_TPREL64` (mimalloc). No IRELATIVE, no RELR.
- [Klepton](https://github.com/shinyquagsire23/Klepton) (MIT) translates
  Android ARM64 ELF `.so` files into signed Mach-O dylibs (`klepton-ld`) and
  supplies a bionic libc / NDK / Vulkan-to-MoltenVK runtime on macOS and
  visionOS. Its relinker handles these glibc binaries as they are:
  `libtier0`, `libengine2`, `libsteam_api` translate cleanly (TLS and x18
  rewrites applied). Its loader covers the four relocation types above;
  TLSDESC/TPREL support is the one loader addition needed.
- System libraries the game expects from the Steam Linux Runtime (no public
  ARM64 depot exists) come from Debian trixie arm64 packages
  (`scripts/fetch_debian_libs.sh`): libstdc++6, glib, png, ssl, udev, …
  They are translated the same way and become guest libraries.
- After the game's own libraries and the Debian ones resolve each other,
  what remains for the host shim is (`tools/census.py`): ~800 glibc
  functions (incl. `__*_chk` fortify variants, `__xstat`/`stat64` family,
  `__isoc23_*`, locale `_l` variants, `__errno_location`, `__ctype_b_loc`,
  `__cxa_thread_atexit_impl`), 113 X11/xcb and 28 Wayland symbols (to stub —
  the game runs with `-nowindow`), and the Adreno Vulkan driver
  (`libvulkan_freedreno_hlvr.so`), which is not loaded at all: Vulkan goes to
  MoltenVK through Klepton's `kl_vulkan`.
- `libopenvr_api.so` is replaced by our own runtime; `libsteam_api.so` talks
  to a Steam client that will not exist here — how to satisfy it is an open
  question (the user owns the game; nothing of Valve's is redistributed).

## Status (2026-10-11, end of day 1)

Working: every library of the depot loads with 0 unresolved imports (glibc
layer in `host/kl_glibc.c`, patched Klepton in `klepton-patches/`); the engine
initialises through tier0, file system, SDL (dummy video + Vulkan surface from
the shim), **our OpenVR runtime** (`vrruntime/`: IVRClientCore/System/
Compositor/Input/Chaperone/Overlay/... built on the SteamVR 2.15.6 headers, plus
the Valve-internal IVRMailbox_001 layout from Proton), Vulkan on MoltenVK with a
device-feature clamp, the sound system (System V semaphores needed an ABI-safe
`semctl`), and renders its first frames to the synthetic swapchain.

Steam: `CNetworkSystem::Init()` requires a working `SteamAPI_Init`. The Linux
steam_api finds `~/.steam/steam.pid` and dlopens `~/.steam/sdkarm64/steamclient.so`;
`host/kl_steambridge.c` answers that with the macOS Steam client's arm64
`steamclient.dylib` (same C++ ABI, talks to the macOS Steam process). Needs
**macOS Steam.app running and signed in** by the owner of the game.

## Status (2026-10-11, day 1 evening): main menu renders in stereo

With Steam signed in, the engine loads `maps/startup`, starts the server and
client game systems, attaches both (emulated Rift S) controllers through our
runtime, brings up Panorama and the main menu, and streams stereo frames
(1632x1056 per eye, ~30 eye pairs/s on an M1 Max with the synchronous readback)
into the shared frame file — verified by dumping a frame. It ran 5 minutes
without a fault. Fixed on the way:

- Steam's own Breakpad (`Breakpad_SteamMiniDumpInit` in the macOS
  steamclient) took the Mach exception port, so every fault became a minidump
  and a silent `_exit(1)`; the bridge answers `Breakpad_*` with no-ops.
- Klepton's image table held 64 images; Alyx loads ~110, so dlsym on later
  images (libclient's `CreateInterface`) missed — raised to 1024.
- libvscript's LuaJIT arena insisted on the low 2 GB (`Plat_VirtualAlloc` with
  a max address); arm64 macOS cannot map below 4 GB at all (a smaller
  `__PAGEZERO` is SIGKILLed at exec), so `host/kl_luafix.c` drops the limit,
  nops the hook's range check and commits the arena RW instead of RWX.
- libvideo's 1703 bare `svc #0` (debug breaks) would run Darwin syscall x16;
  they become nops at load.
- glibc `locale_t` is read directly (`->__ctype_b` at 0x68 by libc++); the
  guest now gets glibc's struct layout wrapping the Darwin locale.
- 4 KB commits inside reservations (Panorama) failed on 16 KB pages; widened.

Later the same day:

- Right eye: Source 2's ARM (Deckard) path sizes the eye texture for the union
  of both frusta but draws every eye with the left eye's range at the left of
  it. The runtime now gives both eyes the union frustum and cuts each eye's
  own range back out (`VERANTYX_SHARED_FRUSTUM=0` to disable). Both eyes full,
  1298x1056 each, right eye correct.
- Readback is asynchronous (two slots, worker thread; `VERANTYX_SYNC_READBACK=1`
  for the old path): the game thread went from 26 ms to 4.5 ms per frame.
  Menu: steady 60 fps at the 60 Hz virtual display; ~69 fps at
  `VERANTYX_DISPLAY_HZ=90` (Wine path on the same Mac: ~30 fps).
- `blr x18` veneer no longer uses the real x18 (kernel race crashed libmpg123).
- `scripts/launch_native_vr.sh` runs MacEncoder + the game; the encoder takes
  `VERANTYX_SHARED_FRAME`. Encoder verified at the game's rate (no headset).

Open: playing with the headset (poses, Joy-Con/hand input through the
bindings) not yet tried; "Attempting to purge m_fencedResourceList" warnings;
in-level performance unmeasured.

Run: `build/host/alyx_host --libdir build/guest/lib --game ~/Verantyx_VR_Drive/alyx_linux -- -vr -steam -noasserts -nopassiveasserts +map startup -vulkan -nowindow`
(`VERANTYX_BENCH=1` synthesises poses without a headset; `VERANTYX_SHARED_FRAME`
picks the frame file the Mac encoder reads; `--probe lib.so` reports imports).

## Plan

1. Loader: TLSDESC/TPREL in Klepton's image loader; glibc symbol table
   (generated forwards where signatures and layouts match, hand-written
   shims otherwise, Darwin struct layouts for stat/dirent/pthread types).
2. Launcher: load `libengine2`, call `Source2Main` with `-vr -nowindow`,
   stub X11/Wayland/SDL video, iterate until the engine reaches
   "no HMD".
3. OpenVR runtime: `IVRSystem` / `IVRCompositor` (WaitGetPoses, Submit of
   Vulkan images → readback → shared frame memory) / `IVRInput` /
   `IVRChaperone`, reusing the driver's pose logic and the capture proxy's
   frame path.
4. Connect to `MacEncoder/HardwareEncoder` and the Vision Pro app; measure.

## Layout

- `scripts/fetch_alyx_arm64.sh` — steamcmd download of depot 546564 (your account)
- `scripts/fetch_debian_libs.sh` — Debian trixie arm64 runtime libraries
- `scripts/fetch_klepton.sh` — clone Klepton at a pinned commit, apply the
  CoreAudio link fix, build `klepton-ld`
- `scripts/translate_alyx.sh` — run `klepton-ld` over every ARM64 library
- `tools/census.py` — dependency / symbol / relocation census
