#!/usr/bin/env python3
"""
Analyze and compare Nix evaluation debug logs (--debug output).

Supports one or more log files. With two or more, produces comparison tables
with deltas. Focuses on:
- Store copies (individual files and full-directory copies)
- Daemon worker operations (by op code)
- File evaluations and evaluation ordering
- Derivation instantiations and per-pass breakdown
- Dependency recording overhead and redundancy
- Event timeline and pass structure
- JSON stats block comparison

Usage:
    analyze-eval-logs.py [--nixpkgs PATH] [--labels L1 L2 ...] LOG1 [LOG2 ...]

Labels default to log file stems. The --nixpkgs path enables pass detection
(passes are split on full-directory copies of that source tree) and path
shortening in output.
"""

import argparse
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

# ── Configuration (set in main) ──────────────────────────────────────────────

_nixpkgs_dir: str | None = None
_nixpkgs_prefix: str | None = None

# ── Known daemon worker op codes (from nix source: worker-protocol.hh) ───────

DAEMON_OP_NAMES = {
    1: "IsValidPath",
    2: "HasSubstitutes",
    3: "QueryPathHash",
    4: "QueryReferences",
    5: "QueryReferrers",
    6: "AddToStore",
    7: "AddTextToStore (legacy)",
    8: "BuildPaths",
    9: "EnsurePath",
    10: "AddTempRoot",
    11: "AddIndirectRoot",
    12: "SyncWithGC",
    13: "FindRoots",
    14: "ExportPath (removed)",
    15: "QueryDeriver",
    16: "SetOptions",
    17: "CollectGarbage",
    18: "QuerySubstitutablePathInfo",
    19: "QueryDerivationOutputs (removed)",
    20: "QueryAllValidPaths",
    21: "QueryFailedPaths (removed)",
    22: "ClearFailedPaths (removed)",
    23: "QueryPathInfo",
    24: "ImportPaths (removed)",
    25: "QueryDerivationOutputNames (removed)",
    26: "QueryPathFromHashPart",
    27: "QuerySubstitutablePathInfos",
    28: "QueryValidPaths",
    29: "QuerySubstitutablePaths",
    30: "QueryValidDerivers",
    31: "OptimiseStore",
    32: "VerifyStore",
    33: "BuildDerivation",
    34: "AddSignatures",
    35: "NarFromPath",
    36: "AddToStoreNar",
    37: "QueryMissing",
    38: "QueryDerivationOutputMap",
    39: "RegisterDrvOutput",
    40: "QueryRealisation",
    41: "AddMultipleToStore",
    42: "AddBuildLog",
    43: "BuildPathsWithResults",
    44: "AddPermRoot",
}


# ── Helpers ──────────────────────────────────────────────────────────────────

COL_WIDTH = 14


def fmt(n) -> str:
    """Format a number with comma separators."""
    if isinstance(n, float):
        return f"{n:,.3f}"
    return f"{n:,}"


def short(path: str) -> str:
    """Shorten a path for display using the configured nixpkgs prefix."""
    if _nixpkgs_prefix:
        path = path.replace(_nixpkgs_prefix, "nixpkgs/")
    if _nixpkgs_dir:
        path = path.replace(_nixpkgs_dir, "nixpkgs")
    return path


def print_section(title: str):
    print(f"\n{'=' * 78}")
    print(f"  {title}")
    print(f"{'=' * 78}")


def print_subsection(title: str):
    print(f"\n--- {title} ---")


def print_table_header(labels: list[str], *, metric_label: str = "Metric",
                       metric_width: int = 45, show_delta: bool = False):
    """Print column headers for a comparison table."""
    parts_h = [f"  {metric_label:<{metric_width}}"]
    parts_s = [f"  {'-' * metric_width}"]
    for lbl in labels:
        parts_h.append(f" {lbl:>{COL_WIDTH}}")
        parts_s.append(f" {'-' * COL_WIDTH}")
    if show_delta:
        parts_h.append(f" {'delta':>{COL_WIDTH}}")
        parts_s.append(f" {'-' * COL_WIDTH}")
    print("\n" + "".join(parts_h))
    print("".join(parts_s))


def print_row(label: str, values: list, *, show_delta: bool = False,
              label_width: int = 45):
    """Print a table row with a label and right-aligned value columns."""
    parts = [f"  {label:<{label_width}}"]
    for v in values:
        parts.append(f" {fmt(v):>{COL_WIDTH}}")
    if show_delta and len(values) >= 2:
        delta = values[-1] - values[0]
        sign = "+" if delta > 0 else ""
        parts.append(f" {sign + fmt(delta):>{COL_WIDTH}}")
    print("".join(parts))


# ── Parsing ──────────────────────────────────────────────────────────────────

class LogEvent:
    """A single parsed event from the log, with its line number."""
    __slots__ = ("lineno", "kind", "data")

    def __init__(self, lineno, kind, data=None):
        self.lineno = lineno
        self.kind = kind
        self.data = data or {}


def parse_log_events(path: str) -> list[LogEvent]:
    """Parse a Nix --debug log into a list of typed events."""
    events = []
    json_lines = []
    in_json = False
    json_depth = 0

    with open(path, "r") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.rstrip("\n")

            # JSON stats block detection
            if in_json:
                json_lines.append(line)
                json_depth += line.count("{") - line.count("}")
                if json_depth == 0:
                    try:
                        obj = json.loads("\n".join(json_lines))
                        events.append(LogEvent(lineno, "stats_json", {"json": obj}))
                    except json.JSONDecodeError:
                        pass
                    in_json = False
                continue
            if line == "{":
                in_json = True
                json_depth = 1
                json_lines = [line]
                continue

            # copying '<path>' to the store...
            m = re.match(r"copying '(.+?)' to the store\.\.\.", line)
            if m:
                events.append(LogEvent(lineno, "copy_start", {"path": m.group(1)}))
                continue

            # copied '<src>' to '<dst>' (hash '<hash>')
            m = re.match(r"copied '(.+?)' to '(.+?)' \(hash '(.+?)'\)", line)
            if m:
                events.append(LogEvent(lineno, "copy_done", {
                    "src": m.group(1), "dst": m.group(2), "hash": m.group(3),
                }))
                continue

            # source path '<path>' is uncacheable
            m = re.match(r"source path '(.+?)' is uncacheable", line)
            if m:
                events.append(LogEvent(lineno, "uncacheable", {"path": m.group(1)}))
                continue

            # performing daemon worker op: <N>
            m = re.match(r"performing daemon worker op: (\d+)", line)
            if m:
                events.append(LogEvent(lineno, "daemon_op", {"op": int(m.group(1))}))
                continue

            # evaluating file '<path>'
            m = re.match(r"evaluating file '(.+?)'", line)
            if m:
                events.append(LogEvent(lineno, "eval_file", {"path": m.group(1)}))
                continue

            # instantiated '<name>' -> '<drv>'
            m = re.match(r"instantiated '(.+?)' -> '(.+?)'", line)
            if m:
                events.append(LogEvent(lineno, "instantiated", {
                    "name": m.group(1), "drv": m.group(2),
                }))
                continue

            # recording <type> (<variant>) dep: input='...' key='...'
            m = re.match(
                r"recording (\w+) \((\w+)\) dep: input='(.*?)' key='(.*?)'", line
            )
            if m:
                events.append(LogEvent(lineno, "recording_dep", {
                    "type": m.group(1), "variant": m.group(2),
                    "input": m.group(3), "key": m.group(4),
                }))
                continue

            # trace verify hit for '<attr>'
            m = re.match(r"trace verify hit for '(.+?)'", line)
            if m:
                events.append(LogEvent(lineno, "trace_hit", {"attr": m.group(1)}))
                continue

            # stat hash store: loading N entries
            m = re.match(r"stat hash store: loading (\d+) entries", line)
            if m:
                events.append(LogEvent(lineno, "stat_hash_store", {"count": int(m.group(1))}))
                continue

            # getting root value
            if line.startswith("getting root value"):
                events.append(LogEvent(lineno, "root_value"))
                continue

            # output JSON line (the result attrset)
            if line.startswith("{\""):
                events.append(LogEvent(lineno, "output_json", {"line": line}))
                continue

            # warnings
            if line.startswith("warning:"):
                events.append(LogEvent(lineno, "warning", {"msg": line}))
                continue

    return events


def aggregate_events(events: list[LogEvent]) -> dict:
    """Aggregate event list into summary statistics."""
    result = {
        "copying_to_store": [],
        "copying_nixpkgs": 0,
        "daemon_ops": Counter(),
        "evaluating_files": [],
        "instantiated": [],
        "recording_deps": Counter(),
        "recording_deps_detail": [],
        "stats_json": None,
        "total_lines": events[-1].lineno if events else 0,
        "events": events,
        "copy_done": [],
        "uncacheable": [],
        "trace_hits": [],
        "passes": [],  # populated after aggregation
    }

    for e in events:
        if e.kind == "copy_start":
            p = e.data["path"]
            result["copying_to_store"].append(p)
            if _nixpkgs_dir and p == _nixpkgs_dir:
                result["copying_nixpkgs"] += 1
        elif e.kind == "copy_done":
            result["copy_done"].append((e.data["src"], e.data["dst"], e.data["hash"]))
        elif e.kind == "uncacheable":
            result["uncacheable"].append(e.data["path"])
        elif e.kind == "daemon_op":
            result["daemon_ops"][e.data["op"]] += 1
        elif e.kind == "eval_file":
            result["evaluating_files"].append(e.data["path"])
        elif e.kind == "instantiated":
            result["instantiated"].append((e.data["name"], e.data["drv"]))
        elif e.kind == "recording_dep":
            label = f"{e.data['type']} ({e.data['variant']})"
            result["recording_deps"][label] += 1
            result["recording_deps_detail"].append(
                (e.data["type"], e.data["variant"], e.data["input"], e.data["key"])
            )
        elif e.kind == "stats_json":
            result["stats_json"] = e.data["json"]
        elif e.kind == "trace_hit":
            result["trace_hits"].append((e.lineno, e.data["attr"]))

    result["passes"] = detect_passes(events)
    return result


# ── Pass detection ───────────────────────────────────────────────────────────

def detect_passes(events: list[LogEvent]) -> list[dict]:
    """Split events into evaluation passes.

    If a nixpkgs directory is configured, passes are split on full-directory
    copies of that path. Otherwise all events form a single pass.
    """
    boundaries = [0]
    if _nixpkgs_dir:
        for i, e in enumerate(events):
            if e.kind == "copy_start" and e.data["path"] == _nixpkgs_dir:
                boundaries.append(i)
    boundaries.append(len(events))

    passes = []
    for j in range(len(boundaries) - 1):
        start = boundaries[j]
        end = boundaries[j + 1]
        span = events[start:end]

        drvs = [(e.data["name"], e.data["drv"]) for e in span if e.kind == "instantiated"]
        evals = [e.data["path"] for e in span if e.kind == "eval_file"]
        copies = [e.data["path"] for e in span if e.kind == "copy_start"]
        daemon = Counter(e.data["op"] for e in span if e.kind == "daemon_op")
        recs = [(e.data["type"], e.data["key"]) for e in span if e.kind == "recording_dep"]
        hits = [(e.lineno, e.data["attr"]) for e in span if e.kind == "trace_hit"]
        uncacheable = [e.data["path"] for e in span if e.kind == "uncacheable"]

        passes.append({
            "index": j,
            "start_line": span[0].lineno if span else 0,
            "end_line": span[-1].lineno if span else 0,
            "instantiated": drvs,
            "drv_paths": set(d[1] for d in drvs),
            "eval_files": evals,
            "copies": copies,
            "daemon_ops": daemon,
            "recordings": recs,
            "trace_hits": hits,
            "uncacheable": uncacheable,
            "event_count": len(span),
        })

    return passes


# ── Analysis functions ───────────────────────────────────────────────────────

def analyze_copy_redundancy(copies: list[str]) -> dict:
    counter = Counter(copies)
    total = len(copies)
    unique = len(counter)
    return {
        "total": total,
        "unique": unique,
        "duplicate_copies": total - unique,
        "top_duplicated": counter.most_common(30),
    }


def analyze_recording_redundancy(details: list[tuple]) -> dict:
    key_counter = Counter((d[0], d[3]) for d in details)
    total = len(details)
    unique = len(key_counter)
    return {
        "total": total,
        "unique_keys": unique,
        "duplicate_recordings": total - unique,
        "top_duplicated": key_counter.most_common(30),
    }


def flat_stats(d, prefix=""):
    """Flatten nested stats dict."""
    out = {}
    for k, v in d.items():
        key = f"{prefix}.{k}" if prefix else k
        if isinstance(v, dict):
            out.update(flat_stats(v, key))
        else:
            out[key] = v
    return out


# ── Report sections ──────────────────────────────────────────────────────────

def report_overview(logs: list[tuple[str, dict]]):
    print_section("1. OVERVIEW")

    labels = [lbl for lbl, _ in logs]
    show_delta = len(logs) == 2
    print_table_header(labels, show_delta=show_delta)

    metrics = [
        ("Total log lines",
         [d["total_lines"] for _, d in logs]),
        ("Files evaluated",
         [len(d["evaluating_files"]) for _, d in logs]),
        ("Derivations instantiated",
         [len(d["instantiated"]) for _, d in logs]),
        ("Unique drv paths",
         [len(set(drv for _, drv in d["instantiated"])) for _, d in logs]),
        ("Total store copies",
         [len(d["copying_to_store"]) for _, d in logs]),
        ("  of which nixpkgs dir copies",
         [d["copying_nixpkgs"] for _, d in logs]),
        ("Uncacheable source paths",
         [len(d["uncacheable"]) for _, d in logs]),
        ("Completed copy+hash ops",
         [len(d["copy_done"]) for _, d in logs]),
        ("Total daemon ops",
         [sum(d["daemon_ops"].values()) for _, d in logs]),
        ("Dep recordings",
         [sum(d["recording_deps"].values()) for _, d in logs]),
        ("Trace cache hits",
         [len(d["trace_hits"]) for _, d in logs]),
    ]
    for metric, values in metrics:
        print_row(metric, values, show_delta=show_delta)


def report_passes(logs: list[tuple[str, dict]]):
    print_section("2. EVALUATION PASS STRUCTURE")

    if not _nixpkgs_dir:
        print("\n  (pass detection requires --nixpkgs)")
        return

    for label, data in logs:
        passes = data["passes"]
        print_subsection(f"{label}: {len(passes)} passes (split by nixpkgs dir copies)")

        for p in passes:
            n_inst = len(p["instantiated"])
            n_drvs = len(p["drv_paths"])
            n_evals = len(p["eval_files"])
            n_copies = len(p["copies"])
            n_recs = len(p["recordings"])
            n_daemon = sum(p["daemon_ops"].values())
            n_uncache = len(p["uncacheable"])
            last_drv = p["instantiated"][-1] if p["instantiated"] else ("\u2014", "\u2014")
            hits = p["trace_hits"]

            print(f"\n  Pass {p['index']}  (lines {p['start_line']:,}\u2013{p['end_line']:,},"
                  f" {p['event_count']:,} events)")
            print(f"    Instantiated:    {fmt(n_inst):>8}  ({fmt(n_drvs)} unique drv paths)")
            print(f"    Files evaluated: {fmt(n_evals):>8}")
            print(f"    Store copies:    {fmt(n_copies):>8}  (uncacheable: {n_uncache})")
            print(f"    Daemon ops:      {fmt(n_daemon):>8}")
            if n_recs:
                print(f"    Dep recordings:  {fmt(n_recs):>8}")
            if hits:
                for ln, attr in hits:
                    print(f"    Trace hit at line {ln:,}: '{attr}'")
            if p["instantiated"]:
                print(f"    Last drv: {last_drv[0]} \u2192 ...{last_drv[1].split('/')[-1]}")

    if len(logs) < 2:
        return

    # Cross-pass drv path overlap (per log)
    for label, data in logs:
        passes = data["passes"]
        has_overlap = False
        for i in range(len(passes)):
            for j in range(i + 1, len(passes)):
                si = passes[i]["drv_paths"]
                sj = passes[j]["drv_paths"]
                if not si or not sj:
                    continue
                overlap = si & sj
                if overlap:
                    if not has_overlap:
                        print_subsection(f"{label}: cross-pass drv path overlap")
                        has_overlap = True
                    bigger = max(len(si), len(sj))
                    print(f"  Pass {i} ({fmt(len(si))}) \u2229 Pass {j} ({fmt(len(sj))})"
                          f" = {fmt(len(overlap))} drvs"
                          f" ({len(overlap) / bigger * 100:.1f}%)")

    # Pairwise pass identity matching
    for i, (lbl_a, data_a) in enumerate(logs):
        for j, (lbl_b, data_b) in enumerate(logs):
            if j <= i:
                continue
            passes_a = data_a["passes"]
            passes_b = data_b["passes"]
            print_subsection(f"Pass identity: {lbl_a} \u2192 {lbl_b}")
            for pi, pa in enumerate(passes_a):
                if not pa["drv_paths"]:
                    continue
                best_j, best_n = -1, 0
                for pj, pb in enumerate(passes_b):
                    if not pb["drv_paths"]:
                        continue
                    n = len(pa["drv_paths"] & pb["drv_paths"])
                    if n > best_n:
                        best_j, best_n = pj, n
                if best_j >= 0:
                    pct = best_n / len(pa["drv_paths"]) * 100
                    print(f"  {lbl_a}[{pi}] ({fmt(len(pa['drv_paths']))}) \u2192 "
                          f"{lbl_b}[{best_j}]"
                          f" ({fmt(len(passes_b[best_j]['drv_paths']))})"
                          f" overlap {fmt(best_n)} ({pct:.1f}%)")

    # Redundant pass identification (within each log)
    for label, data in logs:
        passes = data["passes"]
        if len(passes) <= 1:
            continue
        found = False
        for tp in passes:
            if not tp["drv_paths"]:
                continue
            for tp2 in passes:
                if tp2["index"] == tp["index"] or not tp2["drv_paths"]:
                    continue
                overlap = len(tp["drv_paths"] & tp2["drv_paths"])
                if tp["drv_paths"] and overlap / len(tp["drv_paths"]) > 0.8:
                    if not found:
                        print_subsection(f"{label}: redundant pass identification")
                        found = True
                    print(f"  ** Pass {tp['index']}"
                          f" ({fmt(len(tp['instantiated']))} drvs)"
                          f" is {overlap / len(tp['drv_paths']) * 100:.0f}%"
                          f" duplicate of pass {tp2['index']}")


def report_nixpkgs_copies(logs: list[tuple[str, dict]]):
    if not _nixpkgs_dir:
        return

    print_section("3. NIXPKGS DIRECTORY COPIES")

    for label, data in logs:
        copies = [
            (e.lineno, e.data.get("src", ""), e.data.get("dst", ""),
             e.data.get("hash", ""))
            for e in data["events"]
            if e.kind == "copy_done" and e.data.get("src") == _nixpkgs_dir
        ]
        print(f"\n  {label}: {data['copying_nixpkgs']} copy operations")
        for ln, src, dst, h in copies:
            print(f"    line {ln:>7,}: \u2192 {dst}")
            print(f"               hash: {h}")
        if copies:
            hashes = set(c[3] for c in copies)
            if len(hashes) == 1:
                print(f"    All copies produced the SAME hash \u2014 truly redundant")
            else:
                print(f"    Produced {len(hashes)} distinct hashes \u2014"
                      f" different content?")


def report_store_copies(logs: list[tuple[str, dict]]):
    print_section("4. STORE COPY ANALYSIS")

    for label, data in logs:
        analysis = analyze_copy_redundancy(data["copying_to_store"])
        print_subsection(f"{label}: {fmt(analysis['total'])} copies, "
                         f"{fmt(analysis['unique'])} unique, "
                         f"{fmt(analysis['duplicate_copies'])} duplicates")
        if analysis["top_duplicated"]:
            print(f"  Top duplicated:")
            for path, count in analysis["top_duplicated"][:15]:
                if count > 1:
                    print(f"    {count:>4}x  {short(path)}")

    # Categorize copies by directory
    if _nixpkgs_dir:
        print_subsection("Copy distribution by source directory")
        for label, data in logs:
            dir_counter = Counter()
            for p in data["copying_to_store"]:
                if p == _nixpkgs_dir:
                    dir_counter["nixpkgs/ (whole dir)"] += 1
                elif p.startswith(_nixpkgs_prefix):
                    rel = p[len(_nixpkgs_prefix):]
                    parts = rel.split("/")
                    key = ("/".join(parts[:min(3, len(parts) - 1)])
                           if len(parts) > 1 else parts[0])
                    dir_counter[f"nixpkgs/{key}/"] += 1
                else:
                    dir_counter[p] += 1
            print(f"\n  {label}:")
            for d, c in dir_counter.most_common(20):
                print(f"    {c:>5}  {d}")

    # Per-pass copy analysis
    for label, data in logs:
        passes = data["passes"]
        if len(passes) <= 1:
            continue
        print_subsection(f"Store copies per pass ({label})")
        for p in passes:
            n = len(p["copies"])
            if n:
                print(f"  Pass {p['index']}: {n:>4} copies")


def report_uncacheable(logs: list[tuple[str, dict]]):
    print_section("5. UNCACHEABLE SOURCE PATHS")
    for label, data in logs:
        paths = Counter(data["uncacheable"])
        unique = len(paths)
        total = sum(paths.values())
        print(f"\n  {label}: {fmt(total)} uncacheable events,"
              f" {fmt(unique)} unique paths")
        repeated = [(p, c) for p, c in paths.most_common() if c > 1]
        if repeated:
            print(f"  Paths marked uncacheable multiple times:")
            for p, c in repeated[:10]:
                print(f"    {c:>3}x  {short(p)}")


def report_daemon_ops(logs: list[tuple[str, dict]]):
    print_section("6. DAEMON WORKER OPERATIONS")

    labels = [lbl for lbl, _ in logs]
    all_ops = sorted(set().union(*(d["daemon_ops"].keys() for _, d in logs)))
    show_delta = len(logs) == 2

    # Header
    parts_h = [f"  {'Op':>4} {'Name':<40}"]
    parts_s = [f"  {'-' * 4} {'-' * 40}"]
    for lbl in labels:
        parts_h.append(f" {lbl:>10}")
        parts_s.append(f" {'-' * 10}")
    if show_delta:
        parts_h.append(f" {'delta':>10}")
        parts_s.append(f" {'-' * 10}")
    print("\n" + "".join(parts_h))
    print("".join(parts_s))

    for op in all_ops:
        values = [d["daemon_ops"].get(op, 0) for _, d in logs]
        name = DAEMON_OP_NAMES.get(op, f"Unknown({op})")
        parts = [f"  {op:>4} {name:<40}"]
        for v in values:
            parts.append(f" {fmt(v):>10}")
        if show_delta:
            delta = values[-1] - values[0]
            sign = "+" if delta > 0 else ""
            parts.append(f" {sign + fmt(delta):>10}")
        print("".join(parts))

    # Per-pass daemon ops
    for label, data in logs:
        passes = data["passes"]
        if len(passes) <= 1:
            continue
        print_subsection(f"Daemon ops per pass ({label})")
        for p in passes:
            total = sum(p["daemon_ops"].values())
            if total:
                breakdown = ", ".join(
                    f"op{k}={v}" for k, v in sorted(p["daemon_ops"].items()))
                print(f"  Pass {p['index']}: {fmt(total):>8} total  ({breakdown})")


def report_recording_deps(logs: list[tuple[str, dict]]):
    print_section("7. DEPENDENCY RECORDING")

    for label, data in logs:
        if not data["recording_deps"]:
            continue

        print_subsection(f"{label}: dep type breakdown")
        print(f"\n  {'Dep type':<45} {'Count':>10}")
        print(f"  {'-' * 45} {'-' * 10}")
        for dep_type, count in data["recording_deps"].most_common():
            print(f"  {dep_type:<45} {fmt(count):>10}")

        # Redundancy
        rec = analyze_recording_redundancy(data["recording_deps_detail"])
        dup_rate = (rec["duplicate_recordings"] / rec["total"] * 100
                    if rec["total"] else 0)
        print_subsection(f"{label}: recording redundancy:"
                         f" {fmt(rec['total'])} total,"
                         f" {fmt(rec['unique_keys'])} unique,"
                         f" {dup_rate:.1f}% duplicate")

        if rec["top_duplicated"]:
            print(f"  Top duplicated recordings:")
            for (dep_type, key), count in rec["top_duplicated"][:20]:
                if count > 1:
                    print(f"    {count:>6}x  {dep_type}: {short(key)}")

        # Per dep type breakdown
        print_subsection(f"{label}: by dep type: unique vs duplicate")
        by_type = defaultdict(lambda: {"total": 0, "keys": Counter()})
        for dep_type, variant, inp, key in data["recording_deps_detail"]:
            by_type[dep_type]["total"] += 1
            by_type[dep_type]["keys"][(dep_type, key)] += 1
        for dep_type in sorted(by_type,
                               key=lambda t: -by_type[t]["total"]):
            info = by_type[dep_type]
            unique = len(info["keys"])
            dup = info["total"] - unique
            dup_pct = dup / info["total"] * 100 if info["total"] else 0
            print(f"  {dep_type:<25} total={fmt(info['total']):>8}  "
                  f"unique={fmt(unique):>8}  dup={fmt(dup):>8} ({dup_pct:.1f}%)")

        # Per-pass recording distribution
        passes = data["passes"]
        if len(passes) > 1:
            print_subsection(f"{label}: recordings per pass")
            for p in passes:
                n = len(p["recordings"])
                if n:
                    type_counts = Counter(r[0] for r in p["recordings"])
                    breakdown = ", ".join(
                        f"{k}={v}" for k, v in type_counts.most_common(5))
                    print(f"  Pass {p['index']}: {fmt(n):>8} recordings"
                          f"  ({breakdown})")


def report_file_eval(logs: list[tuple[str, dict]]):
    print_section("8. FILE EVALUATION COMPARISON")

    file_sets = {lbl: set(d["evaluating_files"]) for lbl, d in logs}

    for lbl, fset in file_sets.items():
        print(f"  {lbl} unique files: {len(fset)}")

    if len(logs) >= 2:
        labels = list(file_sets.keys())
        common = file_sets[labels[0]]
        for lbl in labels[1:]:
            common = common & file_sets[lbl]
        print(f"  Common:{'':>15} {len(common)}")

        # Pairwise unique files
        for i, lbl_a in enumerate(labels):
            for j, lbl_b in enumerate(labels):
                if j <= i:
                    continue
                only_a = file_sets[lbl_a] - file_sets[lbl_b]
                only_b = file_sets[lbl_b] - file_sets[lbl_a]
                if only_b:
                    print(f"\n  Files evaluated ONLY in {lbl_b} ({len(only_b)}):")
                    for f in sorted(only_b)[:15]:
                        print(f"    {short(f)}")
                if only_a:
                    print(f"\n  Files evaluated ONLY in {lbl_a} ({len(only_a)}):")
                    for f in sorted(only_a)[:15]:
                        print(f"    {short(f)}")

    # Duplicate evaluations
    for label, data in logs:
        counter = Counter(data["evaluating_files"])
        dups = {k: v for k, v in counter.items() if v > 1}
        if dups:
            print_subsection(f"{label}: files evaluated multiple times"
                             f" ({len(dups)} files)")
            for f, count in sorted(dups.items(), key=lambda x: -x[1])[:15]:
                print(f"    {count}x  {short(f)}")


def report_drv_instantiation(logs: list[tuple[str, dict]]):
    print_section("9. DERIVATION INSTANTIATION DETAILS")

    name_counters = {lbl: Counter(name for name, _ in d["instantiated"])
                     for lbl, d in logs}

    # Count distribution per log
    for label, names in name_counters.items():
        dist = Counter(names.values())
        print_subsection(f"{label}: instantiation count distribution")
        for count in sorted(dist.keys()):
            n_names = dist[count]
            print(f"  {count:>5}x: {fmt(n_names):>6} unique drv names")

    # Pairwise ratio analysis
    if len(logs) >= 2:
        labels = list(name_counters.keys())
        for i, lbl_a in enumerate(labels):
            for j, lbl_b in enumerate(labels):
                if j <= i:
                    continue
                total_a = sum(name_counters[lbl_a].values())
                total_b = sum(name_counters[lbl_b].values())
                if total_a:
                    ratio = total_b / total_a
                    print_subsection(
                        f"Instantiation ratio: {lbl_b}/{lbl_a}"
                        f" = {total_b}/{total_a}"
                        f" = {ratio:.4f}")

    # Top diffs (pairwise)
    if len(logs) >= 2:
        labels = list(name_counters.keys())
        for i, lbl_a in enumerate(labels):
            for j, lbl_b in enumerate(labels):
                if j <= i:
                    continue
                names_a = name_counters[lbl_a]
                names_b = name_counters[lbl_b]
                print_subsection(
                    f"Top drv names with largest count delta"
                    f" ({lbl_a} vs {lbl_b})")
                all_names = set(names_a.keys()) | set(names_b.keys())
                diffs = []
                for name in all_names:
                    ca = names_a.get(name, 0)
                    cb = names_b.get(name, 0)
                    if ca != cb:
                        diffs.append((name, ca, cb, cb - ca))
                diffs.sort(key=lambda x: -abs(x[3]))
                print(f"  {'Name':<50} {lbl_a:>10} {lbl_b:>10} {'delta':>8}")
                print(f"  {'-' * 50} {'-' * 10} {'-' * 10} {'-' * 8}")
                for name, va, vb, d in diffs[:20]:
                    sign = "+" if d > 0 else ""
                    print(f"  {name[:50]:<50} {va:>10} {vb:>10}"
                          f" {sign}{d:>7}")


def report_stats_json(logs: list[tuple[str, dict]]):
    print_section("10. EVALUATION STATS (JSON)")

    stats_logs = [(lbl, d) for lbl, d in logs if d["stats_json"]]
    if not stats_logs:
        print("  (no stats found)")
        return

    labels = [lbl for lbl, _ in stats_logs]
    flat_all = {lbl: flat_stats(d["stats_json"]) for lbl, d in stats_logs}
    all_keys = sorted(set().union(*(f.keys() for f in flat_all.values())))
    show_ratio = len(stats_logs) == 2

    # Header
    parts_h = [f"  {'Stat':<50}"]
    parts_s = [f"  {'-' * 50}"]
    for lbl in labels:
        parts_h.append(f" {lbl:>{COL_WIDTH}}")
        parts_s.append(f" {'-' * COL_WIDTH}")
    if show_ratio:
        parts_h.append(f" {'ratio':>8}")
        parts_s.append(f" {'-' * 8}")
    print("\n" + "".join(parts_h))
    print("".join(parts_s))

    by_ratio = defaultdict(list)
    for key in all_keys:
        values = [flat_all[lbl].get(key) for lbl in labels]
        if not all(isinstance(v, (int, float)) for v in values if v is not None):
            continue
        if any(v is None for v in values):
            continue

        parts = [f"  {key:<50}"]
        for v in values:
            if isinstance(v, float):
                parts.append(f" {v:>{COL_WIDTH}.3f}")
            else:
                parts.append(f" {fmt(v):>{COL_WIDTH}}")

        if show_ratio and values[0] and values[1] and values[0] != 0:
            ratio = values[1] / values[0]
            parts.append(f" {ratio:>7.2f}x")
            by_ratio[round(ratio, 2)].append(key)
        elif show_ratio:
            parts.append(f" {'\u2014':>8}")

        print("".join(parts))

    # Ratio clustering
    if show_ratio and by_ratio:
        print_subsection("Stats grouped by ratio")
        for ratio in sorted(by_ratio.keys()):
            keys = by_ratio[ratio]
            if len(keys) >= 2:
                short_keys = ", ".join(k.split(".")[-1] for k in keys[:6])
                suffix = ", ..." if len(keys) > 6 else ""
                print(f"  {ratio:.2f}x ({len(keys)} stats):"
                      f" {short_keys}{suffix}")


def report_trace_hit_timing(logs: list[tuple[str, dict]]):
    print_section("11. TRACE HIT CONTEXT")

    for label, data in logs:
        if not data["trace_hits"]:
            continue

        events = data["events"]
        passes = data["passes"]

        for p in passes:
            if not p["trace_hits"]:
                continue

            hit_line = p["trace_hits"][0][0]
            hit_attr = p["trace_hits"][0][1]
            pass_start = p["start_line"]
            pass_end = p["end_line"]

            before_hit = [e for e in events
                          if pass_start <= e.lineno < hit_line]
            after_hit = [e for e in events
                         if hit_line <= e.lineno <= pass_end]

            inst_before = sum(1 for e in before_hit
                              if e.kind == "instantiated")
            inst_after = sum(1 for e in after_hit
                             if e.kind == "instantiated")
            rec_before = sum(1 for e in before_hit
                             if e.kind == "recording_dep")
            rec_after = sum(1 for e in after_hit
                            if e.kind == "recording_dep")
            daemon_before = sum(1 for e in before_hit
                                if e.kind == "daemon_op")
            daemon_after = sum(1 for e in after_hit
                               if e.kind == "daemon_op")

            total_inst = inst_before + inst_after
            pass_span = pass_end - pass_start
            hit_pct = ((hit_line - pass_start) / pass_span * 100
                       if pass_span else 0)

            print(f"""
  [{label}] Pass {p['index']}: trace verify hit for '{hit_attr}'
  at line {hit_line:,} (pass spans lines {pass_start:,}\u2013{pass_end:,})

  The verify hit occurs at {hit_pct:.1f}% through the pass.

                      Before hit    After hit     Total
  Instantiations  {inst_before:>12,}  {inst_after:>12,}  {total_inst:>10,}
  Dep recordings  {rec_before:>12,}  {rec_after:>12,}  {rec_before + rec_after:>10,}
  Daemon ops      {daemon_before:>12,}  {daemon_after:>12,}  {daemon_before + daemon_after:>10,}
""")

            # Events around the hit
            hit_idx = None
            for idx, e in enumerate(events):
                if e.lineno == hit_line:
                    hit_idx = idx
                    break
            if hit_idx is not None:
                window = 8
                start = max(0, hit_idx - window)
                end = min(len(events), hit_idx + window + 1)
                print(f"  Events around the trace hit (\u00b1{window}):")
                for e in events[start:end]:
                    marker = " >>>" if e.lineno == hit_line else "    "
                    if e.kind == "recording_dep":
                        detail = (f"{e.data['type']} ({e.data['variant']}):"
                                  f" {short(e.data['key'])[:50]}")
                    elif e.kind == "instantiated":
                        drv_tail = e.data["drv"].split("/")[-1][-40:]
                        detail = f"{e.data['name']} \u2192 ...{drv_tail}"
                    elif e.kind == "daemon_op":
                        detail = DAEMON_OP_NAMES.get(e.data["op"],
                                                     f"op{e.data['op']}")
                    elif e.kind == "trace_hit":
                        detail = f"VERIFY HIT for '{e.data['attr']}'"
                    else:
                        detail = e.kind
                    print(f"  {marker} {e.lineno:>8,}  {detail}")


def report_perpass_recording_uniqueness(logs: list[tuple[str, dict]]):
    print_section("12. PER-PASS RECORDING UNIQUENESS")

    for label, data in logs:
        passes = data["passes"]
        if len(passes) <= 1:
            continue
        has_recordings = any(p["recordings"] for p in passes)
        if not has_recordings:
            continue

        print_subsection(label)
        print(f"\n  {'Pass':<8} {'Total':>8} {'Novel':>8}"
              f" {'Already-seen':>14} {'Novel%':>8}")
        print(f"  {'-' * 8} {'-' * 8} {'-' * 8}"
              f" {'-' * 14} {'-' * 8}")

        seen_keys = set()
        for p in passes:
            novel = 0
            repeat = 0
            for dep_type, key in p["recordings"]:
                k = (dep_type, key)
                if k in seen_keys:
                    repeat += 1
                else:
                    novel += 1
                    seen_keys.add(k)
            total = novel + repeat
            pct = novel / total * 100 if total else 0
            print(f"  Pass {p['index']:<4} {total:>8,} {novel:>8,}"
                  f" {repeat:>14,} {pct:>7.1f}%")

        # Per-pass novel recordings by dep type
        print_subsection(f"{label}: per-pass novel recordings by dep type")
        seen_by_type = defaultdict(set)
        for p in passes:
            type_stats = defaultdict(lambda: {"novel": 0, "repeat": 0})
            for dep_type, key in p["recordings"]:
                k = (dep_type, key)
                if k in seen_by_type[dep_type]:
                    type_stats[dep_type]["repeat"] += 1
                else:
                    type_stats[dep_type]["novel"] += 1
                    seen_by_type[dep_type].add(k)
            if type_stats:
                parts = []
                for dt in sorted(type_stats,
                                 key=lambda t: -(type_stats[t]["novel"]
                                                 + type_stats[t]["repeat"])):
                    s = type_stats[dt]
                    parts.append(f"{dt}={s['novel']}/{s['novel'] + s['repeat']}")
                print(f"  Pass {p['index']}: {', '.join(parts[:6])}")


def report_addtexttostore(logs: list[tuple[str, dict]]):
    print_section("13. AddTextToStore (OP 7) CONTEXT")

    for label, data in logs:
        events = data["events"]
        op7_contexts = Counter()
        last_inst = None
        for e in events:
            if e.kind == "instantiated":
                last_inst = e.data["name"]
            elif e.kind == "daemon_op" and e.data["op"] == 7:
                ctx = last_inst or "(before any instantiation)"
                op7_contexts[ctx] += 1

        total = sum(op7_contexts.values())
        if not total:
            continue
        print_subsection(f"{label}: {total:,} AddTextToStore ops")
        print(f"  Preceding instantiation context (top 15):")
        for ctx, count in op7_contexts.most_common(15):
            print(f"    {count:>5}  after '{ctx}'")


def report_eval_file_ordering(logs: list[tuple[str, dict]]):
    if len(logs) < 2:
        return

    print_section("14. EVALUATION FILE ORDERING")

    # Build unique-ordered file lists per log
    orders = {}
    for label, data in logs:
        order = []
        seen = set()
        for f in data["evaluating_files"]:
            if f not in seen:
                order.append(f)
                seen.add(f)
        orders[label] = order

    # Pairwise divergence
    labels = list(orders.keys())
    for i, lbl_a in enumerate(labels):
        for j, lbl_b in enumerate(labels):
            if j <= i:
                continue
            order_a = orders[lbl_a]
            order_b = orders[lbl_b]

            diverge_idx = None
            for k in range(min(len(order_a), len(order_b))):
                if order_a[k] != order_b[k]:
                    diverge_idx = k
                    break

            if diverge_idx is not None:
                print(f"\n  {lbl_a} vs {lbl_b}: file evaluation order"
                      f" diverges at index {diverge_idx}:")
                start = max(0, diverge_idx - 2)
                end = min(len(order_a), len(order_b), diverge_idx + 5)
                print(f"  {'Idx':>5}  {lbl_a:<45}  {lbl_b:<45}")
                for k in range(start, end):
                    fa = (short(order_a[k]) if k < len(order_a) else "\u2014")
                    fb = (short(order_b[k]) if k < len(order_b) else "\u2014")
                    marker = " **" if fa != fb else ""
                    print(f"  {k:>5}  {fa:<45}  {fb:<45}{marker}")
            else:
                n = min(len(order_a), len(order_b))
                print(f"\n  {lbl_a} vs {lbl_b}: file evaluation order is"
                      f" IDENTICAL (first {n} files)")

    # Per-pass file evaluation
    print_subsection("Files evaluated per pass")
    for label, data in logs:
        passes = data["passes"]
        if len(passes) <= 1:
            continue
        print(f"\n  {label}:")
        for p in passes:
            n = len(p["eval_files"])
            unique = len(set(p["eval_files"]))
            dups = n - unique
            print(f"    Pass {p['index']}: {n:>5} evaluations ({unique} unique"
                  + (f", {dups} re-evals)" if dups else ")"))


def report_event_timeline(logs: list[tuple[str, dict]]):
    print_section("15. EVENT TIMELINE")

    for label, data in logs:
        events = data["events"]
        passes = data["passes"]

        print_subsection(f"{label}: key events in chronological order")

        milestones = []
        for e in events:
            if e.kind == "root_value":
                milestones.append(
                    (e.lineno, "ROOT_VALUE",
                     "getting root value via rootLoader"))
            elif e.kind == "stat_hash_store":
                milestones.append(
                    (e.lineno, "STAT_HASH",
                     f"loading {e.data['count']} entries"))
            elif (e.kind == "copy_start"
                  and _nixpkgs_dir
                  and e.data["path"] == _nixpkgs_dir):
                milestones.append(
                    (e.lineno, "NIXPKGS_COPY",
                     "copying nixpkgs to store"))
            elif e.kind == "trace_hit":
                milestones.append(
                    (e.lineno, "TRACE_HIT",
                     f"verify hit for '{e.data['attr']}'"))
            elif e.kind == "output_json":
                milestones.append(
                    (e.lineno, "OUTPUT",
                     e.data["line"][:80]))
            elif e.kind == "stats_json":
                milestones.append(
                    (e.lineno, "STATS", "JSON stats block"))

        # First/last instantiation per pass
        for p in passes:
            if p["instantiated"]:
                first = p["instantiated"][0]
                last = p["instantiated"][-1]
                milestones.append(
                    (p["start_line"], f"PASS_{p['index']}_START",
                     f"first drv: {first[0]}"))
                pass_inst = [
                    e for e in events
                    if e.kind == "instantiated"
                    and p["start_line"] <= e.lineno <= p["end_line"]
                ]
                if pass_inst:
                    milestones.append(
                        (pass_inst[-1].lineno, f"PASS_{p['index']}_END",
                         f"last drv: {last[0]}"
                         f" ({fmt(len(p['instantiated']))} total)"))

        milestones.sort(key=lambda x: x[0])
        for ln, kind, desc in milestones:
            print(f"  {ln:>8,}  {kind:<20} {desc}")


# ── Main ─────────────────────────────────────────────────────────────────────

def parse_args():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "log_files", nargs="+", metavar="LOG",
        help="One or more Nix --debug log files to analyze",
    )
    parser.add_argument(
        "--nixpkgs", metavar="PATH", default=None,
        help="Path to nixpkgs directory (enables pass detection and path"
             " shortening)",
    )
    parser.add_argument(
        "--labels", nargs="+", metavar="LABEL", default=None,
        help="Labels for log files (default: file stems)",
    )
    return parser.parse_args()


def main():
    global _nixpkgs_dir, _nixpkgs_prefix

    args = parse_args()

    # Validate
    for path in args.log_files:
        if not Path(path).is_file():
            print(f"Error: log file not found: {path}", file=sys.stderr)
            sys.exit(1)

    # Labels
    if args.labels:
        if len(args.labels) != len(args.log_files):
            print(f"Error: {len(args.labels)} labels provided for"
                  f" {len(args.log_files)} log files", file=sys.stderr)
            sys.exit(1)
        labels = args.labels
    else:
        labels = [Path(p).stem for p in args.log_files]

    # Nixpkgs config
    if args.nixpkgs:
        _nixpkgs_dir = args.nixpkgs.rstrip("/")
        _nixpkgs_prefix = _nixpkgs_dir + "/"

    # Parse
    print("Parsing logs...")
    logs = []
    for path, label in zip(args.log_files, labels):
        events = parse_log_events(path)
        if events:
            print(f"  {label}: {len(events):,} events"
                  f" from {events[-1].lineno:,} lines")
        else:
            print(f"  {label}: (empty)")
        data = aggregate_events(events)
        logs.append((label, data))

    # Reports
    report_overview(logs)
    report_passes(logs)
    report_nixpkgs_copies(logs)
    report_store_copies(logs)
    report_uncacheable(logs)
    report_daemon_ops(logs)
    report_recording_deps(logs)
    report_file_eval(logs)
    report_drv_instantiation(logs)
    report_stats_json(logs)
    report_trace_hit_timing(logs)
    report_perpass_recording_uniqueness(logs)
    report_addtexttostore(logs)
    report_eval_file_ordering(logs)
    report_event_timeline(logs)


if __name__ == "__main__":
    main()
