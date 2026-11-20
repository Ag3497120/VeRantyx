#!/usr/bin/env python3
"""Reproducer: Direct3D 1D / 1D-array texture sampling (sample_l, sample_b, sample_d, sample, with and without a
texel offset) must translate to MSL that Metal compiles.

Metal's texture1d / texture1d_array have only sample(sampler, coord[, array]): no level(), bias(), gradient or offset
overload. A pixel shader that does `sample_l ... t3.xxxx, s3, l(0)` on a Texture1D is common (lookup-table textures); the
translator emitted `t3.sample(s3, x, level(...))` and Metal rejected the shader ("did not compile").

No fxc/dxc is available here, so the shaders are assembled by hand (ps_4_0 token streams, a container with ISGN/OSGN
copied from tests/shaders/tex_ps.dxbc) in this script. Subject: hand-assembled fixtures through the offline
translator and the Metal compiler (build/dxbc2msl); it says nothing about the runtime.

usage: dxbc_tex1d_check.py [build/dxbc2msl]    (run from VXRuntime/; exit 0 = every variant translated and compiled and
                                                  no 1D sample carries an argument Metal does not have)
"""
import os, re, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "build", "dxbc2msl")

D1, D1A = 2, 7                               # resource dimension tokens: Texture1D, Texture1DArray
OP = dict(sample=0x45, sample_c=0x46, sample_l=0x48, sample_d=0x49, sample_b=0x4a)


def chunks(blob):
    n = struct.unpack_from("<I", blob, 28)[0]
    out = {}
    for i in range(n):
        off = struct.unpack_from("<I", blob, 32 + 4 * i)[0]
        out[blob[off:off + 4]] = blob[off + 8:off + 8 + struct.unpack_from("<I", blob, off + 4)[0]]
    return out


def container(parts):
    head = 32 + 4 * len(parts)
    offs, body = [], b""
    for tag, data in parts:
        offs.append(head + len(body))
        pad = data + b"\0" * (-len(data) % 4)
        body += tag + struct.pack("<I", len(pad)) + pad
    total = head + len(body)
    return b"DXBC" + b"\0" * 16 + struct.pack("<III", 1, total, len(parts)) + b"".join(struct.pack("<I", o) for o in offs) + body


def shader(dim, op, offset=0, level=None):
    """ps_4_0: o0 = t0.Sample*(s0, v1 ...) with the instruction `op` on a texture of dimension `dim`."""
    t = [0x0300005a, 0x00106000, 0,                                            # dcl_sampler s0
         0x04000058 | dim << 11, 0x00107000, 0, 0x5555,                        # dcl_resource t0 (float4)
         0x03000062 | 2 << 11, 0x001010f2, 1,                                  # dcl_input_ps linear v1.xyzw
         0x03000065, 0x001020f2, 0,                                            # dcl_output o0.xyzw
         0x02000068, 1]                                                        # dcl_temps 1
    body = [0x001000f2, 0, 0x00101e46, 1, 0x00107e46, 0, 0x00106000, 0]        # dst r0, v1.xyzw, t0.xyzw, s0
    if op == "sample_l":
        body += [0x00004001, struct.unpack("<I", struct.pack("<f", level or 0.0))[0]]   # l(level)
    elif op == "sample_b":
        body += [0x0010102a, 1]                                                # v1.z
    elif op == "sample_d":
        body += [0x00101e46, 1, 0x00101e46, 1]                                 # ddx, ddy
    head = OP[op] | (len(body) + 1 + (1 if offset else 0)) << 24
    ins = [head]
    if offset:                                                                 # extended opcode: sample controls
        ins[0] |= 0x80000000
        ins.append(1 | (offset & 0xf) << 9)
    ins += body
    t += ins + [0x05000036, 0x001020f2, 0, 0x00100e46, 0, 0x0100003e]          # mov o0, r0 ; ret
    words = [0x40, len(t) + 2] + t
    shdr = struct.pack("<%dI" % len(words), *words)
    src = chunks(open(os.path.join(HERE, "shaders", "tex_ps.dxbc"), "rb").read())
    isgn = bytearray(src[b"ISGN"])
    isgn[8 + 24 + 20:8 + 24 + 22] = b"\x0f\x0f"                               # TEXCOORD0: mask xyzw
    return container([(b"ISGN", bytes(isgn)), (b"OSGN", src[b"OSGN"]), (b"SHDR", shdr)])


VARIANTS = [
    # name, dim, op, offset, level, text the translated 1D sample call must not contain
    ("1d_sample_l_0 (the common shape)", D1, "sample_l", 0, 0.0),
    ("1d_sample_l_lod2", D1, "sample_l", 0, 2.0),
    ("1d_sample_b", D1, "sample_b", 0, None),
    ("1d_sample_d", D1, "sample_d", 0, None),
    ("1d_sample_l_offset", D1, "sample_l", 1, 0.0),
    ("1d_sample (control)", D1, "sample", 0, None),
    ("1darray_sample_l_0", D1A, "sample_l", 0, 0.0),
    ("1darray_sample_b", D1A, "sample_b", 0, None),
    ("1darray_sample_d", D1A, "sample_d", 0, None),
    ("1darray_sample_l_offset", D1A, "sample_l", -2, 0.0),
]


def main():
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        for (name, dim, op, offset, level), mode in [(v, m) for m in ("d3d11", "d3d12") for v in VARIANTS]:
            name = "%s [%s]" % (name, mode)
            path = os.path.join(tmp, name.split()[0] + ".dxbc")
            open(path, "wb").write(shader(dim, op, offset, level))
            r = subprocess.run([TOOL] + (["-d3d12"] if mode == "d3d12" else []) + [path], capture_output=True, text=True)
            calls = [l.strip() for l in r.stdout.splitlines() if re.search(r"\bt0\.sample\(|\.sample\(s0", l)]
            extra = [c for c in calls if re.search(r"level\(|bias\(|gradient|int2\(|int\d\(", c)]
            ok = r.returncode == 0 and len(calls) == 1 and not extra
            print("%-4s %-48s exit=%d  %s" % ("ok" if ok else "FAIL", name, r.returncode, calls[0] if calls else "(no sample call)"))
            if not ok:
                bad += 1
                msg = [l for l in r.stderr.splitlines() if "error" in l][:2]
                if msg:
                    print("       " + " | ".join(msg))
    print("%d of %d variants failed" % (bad, 2 * len(VARIANTS)))
    return 1 if bad else 0


sys.exit(main())
