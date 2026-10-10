#!/usr/bin/env python3
"""scrub_check.py [repo]: fail if anything that must never be published is tracked.

Never in this repository: the game's files or anything made from them (executables, translated libraries,
packs, compiled shaders, saves, pipeline archives with shader code), Valve's art, and anyone's private setup
(team IDs, bundle IDs, account names, device IDs, home paths). Runs in CI on every push.

The maintainers' own identifiers are listed only as salted SHA-256 hashes of whole words (PRIVATE_WORDS), so
this file doesn't publish what it guards. Add one with: scrub_check.py --hash <word>"""
import hashlib
import os
import re
import subprocess
import sys

SALT = b"glovebox-scrub:"
if sys.argv[1:2] == ["--hash"]:
    print(hashlib.sha256(SALT + sys.argv[2].lower().encode()).hexdigest())
    sys.exit(0)
root = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
root = os.path.realpath(root)
files = subprocess.run(["git", "-C", root, "ls-files", "-z"], capture_output=True, check=True).stdout.decode().split("\0")
files = [f for f in files if f]

BANNED_EXT = re.compile(r"\.(vpk|vcs|foz|sav|so(\.\d+)*|dylib|a|o|ipa|xcarchive|vtex_c|vmdl_c|vmat_c|vmap_c|vpcf_c|"
                        r"vsnd_c|vwrld_c|vres_c|vsndevts_c|bsp|vvd|vtx|mdl)$", re.I)
MAGIC = {b"\x7fELF": "ELF executable", b"\xcf\xfa\xed\xfe": "Mach-O", b"\xca\xfe\xba\xbe": "Mach-O universal",
         b"\x81FOSSILIZEDB": None, b"\x34\x12\xaa\x55": "VPK"}
PRIVATE = re.compile(rb"/Users/(?!maxamillion/|username/|Shared/)[A-Za-z0-9_.-]+/|00008[0-9A-F]{3}-[0-9A-F]{16}|"
                     rb"-----BEGIN [A-Z ]*PRIVATE KEY-----")
# Whole words (letters and digits, compared lowercase): the maintainers' team ID, account and name.
PRIVATE_WORDS = {
    "03ad04bda19a9334c1850dbd570f8767fa049113c4dc42f492ee6347ee244155",
    "5854089bad764e2e3a662f95f0f808428bd4a23f1c333ff2ca2485810c4a9d87",
    "c2f7c74362c7f12f6baee1a7474338385cab801fcfd75ef2b53da3baa864c5fa",
    "e15f4e0c38a6eac215b82bd13fc50bd90c05a39c09a426527d75d8e008b7ed96",
}
WORD = re.compile(rb"[A-Za-z0-9]+")


def private_word(b):
    for w in WORD.findall(b):
        if hashlib.sha256(SALT + w.lower()).hexdigest() in PRIVATE_WORDS:
            return w
    return None
SPIRV = b"\x03\x02\x23\x07"
# Our own binary files, by path: everything else binary needs a reviewed entry here.
ALLOWED_BINARY = re.compile(r"^(klepton/(runtime/data/|tests/data/)|packages/[^/]+/pipelines/[^/]+\.recipe$|"
                            r"brand/|installer/.*\.(png|icns)$|packages/[^/]+/icon/)")
MAX_BYTES = 8 << 20

bad = []
for f in files:
    p = os.path.join(root, f)
    if os.path.islink(p):
        # A link out of the repository is a local path (it names someone's machine) and is broken in every
        # other clone: whatever it points at has to be in the repository itself.
        t = os.readlink(p)
        if os.path.isabs(t) or not os.path.realpath(p).startswith(root + os.sep):
            bad.append((f, "symlink to a path outside the repository: put the real files here"))
        continue
    if not os.path.isfile(p):
        continue
    if BANNED_EXT.search(f):
        bad.append((f, "file type that only comes from a game or a build"))
        continue
    b = open(p, "rb").read()
    if len(b) > MAX_BYTES:
        bad.append((f, f"{len(b) >> 20} MB: too big for source"))
    for m, what in MAGIC.items():
        if b.startswith(m) and what:
            bad.append((f, what))
    if f.endswith(".recipe") or b.startswith(b"\x81FOSSILIZEDB"):
        r = subprocess.run([sys.executable, os.path.join(root, "engine", "pipelines.py"), "check", p], capture_output=True)
        if r.returncode:
            bad.append((f, "pipeline archive with shader code (publish a recipe instead)"))
        continue
    binary = b"\0" in b[:8192]
    if binary and not ALLOWED_BINARY.search(f):
        bad.append((f, "binary file not on the reviewed list"))
    if binary and SPIRV in b and not f.startswith("klepton/tests/"):
        bad.append((f, "contains SPIR-V"))
    m = PRIVATE.search(b)
    if m and f != "tools/scrub_check.py":
        bad.append((f, f"private identifier {m.group(0)[:24]!r}"))
    w = private_word(b)
    if w:
        bad.append((f, "a maintainer's private identifier (a word in PRIVATE_WORDS)"))

for f, why in bad:
    print(f"✗ {f}: {why}")
print(f"{len(files)} files checked, {len(bad)} problems")
sys.exit(1 if bad else 0)
