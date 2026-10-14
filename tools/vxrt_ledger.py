#!/usr/bin/env python3
"""The run ledger: one line per harness run (tools/vxrt_run.py) in runs/ledger.jsonl, each line sealed with the
SHA-256 of its contents and of the line before it, so a changed or removed result shows. The idea is Vera's
(Ag3497120/vera, verantyx/loom/ledger.py); this is a small standalone version.

    tools/vxrt_ledger.py verify        # the chain, and every result.json and artifact against its recorded hash

A game run cannot be replayed (GPU timing differs every time): verifying checks that what was recorded is what is
on disk, and vxrt_run.validate() re-derives the claims from the recorded observations.
"""
import fcntl, hashlib, json, os, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNS = os.path.join(REPO, "runs")
LEDGER = os.path.join(RUNS, "ledger.jsonl")
GENESIS = "0" * 64

def canonical(obj):
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()

def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()

def seal(index, prev, body):
    row = {"i": index, "prev": prev, "body": body}
    row["sha"] = hashlib.sha256(canonical(row)).hexdigest()
    return row

def rows(path=LEDGER):
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8") as f:
        return [json.loads(line) for line in f if line.strip()]

def append(body, path=LEDGER):
    """Seals body onto the end of the ledger (under a lock: runs may finish together). Returns the row."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "a+", encoding="utf-8") as f:
        fcntl.flock(f, fcntl.LOCK_EX)
        f.seek(0)
        lines = [json.loads(line) for line in f if line.strip()]
        prev = lines[-1]["sha"] if lines else GENESIS
        row = seal(len(lines), prev, body)
        f.seek(0, os.SEEK_END)
        f.write(canonical(row).decode() + "\n")
        f.flush()
        os.fsync(f.fileno())
        fcntl.flock(f, fcntl.LOCK_UN)
    return row

def verify(path=LEDGER, check_files=True):
    """{"ok": bool, "rows": n, "head": sha, "problems": [...]}: the chain, then (check_files) each run's result.json
    and artifacts against the hashes recorded in it."""
    problems, prev = [], GENESIS
    rs = rows(path)
    for k, row in enumerate(rs):
        body = {key: row[key] for key in ("i", "prev", "body") if key in row}
        if row.get("i") != k: problems.append(f"row {k}: index {row.get('i')}")
        if row.get("prev") != prev: problems.append(f"row {k}: does not follow the row before it")
        if hashlib.sha256(canonical(body)).hexdigest() != row.get("sha"): problems.append(f"row {k}: contents changed")
        prev = row.get("sha", "")
        if not check_files:
            continue
        b = row.get("body", {})
        result = os.path.join(REPO, b.get("result", ""))
        if not os.path.exists(result):
            problems.append(f"row {k}: {b.get('result')} is gone"); continue
        if sha256_file(result) != b.get("result_sha256"):
            problems.append(f"row {k}: {b.get('result')} changed"); continue
        for item in json.load(open(result)).get("observed", []):
            art = item.get("artifact")
            if art and item.get("sha256") and (not os.path.exists(os.path.join(REPO, art)) or sha256_file(os.path.join(REPO, art)) != item["sha256"]):
                problems.append(f"row {k}: artifact {art} changed or gone")
    return {"ok": not problems, "rows": len(rs), "head": prev if rs else None, "problems": problems}

if __name__ == "__main__":
    if sys.argv[1:] != ["verify"]:
        sys.exit(__doc__)
    r = verify()
    print(json.dumps(r, ensure_ascii=False, indent=2))
    sys.exit(0 if r["ok"] else 1)
