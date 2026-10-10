#!/usr/bin/env python3
"""spirv_index.py <game dir> <out.tsv>: every compiled shader in the game's Vulkan shader packs, by SHA-256 of its
SPIR-V: "<sha256> <size> <pack> <file> <static combo> <offset>" per line. Maintainers build it once per game update;
tools/reports_ingest.py turns players' shader fingerprints back into recipe references with it."""
import hashlib, os, sys
from multiprocessing import Pool
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "engine"))
from gb import vcs, vcsblob
from gb.vpk import VPK

PACKS = ("core/shaders_vulkan_dir.vpk", "hlvr/shaders_vulkan_dir.vpk")
_v = {}

def work(job):
    game, pack, path = job
    if pack not in _v: _v[pack] = VPK(os.path.join(game, pack))
    d = _v[pack].read(path)
    table = vcs.static_table(d)
    if not table: return [], 0
    prog = path.rsplit("_", 1)[1][:-4]
    rows, bad = [], 0
    for sid, off in table.items():
        b = vcs.static_blob(d, off)
        try:
            files, _ = vcsblob.parse(b, prog)
        except Exception:
            bad += 1
            continue
        for f in files:
            if f:
                code = b[f[0]:f[0] + f[1]]
                rows.append(f"{hashlib.sha256(code).hexdigest()} {f[1]} {pack} {path} {sid} {f[0]}")
    return rows, bad

if __name__ == "__main__":
    game, out = sys.argv[1:3]
    jobs = [(game, p, f) for p in PACKS for f in VPK(os.path.join(game, p)).entries if f.endswith(".vcs")]
    n = bad = 0
    with open(out + ".part", "w") as o, Pool(min(24, os.cpu_count() or 4)) as pool:
        for rows, b in pool.imap_unordered(work, jobs):
            bad += b; n += len(rows)
            if rows: o.write("\n".join(rows) + "\n")
    os.replace(out + ".part", out)
    print(f"{n} shaders indexed from {len(jobs)} files; {bad} static combos could not be parsed")
