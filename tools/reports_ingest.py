#!/usr/bin/env python3
"""reports_ingest.py <out dir> <release.recipe> <spirv-index.tsv> <report>... [--game <game dir>]: turn players'
stutter reports into a recipe and a stutter triage.

A report is what the headset app sends (diagnostics/), as the admin routes in reports/ return it (JSON) or as stored
(.json.deflate). Its shader modules carry a SHA-256 fingerprint instead of code; the index (tools/spirv_index.py, one
line per compiled shader in the game's packs) says where that shader sits in the game's own files, which is exactly a
recipe reference. Reports come from anyone, so each one is checked before anything of it is used. Writes in <out dir>:
  sessions/<install>/recorded.recipe   that install's pipelines as a recipe (no shader code)
  sessions/<install>/slow.txt          its stutters, as hitch_report.py reads them
  merged.recipe                        `pipelines.py merge` of the release recipe and every session's (release first)
then runs tools/hitch_report.py on the release recipe and the sessions. With --game, also rebuilds merged.recipe
against the game's files (`pipelines.py build`, into a temporary file) to prove every reference resolves."""
import json, os, re, struct, subprocess, sys, tempfile, zlib
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "engine"))
from gb import foz, vcs
from pipelines import REF_MAGIC

NAME, HEX16, SHA = re.compile(r"[0-9a-f]{40}"), re.compile(r"[0-9a-f]{16}"), re.compile(r"[0-9a-f]{64}")
UUID = re.compile(r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")


def load(path):
    d = open(path, "rb").read()
    if not d.lstrip().startswith(b"{"):
        d = zlib.decompress(d, -15)  # as stored in the bucket: raw deflate
    return json.loads(d)


def check(r):
    """The endpoint's validate() (reports/lib.js) plus what a recipe needs; raises on anything off."""
    if r.get("v") != 1 or not UUID.fullmatch(str(r.get("install"))) or not isinstance(r.get("seq"), int):
        raise ValueError("header")
    for h in r["hitches"]:
        if not HEX16.fullmatch(h["p"]) or not all(isinstance(h[k], (int, float)) for k in ("ms", "t")):
            raise ValueError("hitch")
    for e in r["entries"]:
        if not NAME.fullmatch(e["n"]) or e["n"][22:24] == foz.TAG_SHADER_MODULE:
            raise ValueError(f"entry {e['n'][:40]}")
        json.loads(e["j"])
    for m in r["modules"]:
        if not NAME.fullmatch(m["n"]) or m["n"][22:24] != foz.TAG_SHADER_MODULE or not SHA.fullmatch(m["sha256"]):
            raise ValueError(f"module {m['n'][:40]}")
        (_, sm), = json.loads(m["j"])["shaderModules"].items()
        if sm["codeSize"] != m["size"] or sm["varintOffset"] != 0 or not m["size"] // 4 <= sm["varintSize"] <= m["size"] * 5 // 4:
            raise ValueError(f"module {m['n']}: header does not fit its size")
    for x in r.get("prep", []):  # "Preparing graphics" before Play (prewarm.log), since report v1 gained it
        if not all(isinstance(x.get(k), (int, float)) for k in ("at", "s", "n", "slow", "rc")):
            raise ValueError("prep")
    if any(vcs.SPIRV_MAGIC in x["j"].encode() for x in r["entries"] + r["modules"]):
        raise ValueError("shader code in a report")
    return r


def where(index, wanted):
    """{sha256: (pack, file, static, offset)} for the wanted {sha256: size}; the same choice as `pipelines.py make`
    when a shader sits in several places (the smallest)."""
    out = {}
    with open(index) as f:
        for line in f:
            sha = line[:64]
            if sha in wanted:
                _, size, pack, path, sid, off = line.split()
                if int(size) == wanted[sha]:
                    cand = (pack, path, int(sid), int(off))
                    if sha not in out or cand < out[sha]:
                        out[sha] = cand
    return out


def run(*cmd):
    sys.stdout.flush()
    print("$", " ".join(os.path.relpath(c, ROOT) if c.startswith(ROOT) else c for c in cmd[1:]), flush=True)
    return subprocess.run([sys.executable, *cmd]).returncode


def main(a):
    game = None
    if "--game" in a:
        i = a.index("--game")
        game = a[i + 1]
        a = a[:i] + a[i + 2:]
    if len(a) < 4:
        sys.exit(__doc__)
    out, release, index, paths = a[0], a[1], a[2], a[3:]
    reports, seen, bad, dup = {}, set(), 0, 0
    for p in paths:
        try:
            r = check(load(p))
        except Exception as e:  # noqa: BLE001 (anything malformed is skipped, never fatal)
            print(f"skipped {os.path.basename(p)}: {e}")
            bad += 1
            continue
        if (r["install"], r["seq"]) in seen:
            dup += 1
            continue  # the same batch sent twice (a retry after a lost answer)
        seen.add((r["install"], r["seq"]))
        reports.setdefault(r["install"], []).append(r)
    mods = [m for rs in reports.values() for r in rs for m in r["modules"]]
    found = where(index, {m["sha256"]: m["size"] for m in mods})
    header, rel = foz.read(release)
    release_names = {e[0] for e in rel}
    sessions, missing = [], 0
    for install, rs in sorted(reports.items()):
        d = os.path.join(out, "sessions", install)
        os.makedirs(d, exist_ok=True)
        rs.sort(key=lambda r: r["seq"])
        names = set()
        with open(os.path.join(d, "recorded.recipe"), "wb") as f:
            f.write(header)
            for r in rs:
                for m in r["modules"]:  # modules before the pipelines that use them
                    if m["n"] in names:
                        continue
                    if m["sha256"] not in found:
                        missing += 1
                        continue
                    names.add(m["n"])
                    pack, path, sid, pos = found[m["sha256"]]
                    ref = {"json": m["j"], "pack": pack, "file": path, "static": sid, "offset": pos, "size": m["size"],
                           "sha256": m["sha256"]}
                    p = REF_MAGIC + json.dumps(ref, separators=(",", ":")).encode()
                    f.write(m["n"].encode() + struct.pack("<4I", len(p), foz.FORMAT_RAW, 0, len(p)) + p)
            for r in rs:
                for e in r["entries"]:
                    if e["n"] not in names:
                        names.add(e["n"])
                        f.write(foz.entry_bytes(e["n"], e["j"].encode()))
        with open(os.path.join(d, "slow.txt"), "w") as f:
            for r in rs:
                f.writelines(f"{h['p']} {h['ms']} {h['t']}\n" for h in r["hitches"])
        new = names - release_names
        preps = [x for r in rs for x in r.get("prep", [])]
        print(f"{install}: {len(rs)} reports ({rs[0]['app']['glovebox']}, {rs[0]['device']['model']} "
              f"{rs[0]['device']['os']}), {len(names)} entries, {len(new)} not in the release "
              f"({sum(n[22:24] == '06' for n in new)} graphics pipelines), "
              f"{sum(len(r['hitches']) for r in rs)} stutters"
              + "".join(f"; prepared {x['n']} pipelines in {x['s']:.1f} s ({x['slow']} over 100 ms)" if x["rc"] == 0
                        else f"; preparing could not run ({x['rc']})" for x in preps))
        sessions.append(d)
    print(f"{len(paths)} reports: {bad} skipped, {dup} sent twice, {len(sessions)} installs, {len(mods)} shader fingerprints, "
          f"{len(set(m['sha256'] for m in mods) & set(found))}/{len(set(m['sha256'] for m in mods))} found in the "
          f"index, {missing} module entries dropped (not in this game's shaders)")
    if not sessions:
        return 1
    merged = os.path.join(out, "merged.recipe")
    rc = run(os.path.join(ROOT, "engine", "pipelines.py"), "merge", merged, release,
             *[os.path.join(d, "recorded.recipe") for d in sessions])
    if rc == 0 and game:
        with tempfile.TemporaryDirectory() as t:  # holds the game's shader code: never kept
            rc = run(os.path.join(ROOT, "engine", "pipelines.py"), "build", merged, game, os.path.join(t, "check.foz"))
    rc |= run(os.path.join(ROOT, "tools", "hitch_report.py"), release, *sessions)
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
