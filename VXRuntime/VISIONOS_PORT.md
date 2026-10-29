# VXRuntime on visionOS, as it is (2026-10-30)

The question for this round: what happens when today's VXRuntime (vr-main 79401b9: the ARM64 runtime, D3D12 on
Metal, the NPU race, the static recompiler's modules) is put on Apple Vision Pro without more work on the Mac first.
The answer, with evidence, and a plan for each wall.

## What was built

- **The runtime for `xros`** (`make xros-lib` → `build/xros/libvxrt.a`; `make XROS_SDK=xrsimulator xros-lib` for the
  simulator). 120 of the ARM64 build's 128 sources compile for the device unchanged or with a guarded line or two;
  8 are left out and 3 added (below). Every source is compiled with `xros/vx_xros.h` included first and `xros/include` ahead of
  the SDK, so most macOS-only calls are answered there instead of in the sources.
- **The app "VX Runtime"** (`xros/app`, bundle id `marimokodai.VXRuntimeHost`, team 7YNW843VY6, xcodegen;
  `make xros-app`). Separate from VX Proto (`marimokodai.VisionOSProto`) and the VeRantyx client
  (`marimokodai.VisionSpatialTools`). One flat UIKit window; the Windows program named by `-test <name>` runs in it.
  Entitlements: `increased-memory-limit`, `extended-virtual-addressing` (both granted by the team's automatic
  profile, checked with `codesign -d --entitlements`). 19 MB installed, of which 15.6 MB is the executable.
- **AOT only** (`translator/engine_nojit.c` replaces `engine.c`): nothing is translated at run time. An x86 address
  without a module stops the program with its address and image in the log (see `cxx_exceptions` below).
- **Modules linked into the app** (`tools/xros_aot.sh`, `make xros-aot-tests`): each test program's route-B
  assembly is assembled for the device, its descriptor renamed `vxrt_aot_module_<n>`, and `registry.c` maps the
  file names `translator/aot.c` already asks for (`<name>-<hash>[.p<k>].dylib`) to them. `xa_module_exists` /
  `open_module` (aot.c) are the only change in the loader's logic; on the Mac they are still `access` + `dlopen`.
- **The arena** (0xf00000000, 512 MB) is reserved in a constructor of the app before `main`, instead of the Mac's
  restart-until-free (`execv` is not possible in an app).
- **Presentation** (`src/xros_ui.m` in place of `src/cocoa.m`): the app's root view is the program's display (its
  size in points: 1280×720 by default); each top-level window is a subview at its rectangle (UIKit's y runs down as
  Windows' does, so no flipping); GDI pictures are layer contents as on the Mac; a swap chain gets a `CAMetalLayer`
  sublayer filling the window (`drawableSize` = bounds × display scale). Touches are the left mouse button.
- **Logs**: `Documents/host_log.jsonl` (launch, arena, exe-base and address-space probes, a Metal capability probe,
  `os_proc_available_memory()` and the footprint every 2 s, the program's start) and `Documents/vxrt.log` (the
  runtime's stderr for that launch). `tools/xros_device_run.sh` launches each test and copies both back.

## Compile and link walls (all fixed behind `VX_XROS`; Mac `make check` unchanged: 1822 "ok" lines before and after)

| Wall | Where | What the visionOS build does |
| --- | --- | --- |
| `<mach/mach_vm.h>` refused ("unsupported") | loader, kernel32, arm64, aot, desktop, dbghelp, steambridge | `xros/include/mach/mach_vm.h`: the same calls as `vm_*` (64-bit types on arm64) |
| `MAP_JIT`, `pthread_jit_write_protect_np` unavailable | translator/engine.c | engine left out; `engine_nojit.c` (AOT only) |
| `Hypervisor.framework` absent | translator/vcpu.c, guest.c, vm_hooks.c | left out; vCPU stubs and `vxvm_*` on malloc (`xros_stubs.c`) |
| AppKit / Cocoa | cocoa.m, menus.m, tray.m, clipboard.m, npu_interp.m (NSScreen) | cocoa.m → `xros_ui.m` (UIKit); tray, menus, Windows clipboard → stand-ins already in user32/desktop; NPU race: `CADisplayLink` at 90 Hz |
| CoreGraphics display / event API (`CGDisplayMode*`, `CGEventCreate`, `CGWarpMouseCursorPosition`, `CGEventSourceSecondsSinceLastEventType`) | user32, dxgi, d3d9, dwmapi, winmisc | `vx_xros.h`: the "display" is the app's window; pointer from the last touch |
| `CAMetalLayer.displaySyncEnabled` unavailable | dxgi, d3d12, gpu_ir_metal, vk_wsi, npu_interp | not set (the system paces the layer) |
| `MTLPixelFormatDepth24Unorm_Stencil8`, `X24_Stencil8` unavailable | d3d12.m | `VX_MTL_D24S8` = Invalid on visionOS (Apple GPUs have no D24 on the Mac either) |
| `NSTask` (offline `xcrun metal` shader cache) | d3d12.m | no offline compile on the device (see "shaders" below) |
| `libproc.h`, `sys/proc_info.h` absent | jobs, kernelx, sync, tcptable, dotnet_runtime | `xros/include/libproc.h`: no other processes, no socket details (an app sees only itself) |
| `net/route.h`, `net/if_media.h` absent; SCNetworkInterface unavailable | iphlp.c | no route-change notifications; interfaces keep BSD names |
| SCDynamicStore (DHCP, proxies) unavailable | sysdlls, winhttp, winhttp_proxy | no DHCP details; proxies from `CFNetworkCopySystemProxySettings` (`xros/vx_xros_proxy.h`) |
| CoreAudio HAL absent | audio.m, xaudio2.m | rate and name from `AVAudioSession`; output unit RemoteIO |
| `Security/AuthSession.h` absent | dpapi.c | session id 0 (one user) |
| `LSOpenCFURLRef` absent | dotnet_runtime.c | WPF hyperlinks not opened |
| `posix_spawn_file_actions_addchdir_np` unavailable | kernel32.c (CreateProcess) | dropped; `posix_spawn` itself fails in an app (no child processes) |
| `OpenGL.framework`, `IOKit/hid` absent | opengl.c, directinput.m | left out: no opengl32.dll; dinput8 keeps no devices |
| `__mh_execute_header` missing in Xcode's debug-dylib mode | loader.c | `ENABLE_DEBUG_DYLIB: NO` |
| `main` | loader.c | compiled as `vx_loader_main`; the app calls it on a thread of its own |
| Simulator only: no Metal 4, placement sparse, MetalFX, presented times | d3d12.m, npu_interp.m, upscale_core.m | `VX_XROS_SIM` guards (tiled tier 0 there) |

Not walls at compile time: Rosetta-only paths (the ARM64 build has none), pthread QoS, signals and `ucontext`
(`__ss`, `__es.__esr`), `os_sync_wait_on_address`, GameController, VideoToolbox's low-latency frame interpolation
(available on visionOS 26), CompositorServices (not used yet).

## What runs (visionOS 26.0 simulator on this Mac; the headset run is pending: see below)

Each from the bundle, `VXRT_AOT_STRICT=1`, no JIT, modules linked into the app. Logs in
`~/Verantyx_VR_Drive/wt/runs/avp-port/sim/`.

| Program | Result |
| --- | --- |
| probes | arena at 0xf00000000 reserved; 0x140000000 (exe base) free; largest PROT_NONE reservation 128 GB (simulator = the Mac's limits) |
| hello_crt.exe | prints its line (MinGW CRT, constructors, printf of floats) |
| seh_try.exe | `__except reached -> ok` (faults delivered as signals in an app work) |
| unwind.exe | `62 cases, 0 failed -> ok` |
| suite.exe (tests/recomp) | jump tables, SEH, C++ exceptions, threads (800 000 locked increments, 0 MP violations), TLS, SSE and AVX2/FMA: ok; then D3D11 stops on a simulator-only Metal rule (linear textures need private buffers there) |
| cxx_exceptions.exe | stops as designed: `0x14000182b has no recompiled code` (a block the generator did not find; on the Mac it is 1 fallback) |
| d3d12.exe | device, queues, root signatures and bindless validation ok; the first graphics PSO stops on a simulator-only Metal limit (no dynamic vertex strides). The simulator GPU reports no Metal 3, no BC textures, no argument-buffer tier 2: it is not the headset |

So the runtime's CPU side is sound on visionOS frameworks: loading, imports, the thunks, the AOT runner, faults as
signals, x86 SEH and unwinding, threads and TLS, SIMD. The GPU side has to be judged on the headset.

## On the headset

The app installs (`xcrun devicectl device install app`, done twice today). Launching needs the headset worn and
unlocked: `devicectl device process launch` waited at "Acquired usage assertion" and timed out three times with
`passcodeRequired: true`. Run, with the headset on:

    cd VXRuntime && tools/xros_device_run.sh            # none hello_crt seh_try unwind suite cxx_exceptions d3d12

It prints each test's last lines and leaves `host_log.jsonl` and `<test>.vxrt.log` in
`~/Verantyx_VR_Drive/wt/runs/avp-port/device/`. What to read there: `va_probe` and `exe_base` (does the Windows
layout fit with extended-virtual-addressing: VX Proto's probe the same morning saw 64 GB reservable and 8188 MB
available of 15744 MB), `metal` (BC compression, Metal 3, argument-buffer tier, Metal 4 queue, placement sparse),
`mem` (available memory while D3D12 runs), and how far `d3d12.exe` gets (105 "ok" lines on the Mac).

## Walls ahead for Cyberpunk 2077, and the plan for each

1. **No JIT.** Every address must be in a module. Today 18 addresses still fall back on the Mac (exe 7, PhysX 4,
   REDGalaxy 4, tier0 2, overlay 1; RECOMP.md). Plan: keep running the Mac with `VXRT_AOT_STRICT=1` and the
   misses → roots loop until 0, then also fix the generator's gaps the small tests show (cxx_exceptions'
   0x14000182b, hw_exceptions' one block). A tiny x86 interpreter for the rare leftovers is the fallback plan
   (slow but legal), since generating code is not.
2. **1.3 GB of modules.** Cyberpunk's 36 module dylibs are 1.3 GB (the exe alone 4 parts, 730 MB). Linking that into
   the executable is risky (launch time, `__TEXT` size). Plan: ship them as signed frameworks inside the app
   (`Frameworks/*.framework`, signed at build time: `dlopen` of the app's own signed code is allowed), keeping the
   hash-named lookup; the static registry stays for small programs. They are the game's translated code: built on
   the user's Mac, installed only to the user's own headset, never committed or shared (RECOMP.md).
3. **Memory.** 8188 MB available with `increased-memory-limit` (VX Proto's probe) against a Mac footprint of
   12.4 GB (Metal 5.8 GB) outside, 7 GB at the lowest settings. Plan: the 10-29 budget (TEX_DOWNSCALE, lowest
   settings, file-backed reads of the archives instead of copies), then measure on the headset with the jsonl log;
   jetsam ends the app without a crash report, so the 2 s memory log is the evidence.
4. **Steam.** The Steam version needs a Steam client: on the Mac either the Mac client through `steambridge.c`
   (it loads the Mac's x86-64 `steamclient.dylib`, impossible on visionOS) or the real Windows Steam (several
   processes and CEF: impossible in one app). Legitimate options to study, never a fake ownership answer:
   (a) the same bridge over the local network: a helper on the Mac loads the user's signed-in Mac Steam client and
   answers the game's interface calls (Steam's own answers, the Mac nearby); (b) a DRM-free (GOG) copy if the user
   has one. First a study on the Mac: what the game does at start without a reachable client (log only).
5. **No child processes.** The game starts `REDEngineErrorReporter.exe` (seen on the Mac today); CreateProcess
   fails on the device. Plan: answer it inside the process (a thread running the reporter's entry from its own
   module) or let it fail and check the game goes on.
6. **Shaders.** The Mac compiles large shaders in the background with `xcrun metal` and caches `.metallib`s; the
   device cannot. Without it Cyberpunk's largest shader takes 20–30 s at each start (its watchdog: 120 s). Plan:
   build the cache on the Mac for visionOS (`xcrun -sdk xros metal`, same source hash keys) and copy it to
   `Documents/shadercache`, `VXRT_D3D12_SHADER_CACHE` pointing there.
7. **NPU race.** `VTOpticalFlow` is `API_UNAVAILABLE(visionos)` and an app cannot start the `vxrt-npu` helper. Done
   this round: on visionOS race mode runs on the game's own motion vectors (FSR2's, which never needed the helper),
   interpolation and extrapolation stay off; it logs that. Next: the low-latency frame interpolator in-process
   (available on visionOS 26), or a Core ML flow on the ANE.
8. **MetalFX temporal** is unavailable (spatial only): the game's own FSR2 / XeSS run as compute as they do now.
9. **Input.** Only touches today. Plan: GameController (already the XInput path; the simulator saw a gamepad),
   a Bluetooth keyboard and mouse through `UIKey` / `GCMouse`.
10. **Presentation.** Flat window now; then CompositorServices (VX Proto's hold mode, depth, 90 Hz) once frames come.

## The game's data on the device (needs the user's yes: about 62 GB)

The Steam install is 62 GB: `archive/` 61 GB, `bin/` 552 MB, `engine/` 184 MB, `r6/` 61 MB. Layout in the app's
container (the runtime's C: drive is `$VXRT_ROOT/drive_c`, `VXRT_ROOT` = `Documents/root`):

    Documents/
      root/drive_c/Program Files (x86)/Steam/steamapps/common/Cyberpunk 2077/   (the install, read-only use)
        archive/pc/content/*.archive, bin/x64/..., engine/, r6/
      root/drive_c/users/vxrt/AppData/Local/CD Projekt Red/Cyberpunk 2077/      (UserSettings.json: lowest settings)
      root/drive_c/users/vxrt/Saved Games/CD Projekt Red/Cyberpunk 2077/         (saves: not copied)
      shadercache/                                                              (metallibs made on the Mac)
      host_log.jsonl, vxrt.log

Copy: `xcrun devicectl device copy to --device 717AC0D2-8713-5537-8D5F-A611235E7D35 --domain-type appDataContainer
--domain-identifier marimokodai.VXRuntimeHost --source "<install>" --destination "Documents/root/drive_c/Program
Files (x86)/Steam/steamapps/common/Cyberpunk 2077"` (or Finder file sharing: `UIFileSharingEnabled` is on). The
headset has 1 TB. Only with the user's explicit yes.

Launch there (once 1–4 are answered; not built yet): the app's `-test` would become an `-exe "<C: path of Cyberpunk2077.exe>" --launcher-skip
-skipStartScreen`, with the game's modules as frameworks (2), `VXRT_NPU_INTERP=race`, the shader cache (6) and the
Steam answer (4).
