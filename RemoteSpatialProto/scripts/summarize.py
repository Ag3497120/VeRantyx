#!/usr/bin/env python3
"""Summarize a RemoteSpatialProbe run: the numbers to send back.

    scripts/summarize.py                      # newest frames-*.csv in ~/Library/Logs/RemoteSpatialProbe
    scripts/summarize.py path/to/frames.csv
"""
import csv
import glob
import os
import statistics
import sys

LOG_DIR = os.path.expanduser("~/Library/Logs/RemoteSpatialProbe")


def pct(values, p):
    if not values:
        return float("nan")
    s = sorted(values)
    return s[min(len(s) - 1, int(p / 100 * len(s)))]


def line(name, values, unit="ms"):
    if not values:
        print(f"  {name:38s} n/a")
        return
    print(f"  {name:38s} median {statistics.median(values):7.2f} {unit}   p5 {pct(values, 5):7.2f}   p95 {pct(values, 95):7.2f}   (n={len(values)})")


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else max(glob.glob(os.path.join(LOG_DIR, "frames-*.csv")), key=os.path.getmtime, default=None)
    if not path:
        sys.exit(f"no frames-*.csv in {LOG_DIR}")
    rows = list(csv.DictReader(open(path)))
    if not rows:
        sys.exit(f"{path}: no frames")
    f = lambda r, k: float(r[k]) if r[k] else None
    rows.sort(key=lambda r: int(r["frame_index"]))

    pres = [f(r, "drawable_presentation") for r in rows]
    duration = pres[-1] - pres[0] if len(pres) > 1 else 0
    intervals = [(b - a) * 1000 for a, b in zip(pres, pres[1:])]
    pfi = [int(r["presentation_frame_index"]) for r in rows]
    skipped = sum(max(0, b - a - 1 - int(r["frame_repeat_count"])) for a, b, r in zip(pfi, pfi[1:], rows[1:]))

    print(f"file: {path}")
    print(f"frames: {len(rows)} over {duration:.1f} s  ->  {len(rows) / duration if duration else 0:.1f} fps")
    print(f"late (GPU done after renderingDeadline): {sum(r['late'] == '1' for r in rows)}")
    print(f"skipped compositor frames (presentation_frame_index gaps): {skipped}")
    print(f"device anchor present: {sum(r['anchor_present'] == '1' for r in rows)}/{len(rows)}, tracked: {sum(r['anchor_tracked'] == '1' for r in rows)}")
    print("timing:")
    line("presentation interval", intervals)
    line("frame query -> predicted presentation", [(f(r, 'drawable_presentation') - f(r, 't_query')) * 1000 for r in rows])
    line("input (optimal) -> predicted presentation", [(f(r, 'drawable_presentation') - f(r, 't_input')) * 1000 for r in rows])
    line("optimalInputTime -> renderingDeadline", [(f(r, 'rendering_deadline') - f(r, 'optimal_input_time')) * 1000 for r in rows])
    line("renderingDeadline -> presentation", [(f(r, 'drawable_presentation') - f(r, 'rendering_deadline')) * 1000 for r in rows])
    line("trackableAnchorTime - presentation", [(f(r, 'trackable_anchor_time') - f(r, 'drawable_presentation')) * 1000 for r in rows])
    line("commit -> GPU done", [(f(r, 't_gpu_done') - f(r, 't_commit')) * 1000 for r in rows])
    line("GPU done -> presentation", [(f(r, 'drawable_presentation') - f(r, 't_gpu_done')) * 1000 for r in rows])
    line("GPU time", [f(r, 'gpu_ms') for r in rows])
    line("pose query time - anchor timestamp", [(f(r, 'pose_query_time') - f(r, 'anchor_timestamp')) * 1000 for r in rows if r["anchor_timestamp"]])
    delays = sorted({r["pose_delay_ms"] for r in rows})
    if delays != ["0.000"]:
        print(f"pose delays used (reprojection test): {', '.join(delays)} ms")
    flashes = [r for r in rows if r["flash"] == "1"]
    print(f"flash frames: {len(flashes)} (press times and press->presentation are in the session-*.txt next to the CSV)")


if __name__ == "__main__":
    main()
