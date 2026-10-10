#!/usr/bin/env python3
"""pipelines.py: the game's Vulkan pipeline list, published without any of the game's code.

The app pre-compiles a list of pipelines (shader + settings) at "Preparing graphics" so the headset never compiles
one mid-game (a hitch). The list is a Fossilize archive recorded while playing. Its shader modules are the game's
own SPIR-V, which we may not publish, but every one of them is a byte-identical slice of a shader file in the
player's own copy of the game. So the repository carries a *recipe*: the archive with each shader module replaced
by where to find it (pack, .vcs file, static combo, offset, size, SHA-256). The rest of an archive is Vulkan state
(pipeline settings, layouts), which is ours to describe.

  pipelines.py make  <out.recipe> <game dir> <recorded.foz>...   (maintainers) merge recordings into a recipe
  pipelines.py build <recipe> <game dir> <out.foz>                (every install) rebuild the archive
  pipelines.py merge <out.recipe> <recipe>...                     union of recipes (first copy of each entry wins)
  pipelines.py check <recipe>                                     sanity check: no shader code inside

<game dir> is the depot's game/ folder (it holds core/ and hlvr/ with shaders_vulkan_dir.vpk)."""
import hashlib
import struct
import json
import os
import sys
from multiprocessing import Pool

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gb import foz, vcs  # noqa: E402
from gb.vpk import VPK  # noqa: E402

PACKS = ("core/shaders_vulkan_dir.vpk", "hlvr/shaders_vulkan_dir.vpk")
REF_MAGIC = b"GBREF"
KEY = 64  # bytes of SPIR-V that index a candidate match

_vpks, _keys = {}, {}


def _vpk(game, pack):
    k = (game, pack)
    if k not in _vpks:
        _vpks[k] = VPK(os.path.join(game, pack))
    return _vpks[k]


def _init_scan(keys):
    _keys.update(keys)


def _scan(job):
    game, pack, path = job
    d = _vpk(game, pack).read(path)
    table = vcs.static_table(d)
    found = []
    if not table:
        return found
    for sid, off in table.items():
        blob = vcs.static_blob(d, off)
        for pos in vcs.spirv_starts(blob):
            for size, sha1, name in _keys.get(blob[pos : pos + KEY], ()):
                if hashlib.sha1(blob[pos : pos + size]).digest() == sha1:
                    found.append((name, pack, path, sid, pos))
    return found


def make(out, game, inputs, allow_missing=False):
    header, merged, seen = None, [], set()
    for path in inputs:
        h, entries = foz.read(path)
        header = header or h
        for e in entries:
            if e[0] not in seen:
                seen.add(e[0])
                merged.append(e)
    modules = {}
    for name, fmt, _crc, _size, stored in merged:
        if name[22:24] == foz.TAG_SHADER_MODULE:
            modules[name] = foz.split_module(foz.payload(fmt, stored))
    keys = {}
    for name, (_, code) in modules.items():
        keys.setdefault(code[:KEY], []).append((len(code), hashlib.sha1(code).digest(), name))
    jobs = [(game, pack, p) for pack in PACKS for p in _vpk(game, pack).entries if p.endswith(".vcs")]
    jobs.sort(key=lambda j: -_vpk(game, j[1]).entries[j[2]][2])  # biggest first keeps the pool busy
    where = {}
    with Pool(min(24, os.cpu_count() or 4), initializer=_init_scan, initargs=(keys,)) as pool:
        for found in pool.imap_unordered(_scan, jobs):
            for name, pack, path, sid, pos in found:
                cand = (pack, path, sid, pos)
                if name not in where or cand < where[name]:
                    where[name] = cand  # the same module can sit in several combos: keep a stable choice
    missing = sorted(set(modules) - set(where))
    print(f"{len(merged)} entries, {len(modules)} shader modules, {len(where)} found in the game's shader files, "
          f"{len(missing)} missing")
    if missing and not allow_missing:
        for m in missing[:20]:
            print("  missing", m)
        return 1
    with open(out, "wb") as f:
        f.write(header)
        for name, fmt, crc, size, stored in merged:
            if name[22:24] != foz.TAG_SHADER_MODULE:
                f.write(name.encode() + struct.pack("<4I", len(stored), fmt, crc, size) + stored)
                continue
            if name not in where:
                continue
            pack, path, sid, pos = where[name]
            jh, code = modules[name]
            ref = {"json": jh.decode(), "pack": pack, "file": path, "static": sid, "offset": pos,
                   "size": len(code), "sha256": hashlib.sha256(code).hexdigest()}
            p = REF_MAGIC + json.dumps(ref, separators=(",", ":")).encode()
            f.write(name.encode() + struct.pack("<4I", len(p), foz.FORMAT_RAW, 0, len(p)) + p)
    print(f"recipe: {out} ({os.path.getsize(out)} bytes)")
    return 0


def _resolve(job):
    game, pack, path, sid, refs = job
    d = _vpk(game, pack).read(path)
    table = vcs.static_table(d)
    blob = vcs.static_blob(d, table[sid])
    out = []
    for name, off, size, sha in refs:
        code = blob[off : off + size]
        if hashlib.sha256(code).hexdigest() != sha:
            raise ValueError(f"{path} static {sid} @{off}: not the expected shader (game files changed?)")
        out.append((name, code))
    return out


def build(recipe, game, out):
    header, entries = foz.read(recipe)
    groups, refs = {}, {}
    for name, fmt, _crc, _size, stored in entries:
        p = foz.payload(fmt, stored)
        if p.startswith(REF_MAGIC):
            r = json.loads(p[len(REF_MAGIC):])
            refs[name] = r
            groups.setdefault((r["pack"], r["file"], r["static"]), []).append((name, r["offset"], r["size"], r["sha256"]))
    codes = {}
    jobs = [(game, pack, path, sid, rs) for (pack, path, sid), rs in groups.items()]
    with Pool(min(12, os.cpu_count() or 4)) as pool:
        for got in pool.imap_unordered(_resolve, jobs):
            codes.update(got)
    tmp = out + ".part"
    with open(tmp, "wb") as f:
        f.write(header)
        for name, fmt, crc, size, stored in entries:
            if name in refs:
                f.write(foz.entry_bytes(name, foz.join_module(refs[name]["json"].encode(), codes[name])))
            else:
                f.write(name.encode() + struct.pack("<4I", len(stored), fmt, crc, size) + stored)
    os.replace(tmp, out)
    print(f"pipelines: {out} ({len(entries)} entries, {len(refs)} shader modules from {len(groups)} shader combos)")
    return 0


def merge(out, inputs):
    header, seen, n = None, set(), 0
    tmp = out + ".part"
    with open(tmp, "wb") as f:
        for path in inputs:
            h, entries = foz.read(path)
            if header is None:
                header = h
                f.write(h)
            for name, fmt, crc, size, stored in entries:
                if name in seen:
                    continue
                seen.add(name)
                n += 1
                f.write(name.encode() + struct.pack("<4I", len(stored), fmt, crc, size) + stored)
    os.replace(tmp, out)
    print(f"merged {len(inputs)} recipes: {n} entries -> {out}")
    return check(out)


def check(recipe):
    _, entries = foz.read(recipe)
    bad = 0
    for name, fmt, _crc, _size, stored in entries:
        p = foz.payload(fmt, stored)
        if name[22:24] == foz.TAG_SHADER_MODULE and not p.startswith(REF_MAGIC):
            bad += 1
        if vcs.SPIRV_MAGIC in p:
            bad += 1
    print(f"{recipe}: {len(entries)} entries, {'no shader code' if not bad else f'{bad} entries carry shader code'}")
    return 1 if bad else 0


if __name__ == "__main__":
    a = sys.argv[1:]
    if len(a) >= 4 and a[0] == "make":
        allow = "--allow-missing" in a
        a = [x for x in a if x != "--allow-missing"]
        sys.exit(make(a[1], a[2], a[3:], allow))
    if len(a) == 4 and a[0] == "build":
        sys.exit(build(a[1], a[2], a[3]))
    if len(a) >= 3 and a[0] == "merge":
        sys.exit(merge(a[1], a[2:]))
    if len(a) == 2 and a[0] == "check":
        sys.exit(check(a[1]))
    sys.exit(__doc__)
