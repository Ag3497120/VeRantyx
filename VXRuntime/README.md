# VXRuntime: running Windows programs on Apple silicon without Wine

A research project inside VeRantyx: a Windows compatibility layer written from scratch for Apple silicon
Macs, meant to eventually replace Wine for the Wine version of VeRantyx. Results are expected to come slowly;
each milestone is a small Windows program that has to run unchanged.

```bash
make check          # builds build/vxrt and the test programs, runs them
build/vxrt program.exe [args...]
VXRT_TRACE=1 build/vxrt program.exe     # also lists imports that are not implemented yet
VXRT_DUMP_SHADERS=dir build/vxrt game.exe   # writes every translated shader (Metal source) to dir
build/vxrt build/dllprobe.exe 'Z:\path\to\some.dll' [export ...]   # how far a real DLL gets
```

`C:\` is `$VXRT_ROOT/drive_c` (default `~/.vxrt/drive_c`, created on first run with `C:\windows\system32` and
`C:\windows\temp`); `Z:\` is the whole Mac file system. The registry lives in `$VXRT_ROOT/registry.txt`.
`make check` uses `build/root` so the tests never touch `~/.vxrt`.

Windows run on AppKit: the program runs on a thread of its own and the macOS main thread runs AppKit, so a
program's message loop and window procedures work from any thread. `VXRT_NO_ACTIVATE=1` keeps windows from
taking focus (used by `make check`); `VXRT_SNAPSHOT=file.png` saves a window's view as it is closed.
`build/vxrt build/window.exe --interactive` opens the test window and prints the keyboard and mouse messages it gets.

Needs Xcode's clang and `x86_64-w64-mingw32-gcc` (Homebrew `mingw-w64`) for the test programs.

## Design

| | Wine | VXRuntime |
|---|---|---|
| Process | Windows process = Unix process + wineserver for shared kernel objects | One macOS process; kernel objects live in-process. Cross-process objects later, over Mach IPC, only where needed |
| Windows API | PE DLLs that cross into a separate Unix half (`__wine_unix_call`, syscall-style transitions) | Native Mach-O functions with the Windows calling convention (`ms_abi`), called directly by the program: no boundary to cross |
| CPU | x86-64 code under Rosetta 2 | The same: Rosetta 2 translates the program and the runtime together (the runtime is an x86-64 binary) |
| Thread state | Since Wine 8 on macOS, `gs` is switched to the TEB while Windows code runs | `gs` stays macOS's TSD; the TEB pointer and TLS array are written to TSD slots 0x30/0x58 (Wine's pre-8 macOS approach), so Windows code, this runtime, Metal and D3DMetal-style code share a thread without switching |
| Graphics | wined3d / DXVK / D3DMetal | Direct3D on Metal, written here (planned; Apple's Metal Shader Converter for DXIL) |

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
| 7c. WASAPI on Core Audio: MMDevice enumerator/endpoint/properties, shared event-driven IAudioClient, render client, clock, volume (Core Audio converts formats and rates) | `tests/audio.c` (`--tone` plays a quiet tone) | Works (2026-10-12) |
| 8a. XAudio2 2.8/2.9 on Core Audio: mastering, submix and source voices, an engine thread mixing 10 ms passes (8/16/24/32-bit and float PCM, resampling by frequency ratio, loops, output matrices, volumes), voice and engine callbacks; effects and filters accepted but not applied | `tests/xaudio2.c` (`--tone` plays a quiet tone) | Works (2026-10-12) |
| 8. Geometry and tessellation shaders (Metal has neither: emulation with compute), audio capture, FH4 C++ exceptions (needs MSVC-built test programs), Direct3D 11 at game scale | — | Next |
| 9. Direct3D 12 (Metal Shader Converter), OpenGL | — | |
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
- **Scale.** The Windows API is thousands of functions; the order of work follows what real programs import.
