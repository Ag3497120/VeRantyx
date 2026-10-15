#!/usr/bin/env python3
"""vxrt redist: the Microsoft runtime DLLs Windows games and SteamVR load (msvcp140, vcruntime140, msvcp100,
d3dcompiler_43, ...), taken from the redistributable installers your own Steam installs already carry
(_CommonRedist/vcredist/<year>/VC_redist.x64.exe, _CommonRedist/DirectX/*.cab) and from the copies apps ship
beside their executables, into one folder that VXRuntime loads them from (VXRT_DLL_PATH): no Windows, no Wine
prefix. Nothing is downloaded and nothing leaves this Mac; the folder is yours, like the installs it comes from.

    tools/vxrt_redist.py collect                  # from ~/Verantyx_VR_Drive/vxrt_steam, vxrt_library, and --from
    tools/vxrt_redist.py collect --from "<a Steam library or app folder>" ...
    tools/vxrt_redist.py list                     # what the folder holds: name, version, where it came from

An installer is opened without running it: a WiX bundle (VC_redist) is a Windows executable with cabinets inside
it, and cabinets hold the files (bsdtar, which comes with macOS, reads cabinets). Only 64-bit (x86-64) DLLs whose
names are on the list below are kept; when several copies of one DLL turn up, the highest file version wins.
Output: ~/Verantyx_VR_Drive/vxrt_redist (VXRT_REDIST changes it), with manifest.json (name, version, sha256, source).
"""
import argparse, fnmatch, glob, hashlib, json, os, re, shutil, struct, subprocess, sys, tempfile

DRIVE = os.path.expanduser(os.environ.get("VERANTYX_VR_DRIVE", "~/Verantyx_VR_Drive"))
OUT = os.environ.get("VXRT_REDIST", os.path.join(DRIVE, "vxrt_redist"))
SOURCES = [os.path.join(DRIVE, "vxrt_steam"), os.path.join(DRIVE, "vxrt_library")]
WANTED = ["msvcp1*.dll", "msvcp140_*.dll", "msvcr1*.dll", "vcruntime140*.dll", "concrt140.dll", "vcomp1*.dll",
          "vccorlib1*.dll", "vcamp1*.dll", "mfc1*.dll", "mfcm1*.dll", "atl1*.dll",
          "d3dcompiler_4*.dll", "d3dx9_*.dll", "d3dx10*.dll", "d3dx11_*.dll", "d3dcsx_4*.dll",
          "xinput1_*.dll", "xinput9_1_0.dll", "x3daudio1_*.dll", "xapofx1_*.dll", "xaudio2_*.dll", "xactengine*.dll"]

def wanted(name):
    return any(fnmatch.fnmatch(name.lower(), p) for p in WANTED)

def pe_info(path):
    """(machine, file version tuple or None, original file name or None) of a PE file; None if it is not one."""
    try:
        b = open(path, "rb").read()
    except OSError:
        return None
    if b[:2] != b"MZ" or len(b) < 0x40:
        return None
    pe = struct.unpack_from("<I", b, 0x3C)[0]
    if b[pe:pe + 4] != b"PE\0\0":
        return None
    machine = struct.unpack_from("<H", b, pe + 4)[0]
    ver = None
    i = b.find(b"\xbd\x04\xef\xfe")                               # VS_FIXEDFILEINFO
    if i >= 0:
        ms, ls = struct.unpack_from("<II", b, i + 8)
        ver = (ms >> 16, ms & 0xFFFF, ls >> 16, ls & 0xFFFF)
    name = None
    key = "OriginalFilename".encode("utf-16-le")
    j = b.find(key)
    if j >= 0:
        k = j + len(key)
        while k < len(b) - 1 and b[k:k + 2] == b"\0\0": k += 2   # (padding to the value)
        end = b.find(b"\0\0", k)
        while end > 0 and (end - k) % 2: end = b.find(b"\0\0", end + 1)
        try: name = b[k:end].decode("utf-16-le").strip() or None
        except UnicodeDecodeError: name = None
    return machine, ver, name

def cabinets_in(path):
    """(offset, size) of the cabinets inside a file (a WiX bundle carries its payloads that way)."""
    b = open(path, "rb").read()
    out, i = [], 0
    while True:
        i = b.find(b"MSCF\0\0\0\0", i)
        if i < 0: return out
        size = struct.unpack_from("<I", b, i + 8)[0]
        if 0 < size <= len(b) - i: out.append((i, size))
        i += 4

def extract_cab(data_path, into):
    os.makedirs(into, exist_ok=True)
    r = subprocess.run(["bsdtar", "-xf", data_path, "-C", into], capture_output=True)
    return r.returncode == 0

def unpack(path, work, depth=0):
    """Every file inside an installer or cabinet, nested cabinets opened too (files that are themselves cabinets)."""
    files = []
    os.makedirs(work, exist_ok=True)
    b = open(path, "rb")
    head = b.read(4); b.close()
    cabs = [(0, os.path.getsize(path))] if head == b"MSCF" else cabinets_in(path) if head[:2] == b"MZ" else []
    for n, (off, size) in enumerate(cabs):
        piece = os.path.join(work, f"d{depth}_{n}.cab")
        with open(path, "rb") as f:
            f.seek(off); open(piece, "wb").write(f.read(size))
        into = os.path.join(work, f"d{depth}_{n}")
        if not extract_cab(piece, into):
            continue
        for dp, _, fn in os.walk(into):
            for f in fn:
                p = os.path.join(dp, f)
                with open(p, "rb") as fh: sig = fh.read(4)
                if sig == b"MSCF" and depth < 3:
                    files += unpack(p, os.path.join(work, f"n{depth}_{n}_{len(files)}"), depth + 1)
                else:
                    files.append(p)
    return files

def dll_name(path, info):
    """The DLL's own name: its version resource's original file name when the file's name holds it, else the file
    name without an installer's decoration (msvcp140.dll_amd64, F_CENTRAL_msvcp120_x64 -> msvcp1x0.dll)."""
    _, _, orig = info
    base = os.path.basename(path).lower()
    if orig and orig.lower().endswith(".dll") and orig.lower()[:-4] in base:
        return orig.lower()
    m = re.match(r"(?:f_central_)?(.+?)(?:\.dll)?(?:_(?:system_)?(?:amd64|x64|arm64))?$", base)
    name = m.group(1) if m else base
    return name if name.endswith(".dll") else name + ".dll"

def candidates(roots):
    """(name, version, path to the file, where it came from) of every wanted x86-64 DLL found under roots."""
    found = []
    work = tempfile.mkdtemp(prefix="vxrt_redist_")
    try:
        for root in roots:
            if not os.path.isdir(root): continue
            for dp, dn, fn in os.walk(root):
                for f in fn:
                    p = os.path.join(dp, f)
                    low = f.lower()
                    if "_commonredist" in dp.lower() and (low.endswith(".exe") or low.endswith(".cab")) and \
                       (re.search(r"vc_?redist.*x64|x64\.cab$|_x64\.cab$", low) or low.endswith("_x64.cab")):
                        for inner in unpack(p, os.path.join(work, str(len(found)) + "_" + re.sub(r"\W", "_", low))):
                            info = pe_info(inner)
                            if info and info[0] == 0x8664 and wanted(dll_name(inner, info)):
                                keep = os.path.join(work, "keep", f"{len(found)}_{os.path.basename(inner)}")
                                os.makedirs(os.path.dirname(keep), exist_ok=True); shutil.copy(inner, keep)
                                found.append((dll_name(inner, info), info[1], keep, os.path.relpath(p, DRIVE) + " (installer)"))
                    elif low.endswith(".dll") and wanted(low):
                        info = pe_info(p)
                        if info and info[0] == 0x8664:
                            found.append((low, info[1], p, os.path.relpath(p, DRIVE)))
        return found, work
    except Exception:
        shutil.rmtree(work, ignore_errors=True); raise

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""): h.update(chunk)
    return h.hexdigest()

def collect(roots):
    found, work = candidates(roots)
    try:
        best = {}
        for name, ver, path, src in found:
            if name not in best or (ver or (0,)) > (best[name][0] or (0,)):
                best[name] = (ver, path, src)
        os.makedirs(OUT, exist_ok=True)
        manifest = {}
        for name, (ver, path, src) in sorted(best.items()):
            dest = os.path.join(OUT, name)
            tmp = dest + ".tmp"
            shutil.copyfile(path, tmp); os.replace(tmp, dest)
            manifest[name] = {"version": ".".join(map(str, ver)) if ver else None, "sha256": sha256(dest), "source": src}
        json.dump({"roots": [os.path.relpath(r, DRIVE) if r.startswith(DRIVE) else r for r in roots], "dlls": manifest},
                  open(os.path.join(OUT, "manifest.json"), "w"), indent=1)
        return manifest
    finally:
        shutil.rmtree(work, ignore_errors=True)

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("what", choices=["collect", "list"])
    ap.add_argument("--from", dest="roots", action="append", default=[], help="another Steam library or app folder to look in")
    a = ap.parse_args()
    if not shutil.which("bsdtar"):
        sys.exit("bsdtar is needed (it comes with macOS)")
    if a.what == "collect":
        m = collect(SOURCES + a.roots)
        for name, d in m.items():
            print(f"{name:28} {d['version'] or '?':18} {d['source']}")
        print(f"{len(m)} DLLs in {OUT}")
    else:
        path = os.path.join(OUT, "manifest.json")
        if not os.path.exists(path):
            sys.exit(f"nothing collected yet: tools/vxrt_redist.py collect")
        for name, d in json.load(open(path))["dlls"].items():
            print(f"{name:28} {d['version'] or '?':18} {d['source']}")

if __name__ == "__main__":
    main()
