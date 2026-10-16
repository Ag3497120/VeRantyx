#!/usr/bin/env python3
"""The translator's blocks side by side (XE_DUMP=<file>: each block's x86 range and the ARM64 it became): the x86
instructions, read from a Mach-O image of the program (e.g. build/bench_image) or a raw dump, then the ARM64 code.
For finding what translates badly and for writing translation rules (TRANSLATOR.md, "Rules").

    tools/xlat_view.py <dump> <image> [--block <x86 address>] [--top N]

--top N: the N blocks with the most ARM64 instructions per x86 instruction (where a rule would help most)."""
import argparse, os, re, subprocess, tempfile

def disasm(arch, data, base):
    """llvm-objdump of raw bytes at address base: [(address, text)]."""
    with tempfile.TemporaryDirectory() as d:
        src, obj = os.path.join(d, "b.s"), os.path.join(d, "b.o")
        with open(src, "w") as f: f.write(".text\n" + "".join(f".byte {b}\n" for b in data))
        subprocess.run(["clang", "-arch", arch, "-c", src, "-o", obj], check=True)
        out = subprocess.run(["xcrun", "llvm-objdump", "-d", "--no-show-raw-insn", obj], capture_output=True, text=True).stdout
    res = []
    for l in out.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s*(.*)", l)
        if m: res.append((int(m.group(1), 16) + base, m.group(2).strip()))
    return res

def image_reader(path):
    """bytes at an address of a Mach-O image (its segments), via otool's load commands."""
    out = subprocess.run(["otool", "-l", path], capture_output=True, text=True).stdout
    segs, cur = [], {}
    for l in out.splitlines():
        l = l.strip()
        for k in ("vmaddr", "vmsize", "fileoff", "filesize"):
            if l.startswith(k + " "): cur[k] = int(l.split()[1], 0)
        if l.startswith("filesize "): segs.append(dict(cur))
    data = open(path, "rb").read()
    def read(addr, n):
        for s in segs:
            if s["vmaddr"] <= addr < s["vmaddr"] + s["filesize"]:
                off = s["fileoff"] + addr - s["vmaddr"]
                return data[off:off + n]
        return b""
    return read

def blocks(dump):
    for l in open(dump):
        m = re.match(r"block ([0-9a-f]+)-([0-9a-f]+) at (0x[0-9a-f]+): (.*)", l)
        if m:
            words = [int(w, 16) for w in m.group(4).split()]
            yield int(m.group(1), 16), int(m.group(2), 16), int(m.group(3), 16), words

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump"); ap.add_argument("image")
    ap.add_argument("--block", type=lambda s: int(s, 16)); ap.add_argument("--top", type=int, default=0)
    ap.add_argument("--raw", type=lambda s: int(s, 16), help="the image is raw bytes loaded at this address")
    a = ap.parse_args()
    if a.raw is not None:
        blob = open(a.image, "rb").read()
        read = lambda addr, n: blob[addr - a.raw:addr - a.raw + n] if addr >= a.raw else b""
    else:
        read = image_reader(a.image)
    found = list(blocks(a.dump))
    if a.block is not None: found = [b for b in found if b[0] == a.block][:1]
    elif a.top:
        def ratio(b):
            n = len(disasm("x86_64", read(b[0], b[1] - b[0]), b[0])) or 1
            return len(b[3]) / n
        found = sorted({b[0]: b for b in found}.values(), key=ratio, reverse=True)[:a.top]
    for start, end, host, words in found:
        x86 = disasm("x86_64", read(start, end - start), start)
        arm = disasm("arm64", b"".join(w.to_bytes(4, "little") for w in words), host)
        print(f"== block {start:#x}-{end:#x}: {len(x86)} x86 instructions -> {len(words)} ARM64 words")
        for addr, t in x86: print(f"  x86 {addr:#x}  {t}")
        for addr, t in arm: print(f"      {addr:#x}  {t}")

if __name__ == "__main__":
    main()
