#!/usr/bin/env python3
"""klepton_ldwrap.py <klepton-ld output> [-o out]: relink a klepton-ld guest dylib with Apple's own linker.

klepton-ld writes its Mach-O by hand, and App Store processing stalled on every guest library it made (2026-10-03:
a build without them finished in minutes; any build with them stayed in Processing). This takes klepton-ld's output
as it is (the translated image, its x18 veneer pool and its translation record) and has `clang -dynamiclib` (ld)
link the same bytes at the same addresses, so the result has everything an Xcode-built dylib has: chained fixups,
an exports trie, a populated symbol table, function starts from real symbols, the linker's build version and
signature. The runtime is unchanged: it finds the image by the start of __TEXT,__klelf and the record by
__TEXT,__klstat (kl_image.c), and every address it computes from those is reproduced and then checked here.

Layout reproduced (all from the input):
  __TEXT,__klx18      from 0x4000 (16 KB aligned) to the image: the veneer pool and its padding. The pool is
                      reached by `b` from the guest's code, so its distance to the image is baked in.
  __TEXT,__klelf      the image from its ELF header to the first code (the image base). An instruction section,
                      like the pool and the code, because ld places those before a segment's other sections.
  __TEXT,__kltext     the code, cut at each function start with a local symbol (ld derives LC_FUNCTION_STARTS)
  __TEXT,__klconst    the rest of the read-only image, padded to 64 bytes before the start of __DATA
  __TEXT,__klstat     the record, in those last 64 bytes (the runtime finds it by name)
  __DATA,__kldata     the writable image (16 KB aligned, so __DATA starts where klepton-ld put it), then
  __DATA,__klbss      its zero-fill
One exported marker, _klepton_guest_<name>, at the image base. A pool placed ABOVE the image (__KLX18) is refused.
"""
import os, re, struct, subprocess, sys, tempfile

LC_SEGMENT_64, LC_ID_DYLIB, LC_BUILD_VERSION, LC_FUNCTION_STARTS = 0x19, 0xd, 0x32, 0x26
PLATFORM = {1: ("macosx", "macos"), 11: ("xros", "xros"), 12: ("xrsimulator", "xros")}


def parse(data):
    ncmds = struct.unpack_from("<I", data, 16)[0]
    off, segs, info = 32, [], {}
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, off)
        if cmd == LC_SEGMENT_64:
            name = data[off + 8:off + 24].rstrip(b"\0").decode()
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<QQQQ", data, off + 24)
            nsects = struct.unpack_from("<I", data, off + 64)[0]
            sects = []
            for k in range(nsects):
                so = off + 72 + k * 80
                sn = data[so:so + 16].rstrip(b"\0").decode()
                addr, sz = struct.unpack_from("<QQ", data, so + 32)
                sofs = struct.unpack_from("<I", data, so + 48)[0]
                flags = struct.unpack_from("<I", data, so + 64)[0]
                sects.append(dict(name=sn, addr=addr, size=sz, offset=sofs, flags=flags))
            segs.append(dict(name=name, vmaddr=vmaddr, vmsize=vmsize, fileoff=fileoff, filesize=filesize, sects=sects))
        elif cmd == LC_ID_DYLIB:
            noff = struct.unpack_from("<I", data, off + 8)[0]
            info["install_name"] = data[off + noff:off + size].split(b"\0")[0].decode()
        elif cmd == LC_BUILD_VERSION:
            info["platform"], minos = struct.unpack_from("<II", data, off + 8)
            info["minos"] = f"{minos >> 16}.{(minos >> 8) & 0xff}"
        elif cmd == LC_FUNCTION_STARTS:
            info["fs"] = struct.unpack_from("<II", data, off + 8)
        off += size
    return segs, info


def function_starts(data, fs):
    if not fs:
        return []
    dataoff, datasize = fs
    out, addr, i, end = [], 0, dataoff, dataoff + datasize
    while i < end:
        v = shift = 0
        while True:
            b = data[i]; i += 1
            v |= (b & 0x7f) << shift; shift += 7
            if not b & 0x80:
                break
        if v == 0:
            break
        addr += v
        out.append(addr)
    return out


def sect(segs, seg, name):
    for s in segs:
        if s["name"] == seg:
            for x in s["sects"]:
                if x["name"] == name:
                    return x
    return None


def seg_of(segs, name):
    return next((s for s in segs if s["name"] == name), None)


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    src = args[0]
    out = args[args.index("-o") + 1] if "-o" in args else src
    data = open(src, "rb").read()
    segs, info = parse(data)
    if seg_of(segs, "__KLX18"):
        sys.exit(f"{src}: its veneer pool is above the image (__KLX18); not wrapped")
    text, dseg = seg_of(segs, "__TEXT"), seg_of(segs, "__DATA")
    stat, pool = sect(segs, "__TEXT", "__klstat"), sect(segs, "__TEXT", "__klx18")
    elf, code, const = sect(segs, "__TEXT", "__klelf"), sect(segs, "__TEXT", "__kltext"), sect(segs, "__TEXT", "__klconst")
    kldata = sect(segs, "__DATA", "__kldata")
    if not (text and stat and elf):
        sys.exit(f"{src}: not a klepton-ld guest (no __klstat/__klelf)")
    POOL_AT = 0x4000
    base = elf["addr"]
    if base < POOL_AT or (pool and pool["addr"] != POOL_AT):
        sys.exit(f"{src}: unexpected layout (image at {base:#x}, pool at {pool and pool['addr']:#x})")
    text_end = dseg["vmaddr"] if dseg else text["vmaddr"] + text["vmsize"]
    if dseg and dseg["vmaddr"] % 0x4000:
        sys.exit(f"{src}: __DATA at {dseg['vmaddr']:#x} is not 16 KB aligned")
    fileof = lambda va: text["fileoff"] + (va - text["vmaddr"])      # __TEXT: vmaddr 0 = file 0
    name = os.path.basename(info["install_name"]).replace("-", "_").replace(".", "_").replace("+", "x")
    code_lo = code["addr"] if code else (const["addr"] if const else elf["addr"] + elf["size"])
    code_hi = code["addr"] + code["size"] if code else code_lo
    starts = sorted(a for a in function_starts(data, info.get("fs")) if code_lo <= a < code_hi)
    lines = [f'# klepton_ldwrap.py: {os.path.basename(src)} relinked by ld at its own addresses']
    inc = lambda lo, hi: f'\t.incbin "{SRC}", {fileof(lo)}, {hi - lo}' if hi > lo else ""
    # ld puts a segment's instruction sections before its other sections (input order within each group), so the
    # pieces that must be contiguous from 0x4000 up -- pool, image head, code -- are all instruction sections, and
    # the read-only tail and the record follow as plain sections. The record takes the last bytes of the zero
    # padding before __DATA, so __TEXT still ends exactly where klepton-ld ended it.
    STAT_AT = text_end - 64
    img_end = elf["addr"] + elf["size"]
    for x in (code, const):
        if x: img_end = max(img_end, x["addr"] + x["size"])
    if STAT_AT < img_end or any(data[fileof(STAT_AT):fileof(text_end)]):
        sys.exit(f"{src}: no zero padding before __DATA for the record")
    lines += ["\t.section __TEXT,__klx18,regular,pure_instructions", "\t.p2align 14", inc(POOL_AT, base)]
    lines += ["\t.section __TEXT,__klelf,regular,pure_instructions", f"\t.globl _klepton_guest_{name}",
              f"_klepton_guest_{name}:", inc(base, code_lo)]
    if code_hi > code_lo:
        lines += ["\t.section __TEXT,__kltext,regular,pure_instructions"]
        cuts = [code_lo] + [a for a in starts if a > code_lo] + [code_hi]
        for k in range(len(cuts) - 1):
            lines += [f"_kl_{name}_f{cuts[k]:x}:", inc(cuts[k], cuts[k + 1])]
    lines += ["\t.section __TEXT,__klconst", inc(code_hi, STAT_AT)]
    lines += ["\t.section __TEXT,__klstat", "\t.p2align 3", inc(stat["addr"], stat["addr"] + stat["size"])]
    if dseg:
        dfile = dseg["filesize"]
        lines += ["\t.section __DATA,__kldata", "\t.p2align 14",
                  f'\t.incbin "{SRC}", {dseg["fileoff"]}, {dfile}' if dfile else "\t.space 1"]
        bss = dseg["vmsize"] - dfile
        if bss > 0:
            lines += [f"\t.zerofill __DATA,__klbss,_kl_{name}_bss,{bss},0"]
    asm = "\n".join(l for l in lines if l) + "\n"

    sdk, triple_os = PLATFORM[info["platform"]]
    target = f"arm64-apple-{triple_os}{info['minos']}" + ("-simulator" if info["platform"] == 12 else "")
    with tempfile.TemporaryDirectory() as td:
        s_path = os.path.join(td, "wrap.s")
        open(s_path, "w").write(asm.replace(SRC, os.path.abspath(src)))
        tmp_out = os.path.join(td, "out.dylib")
        r = subprocess.run(["xcrun", "--sdk", sdk, "clang", "-target", target, "-dynamiclib", "-nostdlib",
                            "-lSystem", "-install_name", info["install_name"], "-o", tmp_out, s_path],
                           capture_output=True, text=True)
        if os.environ.get("KLWRAP_KEEP"):
            import shutil
            shutil.copy(s_path, out + ".wrap.s")
        if r.returncode:
            sys.exit(f"{src}: link failed\n{r.stderr[-2000:]}")
        if os.environ.get("KLWRAP_KEEP"):
            shutil.copy(tmp_out, out + ".linked")
        new = open(tmp_out, "rb").read()
        nsegs, _ = parse(new)
        # every address the runtime relies on must be where klepton-ld put it
        want = [("__TEXT", "__klelf", elf["addr"])]
        if pool: want.append(("__TEXT", "__klx18", POOL_AT))
        if code: want.append(("__TEXT", "__kltext", code["addr"]))
        want.append(("__TEXT", "__klstat", STAT_AT))
        if kldata: want.append(("__DATA", "__kldata", dseg["vmaddr"]))
        for sg, sn, addr in want:
            got = sect(nsegs, sg, sn)
            if not got or got["addr"] != addr:
                sys.exit(f"{src}: ld placed {sg},{sn} at {got and hex(got['addr'])}, not {addr:#x}")
        nd = seg_of(nsegs, "__DATA")
        if dseg and (not nd or nd["vmaddr"] != dseg["vmaddr"] or nd["vmaddr"] + nd["vmsize"] < dseg["vmaddr"] + dseg["vmsize"]):
            sys.exit(f"{src}: ld's __DATA {nd and hex(nd['vmaddr'])}+{nd and hex(nd['vmsize'])} does not cover "
                     f"{dseg['vmaddr']:#x}+{dseg['vmsize']:#x}")
        # the bytes, by address: everything from the pool to the end of __TEXT, and the writable image
        def at(buf, sg, va, n):
            for x in sg:
                if x["vmaddr"] <= va and va + n <= x["vmaddr"] + x["filesize"]:
                    o = x["fileoff"] + va - x["vmaddr"]
                    return buf[o:o + n]
            return None
        ranges = [(POOL_AT, STAT_AT - POOL_AT)] + ([(dseg["vmaddr"], dseg["filesize"])] if dseg and dseg["filesize"] else [])
        if at(data, segs, stat["addr"], stat["size"]) != at(new, nsegs, STAT_AT, stat["size"]):
            sys.exit(f"{src}: the __klstat record differs after linking")
        for va, n in ranges:
            if at(data, segs, va, n) != at(new, nsegs, va, n):
                sys.exit(f"{src}: bytes at {va:#x}+{n:#x} differ after linking")
        open(out + ".ldwrap", "wb").write(new)
        os.chmod(out + ".ldwrap", 0o755)
        os.replace(out + ".ldwrap", out)
    print(f"klepton_ldwrap: {os.path.basename(src)} -> {out} ({len(new)} bytes, {len(starts)} function symbols, {target})")


SRC = "@SRC@"
if __name__ == "__main__":
    main()
