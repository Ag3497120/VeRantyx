#!/usr/bin/env python3
"""Translator coverage over the x86-64 PEs in the given folders: runs coverage (coverage.c) over them in parallel, sums,
names the instructions it could not translate (LLVM's disassembler on the first instance's bytes) and prints them by
count. usage: coverage.py <coverage binary> <folders...>"""
import collections, os, subprocess, sys
from concurrent.futures import ThreadPoolExecutor


def pes(folders):
    for root in folders:
        for dp, _, names in os.walk(root):
            for n in names:
                if n.lower().endswith((".dll", ".exe")):
                    yield os.path.join(dp, n)


def run(binary, files):
    out = subprocess.run([binary] + files, capture_output=True, text=True).stdout
    total = ok = undec = 0; miss = {}
    for line in out.splitlines():
        f = line.split()
        if f[0] == "total": total, ok, undec = int(f[1]), int(f[3]), int(f[5])
        elif f[0] == "miss": miss[f[1]] = (int(f[2]), f[3:])
    return total, ok, undec, miss


def names(samples):                                                  # key -> LLVM's text for the bytes
    import re, tempfile
    with tempfile.TemporaryDirectory() as t:
        src = "".join(f"s{i}:\n  .byte {', '.join('0x' + h for h in b)}\n" for i, b in enumerate(samples.values()))
        open(os.path.join(t, "a.s"), "w").write(".text\n" + src)
        subprocess.run(["clang", "-arch", "x86_64", "-c", os.path.join(t, "a.s"), "-o", os.path.join(t, "a.o")], check=True)
        dump = subprocess.run(["xcrun", "llvm-objdump", "-d", "--no-show-raw-insn", os.path.join(t, "a.o")], capture_output=True, text=True).stdout
    first = {}; cur = None
    for line in dump.splitlines():
        m = re.match(r"^[0-9a-f]+ <s(\d+)>:", line)
        if m: cur = int(m.group(1)); continue
        if cur is not None and ":" in line and cur not in first: first[cur] = line.split(":", 1)[1].strip()
    return {k: first.get(i, "?") for i, k in enumerate(samples)}


def main():
    binary, folders = sys.argv[1], sys.argv[2:]
    files = sorted(set(pes(folders)))
    chunks = [files[i::os.cpu_count()] for i in range(os.cpu_count())]
    total = ok = undec = 0; miss = collections.Counter(); sample = {}
    with ThreadPoolExecutor(os.cpu_count()) as ex:
        for t, o, u, m in ex.map(lambda c: run(binary, c), [c for c in chunks if c]):
            total += t; ok += o; undec += u
            for k, (n, b) in m.items():
                miss[k] += n; sample.setdefault(k, b)
    nm = names(sample)
    def avx512(k):                                                   # EVEX, or VEX-encoded opmask (k-register) instructions
        v = int(k, 16); mp, op, vex = v >> 24, v >> 16 & 0xFF, v >> 10 & 3
        return vex == 2 or (vex == 1 and ((mp == 1 and (0x41 <= op <= 0x4B or 0x90 <= op <= 0x93 or op in (0x98, 0x99))) or (mp == 3 and 0x30 <= op <= 0x33)))
    evex = sum(n for k, n in miss.items() if avx512(k))
    traps = {"int3", "ud2", "int", "into", "in", "out", "hlt", "insb", "insl", "outsb", "outsl"}
    trap = sum(n for k, n in miss.items() if nm[k].split()[0] in traps and not avx512(k))
    print(f"{len(files)} files: {total} instructions, {ok} translated ({100 * ok / max(total, 1):.4f} %), "
          f"{undec} not decodable (data in code), {total - ok - undec} not translated: {evex} AVX-512 (Rosetta 2 has none: "
          f"programs never pick it), {trap} traps and port I/O (INT3, UD2, INT n: exceptions, phase 4), "
          f"{total - ok - undec - evex - trap} other")
    by_name = collections.Counter()
    for k, n in miss.items():
        if not avx512(k): by_name[nm[k]] += n
    for t, n in by_name.most_common(int(os.environ.get("TOP", "40"))):
        print(f"  {n:9}  {t}")


if __name__ == "__main__":
    main()
