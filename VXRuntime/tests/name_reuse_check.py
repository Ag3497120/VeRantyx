#!/usr/bin/env python3
"""A named object's hold left by a process that ended, under a process ID that a live process has now (macOS gives
process IDs out again): the name is free. Leaves such a hold (this script's own process ID, another start time) and
stale shared memory under the name, then makes the event with tests/name_reuse.c. Usage: name_reuse_check.py <vxrt>"""
import _posixshmem, mmap, os, struct, subprocess, sys

NAME = "vxrt-test made-up stale event"
OBJ_EVENT = 2

def shm_names(name, kind):
    h = 1469598103934665603 ^ kind
    for c in name.encode():
        h = ((h ^ c) * 1099511628211) & (2**64 - 1)
    obj = "/vxo.%016x" % h
    return obj, "/vxr." + obj[1:]

def put(name, size, fill):
    fd = _posixshmem.shm_open(name, os.O_RDWR | os.O_CREAT, 0o600)
    try:
        if os.fstat(fd).st_size == 0:
            os.ftruncate(fd, size)
        m = mmap.mmap(fd, size)
        fill(m)
        m.close()
    finally:
        os.close(fd)

def main():
    vxrt = sys.argv[1]
    obj, reg = shm_names(NAME, OBJ_EVENT)
    def registry(m):
        struct.pack_into("II", m, 0, 0, 0)
        struct.pack_into("i", m, 8, os.getpid())             # (alive, but not the process that held the name)
        struct.pack_into("Q", m, 8 + 4000, 1)                 # (its start time: 1970)
    put(reg, 8 + 4000 + 8000, registry)
    put(obj, 4096, lambda m: struct.pack_into("I", m, 0, 0x56584f42))
    r = subprocess.run([vxrt, "build/name_reuse.exe", NAME], capture_output=True, text=True, timeout=60)
    sys.stdout.write(r.stdout)
    for n in (obj, reg):
        try:
            _posixshmem.shm_unlink(n)
        except OSError:
            pass
    ok = r.returncode == 0
    print("name_reuse: a hold under a reused process ID ->", "ok" if ok else "FAILED")
    sys.exit(0 if ok else 1)

main()
