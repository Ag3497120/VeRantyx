# VXRuntime: running Windows programs on Apple silicon without Wine

A research project inside VeRantyx: a Windows compatibility layer written from scratch for Apple silicon
Macs, meant to eventually replace Wine for the Wine version of VeRantyx. It uses no Wine code, except about 120 lines
of the x64 unwinder in `src/seh.c` in versions from 2026-10-12 to 2026-10-23, which were derived from Wine and have
since been replaced (see [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)). Results are expected to come slowly;
each milestone is a small Windows program that has to run unchanged. What vxrt is, how it is put together and how
the vxrt version compares with the Wine and native versions: [the repository README](../README.md#the-vxrt-version-vxruntime)
([日本語](../docs/README_ja.md#vxrt版vxruntime)). It is the `vxrt` branch's version of VeRantyx.

**Without Rosetta (in progress):** VXRuntime runs under Rosetta 2 today; macOS 27 is the last release with full Rosetta. Its own x86-64 → ARM64 translator, which takes Rosetta's place for game code while the runtime itself becomes native ARM64, is planned and measured in [TRANSLATOR.md](TRANSLATOR.md) (phase 1, the decoder, is in `translator/`).

```bash
make check          # builds build/vxrt and the test programs, runs them
make bench          # CPU cost of a Direct3D 11 draw (tests/drawbench.c), in microseconds
build/vxrt program.exe [args...]
VXRT_TRACE=1 build/vxrt program.exe     # also lists imports that are not implemented yet
VXRT_DUMP_SHADERS=dir build/vxrt game.exe   # writes every translated shader (Metal source) to dir
build/vxrt build/dllprobe.exe 'Z:\path\to\some.dll' [export ...]   # how far a real DLL gets
build/dxbcscan [-s N] [-t N] files...      # translates (and Metal-compiles) every DXBC shader found in the files
VXRT_OVERLAY=/game/dir:/scratch/dir build/vxrt game.exe   # the game's own folder stays unchanged: writes go to /scratch/dir
VXRT_MUTE=1 ...                            # sound output is silent
VXRT_TRACE=1 VXRT_DEBUGGER=1 ...           # programs that log to a debugger (OutputDebugString) show their logs
VXRT_DLL_PATH=/prefix/drive_c/windows/system32 ...   # also loads DLLs (the VC++ runtime) from a Windows installation
VXRT_TRACE_PIPES=1 ...                     # every named-pipe message, with its first bytes
VXRT_TRACE_NAMES=1 ...                     # processes joining and leaving named objects and mappings
VXRT_TRACE_CALLS=200 ...                   # every call into the runtime (arguments, result, last error), 200 per function
VXRT_TRACE_CALLS_ONLY=CreateFileW,LockFileEx ...   # with VXRT_TRACE_CALLS: only these functions (a browser's file calls)
VXRT_PRESENT_SNAPSHOT=f.png VXRT_PRESENT_SNAPSHOT_AT=300 ...   # the 300th present's back buffer as a PNG
VXRT_TRACE_KEYED=1 VXRT_SHARED_SNAPSHOT=f.png ...              # keyed mutex traffic; shared textures as PNGs at a ReleaseSync
VXRT_TRACE_DRAWS=trigger VXRT_DRAW_SNAPSHOT=<ps hash>:prefix ... # 300 draws after the trigger file appears; a pixel shader's inputs as PNGs
VXRT_STEAM_BRIDGE=1 VXRT_TRACE_STEAM=1 ... # Windows games use the Mac Steam client (log: interfaces, first calls, callbacks)
VXRT_TRACE_TEXTURES=1 ...                  # every Direct3D texture made
VXRT_TRACE_QI=1 ...                        # each interface a Direct3D/DXGI object was asked for and does not have
VXRT_WAIT_DUMP=15,30 ...                   # at 15 and 30 s: the threads waiting for over 3 s, for what, from where
VXRT_SYNC_SHADERS=1 ...                    # compile each shader before CreateXShader returns (not in the background)
VXRT_STATS=1 ...                           # keyed mutex waits and hold times, Map waits, GPU busy time, every few seconds
VXRT_LOG_TIME=1 ...                        # log lines carry the time of day (the same clock in every process)
VXRT_TRACE_KEYED=1 VXRT_TRACE_MAPS=1 ...   # every AcquireSync/ReleaseSync (and waits made while holding one); staging Maps
VXRT_PROFILE=report.txt ...                # sampling profiler through Rosetta: hottest functions (Windows ones by .pdata), every 10 s
VXRT_GPU_TIMESTAMPS=1 ...                  # timestamp queries are the GPU's end times (default: the CPU's, on the same clock)
VXRT_FLUSH_DRAWS=100 ...                   # send recorded work to the GPU every 100 draws, between render passes (default: off)
VXRT_EAGER_CLEARS=1 ...                    # clears as passes of their own (default: folded into the next render pass's load action)
VXRT_EXTRA_ARGS='helper.exe=--a --b' ...   # helper.exe, wherever in the process tree it starts, gets --a --b after its name
```

Half-Life: Alyx without Wine: `scripts/launch_vxrt_alyx.sh` (repository root; Steam running on the Mac and signed
in) starts SteamVR on vxrt, the encoder and the web UI stand-in, then the game with the Steam bridge; extra
arguments go to the game. The first start compiles the game's shaders (a few minutes).

SteamVR without Wine: `scripts/launch_vxrt_steamvr.sh` (repository root) runs vrserver, vrcompositor and the VeRantyx
driver on vxrt next to the HEVC encoder, using the Wine prefix's SteamVR read-only. Measured on 2026-10-12 without a
headset: the driver captures and the encoder encodes 59-60 frames a second (the Vision Pro side is untested).

### The Windows Steam client (research, not working yet)

For games whose executable is wrapped in Steam's DRM (SteamStub): the wrapper is answered by a real Windows Steam
you are signed in to, running on the same "machine" (the same `VXRT_ROOT` registry and VXRuntime's named objects).
Every other game keeps using the Mac Steam client through the bridge (`VXRT_STEAM_BRIDGE=1`). How it is meant to be used:

1. Windows Steam lives in a VXRuntime root of its own, `~/Verantyx_VR_Drive/vxrt_wsteam` (`VXRT_WSTEAM_ROOT`), at
   `drive_c/Program Files (x86)/Steam`, never in the Wine prefix. Installing it there (Valve's installer on
   VXRuntime) is a later step: it downloads Steam from Valve, so it waits for your go-ahead.
2. `scripts/launch_vxrt_windows_steam.sh [<Windows Steam folder>]` (`VXRT_WSTEAM_SILENT=1` for `-silent`) starts it,
   log in `~/Library/Logs/VeRantyx/vxrt-windows-steam.log`. You sign in yourself, in Steam's own window: VeRantyx
   never asks for, reads or keeps your password; Steam keeps its saved login itself, as on Windows (what it protects
   with DPAPI is protected less here: see below). The script refuses a Wine prefix, a 32-bit `steam.exe` and a second
   Windows Steam on VXRuntime (named objects are shared by every root), and sets no `VXRT_STEAM_BRIDGE`. It follows
   symbolic links to where the folder and the root really are, so a Steam folder, a `drive_c`, a root or a game
   folder linked in from a Wine prefix is refused too, as is a `steam.exe` that is itself a link: copy them in (on
   APFS `cp -c -R` makes a copy that takes no extra space). What gets written: the root, if it is not there yet; in
   it, VXRuntime's `registry.txt`, `dpapi/` and its C: drive (`drive_c/windows`, `drive_c/users`); in the Steam
   folder, whatever Steam itself writes there (its updates, logs, config, web cache), as on Windows; and the script's
   log. `VXRT_WSTEAM_DRY_RUN=1` runs only the checks and prints what it would start.
3. A SteamStub game then starts with the same `VXRT_ROOT` and without `VXRT_STEAM_BRIDGE` while Steam runs (the game
   launch scripts do not do this yet).

How the web helper (Chromium) is started, checked on 2026-10-24 against the command line `steam.exe` gave
`steamwebhelper.exe`: Steam's own `-cef-disable-gpu` adds `--disable-gpu --disable-gpu-compositing`,
`-cef-disable-gpu-compositing` only the latter, `-cef-disable-sandbox` and `-no-cef-sandbox` add `--no-sandbox`;
`-cef-single-process` and `-cef-in-process-gpu`, named in older lists, add nothing in this client (2026-09-03 build).
`--single-process`, which Chromium needs here because its child processes are handed handles between processes,
comes from `VXRT_EXTRA_ARGS` (`src/launchargs.c`), so neither Steam's files nor the Wine version's
`steamwebhelper.exe` shim are involved. Where it stops today (2026-10-25, client 1792618285 of 2026-10-21): `steam.exe`
runs on (past Steam's SteamVR check, 88 s of 88 in every run since `OpenThread` finds every running thread; before, it
ended itself about 10 s in: tier0 freed the main thread's data when it could not open it), loads `steamclient64.dll`
once the Steam Service Error box is closed (its connectivity test passes, logged off), and the web helper creates
Steam's shared JS context (`logs/steamui_html.txt`: `BrowserReady`; its page loads, `webhelper_js.txt` ends at
"setting locale: en"). With Steam's `-cef-disable-gpu` (the launch script's default) the web helper composites in
software and draws the "Sign in to Steam" window through GDI. Without it, the GPU side works as far as the EGL
display since 2026-10-25: Steam's ANGLE makes its EGL display and GLES 3 contexts on our Direct3D 11 (the screen's DC
belongs to the desktop window, IDXGIDevice2-4, the adapter's driver version; before, every EGL display type failed
and "Failed to create shared context for virtualization" came about 100 times a second; 0 of them now). The login
window is created but stays black on that path (run of 2026-10-25, 115 s): `eglCreateWindowSurface` fails with
`EGL_BAD_ALLOC` (ANGLE: "Could not create additional swap chains or offscreen surfaces, HRESULT: 0x80004005"), and
`CreateSwapChainForHwnd` on a child window does fail here with E_FAIL (a swap chain needs the window's own Mac
window, `vx_cocoa_metal_layer`; only top-level windows have one); `steam.exe` restarts the helper twice ("Restart
webhelper process, counter 2") and then turns GPU acceleration off ("Disabling GPU acceleration: Disabled/
RuntimeDetect"). Swap chains on child windows are the next step for the GPU path. Seen before stage 2's network
fixes: Chromium's DevTools server (Steam's `-cef-enable-debugging`) listened on 127.0.0.1:8080 but never answered
(no WSAEnumNetworkEvents for its FD_ACCEPT event).

Steam updates itself here as on Windows (2026-10-25: from the 2026-09-03 client, 1788652215, to 1792618285, Valve's
download). Those runs were made in `~/Verantyx_VR_Drive/vxrt_wsteam` itself, not in a copy, two of them with
`-forcesteamupdate -forcepackagedownload` (Steam downloads its packages again and reinstalls them over its files);
the user had approved the download of Steam's update and, told afterwards about the runs in the root and the two forced reinstalls, said to go on (2026-10-25). Trying changes, run
Steam in an APFS copy of the root instead (`cp -c -R ~/Verantyx_VR_Drive/vxrt_wsteam <scratch>/wsteam_root`, then
`VXRT_WSTEAM_ROOT=<scratch>/wsteam_root scripts/launch_vxrt_windows_steam.sh`), so whatever Steam writes (logs, web
cache, its own updates) lands in the copy, and without those two options: Steam's own update check is enough. The
bootstrapper checks at once when the last run did not end cleanly (its `.crash` file is still there: "Checking for
update on startup"); after a clean end it checks from its background loop about two minutes in, so a shorter run
never checks. It saves the downloaded manifest through the C runtime opened for
reading and writing in text mode, which seeks to 1 byte before the end of the new, empty file to look for a CTRL+Z:
that has to fail with `ERROR_NEGATIVE_SEEK` (SetFilePointer's documentation). It failed with `ERROR_INVALID_PARAMETER`
here, so the C runtime gave up on the file, Steam logged `Error: Failed to save package manifest.` and never installed
the update (`tests/updater_files.c`). Installing, it extracts the packages over its own files (images are read into
memory here, so replacing a loaded DLL or the running `steam.exe` is safe), starts `steam.exe` again with its command
line and ends with exit code 42. The new process gets that command line word for word, as on Windows (it got the Mac
path of the program before: `VXRT_COMMAND_LINE`, `src/loader.c`), and the launch script waits for it (it stopped
Steam's whole process group when the first `steam.exe` ended). The October client imports
`SetCurrentProcessExplicitAppUserModelID` (it stopped at once without it) and Transactional NTFS's
`DeleteFileTransactedW`/`MoveFileTransactedW`, which fail here as for a handle that is not a transaction: there is no
`KtmW32.dll`, which Steam loads itself before it uses them.

The web helper's profile (`C:\users\vxrt\AppData\Local\Steam\htmlcache`) keeps its data in SQLite databases (`Web
Data`, `Affiliation Database`, `History`, ...). SQLite maps a database and, at every commit, flushes the whole view
(`FlushViewOfFile` with a byte count of 0); until 2026-10-24 that failed here, so every commit after a database's
first was rolled back, `Web Data` stayed empty and Chromium showed "Profile error occurred". `tests/dbfiles.c` checks
the file calls of SQLite's documented patterns against Microsoft's documentation: byte-range locks of two connections
at the bytes near 1 GiB (`src/filestate.c`, real locks within a process now), reads and writes at OVERLAPPED offsets,
the -journal and -shm files, a mapped database, deleting on close. Deleting on close or by disposition keeps a
`VXRT_OVERLAY` tree as `DeleteFileW` does: only the overlay's copy of a file goes, never the original (`dbfiles.exe
overlay`, also with the tree named through a symbolic link). With an empty profile and the registry of an
earlier run, the web helper also needs `RegDeleteKeyExW` (it stopped at a breakpoint without it).

"Steam Service Error" at start: Steam looks for the Steam Client Service (`bin/SteamService.exe`, a 32-bit
Windows service). Valve's own text in that window: it "helps Steam install or update games, enables anti-cheat
detection in some titles and assists with other system level tasks", and installing it needs administrator rights;
the choices are *Install Service* and *Cancel*. Here it cannot be installed: VXRuntime runs no 32-bit programs and
its service manager has no services and lets nobody create one (`src/scm.c`). Choose *Cancel* (or close the window),
the window's own way on: the Wine version's Steam ran without the service, and on VXRuntime (2026-10-24, seen through Command Clips)
the window opens, `steam.exe` carries on while it is open, and after it was closed (WM_CLOSE) Steam went on without
asking again. Its buttons and labels first showed piled up in the top left corner: Steam's VGUI draws each panel of
the box at a viewport origin (`SetViewportOrgEx`), which GDI here ignored; with DC origins and clip regions
(`src/gdi.c`, 2026-10-24) the box shows as on Windows, its *INSTALL SERVICE* and *CANCEL* buttons labelled, in the
window server's own pixels (Command Clips `snapshot-view`). (The log still has `Win32Font.cpp ... Couldn't get string
length`.) What it is for on Windows: tasks that need
administrator rights there. Valve's window names installing and updating games and anti-cheat in some games;
Valve's install-script documentation has Steam do parts of a game's first-run setup as administrator
(`HKEY_LOCAL_MACHINE` keys, programs to run, firewall exceptions), which may not happen without it. Valve documents
no Steam option that turns the check off.

`C:\` is `$VXRT_ROOT/drive_c` (default `~/.vxrt/drive_c`, created on first run with `C:\windows\system32` and
`C:\windows\temp`); `Z:\` is the whole Mac file system. The registry lives in `$VXRT_ROOT/registry.txt`.
`make check` uses `build/root` so the tests never touch `~/.vxrt`.

What programs keep with DPAPI (`CryptProtectData`: Chromium's and so Steam's cookie key, saved logins) is protected less
at rest than on Windows. Windows encrypts the key of that data with your logon password; VXRuntime keeps it as 32
random bytes in `$VXRT_ROOT/dpapi/user.key`, readable only by you (0600 in a 0700 directory, refused otherwise) and left
out of Time Machine backups, but not encrypted with your Mac password and not in the Keychain. Anyone who can read your
home folder (a copy of `~/.vxrt`, another administrator of the Mac, the disk of a Mac without FileVault) can open that
data. Keep FileVault on, and treat `~/.vxrt` like a password store: do not copy or share it.

Windows run on AppKit: the program runs on a thread of its own and the macOS main thread runs AppKit, so a
program's message loop and window procedures work from any thread. `VXRT_NO_ACTIVATE=1` keeps windows from
taking focus (used by `make check`); `VXRT_SNAPSHOT=file.png` saves a window's view as it is closed.
What GDI draws reaches the screen as Core Animation layers (`src/present.c`, `src/cocoa.m`): a window's client
bitmap is copied into its front buffer when a picture is finished (a window DC given back, EndPaint,
UpdateLayeredWindow, DwmFlush; drawing nobody finishes after 30 ms), and the main thread hands that to the window's
layer at most once in 8 ms, one GDI pixel a point (on a Retina display each covers 2x2 device pixels, scaled by the
GPU without interpolation). Child windows are sublayers, layered windows transparent Mac windows (per-pixel alpha,
color key, constant alpha). A 1134x704 frame as Chromium's software output gives it (BitBlt from a DIB section and
ReleaseDC) costs the program 0.2-0.26 ms (`tests/present.c`, M1 Max).
`build/vxrt build/window.exe --interactive` opens the test window and prints the keyboard and mouse messages it gets.

Needs Xcode's clang and `x86_64-w64-mingw32-gcc` (Homebrew `mingw-w64`) for the test programs.

## Design

| | Wine | VXRuntime |
|---|---|---|
| Process | Windows process = Unix process + wineserver for shared kernel objects | One macOS process; kernel objects live in-process. Cross-process objects later, over Mach IPC, only where needed |
| Windows API | PE DLLs that cross into a separate Unix half (`__wine_unix_call`, syscall-style transitions) | Native Mach-O functions with the Windows calling convention (`ms_abi`), called directly by the program: no boundary to cross |
| CPU | x86-64 code under Rosetta 2 | The same: Rosetta 2 translates the program and the runtime together (the runtime is an x86-64 binary) |
| Thread state | Since Wine 8 on macOS, `gs` is switched to the TEB while Windows code runs | `gs` stays macOS's TSD; the TEB pointer and TLS array are written to TSD slots 0x30/0x58 (Wine's pre-8 macOS approach), so Windows code, this runtime, Metal and translated graphics code share a thread without switching |
| Graphics | wined3d / DXVK / D3DMetal | Direct3D translations are in-tree (DXBC→MSL; DXIL→MSL in progress); Apple Metal is the graphics framework dependency. No D3DMetal or Game Porting Toolkit |

The shared, API-neutral GPU intermediate layer for D3D11, D3D12 and Vulkan is in active use: D3D11/D3D12 copies, resolves and barriers plus Vulkan buffer/image transfers, barriers, compute dispatch and an initial graphics path record retained common resources and ordered commands. The Vulkan frontend creates compute/transfer devices, buffers and images, uniform/storage-buffer and sampled/storage-image/sampler descriptor sets, push constants, SPIR-V compute pipelines, and a limited single-color-attachment graphics pipeline; both static and dynamic viewport/scissor state flow through common GPU IR to Metal. D3D12 validates per-subresource copy, resolve, attachment, vertex/index, finite shader-descriptor and indirect-argument states before queue submission, including Direct/Compute/Copy command restrictions, COMMON promotion and submission-boundary decay. D3D12 root signatures build normalized layout records preserving CBV/SRV/UAV/sampler namespaces and stage visibility. Vulkan now has an initial Win32 surface/swapchain/acquire/present path; broader synchronization, fixed-function graphics states and general Vulkan conformance remain ahead. Each API front end keeps its own handles, validation and state rules; see [the GPU IR design](../docs/GPU-IR.md) for the boundaries and migration stages.

The Vulkan path includes a dependency-free SPIR-V reader and a narrow Vulkan 1.0 compute/transfer/graphics frontend. It advertises `VK_KHR_maintenance1` and an initial `VK_KHR_swapchain` implementation for two or three BGRA8 images, FIFO, binary acquire synchronization and Metal-layer presentation. The WSI test covers swapchain creation and acquire behavior; successful display presentation still needs an idle-runtime check. The frontend can create a Metal-backed device, 2D transfer/storage images with tested array-layer paths, uniform/storage buffers, samplers, descriptor sets and push constants; copy and clear images; dispatch supported SPIR-V buffer/image compute kernels; and draw a restricted graphics pipeline through common IR and MSL. The graphics subset supports triangle list/strip, one RGBA8/BGRA8 attachment, builtin-oriented vertex/fragment shaders, and one static or dynamic viewport/scissor. Fixed-size storage-buffer descriptor arrays accept compile-time constant indices or dynamically uniform 32-bit indices when `shaderStorageBufferArrayDynamicIndexing` is enabled. Fixed-size storage-image arrays accept dynamically uniform indices when `shaderStorageImageArrayDynamicIndexing` is enabled and Metal storage textures are supported. Fixed-size sampled-image, sampler and combined image-sampler arrays accept dynamically uniform indices when `shaderSampledImageArrayDynamicIndexing` is enabled; the frontend rejects dynamic index expressions whose workgroup uniformity it cannot prove. 3D images, broad synchronization, user vertex attributes, depth/blend state, full instruction/SSA validation and general Vulkan conformance are not implemented; see [the SPIR-V implementation plan](../docs/Vulkan-SPIR-V.md).

## Status

| Milestone | Program | State |
|---|---|---|
| 1. PE loader, kernel32 core, UCRT startup and printf | `tests/hello_k32.c`, `tests/hello_crt.c` | Works (2026-10-12) |
| 2. Threads, waits, events, mutexes, semaphores, condition variables; structured exception handling (.pdata/.xdata unwinding), C++ exceptions, CPU faults as exceptions | `tests/threads.c`, `tests/cxx_exceptions.cpp`, `tests/hw_exceptions.c` | Works (2026-10-12) |
| 3. Files and paths (C:\ mapping), registry, file mappings and shared memory, loading real PE DLLs (imports, exports by name/ordinal, forwarders, DllMain, per-DLL TLS) | `tests/files.c`, `tests/dll_test.cpp` (+ `vxdll.dll`, `vxplugin.dll`); `tools/dllprobe.c` | Works (2026-10-12). Of the 109 DLLs in Half-Life: Alyx's `game/bin/win64` (tier0, steam_api64, openvr_api, Qt, ...), 103 load and run their initialization; the rest need user32, ws2_32, I/O completion ports or MSVC's vcruntime140 |
| 4. user32 windows on AppKit (classes, message queues with cross-thread SendMessage, timers, WM_PAINT, keyboard with virtual keys and scan codes, mouse, system metrics, displays, message boxes), gdi32 on Core Graphics (DCs, brushes, pens, fonts via Core Text, DIB sections, StretchDIBits, BitBlt), ws2_32 on BSD sockets, winmm timers | `tests/window.c`, `tests/net.c` | Works (2026-10-12). 104 of Alyx's 109 `bin/win64` DLLs load and initialize |
| 5a. COM (ole32: CoCreateInstance with built-in classes, registered factories and in-process servers from the registry), OLE automation (BSTR, VARIANT, SAFEARRAY), I/O completion ports and overlapped file I/O, vcruntime140 with MSVC C++ exceptions (FH3 tables, catch funclets via STATUS_UNWIND_CONSOLIDATE, rethrow) | `tests/com.c` (+ `vxcom.dll`), `tests/iocp.c`, `tests/msvc_eh.cpp` (built for the MSVC ABI by clang) | Works (2026-10-12). Every 64-bit DLL in Alyx's `bin/win64` (108 of 109) loads and initializes |
| 5b. Direct3D 11 and DXGI on Metal: device and immediate context (textures, buffers, views, clears, copies, uploads, Map/Unmap), DXGI factory/adapter/output, swap chains presenting into the window's CAMetalLayer, resizing. Method tables generated from MinGW's headers (`tools/gen_com_tables.py`) | `tests/d3d11.c` (`--interactive` shows 600 frames) | Works (2026-10-12). Draws need shader translation |
| 6. Shader translation DXBC → Metal Shading Language (`src/dxbc.c`: SM4/5 arithmetic, integer and bit operations, comparisons, control flow, sampling, loads, immediate constant buffers), input layouts, blend/depth-stencil/rasterizer/sampler states, pipeline-state cache, Draw/DrawIndexed(Instanced). Test shaders are compiled by Microsoft's d3dcompiler_43.dll running on vxrt (`tools/hlslc.c`); `build/dxbc2msl` shows a translation | `tests/draw.c` (`--interactive`: a spinning triangle) | Works (2026-10-12) |
| 7a. Compute shaders and UAVs: raw/structured buffers, typed UAV textures, thread-group shared memory, barriers, atomics, Dispatch, UAV clears | `tests/compute.c` | Works (2026-10-12) |
| 7b. Raw input (WM_INPUT mouse deltas, buttons, wheel, keys), SendInput, mouse-look (hidden cursor + ClipCursor), XInput on the Game Controller framework | `tests/input.c` | Works (2026-10-12) |
| 7c. DirectInput 8: attached HID joystick/gamepad discovery, immediate axis/button/POV state, `DIPROP_RANGE`, buffered `GetDeviceData` (`DIGDD_PEEK`, flush and overflow), and `SetEventNotification` | `tests/directinput.c`, `tests/directinput_buffer.c` | API and queue semantics pass; the current host has zero attached controllers, so physical HOTAS axes, buttons and device-specific layouts remain unverified. Event notification is raised when a polled read observes a state change. Force feedback and action mapping are not implemented |
| 7d. WinHTTP synchronous HTTPS/HTTP requests on NSURLSession: session/connection/request handles, request upload, common response headers, buffered response reads, timeouts, default/current-user proxy queries, named HTTP/HTTPS proxy routing and bypass rules | `tests/winhttp.c` (loopback server only) | HTTP request and proxy/bypass routing tested locally; HTTPS against a proxy or Frontier, PAC/WPAD resolution, async callbacks and large streaming bodies are not verified or implemented |
| 7e. WASAPI on Core Audio: MMDevice enumerator/endpoint/properties, shared event-driven IAudioClient, render client, clock, volume (Core Audio converts formats and rates) | `tests/audio.c` (`--tone` plays a quiet tone) | Works (2026-10-12) |
| 8a. XAudio2 2.8/2.9 on Core Audio: mastering, submix and source voices, an engine thread mixing 10 ms passes (8/16/24/32-bit and float PCM, resampling by frequency ratio, loops, output matrices, volumes), voice and engine callbacks; effects and filters accepted but not applied | `tests/xaudio2.c` (`--tone` plays a quiet tone) | Works (2026-10-12) |
| 8b. Translator robustness against real shaders (`tools/dxbcscan`: the DXBC in DLLs and in Half-Life: Alyx's shader archives, translated and compiled by Metal): gathers, LOD, sample info and positions, attribute evaluation, subroutine calls, doubles (as floats), carry/borrow/swap, msad; append/consume buffers and UAV counters, typed UAV atomics; texture views (format, mips, slices, type) and typed buffer views | `tests/compute.c`, `tests/shaders/features_*.hlsl`, a sample of Alyx's 507,000 shader permutations all translate and compile | Works (2026-10-12) |
| 8c. Texture formats and dimensions: block-compressed BC1–BC7 (uploads, partial updates, copies and Map in 4x4 blocks), integer/snorm/16-bit/32-bit variants, typeless depth buffers and stencil views, 1D and 3D textures (and 3D render target slices) | `tests/compute.c` | Works (2026-10-12) |
| 8d. Half-Life: Alyx (`hlvr.exe -novr -dx11`) starts: its ~40 DLLs load and initialize, the DX11 render system creates a device on Metal, settings are written (through the overlay), up to the point where the game needs the Steam client's networking. On the way: address-space walks (VirtualQuery coalesces macOS's regions), calls through null pointers become the program's exceptions, the thread pool, timer queues and waitable timers, PE resources, processor topology, Direct3D 9 adapter queries, D3D11 queries, DXGI 1.4 interfaces, and a few hundred more Win32 functions (`system.c`, `desktop.c`, `services.c`) | running the game (not committed: it is the user's copy) | Works up to Steam (2026-10-12) |
| 9a. Between processes: named pipes on Unix domain sockets (message and byte modes, overlapped connect/read, client process IDs), named events/mutexes/semaphores in shared memory (waiters woken with `os_sync_wait_on_address`), names held by live processes only (a registry of holders), CreateProcess (another vxrt process), process handles (waits, exit codes, OpenProcess, TerminateProcess), DuplicateHandle into another process (an inbox of handles), the PEB's process parameters and `gs:0x60`, the VC++ runtime from a Windows installation (`VXRT_DLL_PATH`, Wine's own DLLs passed over) | `tests/pipes.c`, `tests/ipc.c` (two processes each); SteamVR: `vrcmd --dumpsensors` starts vrserver, which loads the VeRantyx driver and reports its HMD and controllers | Works (2026-10-12) |
| 9b. Direct3D 11 resources shared between processes (IOSurface textures with keyed mutexes, shared-memory buffers, GetSharedHandle/CreateSharedHandle, OpenSharedResource(1)), ID3D11Device1/DeviceContext1, state getters, ResolveSubresource, view GetDesc, sample counts the GPU lacks, WinRT strings (no activatable classes), menus, multimedia timers | `tests/shared.c` (two processes); SteamVR: vrcompositor starts (its shaders through the translator, GPU speed measured, shared textures) and connects to vrserver and the client | Works (2026-10-12) |
| 9c. Fibers (stack switching in `context.S`), non-blocking (PIPE_NOWAIT) pipes as SteamVR polls them, a race-free registry of name holders, overlapped socket receives, ConnectEx/DisconnectEx, the system's control window classes, the native registry calls (NtOpenKeyEx and kin), token information, known folders, hidden `=C:` environment variables, certificate stores (empty), and a call tracer (`VXRT_TRACE_CALLS`) | `tests/threads.c` (fibers), `tests/pipes.c` (PIPE_NOWAIT), `tests/ipc.c`; SteamVR runs a minute without its watchdogs firing; the Windows Steam client starts its bootstrapper, steamui and vgui; Chromium (steamwebhelper) is far off (DirectWrite, sandbox, Mojo), but `tools/steam_fake_webhelper.c` answers Steam's shared-memory connection in its place — Steam then sits idle in its message loop without loading steamclient64 yet | Partly (2026-10-12) |
| 9d. Source 2 without Steam: the low 2 GB for LuaJIT (a 64 KB `__PAGEZERO`, NtAllocateVirtualMemory's ZeroBits), AcceptEx, WSAEventSelect, socket buffers as macOS allows, API sets for advapi32/user32, event queries that do not wait for work never submitted, avifil32/usp10/mixer stand-ins, and `VXRT_PRESENT_SNAPSHOT` (a present's back buffer to PNG) | SteamVR Home (`steamtours.exe -novr -dx11`, Source 2, from SteamVR's tools) loads its map, runs its Lua scripts and its in-process Steam game server, and renders the room correctly through the DXBC→MSL translator | Works (2026-10-12) |
| 9e. SteamVR Home in VR on SteamVR on vxrt: viewport and scissor arrays, SV_ViewportArrayIndex / SV_RenderTargetArrayIndex / SV_ClipDistance from vertex shaders, desktop and window-station names, draw tracing (`VXRT_TRACE_DRAWS`, `VXRT_DRAW_SNAPSHOT`) | `steamtours.exe -vr -retail -dx11` next to `scripts/launch_vxrt_steamvr.sh` (with a valid head pose): both eyes go through vrcompositor to the VeRantyx driver. SteamVR needs `allowDisplayLockedMode` (the launch script sets it in its copy of the settings): without Windows' DisplayManager it otherwise believes the display is locked and shows plain green | Works (2026-10-12) |
| 9f. Apple silicon tuning (any M1-M6: nothing is tied to one chip; core counts, P/E cores and GPU family are read from the system): wake-ups aimed at the threads waiting on an object, thread priorities as QoS classes (P or E cores), ReleaseSync that does not wait for the GPU, small renamed buffers (constant buffers, dynamic vertex data) as slices of 4 MB chunks, pipeline states in a hash table with the last one remembered, draws that only send Metal the state that changed, Map of staging resources that waits only for the copies into them (and `DO_NOT_WAIT` → `DXGI_ERROR_WAS_STILL_DRAWING`) while still flushing recorded work as drivers do, GenerateMips | `make bench`: 17.4 µs per draw before, 1.7 µs after (M1 Max, under Rosetta); SteamVR Home in VR: 12-27 frames a second through the compositor before, 58.8 on average over 6 minutes after (the virtual display runs at 60 Hz); the room is ready about 15 s after start | Works (2026-10-13) |
| 9g. Steam on the Mac for Windows games: a built-in `steamclient64.dll` (`src/steambridge.c`, `VXRT_STEAM_BRIDGE=1`) forwards to the Mac Steam client's own `steamclient.dylib` (x86-64 slice, in process): interface proxies whose static stubs turn Microsoft calls into System V ones, interfaces wrapped by version string, `CSteamID` returns, Windows function pointers (only real function starts, by `.pdata`) through trampolines that give the client's threads a TEB and a 2 MB stack, `SteamNetworkingMessage_t` release functions both ways, the connection-status callback re-laid out (Windows packs to 8), `Steam_IsKnownInterface`. Also: the registry entries steam_api64 looks for, system DLLs by full path, per-page VirtualProtect with image section protections (V8), growable function tables for JIT code (V8), msvcrt's extra functions, signal alternate stacks with stack-overflow reports, D3D format-support bits as Direct3D defines them, multisampled swap-chain back buffers ; shaders compile in the background (Metal's compiler service works on many at once; the first draw that needs one waits), a depth target for shaders writing depth with none bound | Half-Life: Alyx in VR (`hlvr.exe -vr -steam -dx11 +map startup` next to `scripts/launch_vxrt_steamvr.sh`): `SteamAPI_Init succeeded` with the user's own Mac Steam login, Steam Cloud saves, Steam Datagram Relay, its client and server over SteamNetworkingSockets loopback, and the main menu in both eyes through vrcompositor to the VeRantyx driver, Wine-free | Works (2026-10-13) |
| 9h. Half-Life: Alyx's chapters in VR: dual-source blending (a variant of the pixel shader with `o1` as Metal's `color(0) index(1)`, made when a blend state uses SRC1 factors), `TaskDialogIndirect` (fatal errors are logged), and `scripts/vxrt_helper.swift` (`webui-standin`), which stands in for Alyx's SteamVR web UI (vrwebhelper is Chromium and does not run here): it tells the game's vrmailbox the loading-screen page is ready, through vrserver's web server, as the page would | `hlvr.exe -vr -steam -dx11 +hlvr_pause_after_realmap 0 +map a1_intro_world_2` next to `scripts/launch_vxrt_steamvr.sh`: the first chapter renders in both eyes at 55-57 frames a second (60 Hz virtual display), the game's GPU about 40% busy (M1 Max); controllers and the Vision Pro side untested | Works (2026-10-13) |
| 9i. Geometry shaders (Metal has no such stage): a draw with one runs the vertex shader as a compute kernel over its vertices (Metal fetches them through a stage-input descriptor made from the input layout, indexed draws included), the geometry shader as a kernel over its primitives (its triangle strips, with cuts, written out as triangles; the rest degenerate), then draws those triangles with a pass-through vertex function generated from its outputs (position, clip distances, viewport and layer index) and the pixel shader | `tests/draw.c` (a point made into a square, and a cut strip); all 13 of Half-Life: Alyx's geometry shaders translate and compile (line-to-quad expansion for cables and beams, triangle amplification, one multi-viewport shader). Not yet: instancing, outputs other than triangle strips (stream output: 9r) | Works (2026-10-13) |
| 9j. Higher resolution (`VXRT_VR_QUALITY=3k`: 1440x1620 per eye, 2880x1620 HEVC Main10 at 40 Mbps, a 72 Hz virtual display so SteamVR holds the game at 36 rather than halving 60 Hz to 30) at usable speed: reservations kept sorted and mapped in whole 64 KB units (with only their pages mapped, the kernel walked tens of thousands of gaps on every allocation, 2 ms each), released blocks of 1 MB and more reused (and dropped when the program reserves their address: tier0's aligned allocation looped forever otherwise), staging textures of one plain image kept in a buffer row after row so Map hands out their memory instead of copying a tiled image out, and AcquireSync takes a keyed mutex as soon as it is released before waiting for the releaser's GPU work (the game took it back meanwhile, and the compositor missed its frames) | `tests/files.c` (aligned reservations, 2000 small blocks), `shared.exe pingpong` (two processes take turns with one texture, key 0 both ways); Half-Life: Alyx at fidelity level 4 (1542x1735 per eye): 8-9 frames a second before, 36-38 after (M1 Max, 60 Hz virtual display); the 10-bit stream needs the Vision Pro app rebuilt | Works (2026-10-13) |
| 9k. Faster 3K: clears wait for the render pass that draws to their target and become its load action (on Apple's tile-based GPUs this saves writing a whole target out and reading it back per clear), Windows condition variables and WaitOnAddress wait in 256 queues by address (one queue for the process woke every sleeping thread of the game's job system on every wake), SteamVR set not to hold the game back by whole frames; `VXRT_PROFILE`, a sampling profiler that sees through Rosetta (it showed the game's own memcpy and the condition-variable herd) | `tests/d3d11.c` (timestamps on QueryPerformanceCounter's clock; GPU times with `VXRT_GPU_TIMESTAMPS=1`); Half-Life: Alyx at fidelity level 4, 72 Hz: 36-38 → 44-47 frames a second (M1 Max), run-to-run spread checked by alternating runs | Works (2026-10-13) |
| 9l. 3K at 60 fps: the game's capture proxy answers WaitGetPoses from the headset's poses at the virtual display's rate (with the hands' velocities) and stops forwarding frames to SteamVR's compositor, which handed out the next poses only after the GPU had finished the last frame (a frame cost the CPU's time plus the GPU's); at most two frames in flight. The encoder converts frames on the GPU. Also: DLLs are looked up in `VXRT_OVERLAY` first (the proxy goes there, the Steam library stays as it is), sizes up to 2.5K per eye (8192-wide frames) through driver, proxy and encoder, and the compositor's output size apart from the game's (`VERANTYX_DISPLAY_SCALE`) | Half-Life: Alyx at fidelity level 4 (1542x1735 per eye), M1 Max: 44-47 → 60 fps, all 60 encoded (72 at 72 Hz, of which one HEVC session encodes about 60); GPU 67% busy | Works (2026-10-13) |
| 9m. 2.5K per eye: render passes kept across target changes that do not need a new one (targets compared as subresources, not view objects; a draw to depth alone continues a pass that also has color, its color writes masked off; a clear of another target leaves the pass open, done before anything reads it), DiscardView/DiscardResource (the next pass need not load), and `VXRT_STATS` reports render-pass memory traffic and what ended multisampled passes | Half-Life: Alyx at 2.5K per eye (fidelity level 3, 5120x2880 4x multisampled targets), M1 Max: 28-30 → 33-35 fps, GPU 100% busy; attachment traffic 97+129 → 48+66 GB/s | Works (2026-10-13) |
| 9n. OpenGL and more programs (first: Google Earth VR, free): opengl32.dll on macOS's OpenGL (a Windows-convention function for each of the SDK's 1377 GL functions, generated by `tools/gen_gl_thunks.py`; WGL contexts are CGL contexts, legacy 2.1 or 4.1 core), wininet.dll on NSURLSession (HTTPS; a decoded body's length), avrt and enough of GDI+ to start, `extras.c` (disk space, file locks, UI languages, C runtime functions), a million handles (Earth keeps 8000 semaphores), the descriptor limit raised, a thread-safe qsort, a backtrace for unhandled exceptions; multisampled and depth targets without pixel-format views (lossless compression) | `tests/gl.c` (legacy and core contexts, a framebuffer cleared and read back, shaders compiled); Google Earth VR starts on vxrt with SteamVR, fetches its configuration over HTTPS and its frames are captured (OpenGL readback) | In progress (2026-10-13) |
| 9o. 2.5K per eye at 50–60 fps: fragment shaders declare only the built-in inputs they read (every one declared `[[sample_id]]`, which makes Metal shade each sample of a multisampled target), synchronous shader variants compiled under a lock of their own (a deadlock with Metal's compiler queue when every shader recompiled), `VXRT_GPU_PROFILE` (render passes and dispatches timed with Metal's stage-boundary counters, each by how far it moved the end of the GPU's work), `VXRT_MSAA_MAX` / `VXRT_MAX_ANISOTROPY` quality caps and the `VXRT_VR_QUALITY=2.5k` preset (2× MSAA, fidelity level fixed) | Half-Life: Alyx at 2560x2880 per eye, M1 Max: 33–35 → 44–49 fps with 4x MSAA (GPU-bound: 1.4 GB of attachments loaded and 1.9 GB stored a frame), 50–60 fps with 2x (60 held for 4 minutes in one run) | Works (2026-10-13) |
| 9p. Memory held steady in long sessions: threads that submit GPU work get an autorelease pool emptied after each submission (program threads never return to a run loop, so Metal's autoreleased command buffers, encoders and pass descriptors stayed for the thread's life); `VXRT_STATS` also reports live Direct3D objects by kind and the renamed-buffer chunks and pool | Half-Life: Alyx at 2.5K: the game's memory grew 4 MB/s (about one GPU allocation a frame, 14 GB an hour); now flat (8.08–8.09 GB over 3 minutes) | Works (2026-10-13) |
| 9q. Google Earth VR in VR: HttpQueryInfo answers HTTP_QUERY_RAW_HEADERS (the headers as NUL-ended lines; Earth asked for them after reading its configuration and took the error as an empty response: "Loading labels failed") | Google Earth VR on vxrt (OpenGL on macOS's OpenGL, WinINet on NSURLSession): 7454 Earth tiles fetched in 150 s, terrain (Yosemite) drawn in both eyes, ~59 fps captured | Works (2026-10-13) |
| 9r. The Lab (Unity 2019.4, Mono) in VR: TlsSetValue also writes the slot at gs:0x1480 + 8i, where Mono's JIT code reads it; SuspendThread / GetThreadContext / ResumeThread on running threads (a signal parks the thread with its context saved: the Boehm GC's stop-the-world) and QueueUserAPC with alertable waits; VirtualQuery reports macOS mappings with their real protection (the GC walks a thread's stack down while pages are PAGE_READWRITE: with every page PAGE_EXECUTE_READWRITE it scanned no stack and freed live objects: methods were called on the wrong objects); SetDllDirectory (Unity's Plugins folder); the last FreeLibrary of a DLL only LoadLibrary brought in detaches it, so the next load starts fresh (openvr_api unloads vrclient between Unity's two VR_Inits), GetModuleHandleEx holds a reference as on Windows; _Gettnames returns a block of its own (the C++ library frees it when a DLL unloads); stream output with a vertex shader's code (the vertex kernel, then a kernel copying the declared components to the targets; appends continue where the last write ended), as Unity skins meshes; IMMEndpoint, GetThreadDesktop and small leftovers | `tests/threads.c` (suspend, APC, the GC's stack walk), `tests/dll_test.cpp` (unload and reload), `tests/draw.c` (stream output, appended); The Lab's title scene drawn in both eyes through SteamVR on vxrt, 53–58 fps captured (60 Hz display), no managed exceptions (68 000 a run before the VirtualQuery fix). Not yet: hull and domain shaders (Unity carries on without them), stream output from a geometry shader | Works (2026-10-14) |
| 9s. Geometry draws without the waste: the geometry kernel records how many triangles each primitive made and the pass-through function skips the rest (it used to write zeros into every unused slot: Alyx's menu shader may make 13 triangles of 11 registers per input triangle, 4,816 triangles x 2 eyes per draw, 31 draws a frame: about 2 GB written a frame), and the geometry path's buffers are kept from draw to draw instead of allocated per draw (66 MB each). Tried and dropped: running the vertex and geometry stages as rasterization-off vertex functions inside the render pass (no split passes, but slower on Apple GPUs: 16 vs 19.5 fps, A B B A) | `tests/draw.c` (also an indexed geometry draw with a base vertex); Half-Life: Alyx menu at 3k: 18-21 → 54-59 fps (runs 20261014T074457Z, 20261014T074703Z), chapter 1 still 60.0 fps | Works (2026-10-14) |
| 9t. GDI to the screen, for Chromium's software output and Steam's VGUI: BitBlt / StretchBlt / SetDIBitsToDevice / StretchDIBits / GetDIBits / SetDIBits / AlphaBlend / TransparentBlt moving pixels in C (every ROP3, mirroring, HALFTONE), DIB sections over file mappings, DC viewport and window origins, clip regions, SaveDC, update regions (BeginPaint's clip and rcPaint, GetUpdateRgn, ValidateRgn, RedrawWindow's flags), ScrollWindowEx / ScrollDC, UpdateLayeredWindow(Indirect) and SetLayeredWindowAttributes, DwmFlush, PrintWindow; windows presented as Core Animation layers from committed pictures (never half-drawn), child windows as sublayers, child windows' screen coordinates; Command Clips `snapshot-view` (the window server's pixels of the window) | `tests/present.c` with `tests/present_check.py` (gradients through every path, read back from the window server at the backing scale); Steam's "Steam Service Error" box drawn as on Windows | Works (2026-10-24) |
| 8. Tessellation shaders (Metal's tessellation differs: emulation with compute), audio capture, FH4 C++ exceptions (needs MSVC-built test programs), Direct3D 11 at game scale | — | Next |
| 9. Direct3D 12 (VXRuntime's own DXIL reader and DXIL→MSL translator, in progress), OpenGL | — | |
| 10. Cryptography, processes, Chromium-class programs (the Steam client) | — | |

## Known risks (research items)

- **TSD slots 0x30 and 0x58.** macOS uses some low TSD slots itself (0x30 is the pointer-munge key on recent
  systems). Wine relied on this layout for years, but it needs checking against setjmp/longjmp and libdispatch.
- **`gs:[0x10]` and other TEB fields read through `gs` directly** (MSVC's `__chkstk` reads the stack limit at
  `gs:0x10`, which is macOS's MIG reply slot). Such code will need patching at load time.
- **Exceptions (milestone 2).** Exceptions are dispatched and unwound with the program's own `.pdata`/`.xdata`,
  as on Windows. This runtime's functions are native code without Windows unwind data, so every entry into the
  runtime that can call back into the program (RaiseException, RtlUnwindEx, a CPU fault) records the program's
  context, and stack walks jump over the native part to it; an unwind meeting another unwind in progress
  continues from the frame that one was processing (Windows' "collided unwind"). MinGW's C++ runtime depends on
  this: its cleanup handlers raise a new exception from inside an unwind. CPU faults (SIGSEGV/SIGBUS/SIGFPE/
  SIGILL/SIGTRAP) inside the program are redirected to a dispatch on the faulting thread's own stack. Not done
  yet: SuspendThread/GetThreadContext on running threads, MSVC's `__CxxFrameHandler4` (the compressed tables of
  Visual Studio 2019+; FH3 works), stack overflow
  (needs a guard page and an alternate signal stack), and longjmp does not run `__finally` blocks.
- **No sandbox: Chromium's (steamwebhelper's) is not in force.** Its restricted tokens, job objects (their limits are
  accepted and ignored, `desktop.c`) and alternate window stations and desktops (`winsta.c`) isolate nothing here: a
  renderer runs with the same rights, windows, hooks, clipboard (the Mac's pasteboard) and input as the browser
  process, `CreateProcess` does not put a child on `STARTUPINFO.lpDesktop`, and a security descriptor given to a
  window station or desktop is not applied. Making a window station or desktop still succeeds, by decision:
  Chromium stops when it cannot, so failing would not keep its renderers apart either, only stop Steam. The first one
  a process makes says so in its log (`winsta: ... it isolates nothing here`), whatever the trace setting. What is kept as on
  Windows: each window-station and desktop handle's access rights (`SetUserObjectInformation`, `SwitchDesktop` and
  `EnumDesktopWindows` refuse a handle without the right, with `ERROR_ACCESS_DENIED`) and `EnumDesktopWindows` lists
  only the windows made by threads on that desktop. Process mitigation policies are not enforced either, and say so:
  `SetProcessMitigationPolicy` refuses to turn one on with `ERROR_NOT_SUPPORTED` (logged once per policy) and
  `GetProcessMitigationPolicy` reports each off (`extras.c`). Run only web content you would trust outside a sandbox.
- **Scale.** The Windows API is thousands of functions; the order of work follows what real programs import.
