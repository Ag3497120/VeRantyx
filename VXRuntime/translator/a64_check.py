#!/usr/bin/env python3
"""The translator's ARM64 encoders (a64.h) against the assembler: each line of a64_cases.txt is `C expression|assembly`;
both are turned into 32-bit words (clang assembles the assembly, a small C program evaluates the expressions) and must
be equal. Part of make check."""
import os, re, subprocess, sys, tempfile
here = os.path.dirname(os.path.abspath(__file__))
cases = [l.rstrip("\n").split("|") for l in open(os.path.join(here, "a64_cases.txt")) if l.strip()]
with tempfile.TemporaryDirectory() as t:
    open(os.path.join(t, "w.s"), "w").write(".text\n" + "".join(f"  {a}\n" for _, a in cases))
    open(os.path.join(t, "g.c"), "w").write('#include "a64.h"\n#include <stdio.h>\nint main(void) {\n uint32_t v[] = {' +
                                            ", ".join(c for c, _ in cases) + '};\n for (unsigned i = 0; i < sizeof v / 4; i++) printf("%08x\\n", v[i]);\n}\n')
    subprocess.run(["clang", "-arch", "arm64", "-c", os.path.join(t, "w.s"), "-o", os.path.join(t, "w.o")], check=True)
    dump = subprocess.run(["xcrun", "llvm-objdump", "-d", os.path.join(t, "w.o")], capture_output=True, text=True, check=True).stdout
    want = [m.group(1) for m in re.finditer(r"^\s*[0-9a-f]+:\s+([0-9a-f]{8})\s", dump, re.M)]
    subprocess.run(["clang", "-I", here, os.path.join(t, "g.c"), "-o", os.path.join(t, "g")], check=True)
    got = subprocess.run([os.path.join(t, "g")], capture_output=True, text=True, check=True).stdout.split()
bad = [(c, a, w, g) for (c, a), w, g in zip(cases, want, got) if w != g]
for c, a, w, g in bad:
    print(f"  {c}: {g}, the assembler gives {w} for `{a}`")
print(f"ARM64 encoders: {len(cases)} checked against the assembler -> {'ok' if not bad and len(want) == len(cases) else 'FAILED'}")
sys.exit(1 if bad or len(want) != len(cases) else 0)
