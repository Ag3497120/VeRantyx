"""Parse a decompressed VCS v64 (Vulkan) static-combo blob. A port of the reader in VRF (ValveResourceFormat,
MIT): VfxStaticComboData and its parts. Returns the shader files (bytecode offset, size) and, per dynamic combo, which
render state: rasterizer, depth-stencil and blend descriptors."""
import struct

class R:
    def __init__(s, b): s.b, s.p = b, 0
    def u8(s): v = s.b[s.p]; s.p += 1; return v
    def i16(s): v = struct.unpack_from("<h", s.b, s.p)[0]; s.p += 2; return v
    def u16(s): v = struct.unpack_from("<H", s.b, s.p)[0]; s.p += 2; return v
    def i32(s): v = struct.unpack_from("<i", s.b, s.p)[0]; s.p += 4; return v
    def i64(s): v = struct.unpack_from("<q", s.b, s.p)[0]; s.p += 8; return v
    def skip(s, n): s.p += n
    def cstr(s): e = s.b.index(b"\0", s.p); v = s.b[s.p:e]; s.p = e + 1; return v

def var_array(r, reg=True):
    n = r.i32(); r.i32(); r.i32(); r.skip(n * (4 if reg else 2))

CONST_SIZE = {1: 4, 5: 4, 9: 1, 2: 8, 3: 12, 4: 16}
def attribute(r):
    r.cstr(); r.skip(4); t = r.u8(); binding = r.i16()
    if binding != -1: return
    n = r.i32()
    if n > 0: r.skip(n); return
    if t == 41: r.cstr(); return
    r.skip(CONST_SIZE[t])

def parse(b, prog, vcs_version=64):
    r = R(b)
    var_array(r, prog != "features")
    for _ in range(r.i16()): attribute(r)
    if prog in ("vs", "features"):
        r.skip(2 * r.i16())
    for _ in range(r.u16()): var_array(r, True)
    r.skip(2 * r.u16())
    r.i32(); r.u8()
    if vcs_version >= 66: r.u8()
    nfiles = r.i32()
    if vcs_version >= 60: r.u8()
    files = []
    for _ in range(nfiles):
        start = r.p; size = r.i32()
        code = None
        if size > 0:
            r.i32(); bsz = r.i32(); code = (r.p, bsz); r.skip(bsz)
            am = r.i32(); r.skip(max(am, 0))
        files.append(code)
        r.p = start + 4 + max(size, 0) + 16  # then the file's 16-byte MD5
    states = []
    for _ in range(r.i32()):
        dyn = r.i64(); sf = r.i32(); r.i32()
        st = None
        if prog == "ps":
            hr, hd, hb = (not r.u8()), (not r.u8()), (not r.u8())
            st = {}
            if hr: st["rast"] = b[r.p:r.p + 16]; r.skip(16)
            if hd: st["depth"] = b[r.p:r.p + 20]; r.skip(20)
            if hb: st["blend"] = b[r.p:r.p + 75]; r.skip(75)
        elif prog == "hs":
            r.skip(1)
        states.append((dyn, sf, st))
    assert r.p == len(b), (r.p, len(b))
    return files, states
