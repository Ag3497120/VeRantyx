#!/usr/bin/env python3
"""Writes a made-up icon file for tests/tray.c (built into its resources too, tests/tray.rc): three pictures of three
kinds, as icon files carry them.

    16x16  32 bpp DIB with alpha: blue, its outer ring of pixels transparent
    32x32   8 bpp DIB with a palette and an AND mask: red, its four 8x8 corners masked out
    48x48  PNG (RGBA): green, its left half transparent

usage: tray_ico.py <out.ico>"""
import struct, sys, zlib


def dib(w, h, bpp, rows, palette=b"", mask_rows=None):
    """A DIB of an icon: the header counts the color rows and the mask's; rows bottom-up, padded to 4 bytes."""
    stride = ((w * bpp + 31) // 32) * 4
    mstride = ((w + 31) // 32) * 4
    colors = b"".join(r.ljust(stride, b"\0") for r in reversed(rows))
    masks = b"".join((m or bytes(mstride)).ljust(mstride, b"\0") for m in reversed(mask_rows or [None] * h))
    header = struct.pack("<IiiHHIIiiII", 40, w, 2 * h, 1, bpp, 0, len(colors) + len(masks), 0, 0, len(palette) // 4, 0)
    return header + palette + colors + masks


def png(w, h, pixel):
    raw = b"".join(b"\0" + b"".join(bytes(pixel(x, y)) for x in range(w)) for y in range(h))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")


def main():
    # 16x16, 32 bpp BGRA: blue inside, transparent on the border.
    rows16 = [b"".join(struct.pack("<BBBB", 0xff, 0, 0, 0xff) if 0 < x < 15 and 0 < y < 15 else bytes(4) for x in range(16)) for y in range(16)]
    e16 = dib(16, 16, 32, rows16)
    # 32x32, 8 bpp: palette 0 black, 1 red; every pixel red, the corners masked (1 = transparent).
    palette = struct.pack("<4B", 0, 0, 0, 0) + struct.pack("<4B", 0, 0, 0xff, 0) + bytes(4 * 254)
    rows32 = [bytes([1] * 32) for _ in range(32)]
    def mask_row(y):
        bits = [1 if (x < 8 or x >= 24) and (y < 8 or y >= 24) else 0 for x in range(32)]
        return bytes(int("".join(map(str, bits[i:i + 8])), 2) for i in range(0, 32, 8))
    e32 = dib(32, 32, 8, rows32, palette, [mask_row(y) for y in range(32)])
    # 48x48 PNG: green, the left half transparent.
    e48 = png(48, 48, lambda x, y: (0, 0xff, 0, 0xff) if x >= 24 else (0, 0, 0, 0))
    entries = [(16, 16, 0, 32, e16), (32, 32, 0, 8, e32), (48, 48, 0, 32, e48)]
    out = struct.pack("<HHH", 0, 1, len(entries))
    offset = 6 + 16 * len(entries)
    for w, h, colors, bpp, data in entries:
        out += struct.pack("<BBBBHHII", w, h, colors, 0, 1, bpp, len(data), offset)
        offset += len(data)
    out += b"".join(e[4] for e in entries)
    with open(sys.argv[1], "wb") as f:
        f.write(out)


if __name__ == "__main__":
    main()
