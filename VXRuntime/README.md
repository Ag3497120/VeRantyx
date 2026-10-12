# VXRuntime: running Windows programs on Apple silicon without Wine

A research project inside VeRantyx: a Windows compatibility layer written from scratch for Apple silicon
Macs, meant to eventually replace Wine for the Wine version of VeRantyx. Results are expected to come slowly;
each milestone is a small Windows program that has to run unchanged.

```bash
make check          # builds build/vxrt and the test programs, runs them
build/vxrt program.exe [args...]
VXRT_TRACE=1 build/vxrt program.exe     # also lists imports that are not implemented yet
```

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
| 2. Threads, waits, events, interlocked ops; structured exception handling (.pdata/.xdata unwinding) | — | Next |
| 3. Files and paths (C:\ mapping), registry, loading real PE DLLs (the game's own: steam_api64, openvr_api) | — | |
| 4. user32/gdi windows on AppKit, a message loop; Direct3D 11 on Metal (`tools/d3dprobe`) | — | |
| 5. Direct3D 11 at game scale, Direct3D 12 (Metal Shader Converter), audio | — | |
| 6. Networking, cryptography, Chromium-class programs (the Steam client) | — | |

## Known risks (research items)

- **TSD slots 0x30 and 0x58.** macOS uses some low TSD slots itself (0x30 is the pointer-munge key on recent
  systems). Wine relied on this layout for years, but it needs checking against setjmp/longjmp and libdispatch.
- **`gs:[0x10]` and other TEB fields read through `gs` directly** (MSVC's `__chkstk` reads the stack limit at
  `gs:0x10`, which is macOS's MIG reply slot). Such code will need patching at load time.
- **Exceptions.** Windows programs rely on SEH and C++ exceptions unwinding through `.pdata`; macOS signals have
  to be turned into Windows exception dispatch.
- **Scale.** The Windows API is thousands of functions; the order of work follows what real programs import.
