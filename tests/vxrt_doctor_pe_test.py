#!/usr/bin/env python3
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import vxrt_doctor


def put16(data, offset, value):
    struct.pack_into("<H", data, offset, value)


def put32(data, offset, value):
    struct.pack_into("<I", data, offset, value)


def image(managed=True):
    data = bytearray(0x600)
    data[:2] = b"MZ"
    put32(data, 0x3c, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    coff, opt, opt_size = 0x84, 0x98, 0xe0
    put16(data, coff, 0x014c)
    put16(data, coff + 2, 1)
    put16(data, coff + 16, opt_size)
    put16(data, opt, 0x10b)
    put32(data, opt + 28, 0x400000)
    put32(data, opt + 56, 0x2000)
    put32(data, opt + 60, 0x200)
    put32(data, opt + 92, 16)
    if managed:
        put32(data, opt + 96 + 14 * 8, 0x1000)
        put32(data, opt + 96 + 14 * 8 + 4, 72)
    section = opt + opt_size
    put32(data, section + 8, 0x400)
    put32(data, section + 12, 0x1000)
    put32(data, section + 16, 0x400)
    put32(data, section + 20, 0x200)
    if managed:
        put32(data, 0x200, 72)
        put16(data, 0x204, 2)
        put16(data, 0x206, 5)
        put32(data, 0x208, 0x1100)
        put32(data, 0x20c, 0x80)
        put32(data, 0x210, 3)
        put32(data, 0x214, 0x06000001)
        put32(data, 0x300, 0x424a5342)
        put16(data, 0x304, 1)
        put16(data, 0x306, 1)
        put32(data, 0x30c, 12)
        data[0x310:0x31b] = b"v4.0.30319\0"
    return data


def inspect(data):
    with tempfile.NamedTemporaryFile() as f:
        f.write(data)
        f.flush()
        return vxrt_doctor.pe_details(f.name)


with tempfile.TemporaryDirectory() as directory:
    launcher = os.path.join(directory, "EDLaunch.exe")
    with open(launcher + ".config", "w", encoding="utf-8") as f:
        f.write('<configuration><startup><supportedRuntime version="v4.0" sku=".NETFramework,Version=v4.8" /></startup></configuration>')
    runtime = vxrt_doctor.managed_runtime_config(launcher)
    assert runtime == {"file": "EDLaunch.exe.config", "version": "v4.0", "sku": ".NETFramework,Version=v4.8"}


managed = inspect(image())
assert managed and managed["machine"] == "x86" and managed["format"] == "PE32"
assert managed["managed"] and managed["metadata_version"] == "v4.0.30319"
assert managed["il_only"] and managed["requires_32bit"] and not managed["prefers_32bit"]

native = inspect(image(False))
assert native and native["machine"] == "x86" and native["format"] == "PE32" and not native["managed"]

bad = image()
put32(bad, 0x3c, 0xfffffff0)
assert inspect(bad) is None
bad = image()
bad[0x300] = 0
assert inspect(bad) is None
print("vxrt doctor PE/CLR inspection: PE32 native, IL-only flags, config and malformed headers: ok")
