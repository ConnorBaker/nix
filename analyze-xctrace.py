#!/usr/bin/env python3
"""Analyze an xctrace Time Profiler export to find hot functions.

Usage:
    xctrace export --input trace.trace \
        --xpath '/trace-toc/run/data/table[@schema="time-profile"]' \
        | python3 analyze-xctrace.py [--focus SUBSTRING]

Options:
    --focus SUBSTRING   Show detailed breakdown for functions matching SUBSTRING
"""

import sys
import argparse
import xml.etree.ElementTree as ET
import collections


def parse_trace(input_stream):
    tree = ET.parse(input_stream)
    root = tree.getroot()

    frame_names = {}
    for elem in root.iter("frame"):
        fid = elem.get("id") or elem.get("ref")
        name = elem.get("name")
        if fid and name:
            frame_names[fid] = name

    self_counts = collections.Counter()
    total_counts = collections.Counter()
    # For --focus: leaf functions under the target
    focus_leaf_counts = collections.Counter()
    focus_samples = 0
    total_samples = 0
    rows = []

    for row in root.iter("row"):
        bt = row.find(".//backtrace")
        if bt is None:
            continue
        frames = bt.findall("frame")
        if not frames:
            continue
        total_samples += 1

        names = []
        for f in frames:
            fid = f.get("id") or f.get("ref")
            names.append(frame_names.get(fid, f.get("name", "???")))

        rows.append(names)

        # Leaf = first frame (self time)
        self_counts[names[0]] += 1

        # All frames (inclusive time)
        seen = set()
        for n in names:
            if n not in seen:
                total_counts[n] += 1
                seen.add(n)

    return total_samples, self_counts, total_counts, rows


def print_top(label, counts, total_samples, n=30):
    print(f"\n=== Top {n} by {label} ===")
    for name, count in counts.most_common(n):
        pct = 100.0 * count / total_samples
        print(f"  {pct:6.2f}%  {count:6d}  {name}")


def print_focus(target, rows, total_samples):
    leaf_counts = collections.Counter()
    focus_total = 0

    for names in rows:
        if any(target in n for n in names):
            focus_total += 1
            leaf_counts[names[0]] += 1

    self_in_target = sum(c for n, c in leaf_counts.items() if target in n)

    print(f"\n=== Focus: *{target}* ===")
    print(f"  Inclusive: {100 * focus_total / total_samples:.2f}% ({focus_total} samples)")
    print(f"  Self:      {100 * self_in_target / total_samples:.2f}% ({self_in_target} samples)")
    print(f"\n  Leaf functions under *{target}*:")
    for name, count in leaf_counts.most_common(20):
        pct = 100.0 * count / total_samples
        print(f"    {pct:5.2f}%  {count:5d}  {name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--focus", action="append", default=[],
                        help="Show detailed breakdown for functions matching SUBSTRING (repeatable)")
    parser.add_argument("--top", type=int, default=30,
                        help="Number of top functions to show (default: 30)")
    args = parser.parse_args()

    total_samples, self_counts, total_counts, rows = parse_trace(sys.stdin)

    print(f"Total samples: {total_samples}")
    print_top("SELF time", self_counts, total_samples, args.top)
    print_top("INCLUSIVE time", total_counts, total_samples, args.top)

    for target in args.focus:
        print_focus(target, rows, total_samples)


if __name__ == "__main__":
    main()
