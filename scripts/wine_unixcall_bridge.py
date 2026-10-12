#!/usr/bin/env python3
"""Adds ntdll.__wine_unix_call to a Wine >= 8 ntdll.dll, so DLLs built for Wine 7's
unix-call interface (Apple's Game Porting Toolkit D3DMetal DLLs) load and work.

Wine 7 exported a function, __wine_unix_call(handle, code, args). Wine 8 replaced it
with __wine_unix_call_dispatcher, a pointer to the same kind of function. The added
export is 9 bytes of code in a new section,

    mov rax, [rip + __wine_unix_call_dispatcher]   ; the pointer Wine fills in
    jmp rax

and the export directory is rebuilt in that section with every original export (same
ordinals, same RVAs) plus the new name, sorted as the loader's binary search needs.
Nothing else in the image changes. The input must be Wine's own ntdll.dll (LGPL),
never a file from Apple's toolkit, which is used unmodified.

    wine_unixcall_bridge.py <ntdll.dll> [--check]
"""
import struct
import sys

import pefile

NEW_NAME = b"__wine_unix_call"
DISPATCHER = b"__wine_unix_call_dispatcher"
SECTION_NAME = b".vbridge"


def align(value, to):
    return (value + to - 1) // to * to


def exports_of(pe):
    return {s.name: s for s in pe.DIRECTORY_ENTRY_EXPORT.symbols if s.name}


def patch(path):
    pe = pefile.PE(path)
    if not pe.__data__[0x40:0x50].startswith(b"Wine builtin DLL"):
        raise SystemExit(f"{path} is not a Wine builtin DLL; refusing to patch it")
    exports = exports_of(pe)
    if NEW_NAME in exports:
        print(f"{path} already exports {NEW_NAME.decode()}")
        return
    if DISPATCHER not in exports:
        raise SystemExit(f"{path} has no {DISPATCHER.decode()} (Wine 7 or older does not need this bridge)")
    ed = pe.DIRECTORY_ENTRY_EXPORT
    if any(s.forwarder for s in ed.symbols):
        raise SystemExit("forwarded exports are not handled")

    oh = pe.OPTIONAL_HEADER
    sect_align, file_align = oh.SectionAlignment, oh.FileAlignment
    new_rva = align(oh.SizeOfImage, sect_align)

    # Code: mov rax, [rip+disp32]; jmp rax
    stub_rva = new_rva
    disp = exports[DISPATCHER].address - (stub_rva + 7)
    code = b"\x48\x8b\x05" + struct.pack("<i", disp) + b"\xff\xe0"

    # Export directory: functions indexed by ordinal - Base; the new one is appended.
    base = ed.struct.Base
    count = ed.struct.NumberOfFunctions
    functions = [0] * (count + 1)
    for s in ed.symbols:
        functions[s.ordinal - base] = s.address
    functions[count] = stub_rva
    named = sorted([(s.name, s.ordinal - base) for s in ed.symbols if s.name] + [(NEW_NAME, count)])
    dll_name = pe.get_string_at_rva(ed.struct.Name)

    dir_off = 16                                   # after the code, inside the section
    funcs_off = dir_off + 40
    names_off = funcs_off + 4 * len(functions)
    ords_off = names_off + 4 * len(named)
    strings_off = ords_off + 2 * len(named)
    strings = bytearray()
    dll_name_rva = new_rva + strings_off
    strings += dll_name + b"\0"
    name_rvas = []
    for name, _ in named:
        name_rvas.append(new_rva + strings_off + len(strings))
        strings += name + b"\0"
    size = strings_off + len(strings)

    body = bytearray(size)
    body[0:len(code)] = code
    struct.pack_into("<IIHHIIIIIII", body, dir_off,
                     ed.struct.Characteristics, ed.struct.TimeDateStamp, ed.struct.MajorVersion,
                     ed.struct.MinorVersion, dll_name_rva, base, len(functions), len(named),
                     new_rva + funcs_off, new_rva + names_off, new_rva + ords_off)
    struct.pack_into(f"<{len(functions)}I", body, funcs_off, *functions)
    struct.pack_into(f"<{len(named)}I", body, names_off, *name_rvas)
    struct.pack_into(f"<{len(named)}H", body, ords_off, *[o for _, o in named])
    body[strings_off:] = strings

    raw_size = align(size, file_align)
    virt_size = align(size, sect_align)
    data = bytearray(pe.__data__)
    raw_off = align(len(data), file_align)
    data += b"\0" * (raw_off - len(data)) + bytes(body) + b"\0" * (raw_size - size)

    # Section header after the last one (headers have room; checked here).
    last = pe.sections[-1]
    hdr_off = last.get_file_offset() + 40
    first_raw = min(s.PointerToRawData for s in pe.sections if s.PointerToRawData)
    if hdr_off + 40 > first_raw:
        raise SystemExit("no room for another section header")
    struct.pack_into("<8sIIIIIIHHI", data, hdr_off, SECTION_NAME.ljust(8, b"\0"),
                     size, new_rva, raw_size, raw_off, 0, 0, 0, 0,
                     0x60000020 | 0x40000040)          # code, initialized data, execute, read
    # NumberOfSections, SizeOfImage, export data directory.
    struct.pack_into("<H", data, pe.FILE_HEADER.get_file_offset() + 2, pe.FILE_HEADER.NumberOfSections + 1)
    struct.pack_into("<I", data, oh.get_file_offset() + 56, new_rva + virt_size)
    dd = oh.DATA_DIRECTORY[0]
    struct.pack_into("<II", data, dd.get_file_offset(), new_rva + dir_off, size - dir_off)
    with open(path, "wb") as f:
        f.write(data)
    print(f"Added {NEW_NAME.decode()} to {path} (section {SECTION_NAME.decode()} at RVA {new_rva:#x})")


def check(path):
    pe = pefile.PE(path)
    exports = exports_of(pe)
    names = [s.name for s in pe.DIRECTORY_ENTRY_EXPORT.symbols if s.name]
    ok = NEW_NAME in exports and DISPATCHER in exports and names == sorted(names)
    if ok:
        stub = pe.get_data(exports[NEW_NAME].address, 9)
        disp = struct.unpack("<i", stub[3:7])[0]
        ok = stub[:3] == b"\x48\x8b\x05" and stub[7:] == b"\xff\xe0" and \
            exports[NEW_NAME].address + 7 + disp == exports[DISPATCHER].address
    print(f"{path}: {len(exports)} exports, sorted={names == sorted(names)}, bridge={'ok' if ok else 'MISSING'}")
    return ok


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    if "--check" in sys.argv:
        sys.exit(0 if check(sys.argv[1]) else 1)
    before = len(exports_of(pefile.PE(sys.argv[1])))
    patch(sys.argv[1])
    if not check(sys.argv[1]) or len(exports_of(pefile.PE(sys.argv[1]))) not in (before, before + 1):
        raise SystemExit("verification failed")
