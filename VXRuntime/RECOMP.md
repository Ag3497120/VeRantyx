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

## visionOS: what the modules need there

- **No fallback.** Without JIT, every address has to be in a module (`VXRT_AOT_STRICT=1`), or an interpreter has to
  run what is not.
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
