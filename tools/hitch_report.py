#!/usr/bin/env python3
"""hitch_report.py <release.recipe> <feedback dir>...: triage players' stutters.

Each feedback dir is what `glovebox collect` (or the headset's Send diagnostics) produced: slow.txt (one line per
slow compile: pipeline hash, ms, seconds into the session), recorded.recipe and console.log. For every stutter: was
the pipeline already in the release's list (then pre-compiling failed: a bug) or not (a coverage miss: merge the
session's recipe). Prints a summary and the worst stutters with the map that was loaded at the time."""
import os, re, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "engine"))
from gb import foz

def names(path):
    return {n[-16:] for n, *_ in foz.read(path)[1] if n[22:24] == "06"}

def maps_over_time(console):
    """[(seconds into the log, map)] from the console's map loads (best effort)."""
    out, t0 = [], None
    for line in open(console, errors="replace"):
        m = re.match(r"(\d\d)/(\d\d) (\d\d):(\d\d):(\d\d) .*?(?:activate map|Loading map|map_)\s*\"?([a-z0-9_]+)", line)
        if m:
            t = int(m[3]) * 3600 + int(m[4]) * 60 + int(m[5])
            t0 = t if t0 is None else t0
            out.append((t - t0, m[6]))
    return out

release = names(sys.argv[1])
total = covered = 0; rows = []
for d in sys.argv[2:]:
    slow = os.path.join(d, "slow.txt")
    if not os.path.exists(slow):
        continue
    for line in open(slow):
        h, ms, t = line.split()[:3]
        total += 1; inlist = h in release; covered += inlist
        rows.append((float(ms), h, float(t), os.path.basename(d.rstrip("/")), inlist))
print(f"{total} stutters in {len(sys.argv) - 2} sessions: {total - covered} not in the release's list (coverage "
      f"misses: merge those sessions' recipes), {covered} already in it (for a session that pre-compiled this list: pre-compiling failed, investigate)")
for ms, h, t, sess, inlist in sorted(rows, reverse=True)[:15]:
    print(f"  {ms:6.0f} ms  {h}  {t:7.0f} s into {sess}  {'IN LIST (bug)' if inlist else 'miss'}")
