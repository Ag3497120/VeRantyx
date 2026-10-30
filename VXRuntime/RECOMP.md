# Static recompilation (AOT) of x86-64 PE code for the ARM64 runtime

Goal: run a program's x86-64 code on the ARM64 runtime (`build/vxrt-arm64`) without Rosetta and without translating
at run time, as visionOS will require (no JIT there). The feasibility study is in
`~/Verantyx_VR_Drive/wt/runs/recomp/feasibility_ja.md`.

## How it works (route B)

- `build/vxrt-aot` (`tools/vxrt_aot.c`) is a native ARM64 tool built from the translator's own sources. It maps the
  PE at its preferred base, as the runtime does.
- **Stage 0: `vxrt-aot analyze <pe> [--addresses <list>]`.** Finds the code by recursive descent with the
  translator's decoder. Nothing is translated: the output is statistics only. Roots:
  - the entry point, exports and TLS callbacks;
  - `.pdata` and the exception handlers;
  - `__C_specific_handler` scope tables (filters, `__except` targets, `__finally`);
  - `__CxxFrameHandler3` funclets and GCC landing pads (`__gxx_personality_seh0` LSDA);
  - relocated pointers into code, RVA-shaped references to aligned code, `lea [rip]` and `mov r64, imm64` into code;
  - an address list such as CDPR's `cyberpunk2077_addresses.json`.

  Jump tables of the clang/GCC and MSVC shapes are recovered, including a base LEA hoisted above the loop. The
  bound comes from `cmp idx, n` when there is one; the entries are candidates only.
- **Route B: `vxrt-aot gen <pe> <out.s>`.** Translates every block it finds with `xlat_block_linked`, exactly as the
  engine would (ordered mode, lazy flags, no FEAT_AFP), and writes it out as assembly.
  - The engine's sites become direct branches to blocks or to the module's own stubs: enter, exit, lookup (a
    static open-addressed table), the misses, and the flags stub.
  - Host addresses (`xlat_helper`, the split lock, the parity table) are `XR_ABS` sites. These become loads from
    the module's `ext` table, which `xa_load` fills.
  - `clang -arch arm64 -dynamiclib` assembles the output into `<name>-<fnv64 of the file>.dylib`
    (`vxrt-aot name <pe>` prints this name).
- **Runtime (`translator/aot.c`).** The loader loads the module from `VXRT_AOT_DIR` when the file hash and the base
  match. `vx_x86_call` then runs `xa_run`, which handles each address as follows:
  - a module block: entered through the module's prologue;
  - a native function: its thunk is called from C, as the engine's native blocks call it;
  - anything else: the engine translates it. This fallback is counted and logged. With `VXRT_AOT_STRICT=1` it is
    an error instead.

  The engine returns `XE_FOREIGN` when it reaches a module's address. Faults in module code are mapped back to x86
  instructions through the module's imap, so SEH works. LDAPR/STLR accesses that cross 16 bytes are emulated in the
  signal handler (`xa_fix_split`).

## Route A (x86 → C → clang)

Not built. Route B was chosen because its code is the translator's, which is already checked against Rosetta.

- On the stage-1 benchmarks, route B executed the same instructions as the JIT: 5.65 G against 5.63 G retired, at
  2.16 G against 2.04–2.18 G cycles.
- A C lifter would need its own verification. It also cannot deliver hardware exceptions at the faulting
  instruction, because x86 registers live in host registers.
- C remains an option for hot functions later (PGO/LTO).

## Stage 1

`make aot-suite` builds `tests/recomp/suite.cpp`, recompiles it and runs the result strictly. The suite is built for
the MSVC ABI: clang `-target x86_64-pc-windows-msvc` with MinGW's ld, and no C runtime. It covers:

- jump tables;
- SEH: an access violation, division by zero, `RaiseException`, `__finally` and nested handlers;
- C++ exceptions;
- threads with atomics and a message-passing litmus test;
- TLS callbacks and `__declspec(thread)`;
- SSE and AVX2/FMA paths chosen by CPUID;
- a D3D11 clear and a triangle, both read back.

Result on 2026-10-29 (M1 Max):

- 1295 blocks, generated in 0.02 s and assembled in 0.2 s.
- Output identical to Rosetta's (`ROSETTA_ADVERTISE_AVX=1`).
- 0 fallbacks to the translator.

## Stage 2 (Cyberpunk 2077)

- **Parts.** `vxrt-aot gen` splits an image into parts of `VXRT_AOT_PART_MB` (default 96) MB of code, each a dylib
  `<name>-<hash>.p<k>.dylib` for a range of x86 addresses. A branch to another part's block goes through a veneer
  that loads the block's host address from a slot; `xa_link` fills the slots once every part is loaded. Each part's
  lookup hands an address in another part's range to that part's lookup. `make aot-suite` also runs the suite split
  into 11 parts.
- **Assembly.** Words are written as `.inst` runs of 8. The lookup table is a symbol, because ADRP to an `L` label
  becomes section + addend, and Mach-O addends stop at 8 MB. `tools/aot_build.sh <pe> [dir]` generates and assembles
  the parts in parallel; the objects stay in `build/aot_s`.
- **Handlers by shape.** A statically linked CRT has no imported handler names. Each unnamed handler takes the kind
  (C scope table, FH4, FH3) whose parse fits 90% of its functions. FuncInfo4 is read in full: unwind actions, catch
  funclets, and continuations (RVAs or offsets from the function).
- **DLL bases.** DLLs prefer 0x180000000, where macOS on arm64 keeps the shared cache, so a module for the preferred
  base could never be used. With `VXRT_AOT_BASE=<hex>`, `vxrt-aot` maps and relocates the image there. The runtime
  reads the module's base before it maps the file and reserves that range exactly (`mach_vm_allocate` fixed). A
  process built for macOS 14 has its address space end near 0x7c0000000.

Cyberpunk2077.exe (2.3, 66.7 MB):

- 2 808 247 blocks and 275 MB of ARM64 code, in 4 parts.
- Generation takes 15–57 s and 8.3 GB of memory; assembly with 4 jobs takes 5.5 min.
- 506 blocks were refused by the translator.
- The dylibs total 730 MB. The imap, 16 bytes per x86 instruction, is 190 MB of that.

### Runs (M1 Max, 2026-10-30; Low + FSR2 Performance, NPU race on with the game's motion vectors)

| Build | Main menu | Mirror scene | El Coyote | Fallbacks |
|---|---|---|---|---|
| Rosetta (`build/vxrt`) | 60.8 fps | 16.6 fps | 20.4 fps real, 54–56 shown | — |
| ARM64, JIT (`build/vxrt-arm64`) | 42–46 fps | — | — | — |
| ARM64, modules (first profile) | 46.5 fps | 9.6 fps | 7.5 fps real, 60 shown | 2.6 M (220 addresses) |
| ARM64, modules (second profile) | — | 16.5 fps | 20.8 fps real, 52–55 shown | 132 (18 addresses) |

- **Split profile.** The first-profile run spent its time in 58 M signals. These come from LDAPR / STLR accesses
  that cross 16 bytes, which a module cannot patch the way the engine does. After two runs'
  `VXRT_AOT_SPLITS` (3 245 instructions) were fed back, 0.27 M signals were left.
- **Coverage.** The modules cover 31 images: the exe and 30 DLLs, including Steam's, XeSS, Chroma, Aftermath and
  FidelityFX. `VXRT_AOT_MISSES` from a run, given back as `VXRT_AOT_ROOTS`, closes the gaps.
- **Fallbacks left.** 18 addresses: the exe (7), PhysX (4), REDGalaxy (4), tier0 (2) and the overlay (1). All were
  the MSVC CRT's AVX memcpy / memset tails, reached through jump tables of two shapes the analysis now reads: an
  image-relative table through one register (`mov r11d, [r10 + r11*4 + rva]; add r11, r10`, r10 the image base,
  whose LEA is not found on the way back) and the entry loaded into the ADD's other register (`mov eax,
  [r9 + r8*4 + rva]; add r9, rax; jmp r9`). With them, all 238 misses recorded over the runs are found without
  `VXRT_AOT_ROOTS` (`VXRT_AOT_CHECK=<misses>` on `analyze` lists any that are not); before, 98 were not.
- **Native calls.** About 5 M a second each leave the module and enter it again; a thread's cache keeps their cost
  down. In game the result matches Rosetta, because El Coyote is GPU-bound at about 48 ms a frame.
- **Footprint.** 10–11 GB, the same as Rosetta's. The modules' `__TEXT` is 460 MB, clean and file-backed.

### Under the device's constraints (avp-preflight, 2026-10-30; M1 Max, NPU race on)

Modules from static analysis only (no recorded roots; the splits profile kept), `VXRT_AOT_STRICT=1`, ARM64 build,
MetalFX off (FSR2's own path, as visionOS has no temporal scaler), `VXRT_MEM_LIMITS=6144,8192`:

| Settings | Fallbacks | Main menu | Load after character creation | Mirror | El Coyote |
|---|---|---|---|---|---|
| Low, textures High | 0 (4.44 G entries) | 4.8–5.3 GB | 6 GB at 248 s, 8 GB at 252 s | 10.9 GB, 17.6 fps | 11 GB (peak 11.1), 23.5 fps real, 48 shown |
| + textures Low, `TEX_DOWNSCALE=2` (512), `VXRT_GPU_BUDGET_MB=4096`, `VXRT_DEVICE_MEMORY_MB=16384` | 0 (3.72 G entries) | 4.8 GB | 6 GB at 247 s, 8 GB at 254 s | 9.5 GB, 17.2 fps | 9.45 GB (peak 9.74), 22.9 fps real, 48 shown |

- The Vision Pro gives the app 8188 MB (`os_proc_available_memory` at launch with increased-memory-limit), so both
  configurations would be ended a few seconds into the load after character creation. The main menu fits.
- At El Coyote in the lowest configuration: Metal 3.6 GB (IOAccelerator) + 0.8 GB unmapped graphics, the game's own
  `VM_ALLOCATE` heap 3.4 GB (the same with 16 GB reported: it does not size itself by the memory reported), malloc
  1.5 GB. About 1.5 GB has to go, and the load's spike with it.

### Memory under the device's limit (avp-mem, 2026-10-30; M1 Max, NPU race on, AOT strict, 0 fallbacks)

Lowest settings (textures Low, FSR2 Ultra Performance, 1512x982; m4-m7 also crowd density Low), `TEX_DOWNSCALE=2`
(512), `VXRT_MEM_LIMITS=7680,8192`. Footprint in MB; the load peak is the highest one the presents saw from the main
menu to El Coyote (`runs/avp_mem/tools/table.py`).

| Run | GPU budget | Our memory changes | Load peak | 7.5 / 8 GB crossed | Mirror | El Coyote | Metal + unmapped graphics | untagged | malloc | Real / shown fps |
|---|---|---|---|---|---|---|---|---|---|---|
| m1 | 4096 | none | 10053 | 127 s / 130 s | 9655 | 9690 | 3711 + 803 | 3482 | 1498 | 12.4 / 47.6 |
| m2 | 2048 | none | 8933 | 140 s / 163 s | 8426 | 8700 | 2805 + 870 | 3455 | 1426 | 8.5 / 53.9 |
| m3 | 2048 | `EVICT_VOLATILE` | 8597 | 270 s / 505 s | 8417 | 8511 | 2894 + 844 | 3193 | 1425 | 9.3 / 48.8 |
| m4 | 3072 | image cache | 8448 | 261 s / 506 s | 8271 | 8356 | 2920 + 814 | 3139 | 1295 | 6.9 / 48.7 |
| m5 | 2048 | image cache, 8 GB reported | 8363 | 502 s / 554 s | 8161 | 8258 | 2774 + 858 | 3174 | 1274 | 7.8 / 46.8 |
| m6 | 2048 | image cache, file buffers | 7012 | none | 6858 | 6917 | 1456 + 859 | 3127 | 1284 | 8.4 / 49.4 |
| m7 | 4096 | image cache, file buffers, page file | 5228 | none | 5088 | 4688 | 2238 + 771 | 44 | 1430 | 15.7 / 44.1 |

- **The budget.** Under 4 GB the game evicts and makes resident every frame (m5: 9.9 M objects in 97 K calls) and
  keeps about 11 600 upload buffers (950 MB, 1.1 GB of them evicted at a time): about 1 GB less GPU memory than
  4096, at 60 % of the frame rate. With the CPU-visible memory on files (m7) 4096 is the better setting.
- **`VXRT_D3D12_EVICT_VOLATILE`** (purgeable while evicted): the system took 4034 evicted buffers in one session
  (their contents lost), and the footprint hardly moved. Off by default; not for use.
- **Committed and never written** is small (157-237 MB of the program's 3.3 GB): the game's pools (0x7400000000
  64 GB reserved, 1.6 GB committed; 0x8400000000; 0x7000000000) are written. Reporting 8 GB of memory instead of 16
  (m5) does not shrink them; neither does crowd density Low (-40 MB).
- **Image copies** (`VXRT_IMAGE_CACHE=<dir>`): the exe and 30 DLLs were 300 MB of dirty anonymous memory; mapped from
  relocated copies on disk, 24 MB of them stay dirty (written pages), the rest is clean and file-backed.
- **File buffers** (`VXRT_D3D12_FILE_BUFFERS=<dir>`): committed buffers of 64 KB or more (all CPU-visible here) on
  the pages of a removed file, mapped shared: 1.5-1.7 GB out of the footprint, contents kept.
- **Page file** (`VXRT_PAGEFILE=<dir>`, ARM64): the program's reservations of 64 MB or more on the pages of a removed
  file, as Windows backs committed memory with its page file: the untagged 3.1 GB goes to 44 MB. Decommit, MEM_RESET
  and release punch the range out of the file (`F_PUNCHHOLE`: its pages read as zeros).
- **Disk.** With all three, the disk saw 23 MB/s on average over the session (reads and writes, the game's own
  streaming included), 213 MB/s at most (iostat, 10 s samples). Dirty file pages are written back when the system
  needs the memory or syncs.
- **Not yet on the device.** That visionOS leaves dirty pages of a shared file mapping out of the footprint jetsam
  counts, and writes them back instead of ending the app, is measured here on macOS only; a test program in the VX
  Runtime app has to show it there (and the SSD writes it costs).
- **Left in the footprint (m7, 4.7 GB):** GPU 3.0 GB (game heaps 1.5 GB, committed textures, our 0.3 GB), malloc
  1.4 GB (the game's HeapAlloc mostly; ours: recorded commands 33-135 MB, gpu_ir streams 29-55 MB, descriptor
  bookkeeping 104 MB). A load-time spike of 135 MB of recorded commands was seen at the 8 GB crossing in m1.

## visionOS: what the modules need there

- **No fallback.** Without JIT, every address has to be in a module (`VXRT_AOT_STRICT=1`), or an interpreter has to
  run what is not. The profile loop (misses → roots) is how coverage gets there.
- **Ordering.** No TSO there, as here: the ordered accesses (LDAPR / STLR) and the split profile carry over as they
  are. Any split the profile missed is still a signal, which works on visionOS but is slow.
- **Arena.** The arena's fixed address (0xf00000000) has to be reserved before the system's randomized regions fall.
  Here the process starts again when it is covered; an app can reserve it in its first initializer instead.
- **Mac-only today.** Steam's DLLs and the Windows Steam client are not part of the visionOS app. `dlopen` from a
  hash-named directory, `_NSGetExecutablePath` with `execv` for the restart, and the `vxrt-npu` helper process
  (`posix_spawn`, not allowed on visionOS) all have to change.
- **Loading.** The modules are dylibs found by `dlopen` on a hash-named path. An app links them, or embeds them as
  signed frameworks. `vxrt_aot_module` would need a unique name per module and a registry.
- **Address space.** The DLLs' fixed bases and the 0x140000000 exe base have to fit in the app's address space,
  which may need the extended-virtual-addressing entitlement. `__PAGEZERO` is 4 GB, so images based below 4 GB
  must be relocated (they are, with `VXRT_AOT_BASE`).
- **Memory.** The modules' pages are clean and file-backed. The imap is only read on faults, so it could move to a
  compressed side file.

## Known limits

- In the generated code, writes to code pages are not watched (no SMC).
- `__CxxFrameHandler4` separated code (`isSeparated`) uses another IP-to-state layout, which is not read. Its
  funclets are still roots.
