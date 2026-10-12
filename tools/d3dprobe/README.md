# d3dprobe

Which Direct3D runtime answers inside a Wine prefix, and does it work end to end: lists the DXGI adapters, creates
a D3D11 device, clears a texture and reads a pixel back, then creates a D3D12 device, queue and fence.

```bash
x86_64-w64-mingw32-g++ -O1 -static -o d3dprobe.exe d3dprobe.cpp -ld3d11 -ldxgi -luuid
WINEPREFIX=... wine d3dprobe.exe
```

Status 2026-10-12, Apple's D3DMetal (Game Porting Toolkit 3.0) on Wine 11 with `scripts/wine_unixcall_bridge.py`:
the toolkit's DLLs load (the bridge supplies Wine 7's `ntdll.__wine_unix_call`), then the first D3D call crashes.
D3DMetal runs its macOS code directly on Windows threads, which works on Wine 7.7 / CrossOver, where a thread's
`gs` base stays the macOS thread data, but not on upstream Wine 11, which switches `gs` to the TEB while Windows
code runs (`__thread_set_tsd_base`).
