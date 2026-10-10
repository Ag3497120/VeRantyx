"""Fossilize stream archives (.foz): a 16-byte file header, then entries of a 40-character hex name (the tag is
characters 22-24), a payload header (stored size, format 1 = raw / 2 = deflate, CRC32 of the stored bytes or 0,
size after decompression) and the payload."""
import json
import struct
import zlib

TAG_SHADER_MODULE = "04"
FORMAT_RAW, FORMAT_DEFLATE = 1, 2


def read(path):
    """(file header, [(name, format, crc, size, stored bytes)])"""
    d = open(path, "rb").read()
    out, off = [], 16
    while off + 56 <= len(d):
        name = d[off : off + 40].decode()
        stored, fmt, crc, size = struct.unpack_from("<4I", d, off + 40)
        end = off + 56 + stored
        if end > len(d):
            break  # a torn tail (the recorder was killed mid-write): Fossilize skips it too
        out.append((name, fmt, crc, size, d[off + 56 : end]))
        off = end
    return d[:16], out


def payload(fmt, stored):
    return zlib.decompress(stored) if fmt == FORMAT_DEFLATE else stored


def entry_bytes(name, payload_bytes):
    """A raw (uncompressed) entry with its CRC, as Fossilize itself writes one."""
    crc = zlib.crc32(payload_bytes) & 0xFFFFFFFF
    n = len(payload_bytes)
    return name.encode() + struct.pack("<4I", n, FORMAT_RAW, crc, n) + payload_bytes


def split_module(p):
    """A shader module payload -> (its JSON header, SPIR-V bytes). The code follows the JSON's NUL as LEB128
    varints, one per 32-bit word."""
    j = p.index(b"\0")
    meta = json.loads(p[:j])
    (_, m), = meta["shaderModules"].items()
    start = j + 1 + m["varintOffset"]
    words, v, s = [], 0, 0
    for x in p[start : start + m["varintSize"]]:
        v |= (x & 0x7F) << s
        s += 7
        if not x & 0x80:
            words.append(v)
            v, s = 0, 0
    code = struct.pack(f"<{len(words)}I", *words)
    if len(code) != m["codeSize"]:
        raise ValueError(f"shader module decodes to {len(code)} bytes, header says {m['codeSize']}")
    return p[:j], code


def join_module(json_header, code):
    meta = json.loads(json_header)
    (_, m), = meta["shaderModules"].items()
    words = struct.unpack(f"<{len(code) // 4}I", code)
    enc = bytearray()
    for w in words:
        while True:
            b = w & 0x7F
            w >>= 7
            if w:
                enc.append(b | 0x80)
            else:
                enc.append(b)
                break
    if len(enc) != m["varintSize"] or m["varintOffset"] != 0:
        raise ValueError("shader module re-encodes to a different size")
    return json_header + b"\0" + bytes(enc)
