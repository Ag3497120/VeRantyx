#!/usr/bin/env python3
"""The licenses of the Rust crates the ALVR bridge (ALVRBridge/, built into the app) is made of, for the app's
licenses folder: every crate cargo builds for this Mac (cargo tree, normal dependencies), its license expression and
the license and notice files it ships, each distinct text once with the crates that carry it.

    MacApp/crate_licenses.py <output file>
"""
import glob, hashlib, os, re, subprocess, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CARGO = os.path.expanduser("~/.cargo/bin/cargo") if os.path.exists(os.path.expanduser("~/.cargo/bin/cargo")) else "cargo"
HOME = os.path.expanduser(os.environ.get("CARGO_HOME", "~/.cargo"))

def crates():
    out = subprocess.run([CARGO, "tree", "--offline", "-e", "normal", "--prefix", "none", "--format", "{p}\t{l}"],
                         cwd=os.path.join(REPO, "ALVRBridge"), capture_output=True, text=True, check=True).stdout
    seen = {}
    for line in out.splitlines():
        line = line.replace(" (*)", "")
        if "\t" not in line: continue
        p, lic = line.split("\t", 1)
        m = re.match(r"(\S+) v(\S+)(?: \((.*)\))?", p.strip())
        if m and (m[1], m[2]) not in seen:
            seen[(m[1], m[2])] = (lic.strip(), m[3] or "")
    return seen

def folder(name, version, source):
    for d in glob.glob(os.path.join(HOME, "registry", "src", "*", f"{name}-{version}")):
        return d
    if source.startswith("https://github.com/alvr-org/ALVR"):        # (ALVR's crates: its repository's checkout)
        for d in glob.glob(os.path.join(HOME, "git", "checkouts", "alvr-*", "*")):
            if os.path.isdir(os.path.join(d, "alvr")):
                return d
    if source.startswith("/") or not source:
        return os.path.join(REPO, "ALVRBridge")
    return None

def texts(d):
    files = []
    for f in sorted(os.listdir(d)) if d and os.path.isdir(d) else []:
        if re.match(r"(?i)(licen[cs]e|copying|notice|copyright|unlicense)", f) and os.path.isfile(os.path.join(d, f)):
            files.append(open(os.path.join(d, f), errors="replace").read().strip())
    return files

def main():
    out = sys.argv[1]
    by_text, without = {}, []
    found = crates()
    for (name, version), (lic, source) in sorted(found.items()):
        if name == "verantyx_alvr_bridge": continue
        t = texts(folder(name, version, source))
        if not t: without.append(f"{name} {version} ({lic or 'no license field'})")
        for text in t:
            by_text.setdefault(hashlib.sha256(text.encode()).hexdigest(), [text, []])[1].append(f"{name} {version} ({lic})")
    with open(out, "w") as f:
        f.write("Rust crates built into the ALVR bridge (libverantyx_alvr_bridge.dylib) of the VeRantyx app.\n")
        f.write(f"{len(found) - 1} crates; each license or notice text below is followed by the crates that ship it.\n\n")
        for text, names in sorted(by_text.values(), key=lambda x: x[1][0]):
            f.write("=" * 100 + "\n" + "\n".join(names) + "\n" + "-" * 100 + "\n" + text + "\n\n")
        if without:
            f.write("=" * 100 + "\nCrates without a license file of their own (their license expression applies; the standard texts are above):\n")
            f.write("\n".join(without) + "\n")
    print(f"{len(found) - 1} crates, {len(by_text)} distinct texts, {len(without)} without a file -> {out}")

if __name__ == "__main__":
    main()
