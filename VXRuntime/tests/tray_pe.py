#!/usr/bin/env python3
"""Writes made-up PE files for tests/tray.c's ExtractIconEx checks, each with one icon group (one 16x16 32-bpp picture,
blue with a transparent outer ring, the same as tests/tray_ico.py's) laid out in a way a file on disk may have it:

    tray_pe_hdr.dll     the resources in the header area (below SizeOfHeaders, in no section), which the PE format
                        allows: the loader maps the headers at RVA 0, so the icon is there
    tray_pe_nosec.dll   the resource directory at an RVA in no section and past the headers: no resources
    tray_pe_nodata.dll  the directory in a section, the group's data at an RVA in no section: a group without data
    tray_pe_zero.dll    the group named by a string at offset 0 of the directory and its data entry at offset 0
                        (the root directory's header read as one: RVA 0, the size its time stamp): no icon

Formats: Microsoft's "PE Format" (headers, section table, .rsrc) and "Icons" (GRPICONDIR).
usage: tray_pe.py <out dir>"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tray_ico import dib

RT_ICON, RT_GROUP_ICON, LANG = 3, 14, 0x409
SUBDIR = 0x80000000


def directory(entries, stamp=0):
    """A resource directory: its 16-byte header (named entries first, then ids) and 8-byte entries."""
    named = sum(1 for name, _ in entries if name & SUBDIR)
    return struct.pack("<IIHHHH", 0, stamp, 0, 0, named, len(entries) - named) + b"".join(struct.pack("<II", n, o) for n, o in entries)


def resources(base, icon, group, group_rva=None, zero=False):
    """A resource tree at RVA base: RT_ICON 1 and RT_GROUP_ICON 1 (with zero: named by the string at offset 0 and its
    data entry at offset 0, the root's time stamp 0x40)."""
    root, t_icon, t_group, l_icon, l_group, d_icon, d_group, data = 0, 32, 56, 80, 104, 128, 144, 160
    group_at = data + (len(icon) + 7) // 8 * 8
    out = directory([(RT_ICON, SUBDIR | t_icon), (RT_GROUP_ICON, SUBDIR | t_group)], stamp=0x40 if zero else 0)
    out += directory([(1, SUBDIR | l_icon)])
    out += directory([(SUBDIR | 0 if zero else 1, SUBDIR | l_group)])
    out += directory([(LANG, d_icon)])
    out += directory([(LANG, 0 if zero else d_group)])
    out += struct.pack("<IIII", base + data, len(icon), 0, 0)
    out += struct.pack("<IIII", group_rva if group_rva is not None else base + group_at, len(group), 0, 0)
    assert len(out) == data
    out += icon.ljust(group_at - data, b"\0") + group
    return out


def pe(headers_size, sections, rsrc_rva, rsrc_size, header_bytes=b"", header_at=0):
    """A PE32+ DLL: sections are (name, rva, raw offset, bytes); header_bytes go at header_at, inside the headers."""
    image_size = max([0x1000] + [rva + (len(b) + 0xfff) // 0x1000 * 0x1000 for _, rva, _, b in sections])
    dos = b"MZ" + bytes(0x3a) + struct.pack("<I", 0x40)
    coff = b"PE\0\0" + struct.pack("<HHIIIHH", 0x8664, len(sections), 0, 0, 0, 240, 0x2022)
    opt = struct.pack("<HBBIIIII", 0x20b, 14, 0, 0, 0, 0, 0, 0x1000)        # magic .. BaseOfCode
    opt += struct.pack("<QIIHHHHHHIIIIHHQQQQII", 0x180000000, 0x1000, 0x200, 6, 0, 0, 0, 6, 0, 0,
                       image_size, headers_size, 0, 2, 0x160, 0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
    dirs = [(0, 0)] * 16
    dirs[2] = (rsrc_rva, rsrc_size)
    opt += b"".join(struct.pack("<II", r, n) for r, n in dirs)
    assert len(opt) == 240
    table = b"".join(struct.pack("<8sIIIIIIHHI", name, len(b), rva, len(b), raw, 0, 0, 0, 0, 0x40000040)
                     for name, rva, raw, b in sections)
    head = bytearray((dos + coff + opt + table).ljust(headers_size, b"\0"))
    head[header_at:header_at + len(header_bytes)] = header_bytes
    assert len(head) == headers_size
    out = bytearray(head)
    for _, _, raw, b in sections:
        out = out.ljust(raw, b"\0")
        out[raw:raw + len(b)] = b
    return bytes(out)


def main():
    rows = [b"".join(struct.pack("<BBBB", 0xff, 0, 0, 0xff) if 0 < x < 15 and 0 < y < 15 else bytes(4) for x in range(16)) for y in range(16)]
    icon = dib(16, 16, 32, rows)
    group = struct.pack("<HHH", 0, 1, 1) + struct.pack("<BBBBHHIH", 16, 16, 0, 0, 1, 32, len(icon), 1)
    text = (".text\0\0\0".encode(), 0x1000, 0x1000, b"\xc3".ljust(0x200, b"\0"))
    out = sys.argv[1]
    files = {}
    # In the headers: the tree at RVA (and file offset) 0x400, SizeOfHeaders 0x1000; one section after them.
    tree = resources(0x400, icon, group)
    files["tray_pe_hdr.dll"] = pe(0x1000, [text], 0x400, len(tree), tree, 0x400)
    # In a section at 0x1000 (file offset 0x400), the variants.
    tree = resources(0x1000, icon, group)
    rsrc = lambda t: (".rsrc\0\0\0".encode(), 0x1000, 0x400, t.ljust((len(t) + 0x1ff) // 0x200 * 0x200, b"\0"))
    files["tray_pe_nosec.dll"] = pe(0x400, [rsrc(tree)], 0x8000, len(tree))
    nodata = resources(0x1000, icon, group, group_rva=0x9000)
    files["tray_pe_nodata.dll"] = pe(0x400, [rsrc(nodata)], 0x1000, len(nodata))
    zero = resources(0x1000, icon, group, zero=True)
    files["tray_pe_zero.dll"] = pe(0x400, [rsrc(zero)], 0x1000, len(zero))
    for name, data in files.items():
        with open(os.path.join(out, name), "wb") as f:
            f.write(data)


if __name__ == "__main__":
    main()
