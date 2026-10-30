#!/usr/bin/env python3
# gen_big_test.py <dir> <functions> [files]: a synthetic large program (tests/xros_big: VISIONOS_PORT.md) to measure how
# a large AOT module behaves in the visionOS app (a framework's size, signing, install and dlopen time) without a
# game's code. <functions> small functions, each a loop around an 8-way switch (a jump table), spread over [files]
# C files, and main calling every one in a chain and printing the checksum (the same on the Mac and the headset).
import os, sys
d, n = sys.argv[1], int(sys.argv[2]); files = int(sys.argv[3]) if len(sys.argv) > 3 else 16
os.makedirs(d, exist_ok=True)
def K(i, j): return (i * 2654435761 + j * 40503 + 0x9e3779b9) & 0xffffffff
per = (n + files - 1) // files
for f in range(files):
    with open(f"{d}/big{f}.c", "w") as o:
        for i in range(f * per, min(n, (f + 1) * per)):
            cases = "".join(f"case {c}: a {'+^-'[c % 3]}= {K(i, c)}u; a = (a << {1 + c}) | (a >> {31 - c}); break; " for c in range(8))
            o.write(f"unsigned f{i}(unsigned x) {{ unsigned a = x * {K(i, 9) | 1}u + {K(i, 10)}u; "
                    f"for (unsigned k = 0; k < (x & 3) + 2; k++) {{ switch ((a >> (k + {i % 5})) & 7) {{ {cases}}} a ^= a >> 13; }} return a; }}\n")
with open(f"{d}/main.c", "w") as o:
    o.write('#include <stdio.h>\n')
    for i in range(n): o.write(f"unsigned f{i}(unsigned);\n")
    o.write("int main(void) {\n    unsigned s = 1;\n")
    for i in range(n): o.write(f"    s = f{i}(s);\n")
    o.write(f'    printf("xros_big: {n} functions, checksum %08x\\n", s);\n    return 0;\n}}\n')
