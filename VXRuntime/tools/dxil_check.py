#!/usr/bin/env python3
"""Checks src/dxil.c's reading of DXIL against Microsoft's dxcompiler: for each tests/shaders12/<name>.dxil, the
instructions build/dxildump prints and those of dxcompiler's own disassembly of the same container
(<name>.dis.txt, made with tools/dxc.cpp -dis), in order: the instruction, the dx.op intrinsic a call is, the
predicate of a comparison, the number of basic blocks.

    python3 tools/dxil_check.py [build/dxildump] [tests/shaders12]
"""
import glob, os, re, subprocess, sys

FCMP = "false oeq ogt oge olt ole one ord uno ueq ugt uge ult ule une true".split()
ICMP = "eq ne ugt uge ult ule sgt sge slt sle".split()

def reference(path):
    """(instructions, blocks) of the defined functions in dxcompiler's disassembly."""
    out, blocks, inside, cases = [], 0, False, False
    for line in open(path, errors="replace"):
        if cases:                                          # (a switch's cases, one a line, up to "]")
            cases = not line.strip().startswith("]")
            continue
        s = line.split(";")[0].strip() if not line.strip().startswith("; <label>") else line.strip()
        if line.startswith("define "):
            inside, blocks = True, blocks + 1
            continue
        if not inside:
            continue
        if line.startswith("}"):
            inside = False
            continue
        if line.strip().startswith("; <label>") or re.match(r"^[\w.]+:", line):
            blocks += 1
            continue
        if not s:
            continue
        rhs = s.split(" = ", 1)[1] if re.match(r"^%[\w.]+ = ", s) else s
        word = rhs.split()[0]
        if word == "tail":
            word = rhs.split()[1]
        if word == "call":
            m = re.search(r"@(dx\.op\.[\w.]+)\(i32 (-?\d+)", rhs)
            out.append(f"call:{m.group(2)}" if m else "call:" + (re.search(r"@([\w.]+)", rhs).group(1) if "@" in rhs else "?"))
        elif word in ("fcmp", "icmp"):
            pred = rhs.split()[1] if rhs.split()[1] not in ("fast", "nnan", "ninf", "nsz", "arcp") else rhs.split()[2]
            out.append(f"{word}:{FCMP.index(pred) if word == 'fcmp' else 32 + ICMP.index(pred)}")
        else:
            out.append(word)
            cases = word == "switch" and s.rstrip().endswith("[")
    return out, blocks

def ours(dump, path):
    text = subprocess.run([dump, path], capture_output=True, text=True)
    if text.returncode:
        return None, 0, text.stderr.strip()
    out, blocks = [], 0
    for line in text.stdout.splitlines():
        m = re.match(r"define @\S+ \((\d+) blocks\)", line)
        if m:
            blocks += int(m.group(1))
            continue
        if not line.startswith("  "):
            continue
        s = line.split("  ;")[0].strip()
        rhs = s.split(" = ", 1)[1] if re.match(r"^%v\d+ = ", s) else s
        word = rhs.split()[0]
        if word == "call":
            m = re.search(r"@(dx\.op\.[\w.]+)\((-?\d+|null)", rhs)
            out.append(f"call:{0 if m and m.group(2) == 'null' else m.group(2)}" if m else "call:" + re.search(r"@([\w.?]+)", rhs).group(1))
        elif word in ("fcmp", "icmp"):
            out.append(f"{word}:{rhs.split()[1][1:]}")
        else:
            out.append(word)
    return out, blocks, None

def main():
    dump = sys.argv[1] if len(sys.argv) > 1 else "build/dxildump"
    folder = sys.argv[2] if len(sys.argv) > 2 else "tests/shaders12"
    bad = 0
    files = sorted(glob.glob(os.path.join(folder, "*.dxil")))
    for path in files:
        ref = path[:-5] + ".dis.txt"
        if not os.path.exists(ref):
            print(f"{path}: no reference disassembly ({ref})"); bad += 1; continue
        want, want_blocks = reference(ref)
        got, got_blocks, err = ours(dump, path)
        name = os.path.basename(path)
        if err:
            print(f"{name}: {err}"); bad += 1; continue
        if got != want or got_blocks != want_blocks:
            bad += 1
            first = next((i for i, (a, b) in enumerate(zip(got, want)) if a != b), min(len(got), len(want)))
            print(f"{name}: differs at instruction {first}: ours {got[first:first + 3]} dxc {want[first:first + 3]}; "
                  f"{len(got)} vs {len(want)} instructions, {got_blocks} vs {want_blocks} blocks")
        else:
            print(f"{name}: {len(got)} instructions, {got_blocks} blocks, as dxcompiler reads it")
    print(f"dxil: {len(files) - bad}/{len(files)} shaders read as dxcompiler reads them -> {'ok' if not bad and files else 'FAILED'}")
    return 1 if bad or not files else 0

if __name__ == "__main__":
    sys.exit(main())
