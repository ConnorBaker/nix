#!/usr/bin/env python3
"""Compare two xctrace Time Profiler exports by leaf function.

Usage: python3 compare-xctrace.py <trace-a> <trace-b> [label-a] [label-b]
"""

import sys
import xml.etree.ElementTree as ET
import subprocess
from collections import defaultdict


def parse_profile(path):
    result = subprocess.run(
        ["xctrace", "export", "--input", path,
         "--xpath", "/trace-toc/run/data/table[@schema='time-profile']"],
        capture_output=True, text=True, timeout=120,
    )
    result.check_returncode()
    root = ET.fromstring(result.stdout)

    # Build id->element map for ref resolution
    id_map = {}
    for el in root.iter():
        eid = el.get("id")
        if eid:
            id_map[eid] = el

    def resolve(el):
        ref = el.get("ref")
        return id_map[ref] if ref and ref in id_map else el

    fn_counts = defaultdict(int)
    total = 0
    for row in root.iter("row"):
        total += 1
        bt = row.find(".//tagged-backtrace")
        if bt is not None:
            bt = resolve(bt)
            inner = bt.find("backtrace")
            if inner is not None:
                bt = inner
        else:
            bt = row.find(".//backtrace")
            if bt is not None:
                bt = resolve(bt)
        if bt is None:
            continue
        frames = bt.findall("frame")
        if frames:
            fn_counts[resolve(frames[0]).get("name", "???")] += 1

    return fn_counts, total


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        sys.exit(1)

    path_a, path_b = sys.argv[1], sys.argv[2]
    label_a = sys.argv[3] if len(sys.argv) > 3 else "A"
    label_b = sys.argv[4] if len(sys.argv) > 4 else "B"

    a, a_n = parse_profile(path_a)
    b, b_n = parse_profile(path_b)

    print(f"{label_a}: {a_n} samples ({a_n/1000:.1f}s)")
    print(f"{label_b}: {b_n} samples ({b_n/1000:.1f}s)")
    print()

    # Top 30 for each
    for label, data, n in [(label_a, a, a_n), (label_b, b, b_n)]:
        print(f"=== {label}: top 20 ===")
        print(f"{'ms':>7} {'%':>6}  Function")
        for fn, c in sorted(data.items(), key=lambda x: -x[1])[:20]:
            print(f"{c:>7} {c/n*100:>5.1f}%  {fn[:100]}")
        print()

    # Deltas
    all_fns = set(a.keys()) | set(b.keys())
    diffs = [(fn, a.get(fn, 0) - b.get(fn, 0), a.get(fn, 0), b.get(fn, 0))
             for fn in all_fns]
    diffs.sort(key=lambda x: -abs(x[1]))

    print(f"=== Biggest deltas ({label_a} - {label_b}) ===")
    print(f"{'delta':>7} {label_a:>7} {label_b:>7}  Function")
    for fn, d, va, vb in diffs[:25]:
        print(f"{d:>+7} {va:>7} {vb:>7}  {fn[:90]}")


if __name__ == "__main__":
    main()
