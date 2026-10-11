#!/usr/bin/env python3
"""Census of the Alyx Linux ARM64 build: dependencies, relocation types and the
symbols that are still unresolved after the game's own libraries and the
Debian runtime libraries resolve each other — i.e. what the host shim must
provide.

    NativeARM/tools/census.py <depot_546564 dir> [debian lib dir]

Needs macOS's objdump (LLVM), which reads ELF.
"""
import collections
import os
import re
import subprocess
import sys


def elf_files(directory):
    for name in sorted(os.listdir(directory)):
        path = os.path.join(directory, name)
        if os.path.isfile(path) and not os.path.islink(path) and not name.endswith(".py"):
            with open(path, "rb") as f:
                if f.read(4) == b"\x7fELF":
                    yield path


def dyn_symbols(path):
    out = subprocess.run(["objdump", "-T", path], capture_output=True, text=True).stdout
    defined, undefined = set(), set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 3 or not re.match(r"^[0-9a-f]{16}$", parts[0]):
            continue
        (undefined if "*UND*" in line else defined).add(parts[-1])
    return defined, undefined


def main():
    depot = sys.argv[1]
    debian = sys.argv[2] if len(sys.argv) > 2 else None
    libs = []
    for sub in ("game/bin/linuxsteamrtarm64", "game/hlvr/bin/linuxsteamrtarm64"):
        libs += list(elf_files(os.path.join(depot, sub)))
    if debian and os.path.isdir(debian):
        libs += list(elf_files(debian))

    needed, relocs = collections.Counter(), collections.Counter()
    defined, undefined = set(), collections.defaultdict(set)
    for path in libs:
        for m in re.finditer(r"NEEDED\s+(\S+)", subprocess.run(["objdump", "-p", path], capture_output=True, text=True).stdout):
            needed[m.group(1)] += 1
        relocs.update(re.findall(r"R_AARCH64_[A-Z0-9_]+", subprocess.run(["objdump", "-R", path], capture_output=True, text=True).stdout))
        d, u = dyn_symbols(path)
        defined |= d
        for name in u:
            undefined[name].add(os.path.basename(path))

    unresolved = {n: v for n, v in undefined.items() if n not in defined}
    groups = collections.defaultdict(list)
    for name in unresolved:
        if name.startswith("_Z"):
            group = "C++"
        elif name.startswith(("X", "xcb_")):
            group = "X11/xcb"
        elif name.startswith("wl_"):
            group = "wayland"
        elif name.startswith("SDL_"):
            group = "SDL"
        elif name.startswith("vk"):
            group = "vulkan"
        elif name.startswith("udev_"):
            group = "udev"
        else:
            group = "libc"
        groups[group].append(name)

    print(f"ELF files: {len(libs)}")
    print("\nNEEDED (number of libraries needing each):")
    for name, count in needed.most_common():
        print(f"  {count:3d} {name}")
    print("\nRelocation types:")
    for name, count in relocs.most_common():
        print(f"  {count:8d} {name}")
    print(f"\nUnresolved symbols (host shim must provide): {len(unresolved)}")
    for group, names in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        print(f"\n[{group}] {len(names)}")
        print("  " + " ".join(sorted(names)[:400]))


if __name__ == "__main__":
    main()
