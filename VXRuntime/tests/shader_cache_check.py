"""Real Metal, fresh processes: verify IR reuse, strict GPU archive hits and rendered/compute output."""
import concurrent.futures
import json
import os
from pathlib import Path
import subprocess
import tempfile


def run(cache, *args, **extra):
    env = dict(os.environ, VXRT_SHADER_CACHE_DIR=str(cache), VXRT_SHADER_CACHE="1")
    env.update(extra)
    p = subprocess.run(["build/shader_cache_test", *args], env=env, capture_output=True, text=True, timeout=90)
    assert p.returncode == 0, (p.stdout, p.stderr)
    return json.loads(p.stdout.strip().splitlines()[-1])


with tempfile.TemporaryDirectory(prefix="shader-cache-", dir="build") as directory:
    root = Path(directory).resolve()
    cold = run(root / "normal")
    assert cold["source_compiles"] == 3 and cold["library_memory_hits"] == 32, cold
    assert cold["pipeline_misses"] == 6 and cold["archives_saved"] == 6 and cold["errors"] == 0, cold
    warm = run(root / "normal")
    assert warm["source_compiles"] == 0 and warm["library_disk_hits"] == 3, warm
    assert warm["pipeline_hits"] == 6 and warm["pipeline_misses"] == 0 and warm["errors"] == 0, warm
    changed = run(root / "normal", "changed")
    assert changed["source_compiles"] == 1 and changed["library_disk_hits"] == 2, changed
    assert changed["pipeline_misses"] == 1 and changed["pipeline_hits"] == 5, changed
    for path in (root / "normal").rglob("*.metallib"):
        path.write_bytes(b"broken archive")
    repaired = run(root / "normal")
    assert repaired["source_compiles"] == 3 and repaired["pipeline_misses"] == 6 and repaired["errors"] >= 3, repaired
    assert run(root / "normal")["source_compiles"] == 0
    for path in (root / "normal").rglob("*.index"):
        path.write_text("../../outside.metallib")
    assert run(root / "normal")["source_compiles"] == 3
    assert run(root / "normal")["source_compiles"] == 0
    scope = next((root / "normal" / "v1").iterdir())
    old = scope / ("0" * 64 + ".metallib")
    newer = scope / ("1" * 64 + ".metallib")
    for path, date in ((old, 1), (newer, 2)):
        with path.open("wb") as file:
            file.truncate(600 * 1024 * 1024)  # Sparse: test the budget without allocating a gigabyte.
        os.utime(path, (date, date))
    assert run(root / "normal")["pipeline_hits"] == 6
    assert not old.exists() and newer.exists()
    assert sum(p.stat().st_size for p in scope.glob("*.metallib")) <= 1024**3
    failed = run(root / "failure", "failure")
    assert failed["source_compiles"] == 2 and failed["archives_saved"] == 0, failed
    assert not list((root / "failure").rglob("*.index"))
    for path in (root / "normal").rglob("*.index"):
        path.unlink()
    assert run(root / "normal")["source_compiles"] == 3
    assert run(root / "normal")["source_compiles"] == 0
    # Writers only replace a single descriptor's artifact, so neither process drops unrelated variants.
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        results = list(executor.map(lambda _: run(root / "parallel"), range(2)))
    assert all(r["errors"] == 0 for r in results), results
    assert run(root / "parallel")["pipeline_hits"] == 6
    disabled = run(root / "disabled", VXRT_SHADER_CACHE="0")
    assert disabled["library_disk_hits"] == disabled["pipeline_hits"] == disabled["archives_saved"] == 0, disabled
    assert not (root / "disabled").exists()
    (root / "file").write_text("not a directory")
    unwritable = run(root / "file")
    assert unwritable["source_compiles"] > 0 and unwritable["archives_saved"] == 0, unwritable
    print(f"shader cache: cold {cold['elapsed_ms']:.1f} ms, warm {warm['elapsed_ms']:.1f} ms; "
          "0 warm source compiles, 6 strict GPU archive hits; pixels, compute, variants, corruption, quota, failures, concurrent writers and disabled/unwritable cache -> ok")
