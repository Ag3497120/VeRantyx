#!/usr/bin/env python3
"""Game profiles: games/<Steam app id>.json, one per game, checked against games/profile.schema.json. They say how a
game starts, the scenarios tools/vxrt_run.py measures, its frozen baselines and what is known about it. Anyone can add
or change one (with a run that shows it); the harness, the MCP server and VXRT Studio read them.

    tools/vxrt_games.py check          # every profile against the schema and the rules below (make check runs it)
    tools/vxrt_games.py list
"""
import json, os, re, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAMES_DIR = os.path.join(REPO, "games")
SCHEMA = os.path.join(GAMES_DIR, "profile.schema.json")

# ---- a small JSON Schema check (the keywords profile.schema.json uses) --------------------------------------------------
_TYPES = {"string": str, "number": (int, float), "boolean": bool, "object": dict, "array": list, "null": type(None)}

def _is(value, name):
    if name == "number" and isinstance(value, bool):
        return False
    return isinstance(value, _TYPES[name])

def schema_problems(value, schema, at="$"):
    out = []
    t = schema.get("type")
    if t and not any(_is(value, x) for x in ([t] if isinstance(t, str) else t)):
        return [f"{at}: should be {t}"]
    if "enum" in schema and value not in schema["enum"]:
        out.append(f"{at}: {value!r} is not one of {schema['enum']}")
    if isinstance(value, str) and "pattern" in schema and not re.search(schema["pattern"], value):
        out.append(f"{at}: {value!r} does not match {schema['pattern']}")
    if _is(value, "number"):
        if "minimum" in schema and value < schema["minimum"]: out.append(f"{at}: below {schema['minimum']}")
        if "maximum" in schema and value > schema["maximum"]: out.append(f"{at}: above {schema['maximum']}")
    if isinstance(value, list) and "items" in schema:
        for i, item in enumerate(value):
            out += schema_problems(item, schema["items"], f"{at}[{i}]")
    if isinstance(value, dict):
        for key in schema.get("required", []):
            if key not in value: out.append(f"{at}: missing {key}")
        if len(value) < schema.get("minProperties", 0): out.append(f"{at}: needs at least {schema['minProperties']} entries")
        props = schema.get("properties", {})
        for key, v in value.items():
            if "propertyNames" in schema:
                out += schema_problems(key, schema["propertyNames"], f"{at} key {key!r}")
            if key in props:
                out += schema_problems(v, props[key], f"{at}.{key}")
            elif schema.get("additionalProperties") is False:
                out.append(f"{at}: unknown field {key}")
            elif isinstance(schema.get("additionalProperties"), dict):
                out += schema_problems(v, schema["additionalProperties"], f"{at}.{key}")
    return out

# ---- profiles --------------------------------------------------------------------------------------------------------------
def problems(profile, filename=None):
    """What is wrong with a profile: the schema, then rules a schema cannot say."""
    out = schema_problems(profile, json.load(open(SCHEMA)))
    if out:
        return out
    if filename and os.path.splitext(os.path.basename(filename))[0] != profile["appid"]:
        out.append(f"the file should be named {profile['appid']}.json")
    if profile["default_scenario"] not in profile["scenarios"]:
        out.append(f"default_scenario {profile['default_scenario']!r} is not in scenarios")
    for name in profile["baselines"]:
        if name.split("@")[0] not in profile["scenarios"]:
            out.append(f"baseline {name!r}: no scenario {name.split('@')[0]!r}")
    return out

def load_all():
    """{app id: profile} of every valid profile; invalid ones are left out (check reports them)."""
    out = {}
    if not os.path.isdir(GAMES_DIR):
        return out
    for f in sorted(os.listdir(GAMES_DIR)):
        if not re.match(r"[0-9]+\.json$", f):
            continue
        try:
            p = json.load(open(os.path.join(GAMES_DIR, f)))
        except ValueError:
            continue
        if not problems(p, f):
            out[p["appid"]] = p
    return out

def by_key(key):
    return next((p for p in load_all().values() if p["key"] == key or p["appid"] == key), None)

def default_profile(appid, name=None):
    """What an installed game without a profile gets: the generic launcher, one scenario, nothing known."""
    return {"appid": appid, "key": appid, "name": name or appid, "launcher": "game", "engine": "vxrt", "default_scenario": "start",
            "scenarios": {"start": {"args": [], "seconds": 150}}, "status": {"state": "untested", "summary": {"ja": "未確認", "en": "Not tried yet"},
            "checked": None, "machine": None}, "baselines": {}, "known_issues": [], "notes": []}

def main():
    if sys.argv[1:] == ["list"]:
        for p in load_all().values():
            print(f"{p['appid']:8} {p['key']:10} {p['status']['state']:9} {p['name']}  scenarios: {', '.join(p['scenarios'])}")
        return
    if sys.argv[1:] != ["check"]:
        sys.exit(__doc__)
    bad = 0
    for f in sorted(os.listdir(GAMES_DIR)):
        if not f.endswith(".json") or f == "profile.schema.json":
            continue
        try:
            p = json.load(open(os.path.join(GAMES_DIR, f)))
        except ValueError as e:
            print(f"games/{f}: not JSON: {e}"); bad += 1; continue
        for msg in problems(p, f):
            print(f"games/{f}: {msg}"); bad += 1
    keys = [p["key"] for p in load_all().values()]
    for k in {k for k in keys if keys.count(k) > 1}:
        print(f"key {k!r} is used by more than one profile"); bad += 1
    print(f"game profiles: {len(load_all())} valid, {bad} problems -> {'FAILED' if bad else 'ok'}")
    sys.exit(1 if bad else 0)

if __name__ == "__main__":
    main()
