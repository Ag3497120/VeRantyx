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
- **One process, two instruction sets (later; docs/VISION.md).** Translated x86-64 and native ARM64 code are meant
  to call each other, as Arm64EC does on Windows. A C type alone does not bridge everything: the mixed ABI starts at
  C function boundaries (integer, pointer, floating-point and struct arguments by the Windows x64 rules) and adds
  callbacks, exceptions and unwinding across the boundary, variadic functions and TLS layout one at a time, each
  with its own tests. The register convention of translated code (x86 registers in x0-x15, flags in x27, the context
  in x28) is the boundary's contract and is kept stable from phase 2 on.
- **Calls into VXRuntime are direct.** A translated `call [IAT]` becomes a direct call of the native function through
  a thunk generated at build time from the function's C signature (integer vs. floating-point arguments, stack
  arguments): no dispatcher, no run-time marshalling.
- **Where translated code runs: decided by measurement (phase 4).**
  - *In-process*: ordinary threads, 16 KB pages, no TSO: memory ordering comes from LRCPC (LDAPR) / STLR on accesses
    that can be shared, with analysis to leave stack and thread-local accesses plain.
    Measured (M1 Max, 2026-10-15): a loop of two loads and a store costs 1.5-1.7x with LDAPR/STLR as with plain
    LDR/STR (0.92-1.04 ns vs. 1.55-1.57 ns per iteration); what that means for whole programs depends on how many
    accesses can stay plain (stack, thread-local) and is measured on game code next.
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
- **Phase 2b result (2026-10-15).** 70 more forms (RCL/RCR, SHLD/SHRD, PUSH/POP r/m, LEAVE, MOV moffs, XLAT,
  MOVS/CMPS/STOS/LODS/SCAS with REP/REPE/REPNE and DF, LOCK on every read-modify-write: CASAL with a retry, SWPAL
  for XCHG, CASAL for CMPXCHG, one shared lock for misaligned locked accesses) and **random programs**: 2-7 blocks
  with Jcc rel8/rel32, JMP rel/indirect, CALL rel/indirect and RET, LOOP/LOOPcc/JRCXZ and counted loops, placed at
  the same address in both processes (ref_x86 jumps into them; difftest translates block by block, `xlat_block`,
  through a dispatcher). 5 seeds x (224 forms x 3000 + 12,000 programs): 3,420,000 cases, 60,000 programs, all
  identical. Rosetta 2 deviates from x86 once: a 16-bit LOCK NEG across a 16-byte boundary sets the flags but does
  not store; those cases are skipped and the translator stores.
- **Phase 3a result (2026-10-15).** XMM0-15 live in V0-V15. 128 SSE/SSE2 forms: moves (aligned, unaligned,
  scalar, low/high halves, MOVD/MOVQ), ADD/SUB/MUL/DIV/SQRT/MIN/MAX in ps/pd/ss/sd, logic, shuffles and unpacks,
  CMPxx predicates, (U)COMISS/SD, conversions (CVTSI2SS/SD, CVT(T)SS/SD2SI, CVTPS2PD, CVTPD2PS, CVTSS2SD,
  CVTSD2SS, CVTDQ2PS, CVT(T)PS2DQ, CVTDQ2PD), MOVMSKPS/PD, PSHUFD/LW/HW, integer adds, subtracts, logic,
  compares, unpacks, PMADDWD, PMULLW, shifts by immediate. Inputs mix zeros, infinities, quiet and signalling NaNs
  with payloads, denormals and the integer conversion limits. x86 floating-point semantics kept on ARM: NaN
  propagation and the negative "indefinite" NaN (one compare on the result; the lanes are rebuilt only when a NaN
  appears), MIN/MAX as compare-and-select, 0x80000000 for invalid conversions. 5 seeds x (354 forms x 3000 + 12,000
  programs): 5,355,000 cases, all identical. Rosetta 2 detail matched: without VEX, CMPSS/CMPSD take imm & 15 and
  CMPPS/CMPPD imm & 7 (the SDM reserves everything above 7). Not yet: MXCSR exception flags and rounding modes
  (LDMXCSR is not translated). `translator/a64_check.py` (make check) checks every ARM64 encoder against the
  assembler (124).
- **Phase 3b result (2026-10-15).** AVX, AVX2, FMA, SSE3, SSSE3, SSE4.1, AES, PCLMULQDQ and the rest of SSE2: 785
  forms in all (with 3a's). YMM upper halves live in the thread's context (16 registers would not fit next to V0-V15
  and the scratch registers): a 256-bit operation loads the halves it reads and stores its result's, a VEX instruction
  with a 128-bit result clears its destination's (two stores). Most VEX forms are the SSE translation run once per
  half on registers chosen for it (VEX.vvvv as a separate first source), so a form checked at 128 bits is the same code
  at 256. Rosetta 2 runs AVX, AVX2 and FMA (its CPUID says so only with `ROSETTA_ADVERTISE_AVX=1`), so it is the
  reference here too; its state now includes the upper halves (VINSERTF128 / VEXTRACTF128 in ref_x86).
  5 seeds x (773 forms x 3000 + 12,000 programs) plus 5 x 12 AES / PCLMULQDQ forms x 3000: 11,820,000 cases, all
  identical (83,155 of them are Rosetta deviations below, counted instead of compared).
  Where x86 lets processors differ, x86's bound is checked instead of Rosetta's bits: RCPPS / RSQRTPS (and SS) are
  the ARM estimate plus one Newton step (about 2^-16), denormal inputs as zero, tiny reciprocals as zero, the
  indefinite NaN for negative square roots; every lane of both results is checked against the exact value (relative
  error at most 1.5 * 2^-12). Rosetta's own estimates have 12 bits and match neither Intel's nor ours bit for bit.
  Rosetta 2 deviations from x86 found (the translator does what x86 does; the cases are counted, not compared):
  PTEST / VPTEST / VTESTPS/PD leave AF OF PF SF as they were (x86 clears them); HSUBPS/PD and ADDSUBPS/PD change the
  sign of a subtracted NaN (x86 keeps it, as SUBPS does, and Rosetta's SUBPS does too); VPSRLVQ / VPSLLVQ read only
  the low 32 bits of each 64-bit count. FMA's NaN rule is Rosetta's, measured operand by operand: the first NaN of
  multiplicand 1, multiplicand 2, addend (by role, not by the 132/213/231 position; no preference for SNaNs), negated
  where the form negates that operand (the addend of VFMSUB, multiplicand 1 of VFNMADD); the SDM is still to be checked
  for this. Not yet: MXCSR modes and exception flags (LDMXCSR, STMXCSR: 3c), gathers, VCVTPH2PS/PS2PH, PHMINPOSUW,
  MPSADBW, PCMPxSTRx, AESKEYGENASSIST, CMPXCHG16B, BMI1/BMI2, AVX-512 (Rosetta has none). Of the census's 400 most
  common mnemonics, the SIMD ones not translated yet appear 14,428 times in the four titles (AES rounds, CMPXCHG16B,
  MOVNTI, MULX, gathers, AVX-512, LDMXCSR) against about 20 million SIMD instructions.
- **Phase 3c, MXCSR (2026-10-15).** Translated code runs with FPCR made from MXCSR (on entry and at LDMXCSR):
  x86's rounding control is ARM's RMode, FTZ and DAZ together are ARM's FZ. MXCSR's exception flags are FPSR's
  cumulative bits (IOC DZC OFC UFC IXC = IE ZE OE UE PE), read back at STMXCSR and on exit, so keeping them costs no
  instruction in the code that raises them; translations were made flag-exact where they computed more than x86 does
  (scalar forms now use scalar ARM instructions, ADDSUB one addition, DPPS no left-out products, conversions round
  first and convert the value as it was only for the flags, RCPPS/RSQRTPS save and restore FPSR as x86 raises nothing
  there, CMPxx predicates quiet or signalling as x86 defines them). Conversions that round (CVTSS2SI, CVTPS2DQ, ...)
  and ROUNDxx with imm bit 2 follow MXCSR's mode. The differential test now starts each case in one of the four
  rounding modes, FTZ+DAZ, FTZ or DAZ alone, some with flags already set, and compares MXCSR after the instruction:
  3 seeds x 2,376,000 cases (789 forms), all identical. Known differences of ours, counted rather than compared, all in
  rare corners and each a choice for zero cost: DE (denormal operand) is not kept (it would need a check of every
  input); FTZ alone and DAZ alone run as both (ARM's FZ cannot tell them apart without FEAT_AFP, which the M1 lacks), so
  denormal inputs (FTZ alone) or results (DAZ alone) differ (0.16% of the random cases, which favour denormals); ARM
  finds a result tiny before rounding, x86 after (UE, and with FTZ a result that rounds up to exactly 2^-126 becomes 0);
  ARM's FMA raises invalid for 0 * inf + QNaN, x86 does not. Rosetta 2 detail matched: CMPSS/CMPSD without VEX take
  bit 4 of imm too (signalling or quiet, as VEX's 32 predicates). Rosetta 2 deviation found (counted): it raises the
  flags of both the addition and the subtraction in every lane of VFMADDSUB / VFMSUBADD.
- **Phase 3c, x87 (2026-10-15).** x87 instructions run in software (`translator/x87.c`, called as a helper: x87 is 0.03 %
  of the games' instructions, mostly in the C runtime), exact as the SDM describes the unit: 80-bit values with 64-bit
  significands (128-bit intermediates, so every result is rounded once), precision and rounding control, the stack
  with overflow and underflow, all six exception flags and C0-C3, loads and stores of every format (single, double,
  extended, 16/32/64-bit integers, BCD), FPREM/FPREM1 (partial remainders reduce the exponent difference to 32 + d mod
  32, as Rosetta 2 does), FXTRACT, FSCALE, FRNDINT, the constants rounded as RC says, environments, FNSAVE/FRSTOR,
  FXSAVE/FXRSTOR. Unmasked exceptions only set ES and B (#MF is not raised). The differential test loads and saves
  the x87 state through FXRSTOR64/FXSAVE64 in the reference and draws random states (any TOP and tags, zeros,
  infinities, NaNs with payloads, denormals, unnormals, integers, halves, extreme exponents; any PC and RC); 133 x87
  forms plus FXSAVE/FXRSTOR, with all the earlier forms: 3 seeds x 2,805,000 cases (932 forms), identical but for one
  case the test itself misfiled (a DAZ-alone flag difference from a flushed partial result of DPPS: now counted with the
  other FTZ/DAZ-alone cases, and that form rerun). Measured and matched where the SDM leaves a choice: an empty register reads
  as the indefinite NaN and the instruction goes on with it (NaN rules: unsupported formats first, then a QNaN over an
  SNaN, then the larger significand); a single or double SNaN operand stays an SNaN for those rules; DE of a
  denormal single or double comes before divide-by-zero, an extended one's after; FCMOVcc underflows on an empty ST(i)
  whatever the condition; FIST has no DE. Rosetta 2 deviations found (the translator follows x86; counted): loads
  (FLD, FILD, FLD1 ...) keep C1 where the SDM clears it; FBLD does not check for a stack overflow; the undocumented
  aliases (FSTP1, FCOM2, FXCH4, FFREEP, ...) raise SIGILL; after FXRSTOR every register in use is tagged "zero" in
  FNSTENV's tag word; a popped register holds the indefinite NaN (its content means nothing either way); a denormal divisor or an
  exponent difference of exactly 63 gives FPREM/FPREM1 results larger than the divisor; FSQRT of an exact square is
  1 ulp off (and inexact) when rounding up or down.
  The transcendental instructions (FSIN, FCOS, FSINCOS, FPTAN, FPATAN, F2XM1, FYL2X, FYL2XP1) compute with 128-bit
  significands and round once, so their results are correctly rounded (x86 only promises about 1 ulp, and processors
  differ); FSIN, FCOS, FSINCOS and FPTAN reduce their argument by Intel's 66-bit pi exactly, as x86 processors do (and
  Rosetta 2: with that reduction, its results are within 1-2 ulps of the exact ones). `make x87-exact` compares both
  with exact values (`translator/diff/x87_exact.py`, decimal arithmetic) in every rounding mode: VXRuntime's are all
  correctly rounded (400-800 per instruction and mode; earlier runs 1,500-3,000 each); Rosetta's 50-99 %, off by 1-2
  ulps otherwise, and FYL2XP1's mostly far off (it loses the precision this instruction exists for). In the
  differential test the transcendentals' values count as equal within 1 ulp of Rosetta's (2 when rounding up or
  down) with C1 left out; everything else is exact, and F2XM1 outside [-1, 1] (undefined on x86) is not compared.
  More Rosetta 2 deviations found there: F2XM1 and FPATAN raise no PE (nor UE) for small arguments, though the
  results are inexact.
- **Remaining instructions and coverage (2026-10-15).** Added: CMPXCHG8B/16B (CASAL / CASPAL; 8 bytes across 16
  under the split lock; 16 bytes must be aligned), BMI1/BMI2 (ANDN BEXTR BLSR BLSMSK BLSI BZHI PDEP PEXT MULX SHLX SHRX
  SARX RORX), AVX2 gathers (8 forms, in C: elements in order, the mask cleared), F16C (VCVTPH2PS, VCVTPS2PH with its
  own rounding mode), SSE4.2's PCMPESTRI/ESTRM/ISTRI/ISTRM (in C, as the SDM's algorithm; an independent model of it
  in Python agreed with Rosetta on every case where ours first did not), CRC32, MOVBE, AESKEYGENASSIST, PUSHFQ/POPFQ,
  MOVNTI, RDRAND/RDSEED (the system's random numbers), ADCX/ADOX: 978 forms in all, 3 seeds x 2,943,000 cases,
  identical (one F2XM1 case where Rosetta 2 also omits UE: counted with its other F2XM1 flag deviations, and that form
  rerun). Rosetta 2 lacks ADCX/ADOX and RDSEED (SIGILL; its
  CPUID does not report them either): those are checked against a model of x86 instead. Rosetta 2 deviation found:
  VCVTPS2PH swaps rounding up and down when imm bits 7-3 (to be ignored) are set. `make coverage`
  (`translator/coverage.c`, `.py`) translates every function the games' PEs list in .pdata, instruction by
  instruction: of 221,978,344 instructions in 765 files (Half-Life: Alyx, SteamVR, Google Earth VR), 99.22 % translate;
  of the rest, 214,847 do not decode (data inside functions), 673,466 are AVX-512 (EVEX and opmask instructions:
  Rosetta 2 has none and reports none, so programs never choose those paths), 780,366 are INT3, UD2, INT n and port
  I/O (exceptions: phase 4), and the other 69,061 are mostly data decoded as code (IN/OUT, CLI, IRET, segment moves,
  16,491 LLVM cannot name either), with CPUID, RDTSC, XGETBV (what to report is the runtime's: phase 4) and SHA
  (not reported by Rosetta's CPUID, nor will be by ours) as the real ones.
- **Phase 4a, the execution engine (2026-10-15).** `translator/engine.c` runs x86-64 code block by block from a
  translation cache (64 MB MAP_JIT), with the x86 state in ARM registers from block to block: one stub enters
  (prologue, then the block), one leaves (epilogue). A direct branch is first a BL to a link stub that leaves to C
  with the site; C translates the target and patches the site into a B to it. Indirect branches (JMP/CALL r/m, RET on a
  miss) read a direct-mapped table (2^16 entries) in the lookup stub. CALL keeps the x86 return address and the
  translated code after it in a ring of 256 entries (X29) and reaches its target with BL; RET returns with ARM's RET
  when the ring's last entry is its address, so the host's return prediction works for x86 calls. Traps (INT3, INT n,
  UD2, #DE, #GP, INT 29h's RCX) leave with `ctx->fault` and RIP at the instruction. CPUID reports what Rosetta 2
  reports with AVX advertised (`cpuid_table.h`, made by `translator/tools/cpuid_dump.c` under Rosetta); RDTSC/RDTSCP count at 1 GHz
  (CNTVCT scaled); XGETBV(0) = 7. In-process memory ordering (`xe_new(1)`): accesses that may be shared (not RSP- or
  RBP-based) are LDAPR/STLR; one across 16 bytes faults (LSE2 allows the others) and its instruction is patched into
  a branch to DMB, plain access, DMB (FEX-Emu does the same).
  What makes the code fast, each checked by mutation (a deliberate bug in each must make the tests fail):
  flags per flag (CF PF AF ZF SF OF) only where read, across the block and into its successors (looked at where they
  are mapped, through JMP and CALL; the engine's return address counts as reading all); a CMP / TEST / SUB / AND ...
  and its Jcc as one ARM compare and B.cond (also with flag-free, memory-free instructions between: the condition
  waits in X25); register operations, LEA, MOV/MOVZX/MOVSX, PUSH/POP (pre/post-index), shifts and INC/DEC on the
  registers themselves when their flags are dead; displacements in the loads and stores; the NaN fix of SSE
  arithmetic after the block; scalar SSE straight into the destination when its upper lanes are known zero (tracked
  through the block from MOVSS/MOVSD loads). Not done: flags in NZCV (Rosetta's way), RET treating flags as dead (the
  ABI allows it, hand-written code may not), register allocation across blocks.
  Benchmark (`make xlat-bench`: eight C kernels built once for x86-64; best of 10, ms, M1 Max): engine (ordered) /
  Rosetta 2 / native ARM64: crc 2.52 (2.50) / 1.40 / 1.33; matmul 10.1 (10.0) / 4.49 / 5.51; sort 30.2 (32.6) / 20.5 /
  19.8; tree 40.8 (44.6) / 25.6 / 21.6; fib 0.305 (0.304) / 0.124 / 0.138; physics 8.37 (8.97) / 3.00 / 2.12; interp
  36.1 (36.0) / 26.8 / 17.0; string 35.9 (37.4) / 11.5 / 11.0, from 7.67, 35.6, 103.5, 77.5, 0.889, 14.3, 95.5, 99.8 for
  the first working engine: 1.3-3.1x Rosetta's time now; 1.08 ARM instructions per x86 byte (from 3.09). Ordered
  accesses cost 0-9 %. The scalar SSE kernels stay furthest behind: Rosetta runs them at native speed on the M1, which
  has no FEAT_AFP, so it presumably uses a mode of Apple's that x86's NaN and upper-lane rules need.
  Checks: the random programs run on the engine (in both modes) and now also hold CMP/TEST/SUB ... + Jcc with
  instructions between, blocks that start by overwriting the flags, scalar SSE chains on few registers, and special
  floats (zeros, infinities, QNaN, SNaN, denormals) where the loads read. 3 seeds x 988,000 cases (985 forms) and
  6,000 programs each, plus 2 x 8,000 programs in ordered mode (1,800 split accesses patched): identical (one more
  classification: a VFMSUBADD case where Rosetta raises its other operation's IE in one lane and ours a before-rounding
  UE in another, both known). `make coverage`: 99.58 % of the 221,978,344 instructions translate (traps now do).
- **Phase 4b, the runtime as ARM64 code (2026-10-15).** `make build/vxrt-arm64`: the same sources built for ARM64,
  the program's x86-64 code run by the engine in the process (`src/arm64.c`), and `make check-arm64` runs all the
  runtime's tests that way: every one passes (threads, fibers, SuspendThread/GetThreadContext, MinGW and MSVC C++
  exceptions, access violations and divide errors in the program's code, DLLs, files and the overlay, sockets, COM,
  IOCP, windows, Direct3D 11 draws and compute, OpenGL, input, WASAPI, XAudio2, pipes and the four tests between
  processes). The boundary, both ways:
  - *The program calling the runtime.* `tools/vx_abi_scan.py` reads the sources with libclang as the x86 build sees
    them and lists every function with the Windows calling convention (7,982: exports, COM methods, OpenGL
    wrappers); `tools/vx_thunks.py` writes a C thunk for each (generated C, no machine code), included at the end of
    its file: it reads the arguments where x86 code left them (RCX RDX R8 R9 / XMM0-3, then above the home area),
    cut to their C type's width (x64 leaves the bits above a narrow argument undefined), calls the function and puts
    the result in RAX or XMM0; variadic ones get the argument slots as their va_list (the format code already took
    "a pointer to 8-byte slots"). The engine makes a native function's address a block that stores the x86 state,
    calls the thunk and returns like RET. Eight functions that work on the x86 state itself (RtlCaptureContext,
    RaiseException, RtlUnwind(Ex), _CxxThrowException, setjmp/longjmp, __chkstk, SwitchToFiber) have thunks written
    by hand.
  - *The runtime calling the program* (entry points, DllMain, window procedures, thread start routines, callbacks:
    176 calls through ms_abi pointers): x86 code is never mapped executable, so such a call faults on its first
    instruction; the handler sends it to `vx_cb_tramp` (`src/context_arm64.S`), which runs it on the engine with
    x0-x7 as RCX RDX R8 R9 and the stack. No call site changes: every one passes integers or pointers only, and
    `tools/vx_thunks.py` fails the build if one does not. Each such call is a level with a jmp_buf: an exception
    handled in a frame of level L resumes L's engine loop, leaving the native frames above it as the x86 build leaves
    them by moving RSP; the SEH walk (seh.c, unchanged otherwise) goes on over the runtime's frames where the program
    called into it. A fault in translated code finds its x86 instruction from a map of host addresses (recorded per
    instruction) and the state in the registers the translation keeps it in.
  - Each thread has its own x86 stack (the PE's reserve for the main thread) and context; translated code's gs is
    the TEB. The engine is shared by the threads (translation and linking under a lock, table entries written with
    one STP). Found on the way: the loader read the entry point from the file's copy after freeing it (it worked by
    luck under Rosetta; fixed for both builds).
  - *Code that changes* (JIT compilers: V8 in Alyx's UI, Mono, LuaJIT). The host pages (16 KB) translated code came
    from are made read-only; a write there forgets every block made from the page (their entries, and the branches
    linked to them go back to their stubs) and gives the page back; a write from translated code goes on at the
    writing instruction, run alone in a block made for that once, so the code after it is translated as written (a
    store changing the next instruction of its own block works). A page written 16 times after its code ran (code
    and data together) stays writable, and its blocks compare their x86 bytes on entry instead. VirtualFree drops
    the translations of what it frees. `tests/smc.c` (in place, data on the code's page, self-modifying, freed and
    allocated again) passes on both builds.
  - *Memory order*: the ARM64 runtime runs the engine in ordered mode by default (LDAPR / STLR for accesses other
    threads may see, phase 4a; `VXRT_ORDERED=0` turns it off); its fault handler patches the ones across 16 bytes
    first (`xe_fix_split`). The draw benchmark costs the same either way. `VXRT_PROFILE` names the program's
    instructions in translated code (the host pc's x86 instruction).
  - *Measured against the x86 build under Rosetta* (M1 Max): a Direct3D 11 draw costs 0.72 µs of CPU instead of 1.92
    (`build/drawbench.exe`; the runtime's own work is ARM64 code now), an event round trip between processes 7.0 µs
    instead of 8.8. Games: `VXRT_ARM64=1` makes the launch scripts use `build/vxrt-arm64` (not tried on SteamVR or a
    game yet). The Steam bridge works there too, with the Mac client's ARM64 slice (`build/steam.exe`).
  **A limit of this in-process design, measured:** an ARM64 process on macOS cannot map anything below 4 GB (the
  kernel kills an ARM64 binary with a __PAGEZERO under 4 GB, and with the default one the VM map starts there; the
  x86 build gets its low 4 GB with `-pagezero_size 0x10000`). Programs that ask for low memory (LuaJIT 2.0's
  allocator, through NtAllocateVirtualMemory ZeroBits: SteamVR Home's vscript.dll; Half-Life: Alyx's does not import
  it) cannot run this way; under a virtual CPU the guest's address space is ours, so that is where they would go.
- **Phase 4c, translated code in a vCPU: first part (2026-10-15).** `translator/vcpu.c`, `xe_new_vcpu()`: the
  engine's code runs on a Hypervisor.framework vCPU of the process (one per thread) at EL1 with ACTLR_EL1.EnTSO (x86's
  memory order in hardware, so plain loads and stores), page tables of our own (4 KB granule, 48-bit VA; the guest
  sees host memory at the same addresses), and leaves with HVC: #1 the engine's exit, #2 a helper, #3 a native
  function, #0x10+n an exception, which brings a host page in (16 KB, at a guest physical address of its own, up to
  the M5's and M1's 40-bit IPA) when the address is mapped in the process. **Found the hard way:** putting an
  executable host page (the engine's MAP_JIT code) into the VM with hv_vm_map panics macOS 26 ("[SPTM]
  VIOLATION_ILLEGAL_MAPPING_TYPE") instead of failing; the guest's code is now plain read-write memory the host never
  runs, the parity table a copy, and executable host pages are never mapped. Experiments run on a second Mac (a
  MacBook Air, M5) over SSH. Results there (best of 10, ms): in-process / in-process ordered (LDAPR/STLR) / vCPU (TSO)
  / Rosetta / native: sort 20.1 / 22.0 / 20.4 / 15.4 / 15.0, tree 27.5 / 31.1 / 28.0 / 18.1 / 15.6, physics 4.57 /
  4.94 / 4.91 / 1.84 / 1.19, interp 16.1 / 17.2 / 17.5 / 10.6 / 5.37, matmul 6.53 / 6.49 / 6.65 / 2.72 / 4.94: the
  vCPU costs what unordered in-process code does and gives x86's order exactly; same checksums. Random programs in
  the vCPU (`DT_VCPU=1`): 4 seeds, 20,000 programs, identical to Rosetta.
  **The runtime on it** (`VXRT_VCPU=1 build/vxrt-arm64`): every runtime test passes in the vCPU on the MacBook Air
  (the 22 programs of make check's list and the 4 between processes). Native functions and helpers by HVC; the
  program's access faults come from the guest's vectors with its registers and go to SEH; code that changes is
  watched with the guest's own page tables (4 KB) and, for the host's writes (the runtime's memcpy into a JIT's
  buffer), with mprotect. Found: a host page put in the VM keeps its old contents for the guest after the host
  unmaps it and maps something new there (measured), so `libvxrt_vm.dylib` (linked, its __interpose tuples reach
  malloc's calls too; the main executable's own do not) reports every munmap, mach_vm_deallocate and overwriting
  map, and the page leaves the guest (guest physical addresses are never reused: a stale TLB entry faults, the TLB
  is flushed, the access is retried). Stage 2 is mapped by the host's maximum protection (the SMC watch makes pages
  read-only for a while).
  **The cost, measured (MacBook Air, M5):** an HVC round trip is 804 ns; a Direct3D 11 draw costs 10.6 µs of CPU in
  the vCPU against 0.36 µs in-process (about 12 exits per draw), so a game's thousands of runtime calls per frame
  make the vCPU unusable as it is: it needs the calls that return nothing (the immediate context's setters and
  draws) queued in the guest and run by the host in batches, or the program stays in-process. In-process stays the
  way games run; the vCPU is for what in-process cannot do (the low 4 GB, x86's order without LDAPR/STLR).
  **Calls answered in the guest (2026-10-15).** `translator/guest.c` (built freestanding into its own section and
  copied into the guest's code) answers the runtime's most frequent calls without an exit: recorded Direct3D 11
  calls (run by the host at the next exit), TLS slots, last error, the performance counter and tick count, memcpy,
  memmove, memset, memcmp, strlen, wcslen, the heap (malloc, calloc, realloc, free, new/delete, HeapAlloc and the
  rest, LocalAlloc, _msize: `translator/xvheap.h`, size classes up to 2 KB with lock-free lists in one reserved
  range that `libvxrt_vm.dylib` owns, so every image's free, realloc and malloc_size take its blocks), critical
  sections and SRW locks (lock words in the program's memory, `src/sync.c`: a free lock is taken in the guest,
  a held one waits in the host on `__ulock_wait`). Two kinds of memory the guest could not see as the host does:
  the pages a Map hands out (a Metal buffer the kernel unmapped and replaced at the same address: the page leaves
  the guest when its VM object changed, `xv_refresh`), and an image's `__TEXT` constants (an executable page never
  goes into the VM: a read-only copy does, as for a file view's page past the file's end). A guest handler's write
  to a page the SMC watch protects is handled as the program's own write.
  Per call, MacBook Air M5, in-process / vCPU before / vCPU now (`tests/callbench.c`): Enter+LeaveCriticalSection
  15 / 1418 / 10.6 ns, TlsGetValue 6.4 / 720 / 5.8, QueryPerformanceCounter 13 / 720 / 5.2, malloc+free 12.7 /
  1454 / 12.8, HeapAlloc+HeapFree 27 / 2898 / 22, memcpy 256 B 10 / 720 / 9.4, strlen 255 B 8.5 / 724 / 11.6.
  Every runtime test passes in the vCPU (`steam` needs a Steam client the Air does not have).
- **Translation vs. Rosetta, while Rosetta exists.** Every instruction form the census finds is run with random
  register, flag and memory inputs twice: natively on x86 under Rosetta (today's VXRuntime) and translated, and the
  results (registers, flags, memory, exceptions) must match. Keep a Mac on macOS 26/27 for this.
- **Games with the harness.** `make -C VXRuntime vr-check` and the levels (AGENTS.md) with the translator instead of
  Rosetta; fps and CPU time compared run for run.

## Rules (translation improvements proposed by an AI, accepted by tests)

A rule is a pattern of x86 instructions with the conditions under which a shorter ARM64 sequence gives the same
results. Anyone (an AI coding agent included) may propose one; only the tests accept it. `tools/xlat_view.py <XE_DUMP
file> <image>` shows blocks side by side (x86, then the ARM64 they became); `tools/xlat_rules.py` writes a brief for
an agent from the blocks with the most ARM64 per x86 instruction (what a rule must keep, how it is checked). A rule
is in only when difftest matches Rosetta (`build/difftest build/ref_x86 20000 <seed> programs` and its forms), when
a deliberate mistake planted in it makes difftest fail (the generator reaches it: add a case if not), and when
`make xlat-bench` shows it helps.

| Rule | Where | Measured |
|---|---|---|
| CMP of memory with an immediate, read by JE/JNE only: the value loaded, CBZ/CBNZ for 0 (kept in X25 across instructions in between), CMP #imm otherwise | `mem_compare` | string 36.3 -> 30.9 ms (MacBook Pro M1 Max) |
| IMUL with every flag dead: the product alone | `imul:` | 16 -> 5 instructions for `imul $imm, r, r` |
| Lazy flags: the last instruction to set the flags, when only code past where the look-ahead stops (a RET, an indirect branch) may read them, records its operands and result (`ctx->pend_*`, x27 bit 3, reserved in EFLAGS) instead of computing them; a block's first reader (or a writer of some flags whose others are read later) computes them through the engine's stub; merge_flags clears the bit at no cost; leaving translated code and C (faults, SuspendThread: `xlat_flags_resolve`) compute them | `flags_arith`, `xlat_flags_stub`, block_linked | MacBook Pro M1 Max: fib 0.304 -> 0.210 ms, interp 36.3 -> 30.5 ms, others unchanged (ABBA); MacBook Air M5: interp 16.1 -> 10.8 ms (Rosetta 11.4), fib 0.176 -> 0.144 ms |
| (U)COMISS/SD whose flags only a JA/JAE/JB/JBE/JP/JNP right after reads: FCMP (FCMPE for COMIS) and B.GT/GE/LT/LE/VS/VC on its NZCV (unordered: C and V set, so these match x86's CF ZF PF; JE/JNE need two conditions and stay as they were) | `fp_fuse`, `fuse_kind` 6 | physics 8.40 -> 6.37 ms (MacBook Pro M1 Max, ABBA); `ucomiss; jbe` 23 -> 8 words |
| INC/DEC whose flags only a Jcc right after reads (not one reading CF, which they keep): ADDS/SUBS #1 on the register itself (32/64 bits) and the B.cond; other sizes and memory compare their operands (INC's ZF SF OF are those of x - (-1)) | `direct_compare`, `fused_cond` | `decq; jne` 22 -> 9 words; no change in the bench on its own |
| Zero idioms: XORPS/XORPD/PXOR/ANDNPS/PANDN/PSUBx/PCMPGTx of a register with itself are MOVI #0, XOR/SUB r, r (flags dead) MOVZ #0. EOR v, v, v is not a zero idiom on the Apple cores: it waits for v's last value, so the chains of a loop that each start from `xorps acc, acc` ran one after another instead of side by side | `xlat_sse` (0x57, the integer kernel), `fast` | matmul 10.2 -> 6.8 ms (MacBook Pro M1 Max; Rosetta 4.4, the rest is the NaN checks the M1 needs), 5.86 -> 2.30 ms (MacBook Air M5; Rosetta 2.81) |
| One instruction for: AND/OR/XOR with a logical immediate, NEG with dead flags, PSHUFD's broadcasts and swaps (DUP, EXT, REV64), MOVD/MOVQ xmm to a register (UMOV into it), a 32-bit LEA with an index (no extra MOV); the forms without a ModRM byte (`and $imm, %eax`, `test $imm, %eax`, MOV r, imm) reach the fast paths at all (they were taken for memory operands) | `fast`, `shuf4`, `a64_logimm` | crc 2.57 -> 1.54 ms (M1 Max; Rosetta 1.49), 3.19/1.80 -> 1.09 ms (M5) |
| Stack runs: PUSH / POP of registers and MOV r64, RSP as stores and loads at offsets from X4, RSP's change kept (`sp_delta`) until an instruction needs RSP (one ADD / SUB) or a CALL / RET takes it up with its own. Faults and SuspendThread get RSP = X4 + imap's `sp` | `stack_run`, `stack_op`, `sp_settle`, `xe_rip_sp_at` | fib 0.139 -> 0.131 ms on the MacBook Air M5 (-6 %), unchanged on the M1 Max; prologues 5 -> 3 + 1 instructions |
| FEAT_AFP (Apple M2+; not in the vCPU): FPCR.AH = 1, so ADD/SUB/MUL/DIV (ss sd ps pd) need no NaN fix-up; FZ/FIZ are MXCSR's FTZ/DAZ exactly | `afp`, `mxcsr_to_fpcr` | physics 4.7 -> 3.7 ms, matmul 6.7 -> 6.35 (MacBook Air M5); on the M5 the old path's FMA NaNs differed from Rosetta's there, with AH they match (985 forms) |

Lazy flags were checked the same way: the record's operand, its kind, the reader's check and the exit's check, the
bit merge_flags clears (each made difftest fail), and the C version against Rosetta for every kind and size ("flags
record" in difftest). A drop before writers of all six that bypass merge_flags turned out unneeded (none do) and is
not there. Found on the way: STP/LDP's offset reaches only 504 bytes and wrapped silently (the record went 96 bytes
before the context): the encoders now trap on an offset they cannot hold. The first version, checking at every block's
first flag writer, cost 3-9 %; the second, checking at partial writers whatever their liveness, made interp 38 %
slower (an INC after every switch case computed the record each time).

Mutations checked (each made difftest fail): CBZ/CBNZ swapped (adjacent and across), CMP with the immediate + 1,
IMUL of the wrong operand, INC/DEC by 2, INC's -1 off by one, INC/DEC fused with a Jcc that reads CF; the logical
immediate's length off by one, NEG of the wrong operand, PSHUFD's DUP lane, EXT amount and REV64 replaced, MOVD's lane,
XORPS / PXOR / XOR of two different registers taken as zero, a 32-bit LEA without its zero extension (the generator now
has `lea d(r), r32`, PSHUFD, MOVD to registers, imm32 masks and the accumulator forms).

Calls and returns (fib, 1.7x Rosetta's time on both Macs), taken apart in assembly (the engine's fib as translated,
then one change at a time): the lazy flags recorded before each RET cost 4 %, the ring's pointer arithmetic nothing,
the B a return lands on 8-10 %; the pushes and pops with writeback were the one large part on the M5 (a store and the
load of the same place round-trip in 5.1 cycles with writeback, 2.9 at a fixed offset; 7.0 against 6.3 on the M1),
hence stack runs. The rest of the gap is not found yet. A sample's hottest instruction was the ring's load, which
turned out to cost nothing: a sample points at the instruction that waits, not at what it waits for. Mutations
caught: a pop's or a push's offset off by 8, CALL or RET ignoring the delta, MOV r, RSP's sign, no ADD before an
instruction that uses RSP (the generator now has pushes, something that reads RSP, pops); and without imap's sp,
tests/threads.c's Rsp check fails.

Found by reproducing the engine's blocks as they are in assembly (the loop alone, called per output, ran at Rosetta's
speed; the same blocks chained as the engine links them did not): the difference was `eor v2, v2, v2`, not the
loop.

Found with a profiler (`sample`, the hot addresses against XE_DUMP; `XB_ONLY=<kernel>` runs one kernel of the bench):
a branch back to its own block (a loop of one block, a recursive call) was never linked: the block is not in the map
while it is translated, so the site stayed a BL to the link stub, whose lookup then found the block every time and
patched nothing. 70 % of string's time was the lookup stub. Such sites are now linked to the block's own start when it
is put in (`self` in block_ex): string 30.9 -> 14.1 ms (Rosetta 11.4), sort 30.4 -> 27.4, tree 40.5 -> 38.9 (MacBook
Pro M1 Max, ABBA). Seen next by the brief: `cmp r, r; jge` whose flags the successors read again (40 ARM64
instructions computing all six flags), a candidate for flags computed only when read.

## Phases

| Phase | Done when |
|---|---|
| 0 Measure | census, CPU features, TSO and exit cost (above) — done |
| 1 Decoder | lengths of every instruction in the four apps match LLVM (except data LLVM reads as APX) — done |
| 2 Integer translator + differential tests | integer and flag instructions match Rosetta on random inputs (2a, done: 154 forms); control flow, blocks, string instructions, RCL/RCR, SHLD/SHRD, atomics with LSE (2b, done: 224 forms and random programs); small PE tests run translated in-process (with phase 4) |
| 3 SSE, AVX, x87 | every SIMD/FP form in the census matches Rosetta (MXCSR rounding, denormals, NaNs): 3a SSE/SSE2 done (128 forms); 3b AVX/AVX2/FMA/SSE3-4.1/AES done (785 forms with 3a's); 3c MXCSR modes and flags, x87 (with the transcendentals) done |
| 4 Runtime on ARM64 + execution environment | 4a engine (cache, linking, indirect lookup, return ring, traps, CPUID/RDTSC/XGETBV, ordered mode) done, 1.3-3.1x Rosetta's time on the kernels; then `make check` passes translated; in-process vs. vCPU measured on SteamVR and a game |
| 5 Games | SteamVR, The Lab (Mono JIT), Earth, Alyx at level `controls` with fps at least Rosetta's — started 2026-10-17: Earth VR runs (57.8 fps, 2026-10-16); The Lab runs its 150 s title scenario at 52.7-56.4 fps (Rosetta 59.4-59.6, 2026-10-17); since 2026-10-23 (be86f36) without Mono's collation error and three runs in a row whole; Alyx needs the vCPU (LuaJIT wants memory below 2 GB) |

Machine code written at run time is allowed here, and only here: the translator's code emitter (AOT cache and JIT).
Everything else in VXRuntime keeps the rule in AGENTS.md.

## The Lab on the ARM64 runtime (2026-10-17)

Unity with Mono's JIT was the first program here whose code changes while it runs, all the time: Mono writes new
methods next to old ones and patches call sites in code it is running, so its pages soon become checked ones (a block
compares its bytes at its start). What it found:

- A return into a checked block went through the return-address ring straight into the middle of the block's old
  translation, past the check: Mono's char.ToLower and TextInfo.ToLower then called each other until the stack ran out
  (the stack showed one function's return address in the other's frame). A call in a checked block now puts no x86
  address in the ring (its return goes through the lookup to code made from the bytes there now), and a block
  forgotten as stale, or code freed, clears the rings (b3b2368).
- With that, Unity's heap came apart about 40 s in (List.EnsureCapacity on a null list, allocator headers garbage).
  Bisected over the translator's commits (b3b2368 applied to each): add02ef's self-linked sites, only in checked
  blocks. Not linked there now (eff0df3); why the two together break is not found yet.
- Mono reported its collation tailoring table broken for the invariant culture (a managed check in
  Mono.Globalization.Unicode.SimpleCollator; not with Rosetta), and one run of two ended early. The forms were
  identical to Rosetta in the ordered mode too (difftest DT_ORDERED, now part of translator-check), and random
  programs as well: the cause was in the engine's linking, which those do not reach (2026-10-23).
  The table itself is right (mscorlib's `collation.tailoring.bin`: the invariant culture's 223 chars are
  replacements, `3, from, 0, to..., 0`); the parse went wrong at char 17, the end of the third (ß -> "ss"): the loop
  `while (tarr[l] != 0) l++` (Mono's JIT: `add [rbp-40h], 1; ...; movzx eax, word [rax]; test eax, eax; jne`) left
  one char early. Reproduced without SteamVR by a small program that loads The Lab's mono-2.0-bdwgc.dll and calls
  String.Equals(a, b, InvariantCultureIgnoreCase) (half a second; on this runtime it then also stopped at Mono's
  assertion `tls_offsets [key]`, a consequence: the exception path wanted TLS slot 0). Blocks of 1-3 instructions
  (`XE_MAX_INSNS`) made it pass, and `XE_BISECT` found the one block that broke it whole: that loop, one checked
  block jumping to itself. Translated right; what went wrong was the link: the link stub keeps the site it came from
  in ctx->link and goes on through the lookup when the target is translated already, so the site stays in ctx->link
  while the code runs on; when it next came back to C from a checked block that found its bytes changed (S1 = that
  block, frequent in Mono's JIT code), the engine linked the old site to that block. The loop's back edge (or its
  exit) then went to unrelated code. Now a site is linked only when its own target is where the code came back.
  `tests/smc.c` case 5 (in check-arm64) builds exactly that: A's JNZ to B, B already translated, C changed; before,
  the second call of A skipped B (9 instead of 11).

Tools made for it: `XE_MAX_INSNS=n` (blocks of at most n instructions) and `XE_BISECT=bits:value` (with it, only the
blocks whose first 12 bytes hash to value in their low bits are whole, and are listed: double bits until one block
is left); XE_DUMP also writes each block's x86 bytes (an `x86` line: JIT code is gone or different later).
`VXRT_TRACE_FAULTS=1` logs each fault with RSP, the stack's range and the code addresses above RSP
(module+offset, or JIT code); `VXRT_TRACE_FAULTS_DUMP=<file>` writes the 8 KB of x86 code around RIP and the stack's
last 4 KB (a disassembler takes the code as `.byte` lines assembled for x86_64 and `llvm-objdump --start-address`).

