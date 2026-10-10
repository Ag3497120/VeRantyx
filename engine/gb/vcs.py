"""Source 2 compiled shader files (.vcs, Vulkan): each static combo is an LZMA blob holding the SPIR-V modules of
its dynamic combos. Only what Glovebox needs: find the static-combo table, decompress one combo, find SPIR-V."""
import lzma
import struct

SPIRV_MAGIC = b"\x03\x02\x23\x07"
_LZMA_MAGIC = 0x414D5A4C  # "LZMA"


def static_table(d):
    """{static combo id: file offset} or None (a *_features.vcs file has no code). The table sits just before a
    field holding the file's length and is found by its shape: n, n int64 ids, n int32 offsets."""
    key = struct.pack("<i", len(d))
    q = d.find(key)
    while q >= 0:
        for n in range(1, 50000):
            p = q - 12 * n - 4
            if p < 0:
                break
            if struct.unpack_from("<i", d, p)[0] == n and struct.unpack_from("<i", d, p + 4 + 8 * n)[0] == q + 4:
                ids = struct.unpack_from(f"<{n}q", d, p + 4)
                offs = struct.unpack_from(f"<{n}i", d, p + 4 + 8 * n)
                return dict(zip(ids, offs))
        q = d.find(key, q + 1)
    return None


def static_blob(d, off):
    _, magic, usize, csize = struct.unpack_from("<4i", d, off)
    if magic != _LZMA_MAGIC:
        raise ValueError(f"static combo at {off}: not LZMA ({magic:#x})")
    props = d[off + 16 : off + 21]
    lc, lp, pb = props[0] % 9, (props[0] // 9) % 5, props[0] // 45
    dict_size = struct.unpack_from("<I", props, 1)[0]
    dec = lzma.LZMADecompressor(
        lzma.FORMAT_RAW,
        filters=[{"id": lzma.FILTER_LZMA1, "dict_size": dict_size, "lc": lc, "lp": lp, "pb": pb}],
    )
    return dec.decompress(d[off + 21 : off + 21 + csize], max_length=usize)


def spirv_starts(blob):
    pos = blob.find(SPIRV_MAGIC)
    while pos >= 0:
        yield pos
        pos = blob.find(SPIRV_MAGIC, pos + 4)
