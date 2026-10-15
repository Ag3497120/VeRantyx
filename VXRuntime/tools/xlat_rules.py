#!/usr/bin/env python3
"""A brief for an AI coding agent (or a person) to propose translation rules (TRANSLATOR.md, "Rules"): the blocks of a
run that became the most ARM64 per x86 instruction, side by side, with what a rule must keep and how it is checked.
The agent proposes; only the differential test against Rosetta (and a planted bug it must catch) accepts a rule.

    XE_DUMP=/tmp/blocks.txt build/bench build/bench_image 1 0          # (or any run of the engine)
    tools/xlat_rules.py /tmp/blocks.txt build/bench_image [--top 8] > brief.md"""
import argparse, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xlat_view as v

BRIEF = """# Translation rules: proposals wanted

VXRuntime's x86-64 -> ARM64 translator (VXRuntime/translator/xlat.c, xlat_sse.inc) turned the blocks below into the
ARM64 shown. Propose rules that make them shorter or faster **without changing any result an x86 program can see**.

How the translated code holds x86's state: RAX..R15 in x0..x15, XMM0..15 in v0..v15, the six arithmetic flags in x27
(x86 bit positions), the context in x28, scratch x16 x17 x19-x24 v16-v23; x25 carries a fused condition across
instructions. A flag no instruction reads before it is written again is dead (b->dead_flags, computed per block and
into its successors); a CMP/TEST and the Jcc after it are fused (b->fuse_kind) when nothing else reads the flags.

For each rule give:
1. the x86 pattern (instructions, operand kinds and sizes) and the conditions (which flags must be dead, alignment,
   what may come between, FEAT_ requirements of the ARM side);
2. the ARM64 to emit, and why it equals x86 for every input (flags, NaNs and signed zeros, partial registers, faults:
   an access that faults must fault at the same x86 instruction with the state before it);
3. where in xlat.c it goes (the function), and a generator case for translator/diff/difftest.c that reaches it.

A rule is accepted only when `build/difftest build/ref_x86 20000 <seed> programs` (and the forms it touches) match
Rosetta, when a deliberate mistake in it makes difftest fail (it is really reached), and when `make xlat-bench`
shows it helps. Machine code is generated only by the translator's emitter (AGENTS.md).

## The blocks (most ARM64 per x86 instruction first)
"""

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump"); ap.add_argument("image"); ap.add_argument("--top", type=int, default=8)
    a = ap.parse_args()
    read = v.image_reader(a.image)
    seen, rows = set(), []
    for start, end, host, words in v.blocks(a.dump):
        if start in seen: continue
        seen.add(start)
        x86 = v.disasm("x86_64", read(start, end - start), start)
        if x86: rows.append((len(words) / len(x86), start, end, host, words, x86))
    rows.sort(reverse=True)
    out = [BRIEF]
    for ratio, start, end, host, words, x86 in rows[:a.top]:
        arm = v.disasm("arm64", b"".join(w.to_bytes(4, "little") for w in words), host)
        out.append(f"\n### {start:#x}-{end:#x}: {len(x86)} x86 -> {len(words)} ARM64 ({ratio:.1f} per instruction)\n```")
        out += [f"x86   {addr:#x}  {t}" for addr, t in x86]
        out += [f"arm64 {addr:#x}  {t}" for addr, t in arm if not t.startswith("udf")]
        out.append("```")
    print("\n".join(out))

if __name__ == "__main__":
    main()
