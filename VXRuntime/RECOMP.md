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

## Known limits

- Modules are made for the image's preferred base. A relocated image does not use its module.
- Branches within a module must stay within ±128 MB. Cyberpunk's executable would exceed this and needs splitting.
- `__CxxFrameHandler4` continuation addresses are not read. Handlers linked statically, as in Cyberpunk's CRT, are
  not recognized by name.
- In the generated code, writes to code pages are not watched (no SMC).
