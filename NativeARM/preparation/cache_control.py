#!/usr/bin/env python3
"""Local Alyx pipeline identity metadata; this never certifies that warmup can be skipped.

Examples:
  cache_control.py verify --state cache-identity.json \
    --input host=/path/alyx_host --input moltenvk=/path/libMoltenVK.dylib \
    --input recipe=/path/alyx.foz --vpk core=/path/core/shaders_vulkan_dir.vpk \
    --policy msaa=4 --policy argument_buffers=0
  cache_control.py status --state cache-identity.json
  cache_control.py invalidate-metadata --state cache-identity.json

Hash reuse checks local file metadata, not every byte. Use verify --force after
suspected corruption. Files are opened and their identities checked before/after
both full hashing and metadata-cache hits. State updates use atomic replacement.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import sys
import tempfile
from typing import Any

SCHEMA = 1
CHUNK = 1024 * 1024

class IdentityError(ValueError):
    pass

class ChangedDuringRead(IdentityError):
    pass

def _signature(st: os.stat_result) -> dict[str, int]:
    return {"device": st.st_dev, "inode": st.st_ino, "size": st.st_size,
            "mtime_ns": st.st_mtime_ns, "ctime_ns": st.st_ctime_ns}

def _path(path: str | Path) -> Path:
    return Path(os.path.abspath(os.path.expanduser(str(path))))

def _json_hash(value: Any) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":"),
                                     ensure_ascii=True).encode()).hexdigest()

def _valid_digest(value: Any) -> bool:
    return isinstance(value, str) and len(value) == 64 and all(c in "0123456789abcdef" for c in value)

def load_state(path: str | Path) -> dict[str, Any]:
    try:
        state = json.loads(Path(path).read_text())
        if not isinstance(state, dict) or state.get("schema") != SCHEMA:
            raise ValueError("Unknown identity schema")
        if not isinstance(state.get("digests"), dict):
            raise ValueError("Invalid digest map")
        return state
    except FileNotFoundError:
        return {"schema": SCHEMA, "digests": {}}
    except (OSError, ValueError, TypeError) as exc:
        # A damaged certificate must cause rehashing, never a false cache hit.
        return {"schema": SCHEMA, "digests": {}, "state_error": str(exc)}

def atomic_write(path: str | Path, data: dict[str, Any]) -> None:
    p = _path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix="." + p.name + ".", dir=p.parent)
    try:
        with os.fdopen(fd, "w") as stream:
            json.dump(data, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(name, p)
    finally:
        try:
            os.unlink(name)
        except FileNotFoundError:
            pass

def _hash_stream(fd: int) -> str:
    digest = hashlib.sha256()
    while True:
        block = os.read(fd, CHUNK)
        if not block:
            break
        digest.update(block)
    return digest.hexdigest()

def file_identity(path: str | Path, cached: dict[str, Any] | None = None,
                  force: bool = False) -> tuple[dict[str, Any], bool]:
    """Return identity and whether a metadata-validated digest was reused."""
    p = _path(path)
    with p.open("rb", buffering=0) as stream:
        before = os.fstat(stream.fileno())
        if not stat.S_ISREG(before.st_mode):
            raise IdentityError(f"Not a regular file: {p}")
        signature = _signature(before)
        resolved = str(p.resolve(strict=True))
        cached = cached if isinstance(cached, dict) else {}
        reuse = (not force and cached.get("stat") == signature
                 and cached.get("resolved") == resolved and _valid_digest(cached.get("sha256")))
        # The open verifies read access even for a reused digest. Do not turn an
        # inaccessible path into a successful status just because stat matches.
        digest = cached["sha256"] if reuse else _hash_stream(stream.fileno())
        if (_signature(os.fstat(stream.fileno())) != signature
                or _signature(p.stat()) != signature or str(p.resolve(strict=True)) != resolved):
            raise ChangedDuringRead(f"File changed during identity check: {p}")
    return {"path": str(p), "resolved": resolved, "stat": signature, "sha256": digest}, reuse

def vpk_inputs(label: str, directory_file: str | Path) -> dict[str, Path]:
    """Include a VPK directory and every referenced external archive part.

    Also include existing numbered parts so adding a part invalidates identity.
    VPK directory trees are parsed solely to detect missing referenced parts.
    """
    p = _path(directory_file)
    if not p.name.endswith("_dir.vpk"):
        raise IdentityError(f"Expected a *_dir.vpk file: {p}")
    data = p.read_bytes()
    if len(data) < 12:
        raise IdentityError(f"Truncated VPK header: {p}")
    magic, version, tree_size = struct.unpack_from("<III", data)
    if magic != 0x55AA1234 or version not in (1, 2):
        raise IdentityError(f"Unsupported VPK directory: {p}")
    offset = 12 if version == 1 else 28
    end = offset + tree_size
    if end > len(data):
        raise IdentityError(f"Truncated VPK directory tree: {p}")
    tree = data[offset:end]
    at = 0
    archives: set[int] = set()
    def word() -> bytes:
        nonlocal at
        stop = tree.find(b"\0", at)
        if stop < 0:
            raise IdentityError(f"Unterminated VPK tree: {p}")
        value = tree[at:stop]
        at = stop + 1
        return value
    while word():
        while word():
            while word():
                if at + 18 > len(tree):
                    raise IdentityError(f"Truncated VPK entry: {p}")
                _, preload, index, _, length, marker = struct.unpack_from("<IHHIIH", tree, at)
                if marker != 0xFFFF:
                    raise IdentityError(f"Invalid VPK entry marker: {p}")
                at += 18 + preload
                if at > len(tree):
                    raise IdentityError(f"Truncated VPK preload: {p}")
                if index != 0x7FFF and length:
                    archives.add(index)
    stem = p.name[:-8]
    result = {label + ":dir": p}
    for index in sorted(archives):
        part = p.with_name(f"{stem}_{index:03d}.vpk")
        if not part.is_file():
            raise IdentityError(f"Missing VPK archive part: {part}")
        result[f"{label}:part:{index}"] = part
    for part in sorted(p.parent.glob(stem + "_[0-9][0-9][0-9].vpk")):
        index = int(part.stem[-3:])
        result[f"{label}:part:{index}"] = part
    return result

def verify(state_path: str | Path, inputs: dict[str, str | Path],
           policy: dict[str, str], force: bool = False, on_progress=None) -> dict[str, Any]:
    if not inputs:
        raise IdentityError("At least one input file is required")
    state = load_state(state_path)
    records: dict[str, Any] = {}
    digests: dict[str, Any] = {}
    hashed = reused = 0
    for label, path in sorted(inputs.items()):
        key = str(_path(path))
        record, hit = file_identity(path, state["digests"].get(key), force)
        records[label] = record
        digests[key] = record
        reused += int(hit)
        hashed += int(not hit)
        if on_progress is not None:on_progress(hashed+reused,len(inputs),hashed,reused)
    identity = {"schema": SCHEMA, "inputs": records, "policy": dict(sorted(policy.items()))}
    fingerprint = _json_hash(identity)
    result = {"fingerprint": fingerprint, "identity": identity,
              "hashed": hashed, "reused": reused, "forced": force,
              "warmup_skip_certified": False}
    atomic_write(state_path, {"schema": SCHEMA, "digests": digests, "last_verified": result})
    return result

def _observed_identity(observed: Any) -> dict[str, Any]:
    """Require a structurally intact verification result, not a metadata status."""
    if not isinstance(observed, dict):
        raise IdentityError("Missing observed identity")
    identity = observed.get("identity")
    if (not isinstance(identity, dict) or identity.get("schema") != SCHEMA
            or not isinstance(identity.get("inputs"), dict) or not identity["inputs"]
            or not isinstance(identity.get("policy"), dict)):
        raise IdentityError("Missing or malformed observed identity")
    if observed.get("fingerprint") != _json_hash(identity):
        raise IdentityError("Observed identity fingerprint is missing or inconsistent")
    for label, record in identity["inputs"].items():
        if (not isinstance(label, str) or not isinstance(record, dict)
                or not _valid_digest(record.get("sha256"))):
            raise IdentityError("Malformed observed content hash")
    return identity


def make_contract(observed: dict[str, Any], input_labels: list[str],
                  policy_keys: list[str] | None = None,
                  input_prefixes: list[str] | None = None) -> dict[str, Any]:
    """Build an expected CONTENT contract at known-good recipe deployment only.

    The installer selects recipe/game-shader labels explicitly. Exclude host or
    MoltenVK labels when a runtime rebuild alone does not invalidate that recipe.
    Hash observations never update this contract. The caller can persist it with
    atomic_write, but must not regenerate it on an ordinary launch/verification.
    Path/stat changes are ignored by contract validation when bytes are identical.
    """
    identity = _observed_identity(observed)
    labels = sorted(set(input_labels))
    if not labels:
        raise IdentityError("A recipe contract must name at least one content input")
    expected = {}
    for label in labels:
        if label not in identity["inputs"]:
            raise IdentityError(f"Contract input is missing from verification: {label}")
        expected[label] = identity["inputs"][label]["sha256"]
    keys = sorted(identity["policy"] if policy_keys is None else set(policy_keys))
    policy = {}
    for key in keys:
        if key not in identity["policy"]:
            raise IdentityError(f"Contract policy is missing from verification: {key}")
        policy[key] = identity["policy"][key]
    # Expanded VPK labels are namespace:dir / namespace:part:N. Also reject a
    # newly appearing part in those tracked namespaces, not just changed parts.
    prefixes = sorted(set(input_prefixes if input_prefixes is not None else
                          (label.split(":", 1)[0] + ":" for label in labels if ":" in label)))
    return {"schema": SCHEMA, "kind": "alyx-recipe-content-contract",
            "expected_inputs": expected, "expected_policy": policy,
            "input_prefixes": prefixes}


def validate_contract(contract: str | Path | dict[str, Any],
                      observed: dict[str, Any]) -> dict[str, Any]:
    """Compare observations with deployment expectations, without writing either.

    A successful comparison establishes recipe-content compatibility only. It
    never establishes that compiled GPU pipelines are present or warmup skippable.
    Missing/corrupt identities or contracts always fail closed with needs_rebuild.
    """
    result: dict[str, Any] = {"valid": False, "needs_rebuild": True,
                              "warmup_skip_certified": False}
    try:
        expected = json.loads(Path(contract).read_text()) if isinstance(contract, (str, Path)) else contract
        if (not isinstance(expected, dict) or expected.get("schema") != SCHEMA
                or expected.get("kind") != "alyx-recipe-content-contract"
                or not isinstance(expected.get("expected_inputs"), dict)
                or not expected["expected_inputs"]
                or not isinstance(expected.get("expected_policy"), dict)):
            raise IdentityError("Missing or malformed recipe deployment contract")
        if not all(isinstance(k, str) and _valid_digest(v)
                   for k, v in expected["expected_inputs"].items()):
            raise IdentityError("Malformed expected content hash")
        identity = _observed_identity(observed)
        inputs = identity["inputs"]
        policy = identity["policy"]
        prefixes = expected.get("input_prefixes", [])
        if not isinstance(prefixes, list) or not all(isinstance(p, str) and p for p in prefixes):
            raise IdentityError("Malformed contract input prefixes")
        result["extra_inputs"] = sorted(k for k in inputs if k not in expected["expected_inputs"]
                                         and any(k.startswith(p) for p in prefixes))
        result["missing_inputs"] = sorted(k for k in expected["expected_inputs"] if k not in inputs)
        result["mismatched_inputs"] = sorted(k for k, value in expected["expected_inputs"].items()
                                               if k in inputs and inputs[k]["sha256"] != value)
        result["missing_policy"] = sorted(k for k in expected["expected_policy"] if k not in policy)
        result["mismatched_policy"] = sorted(k for k, value in expected["expected_policy"].items()
                                               if k in policy and policy[k] != value)
        valid = not any(result[key] for key in ("missing_inputs", "mismatched_inputs", "extra_inputs",
                                                "missing_policy", "mismatched_policy"))
        result["valid"] = valid
        result["needs_rebuild"] = not valid
        if not valid:
            result["message"] = "Recipe inputs or policy changed. Rebuild preparation data or use the stable launcher."
    except (OSError, ValueError, TypeError, KeyError) as exc:
        result["contract_error"] = str(exc)
    return result


def status(state_path: str | Path) -> dict[str, Any]:
    state = load_state(state_path)
    previous = state.get("last_verified")
    result: dict[str, Any] = {"known": False, "valid": False, "metadata_current": False,
                              "warmup_skip_certified": False}
    if "state_error" in state:
        result["state_error"] = state["state_error"]
    if not previous:
        return result
    try:
        identity = _observed_identity(previous)
    except (TypeError, KeyError, ValueError) as exc:
        result["state_error"] = str(exc)
        return result
    result["known"] = True
    result["fingerprint"] = previous["fingerprint"]
    changed = []
    for label, record in identity["inputs"].items():
        try:
            path = _path(record["path"])
            with path.open("rb", buffering=0) as stream:
                before = _signature(os.fstat(stream.fileno()))
                after = _signature(path.stat())
                if (before != record["stat"] or after != before
                        or str(path.resolve(strict=True)) != record["resolved"]):
                    changed.append(label)
        except (OSError, ValueError, TypeError, KeyError):
            changed.append(label)
    result["changed_inputs"] = changed
    result["metadata_current"] = not changed and not state.get("invalidated", False)
    # This flag validates the last observed metadata only; callers still need
    # validate_contract for compatibility with a specific prepared recipe.
    result["valid"] = result["metadata_current"]
    return result


def invalidate_metadata(state_path: str | Path) -> dict[str, Any]:
    state = load_state(state_path)
    state["digests"] = {}
    state["invalidated"] = True
    state.pop("state_error", None)
    atomic_write(state_path, state)
    return {"invalidated": True, "deleted_game_files": False, "warmup_skip_certified": False}

def _pairs(values: list[str]) -> dict[str, str]:
    result = {}
    for value in values:
        key, separator, item = value.partition("=")
        if not separator or not key or not item or key in result:
            raise IdentityError(f"Expected a unique nonempty NAME=VALUE: {value}")
        result[key] = item
    return result

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("status", "verify", "invalidate-metadata"))
    parser.add_argument("--state", type=Path, required=True)
    parser.add_argument("--input", action="append", default=[], metavar="LABEL=FILE")
    parser.add_argument("--vpk", action="append", default=[], metavar="LABEL=DIRECTORY_VPK")
    parser.add_argument("--policy", action="append", default=[], metavar="NAME=VALUE")
    parser.add_argument("--force", action="store_true", help="Read every input byte instead of reusing metadata-cached digests")
    args = parser.parse_args(argv)
    try:
        if args.command == "verify":
            inputs = _pairs(args.input)
            for label, path in _pairs(args.vpk).items():
                extra = vpk_inputs(label, path)
                if inputs.keys() & extra.keys():
                    raise IdentityError("Duplicate expanded input label")
                inputs.update(extra)
            result = verify(args.state, inputs, _pairs(args.policy), args.force)
        elif args.command == "status":
            result = status(args.state)
        else:
            result = invalidate_metadata(args.state)
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(json.dumps({"error": str(exc), "warmup_skip_certified": False}, sort_keys=True))
        return 1

if __name__ == "__main__":
    sys.exit(main())
