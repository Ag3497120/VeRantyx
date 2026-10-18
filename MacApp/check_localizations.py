#!/usr/bin/env python3
"""Checks the app's translation tables against its sources.

Every English text the app shows is the first argument of T("English", "日本語") or TF("English %@", "日本語 %@", ...)
in MacApp/Sources/*.swift. Each table, MacApp/Localizations/<code>.json, maps those English texts to one language.
This reports, for each table: the texts it lacks, the ones it has that the sources no longer use, and translations
whose %@ placeholders do not match the English. It also reports T( calls whose English is not a constant (interpolated
with \\(...): use TF with %@ instead). Exit status 1 when anything is missing or wrong.

    python3 MacApp/check_localizations.py            # check
    python3 MacApp/check_localizations.py --keys     # print the English texts, one JSON list
"""
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCES = sorted((HERE / "Sources").glob("*.swift"))
TABLES = sorted((HERE / "Localizations").glob("*.json"))
CALL = re.compile(r'(?<![A-Za-z0-9_.])(TF?)\(\s*"')
PLACEHOLDER = re.compile(r"%(?:\d+\$)?@")


def swift_string(text, i):
    """The Swift string literal whose opening quote is at text[i]: (its value, the index after it, interpolated?)."""
    assert text[i] == '"'
    out, j, interpolated = [], i + 1, False
    while True:
        c = text[j]
        if c == '"':
            return "".join(out), j + 1, interpolated
        if c == "\\":
            n = text[j + 1]
            if n == "(":
                interpolated = True
                depth, j = 1, j + 2
                while depth:
                    depth += {"(": 1, ")": -1}.get(text[j], 0)
                    j += 1
                out.append("\\(…)")
                continue
            if n == "u":
                end = text.index("}", j)
                out.append(chr(int(text[j + 3:end], 16)))
                j = end + 1
                continue
            out.append({"n": "\n", "t": "\t", "0": "\0", "r": "\r"}.get(n, n))
            j += 2
            continue
        out.append(c)
        j += 1


def keys():
    """The English texts of the sources: {text: [where, ...]}, and the problems found on the way."""
    found, problems = {}, []
    for path in SOURCES:
        text = path.read_text()
        for m in CALL.finditer(text):
            value, _, interpolated = swift_string(text, m.end() - 1)
            line = text.count("\n", 0, m.start()) + 1
            where = f"{path.name}:{line}"
            if interpolated:
                problems.append(f"{where}: {m.group(1)}( with an interpolated English text (use TF and %@): {value!r}")
                continue
            if m.group(1) == "T" and PLACEHOLDER.search(value):
                problems.append(f"{where}: T( with %@ (use TF): {value!r}")
            found.setdefault(value, []).append(where)
    return found, problems


def main():
    found, problems = keys()
    if "--keys" in sys.argv:
        print(json.dumps(sorted(found), ensure_ascii=False, indent=1))
        return 0
    for p in problems:
        print(p)
    bad = bool(problems)
    if not TABLES:
        print("no tables in", HERE / "Localizations")
        return 1
    for table_path in TABLES:
        try:
            table = json.loads(table_path.read_text())
        except json.JSONDecodeError as e:
            print(f"{table_path.name}: not valid JSON: {e}")
            bad = True
            continue
        missing = [k for k in found if not table.get(k, "").strip()]
        unused = [k for k in table if k not in found]
        mismatched = [k for k in found if k in table and
                      len(PLACEHOLDER.findall(k)) != len(PLACEHOLDER.findall(table[k]))]
        for k in missing:
            print(f"{table_path.name}: missing {k!r} ({', '.join(found[k])})")
        for k in mismatched:
            print(f"{table_path.name}: placeholders differ: {k!r} -> {table[k]!r}")
        for k in unused:
            print(f"{table_path.name}: not used by the sources (remove it): {k!r}")
        bad = bad or bool(missing or mismatched or unused)
    status = "problems above" if bad else "nothing missing"
    print(f"{len(found)} texts, {len(TABLES)} tables ({', '.join(t.stem for t in TABLES)}): {status}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
