#!/usr/bin/env python3
"""The visionOS build's copies of the sources (VISIONOS_PORT.md, wall 0): every call through an ms_abi function
pointer (the runtime calling what may be x86 code; tools/vx_abi_scan.py finds them) gets its callee wrapped in
VX_X86FN(...), which sends x86 targets to vx_cb_tramp_tls (src/arm64.c, src/context_arm64.S) instead of branching to
them. On the Mac such a branch faults and the signal handler sends it on; visionOS kills a process that fetches an
instruction from a page without signed code ("CODESIGNING, Invalid Page"), so there the call must never be made.

usage: xros_x86calls.py <outdir> <source.c|.m> ...   writes <outdir>/<source> (with #line to the original) for each
source that has such calls, and prints the list of written files; the others are compiled as they are."""
import json, os, re, subprocess, sys

def main():
    out, srcs = sys.argv[1], sys.argv[2:]
    here = os.path.dirname(os.path.abspath(__file__))
    scan = json.loads(subprocess.run([sys.executable, os.path.join(here, "vx_abi_scan.py")] + srcs,
                                     capture_output=True, text=True, check=True).stdout)
    edits = {}                                           # file -> {(begin, end)}
    for src, r in scan.items():
        for cb in r["callbacks"]:
            f = cb.get("callee_file") or ""
            if not f: raise SystemExit(f"{src}:{cb['line']}: a callee across files or macros; wrap it by hand")
            a, b = cb["callee_at"]
            if re.match(rb"[A-Z][A-Z0-9_]*\(", open(f, "rb").read()[a:b]): continue   # (a macro's call: wrapped in its definition)
            edits.setdefault(os.path.normpath(f), set()).add((a, b))
    for f, spans in sorted(edits.items()):
        text = open(f, "rb").read()
        spans = sorted(spans)
        for a, b in zip(spans, spans[1:]):
            if b[0] < a[1] and not (a[0] <= b[0] and b[1] <= a[1]): raise SystemExit(f"{f}: overlapping callees at {a} {b}")
        # inner ones first is not needed: wrapping text, outermost spans contain inner ones' edits applied after
        res, last = [], 0
        def emit(lo, hi):                                   # text [lo, hi) with the spans inside it wrapped
            parts, at = [], lo
            for s in spans:
                if s[0] < at or s[1] > hi or (s[0] == lo and s[1] == hi): continue
                if any(o != s and o[0] <= s[0] and s[1] <= o[1] and o[0] >= lo and o[1] <= hi and not (o[0] == lo and o[1] == hi) for o in spans):
                    continue                                # (inside another span: done by that one's emit)
                parts.append(text[at:s[0]])
                parts.append(b"VX_X86FN(" + emit(s[0], s[1]) + b")")
                at = s[1]
            parts.append(text[at:hi])
            return b"".join(parts)
        body = emit(0, len(text))
        dst = os.path.join(out, f)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        new = b'#line 1 "' + f.encode() + b'"\n' + body
        if not os.path.exists(dst) or open(dst, "rb").read() != new: open(dst, "wb").write(new)
        print(dst)

if __name__ == "__main__": main()
