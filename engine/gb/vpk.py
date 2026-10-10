"""Read-only access to a Valve pack (VPK v1/v2): list entries and read one file's bytes without extracting."""
import struct


class VPK:
    def __init__(self, dir_path):
        if not dir_path.endswith("_dir.vpk"):
            raise ValueError(f"{dir_path}: not a *_dir.vpk")
        self.dir_path = dir_path
        self.base = dir_path[: -len("_dir.vpk")]
        d = open(dir_path, "rb").read()
        sig, ver, tree = struct.unpack_from("<III", d, 0)
        if sig != 0x55AA1234:
            raise ValueError(f"{dir_path}: bad VPK signature")
        hdr = 12 if ver == 1 else 28
        self._inline_base = hdr + tree
        self._dir = d
        self.entries = {}
        off = hdr

        def rs():
            nonlocal off
            e = d.index(b"\0", off)
            s = d[off:e].decode("latin1")
            off = e + 1
            return s

        while True:
            ext = rs()
            if not ext:
                break
            while True:
                path = rs()
                if not path:
                    break
                while True:
                    name = rs()
                    if not name:
                        break
                    _crc, pre, arc, aoff, alen, _term = struct.unpack_from("<IHHIIH", d, off)
                    off += 18
                    full = (f"{path}/" if path.strip() else "") + f"{name}.{ext}"
                    self.entries[full] = (arc, aoff, alen, off, pre)
                    off += pre

    def read(self, name):
        arc, aoff, alen, poff, pre = self.entries[name]
        data = self._dir[poff : poff + pre]
        if alen:
            if arc == 0x7FFF:
                start = self._inline_base + aoff
                data += self._dir[start : start + alen]
            else:
                with open(f"{self.base}_{arc:03d}.vpk", "rb") as f:
                    f.seek(aoff)
                    data += f.read(alen)
        return data
