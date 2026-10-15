# VXRuntime's own x86-64 → ARM64 translator (in progress)

## Why

VXRuntime is a macOS x86-64 program: Rosetta 2 translates it and the Windows game together. Apple has said macOS 27 is
the last release with full Rosetta 2, and that later releases keep only a subset for older games
([AppleInsider](https://appleinsider.com/articles/25/06/10/macos-27-will-be-the-last-operating-system-to-fully-support-rosetta-2));
Apple's own pages disagree on the exact release
([Mac Observer](https://www.macobserver.com/news/apple-rosetta-end-date-contradiction-macos-26-27-28/)). Whether
that subset would run VXRuntime is unknown. This translator removes the dependency: VXRuntime becomes a native ARM64
program and translates only the game's code.

The aim is no loss against Rosetta, and better where VXRuntime knows more than Rosetta can: VXRuntime owns the
Windows API boundary, the loader and every PE's exception tables.

## What Rosetta does (from public analyses, not Apple documentation)

- Translates ahead of time (AOT) when a binary is first run, caches the result, and translates at run time (JIT) only
  code the AOT pass could not see (generated or self-modifying code).
- Runs translated code with the CPU in TSO mode (Apple silicon's hardware x86 memory ordering), so ordinary loads and
  stores need no barriers.
- Maps x86 registers to fixed ARM64 registers, keeps x86 flags in NZCV where they match, and uses the flag-handling
  instructions Apple helped add to ARMv8.4/8.5 (FlagM: CFINV, RMIF, SETF8/16; FlagM2: AXFLAG, XAFLAG) for the rest;
  parity and adjust flags are computed only when read.
- SSE maps to NEON nearly one to one; x87 is emulated; AVX/AVX2 are supported since macOS 15.

## Phase 0 measurements (2026-10-15, MacBook Pro M1 Max, macOS 26)

**What the games contain** (`llvm-objdump` over every x86-64 PE; int3 padding left out):

| | instructions | SSE | AVX | x87 | atomics / fences | system (cpuid, rdtsc, int) |
|---|---|---|---|---|---|---|
| Half-Life: Alyx (166 PEs) | 57.9 M | 32.5 % | 3.81 % | 0.028 % | 0.11 % | 5.5 k |
| SteamVR (86 PEs) | 13.2 M | 32.5 % | 0.21 % | 0.029 % | 0.21 % | 1.6 k |
| Google Earth VR (4 PEs) | 4.2 M | 35.7 % | 0.01 % | 0.007 % | 0.46 % | 127 |
| The Lab (13 PEs) | 7.9 M | 39.2 % | 2.37 % | 0.044 % | 0.09 % | 601 |

So: integer and SSE are the bulk; AVX (mostly 256-bit `vmovups`, `vmulps`, FMA, `vbroadcastss`) matters for Alyx and
The Lab; x87 is a rounding error and can be slow but must be exact.

**The CPU** (`sysctl hw.optional.arm`): FlagM, FlagM2, LSE, LSE2, LRCPC, LRCPC2, JSCVT, DotProd, FHM present; AFP
(x86-like floating-point flush/NaN behaviour), BF16 and RPRES absent. Host pages are 16 KB; x86 programs expect 4 KB.

**TSO outside Rosetta.** No public API puts an ordinary process in TSO mode. Hypervisor.framework does, for a virtual
CPU: since macOS 15 `HV_SYS_REG_ACTLR_EL1` exposes the EnTSO bit. Measured with an ad-hoc-signed probe (entitlement
`com.apple.security.hypervisor`): EnTSO sets (ACTLR_EL1 0 → 0x2), and a guest → host → guest round trip (HVC) costs
**856 ns**. A guest can also use 4 KB pages of its own over the host's 16 KB ones.

## Design

- **VXRuntime native.** The runtime (Windows API, Direct3D 11 on Metal, audio, Steam bridge) is built for ARM64;
  only game and SteamVR code is translated. Under Rosetta today, the runtime is translated too.
- **Discovery from the PE, not guesswork.** Every x86-64 PE lists its functions with their unwind information
  (`.pdata`/`.xdata`); exports, relocations and the import table give the rest. Functions are translated whole,
  ahead of time, into a cache keyed by the PE's hash; code found only at run time (Mono's JIT in Unity games,
  self-modifying code) is translated on first execution and invalidated when its page is written.
- **Calls into VXRuntime are direct.** A translated `call [IAT]` becomes a direct call of the native function through
  a thunk generated at build time from the function's C signature (integer vs. floating-point arguments, stack
  arguments): no dispatcher, no run-time marshalling.
- **Where translated code runs: decided by measurement (phase 4).**
  - *In-process*: ordinary threads, 16 KB pages, no TSO: memory ordering comes from LRCPC (LDAPR) / STLR on accesses
    that can be shared, with analysis to leave stack and thread-local accesses plain.
  - *In a virtual CPU* (Hypervisor.framework, one vCPU per game thread): hardware TSO and 4 KB guest pages, as
    Rosetta has them; leaving the guest costs 856 ns, so calls that need no answer (Direct3D 11 immediate-context
    calls, audio submits) go into rings the native side drains, and small functions (memory and string routines,
    TLS, uncontended critical sections, interlocked operations, timers) run inside the guest.
- **Beyond Rosetta.** Flags kept only where a later instruction reads them, across whole functions (Rosetta works per
  block); hot functions re-translated with what harness runs observed (profile-guided); API semantics known
  (`memcpy`, `QueryPerformanceCounter`, ...) used directly.

## Verification

- **Decoder vs. LLVM.** `build/x86dec_check` decodes every instruction `llvm-objdump` lists and compares lengths;
  `build/x86dec_test` (in `make check`) holds hand-checked cases. Over all 372 x86-64 PEs of the four apps
  (2026-10-15): 109,193,188 instructions, 101 differ, all of them data inside code sections that LLVM reads as
  APX/REX2 instructions (`d5` after a prefix; no Apple-era x86 code uses APX) plus `66 E9` (`jmp` with an operand-size
  prefix, which LLVM decodes as AMD does, rel16, and the decoder as Intel does, rel32).
- **Translation vs. Rosetta: phase 2a result (2026-10-15).** `build/difftest build/ref_x86 N` (`make
  translator-check`; 200 per form in `make check`): 154 instruction forms (ALU in all six encodings, group 1, TEST,
  MOV/MOVZX/MOVSX/MOVSXD/LEA/XCHG, IMUL in three forms, MUL/IMUL/DIV/IDIV, NOT/NEG/INC/DEC, ROL/ROR/SHL/SHR/SAR by
  1/imm8/CL, PUSH/POP, CLC/STC/CMC, LAHF/SAHF, CMOVcc, SETcc, BSWAP, BSF/BSR, TZCNT/LZCNT/POPCNT, BT/BTS/BTR/BTC,
  XADD, CMPXCHG) with 8/16/32/64-bit operands, REX and high-byte registers, register and memory operands: 4 seeds
  x 3000 cases per form, 1,848,000 cases, all registers, defined flags, memory and #DE faults identical. Where
  Rosetta 2 differs from a plain reading of the SDM the translator follows Rosetta: a 32-bit shift or rotate by 0
  still clears the upper half; MOVSXD sign-extends to 64 bits with any operand size; CMPXCHG sets flags as
  destination - accumulator and clears a 32-bit accumulator's (and, when unequal, register destination's) upper half.
- **Translation vs. Rosetta, while Rosetta exists.** Every instruction form the census finds is run with random
  register, flag and memory inputs twice: natively on x86 under Rosetta (today's VXRuntime) and translated, and the
  results (registers, flags, memory, exceptions) must match. Keep a Mac on macOS 26/27 for this.
- **Games with the harness.** `make -C VXRuntime vr-check` and the levels (AGENTS.md) with the translator instead of
  Rosetta; fps and CPU time compared run for run.

## Phases

| Phase | Done when |
|---|---|
| 0 Measure | census, CPU features, TSO and exit cost (above) — done |
| 1 Decoder | lengths of every instruction in the four apps match LLVM (except data LLVM reads as APX) — done |
| 2 Integer translator + differential tests | integer and flag instructions match Rosetta on random inputs (2a, done: 154 forms); control flow, blocks, string instructions, RCL/RCR, SHLD/SHRD, atomics with LSE (2b); small PE tests run translated in-process |
| 3 SSE, AVX, x87 | every SIMD/FP form in the census matches Rosetta (MXCSR rounding, denormals, NaNs) |
| 4 Runtime on ARM64 + execution environment | `make check` passes translated; in-process vs. vCPU measured on SteamVR and a game |
| 5 Games | SteamVR, The Lab (Mono JIT), Earth, Alyx at level `controls` with fps at least Rosetta's |

Machine code written at run time is allowed here, and only here: the translator's code emitter (AOT cache and JIT).
Everything else in VXRuntime keeps the rule in AGENTS.md.
