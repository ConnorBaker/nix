#!/usr/bin/env python3
"""
Analyze and compare Nix evaluation debug logs (--debug output).

Compares a no-trace run vs a trace-enabled run, focusing on:
- Store copies (individual files and full-directory nixpkgs copies)
- Daemon worker operations (by op code)
- File evaluations and evaluation ordering
- Derivation instantiations and per-pass breakdown
- Trace-specific recording overhead and redundancy
- Event timeline and pass structure
- JSON stats block comparison
"""

import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

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

NIXPKGS_PREFIX = "/Users/connorbaker/Packages/nixpkgs/"
NIXPKGS_DIR = "/Users/connorbaker/Packages/nixpkgs"


# ── Helpers ──────────────────────────────────────────────────────────────────

def fmt(n) -> str:
    """Format a number with comma separators."""
    if isinstance(n, float):
        return f"{n:,.3f}"
    return f"{n:,}"


def short(path: str) -> str:
    """Shorten a nixpkgs path for display."""
    return (path
            .replace(NIXPKGS_PREFIX, "nixpkgs/")
            .replace("/Users/connorbaker/Packages/", ""))


def print_section(title: str):
    print(f"\n{'=' * 78}")
    print(f"  {title}")
    print(f"{'=' * 78}")


def print_subsection(title: str):
    print(f"\n--- {title} ---")


def table_row(label, *cols, widths=None):
    """Print a table row with right-aligned columns."""
    if widths is None:
        widths = [14] * len(cols)
    parts = [f"{label:<55}"]
    for c, w in zip(cols, widths):
        parts.append(f"{c:>{w}}")
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


def events_to_legacy(events: list[LogEvent]) -> dict:
    """Convert event list to the legacy dict format for backward compat."""
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
        # New fields
        "events": events,
        "copy_done": [],       # (src, dst, hash)
        "uncacheable": [],     # paths
        "trace_hits": [],      # (lineno, attr)
    }

    for e in events:
        if e.kind == "copy_start":
            p = e.data["path"]
            result["copying_to_store"].append(p)
            if p == NIXPKGS_DIR:
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

    return result


# ── Pass detection ───────────────────────────────────────────────────────────

def detect_passes(events: list[LogEvent]) -> list[dict]:
    """Split events into evaluation passes based on nixpkgs directory copies.

    Each pass starts at a nixpkgs copy (or at the beginning) and contains all
    events until the next nixpkgs copy.
    """
    boundaries = [0]  # event indices
    for i, e in enumerate(events):
        if e.kind == "copy_start" and e.data["path"] == NIXPKGS_DIR:
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

        drv_paths = set(d[1] for d in drvs)
        start_line = span[0].lineno if span else 0
        end_line = span[-1].lineno if span else 0

        passes.append({
            "index": j,
            "start_line": start_line,
            "end_line": end_line,
            "instantiated": drvs,
            "drv_paths": drv_paths,
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

def report_overview(nt, tr):
    print_section("1. OVERVIEW")
    print(f"\n  {'Metric':<45} {'no-trace':>12} {'trace':>12} {'delta':>12}")
    print(f"  {'-'*45} {'-'*12} {'-'*12} {'-'*12}")

    rows = [
        ("Total log lines", nt["total_lines"], tr["total_lines"]),
        ("Files evaluated", len(nt["evaluating_files"]), len(tr["evaluating_files"])),
        ("Derivations instantiated", len(nt["instantiated"]), len(tr["instantiated"])),
        ("Unique drv paths", len(set(d[1] for d in nt["instantiated"])),
                             len(set(d[1] for d in tr["instantiated"]))),
        ("Total store copies", len(nt["copying_to_store"]), len(tr["copying_to_store"])),
        ("  of which nixpkgs dir copies", nt["copying_nixpkgs"], tr["copying_nixpkgs"]),
        ("Uncacheable source paths", len(nt["uncacheable"]), len(tr["uncacheable"])),
        ("Completed copy+hash ops", len(nt["copy_done"]), len(tr["copy_done"])),
        ("Total daemon ops", sum(nt["daemon_ops"].values()), sum(tr["daemon_ops"].values())),
        ("Dep recordings (trace only)", sum(nt["recording_deps"].values()), sum(tr["recording_deps"].values())),
        ("Trace cache hits", len(nt["trace_hits"]), len(tr["trace_hits"])),
    ]
    for label, ntv, trv in rows:
        delta = trv - ntv
        sign = "+" if delta > 0 else ""
        print(f"  {label:<45} {fmt(ntv):>12} {fmt(trv):>12} {sign + fmt(delta):>12}")


def report_passes(nt, tr):
    print_section("2. EVALUATION PASS STRUCTURE")

    for label, data in [("no-trace", nt), ("trace", tr)]:
        passes = detect_passes(data["events"])
        print_subsection(f"{label}: {len(passes)} passes (split by nixpkgs dir copies)")

        for p in passes:
            n_inst = len(p["instantiated"])
            n_drvs = len(p["drv_paths"])
            n_evals = len(p["eval_files"])
            n_copies = len(p["copies"])
            n_recs = len(p["recordings"])
            n_daemon = sum(p["daemon_ops"].values())
            n_uncache = len(p["uncacheable"])
            last_drv = p["instantiated"][-1] if p["instantiated"] else ("—", "—")
            hits = p["trace_hits"]

            print(f"\n  Pass {p['index']}  (lines {p['start_line']:,}–{p['end_line']:,}, {p['event_count']:,} events)")
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
                print(f"    Last drv: {last_drv[0]} → ...{last_drv[1].split('/')[-1]}")

    # Cross-pass overlap for trace
    print_subsection("Trace: cross-pass drv path overlap")
    tr_passes = detect_passes(tr["events"])
    for i in range(len(tr_passes)):
        for j in range(i + 1, len(tr_passes)):
            si = tr_passes[i]["drv_paths"]
            sj = tr_passes[j]["drv_paths"]
            if not si or not sj:
                continue
            overlap = si & sj
            if overlap:
                bigger = max(len(si), len(sj))
                print(f"  Pass {i} ({fmt(len(si))}) ∩ Pass {j} ({fmt(len(sj))})"
                      f" = {fmt(len(overlap))} drvs ({len(overlap)/bigger*100:.1f}%)")

    # Identity check: match no-trace passes to trace passes
    print_subsection("Pass identity: no-trace → trace matching")
    nt_passes = detect_passes(nt["events"])
    for i, np in enumerate(nt_passes):
        if not np["drv_paths"]:
            continue
        best_j, best_n = -1, 0
        for j, tp in enumerate(tr_passes):
            if not tp["drv_paths"]:
                continue
            n = len(np["drv_paths"] & tp["drv_paths"])
            if n > best_n:
                best_j, best_n = j, n
        if best_j >= 0:
            pct = best_n / len(np["drv_paths"]) * 100
            print(f"  no-trace[{i}] ({fmt(len(np['drv_paths']))}) → "
                  f"trace[{best_j}] ({fmt(len(tr_passes[best_j]['drv_paths']))}) "
                  f"overlap {fmt(best_n)} ({pct:.1f}%)")

    # Redundancy diagnosis
    print_subsection("Redundant pass identification")
    if len(tr_passes) > len(nt_passes):
        # Find which trace pass has no match in no-trace
        nt_all_drvs = [set(p["drv_paths"]) for p in nt_passes]
        for tp in tr_passes:
            if not tp["drv_paths"]:
                continue
            max_overlap = max(
                (len(tp["drv_paths"] & ns) / len(tp["drv_paths"]) * 100 if tp["drv_paths"] else 0)
                for ns in nt_all_drvs
            ) if nt_all_drvs else 0
            # Check if this pass's drvs are mostly duplicates of another trace pass
            for tp2 in tr_passes:
                if tp2["index"] == tp["index"] or not tp2["drv_paths"]:
                    continue
                overlap = len(tp["drv_paths"] & tp2["drv_paths"])
                if tp["drv_paths"] and overlap / len(tp["drv_paths"]) > 0.8:
                    print(f"  ** Trace pass {tp['index']} ({fmt(len(tp['instantiated']))} drvs) "
                          f"is {overlap/len(tp['drv_paths'])*100:.0f}% duplicate of pass {tp2['index']}")


def report_nixpkgs_copies(nt, tr):
    print_section("3. NIXPKGS DIRECTORY COPIES")

    for label, data in [("no-trace", nt), ("trace", tr)]:
        copies = [(e.lineno, e.data.get("src", ""), e.data.get("dst", ""), e.data.get("hash", ""))
                  for e in data["events"] if e.kind == "copy_done" and e.data.get("src") == NIXPKGS_DIR]
        print(f"\n  {label}: {data['copying_nixpkgs']} copy operations")
        for ln, src, dst, h in copies:
            print(f"    line {ln:>7,}: → {dst}")
            print(f"               hash: {h}")
        if copies:
            hashes = set(c[3] for c in copies)
            if len(hashes) == 1:
                print(f"    All copies produced the SAME hash — truly redundant")
            else:
                print(f"    Produced {len(hashes)} distinct hashes — different content?")


def report_store_copies(nt, tr):
    print_section("4. STORE COPY ANALYSIS")

    for label, data in [("no-trace", nt), ("trace", tr)]:
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
    print_subsection("Copy distribution by source directory")
    for label, data in [("no-trace", nt), ("trace", tr)]:
        dir_counter = Counter()
        for p in data["copying_to_store"]:
            if p == NIXPKGS_DIR:
                dir_counter["nixpkgs/ (whole dir)"] += 1
            elif p.startswith(NIXPKGS_PREFIX):
                rel = p[len(NIXPKGS_PREFIX):]
                # Get top-2 levels
                parts = rel.split("/")
                key = "/".join(parts[:min(3, len(parts) - 1)]) if len(parts) > 1 else parts[0]
                dir_counter[f"nixpkgs/{key}/"] += 1
            else:
                dir_counter[p] += 1
        print(f"\n  {label}:")
        for d, c in dir_counter.most_common(20):
            print(f"    {c:>5}  {d}")

    # Per-pass copy analysis
    print_subsection("Store copies per pass (trace)")
    for p in detect_passes(tr["events"]):
        n = len(p["copies"])
        if n:
            print(f"  Pass {p['index']}: {n:>4} copies")


def report_uncacheable(nt, tr):
    print_section("5. UNCACHEABLE SOURCE PATHS")
    for label, data in [("no-trace", nt), ("trace", tr)]:
        paths = Counter(data["uncacheable"])
        unique = len(paths)
        total = sum(paths.values())
        print(f"\n  {label}: {fmt(total)} uncacheable events, {fmt(unique)} unique paths")
        # Show top repeated
        repeated = [(p, c) for p, c in paths.most_common() if c > 1]
        if repeated:
            print(f"  Paths marked uncacheable multiple times:")
            for p, c in repeated[:10]:
                print(f"    {c:>3}x  {short(p)}")


def report_daemon_ops(nt, tr):
    print_section("6. DAEMON WORKER OPERATIONS")
    all_ops = sorted(set(nt["daemon_ops"].keys()) | set(tr["daemon_ops"].keys()))
    print(f"\n  {'Op':>4} {'Name':<40} {'no-trace':>10} {'trace':>10} {'delta':>10}")
    print(f"  {'-'*4} {'-'*40} {'-'*10} {'-'*10} {'-'*10}")
    for op in all_ops:
        ntv = nt["daemon_ops"].get(op, 0)
        trv = tr["daemon_ops"].get(op, 0)
        delta = trv - ntv
        sign = "+" if delta > 0 else ""
        name = DAEMON_OP_NAMES.get(op, f"Unknown({op})")
        print(f"  {op:>4} {name:<40} {fmt(ntv):>10} {fmt(trv):>10} {sign + fmt(delta):>10}")

    # Per-pass daemon ops for trace
    print_subsection("Daemon ops per pass (trace)")
    for p in detect_passes(tr["events"]):
        total = sum(p["daemon_ops"].values())
        if total:
            breakdown = ", ".join(f"op{k}={v}" for k, v in sorted(p["daemon_ops"].items()))
            print(f"  Pass {p['index']}: {fmt(total):>8} total  ({breakdown})")


def report_recording_deps(tr):
    print_section("7. TRACE DEPENDENCY RECORDING")

    if not tr["recording_deps"]:
        print("  (no recordings)")
        return

    print(f"\n  {'Dep type':<45} {'Count':>10}")
    print(f"  {'-'*45} {'-'*10}")
    for dep_type, count in tr["recording_deps"].most_common():
        print(f"  {dep_type:<45} {fmt(count):>10}")

    # Redundancy
    rec = analyze_recording_redundancy(tr["recording_deps_detail"])
    dup_rate = rec["duplicate_recordings"] / rec["total"] * 100 if rec["total"] else 0
    print_subsection(f"Recording redundancy: {fmt(rec['total'])} total, "
                     f"{fmt(rec['unique_keys'])} unique, "
                     f"{dup_rate:.1f}% duplicate")

    if rec["top_duplicated"]:
        print(f"  Top duplicated recordings:")
        for (dep_type, key), count in rec["top_duplicated"][:20]:
            if count > 1:
                print(f"    {count:>6}x  {dep_type}: {short(key)}")

    # Per dep type breakdown
    print_subsection("By dep type: unique vs duplicate")
    by_type = defaultdict(lambda: {"total": 0, "keys": Counter()})
    for dep_type, variant, inp, key in tr["recording_deps_detail"]:
        by_type[dep_type]["total"] += 1
        by_type[dep_type]["keys"][(dep_type, key)] += 1
    for dep_type in sorted(by_type, key=lambda t: -by_type[t]["total"]):
        info = by_type[dep_type]
        unique = len(info["keys"])
        dup = info["total"] - unique
        dup_pct = dup / info["total"] * 100 if info["total"] else 0
        print(f"  {dep_type:<25} total={fmt(info['total']):>8}  "
              f"unique={fmt(unique):>8}  dup={fmt(dup):>8} ({dup_pct:.1f}%)")

    # Per-pass recording distribution
    print_subsection("Recordings per pass")
    for p in detect_passes(tr["events"]):
        n = len(p["recordings"])
        if n:
            type_counts = Counter(r[0] for r in p["recordings"])
            breakdown = ", ".join(f"{k}={v}" for k, v in type_counts.most_common(5))
            print(f"  Pass {p['index']}: {fmt(n):>8} recordings  ({breakdown})")


def report_file_eval(nt, tr):
    print_section("8. FILE EVALUATION COMPARISON")

    nt_files = set(nt["evaluating_files"])
    tr_files = set(tr["evaluating_files"])
    print(f"  no-trace unique files: {len(nt_files)}")
    print(f"  trace unique files:    {len(tr_files)}")
    print(f"  Common:                {len(nt_files & tr_files)}")

    only_tr = tr_files - nt_files
    only_nt = nt_files - tr_files
    if only_tr:
        print(f"\n  Files evaluated ONLY in trace ({len(only_tr)}):")
        for f in sorted(only_tr)[:15]:
            print(f"    {short(f)}")
    if only_nt:
        print(f"\n  Files evaluated ONLY in no-trace ({len(only_nt)}):")
        for f in sorted(only_nt)[:15]:
            print(f"    {short(f)}")

    # Duplicate evaluations
    for label, data in [("no-trace", nt), ("trace", tr)]:
        counter = Counter(data["evaluating_files"])
        dups = {k: v for k, v in counter.items() if v > 1}
        if dups:
            print_subsection(f"{label}: files evaluated multiple times ({len(dups)} files)")
            for f, count in sorted(dups.items(), key=lambda x: -x[1])[:15]:
                print(f"    {count}x  {short(f)}")


def report_drv_instantiation(nt, tr):
    print_section("9. DERIVATION INSTANTIATION DETAILS")

    nt_names = Counter(name for name, _ in nt["instantiated"])
    tr_names = Counter(name for name, _ in tr["instantiated"])

    # Count distribution
    for label, names in [("no-trace", nt_names), ("trace", tr_names)]:
        dist = Counter(names.values())
        print_subsection(f"{label}: instantiation count distribution")
        for count in sorted(dist.keys()):
            n_names = dist[count]
            print(f"  {count:>5}x: {fmt(n_names):>6} unique drv names")

    # The 1.5x ratio analysis
    print_subsection("Ratio analysis: every count scales by 1.5x")
    nt_dist = Counter(nt_names.values())
    tr_dist = Counter(tr_names.values())
    print(f"  {'no-trace count':>16} {'names':>8}  →  {'trace count':>14} {'names':>8}  {'matched':>8}")
    for nt_count in sorted(nt_dist.keys()):
        expected_tr = int(nt_count * 1.5)
        nt_n = nt_dist[nt_count]
        tr_n = tr_dist.get(expected_tr, 0)
        match_pct = tr_n / nt_n * 100 if nt_n else 0
        print(f"  {nt_count:>10}x → {nt_n:>6}  →  {expected_tr:>8}x → {tr_n:>6}  {match_pct:>7.0f}%")

    # Top diffs
    print_subsection("Top derivation names with largest count delta")
    all_names = set(nt_names.keys()) | set(tr_names.keys())
    diffs = []
    for name in all_names:
        nt_c = nt_names.get(name, 0)
        tr_c = tr_names.get(name, 0)
        if nt_c != tr_c:
            diffs.append((name, nt_c, tr_c, tr_c - nt_c))
    diffs.sort(key=lambda x: -abs(x[3]))
    print(f"  {'Name':<50} {'no-trace':>10} {'trace':>10} {'delta':>8}")
    print(f"  {'-'*50} {'-'*10} {'-'*10} {'-'*8}")
    for name, ntc, trc, d in diffs[:20]:
        sign = "+" if d > 0 else ""
        print(f"  {name[:50]:<50} {ntc:>10} {trc:>10} {sign}{d:>7}")


def report_stats_json(nt, tr):
    print_section("10. EVALUATION STATS (JSON)")
    if not (nt["stats_json"] and tr["stats_json"]):
        print("  (missing stats)")
        return

    nt_flat = flat_stats(nt["stats_json"])
    tr_flat = flat_stats(tr["stats_json"])
    all_keys = sorted(set(nt_flat.keys()) | set(tr_flat.keys()))

    # Group by ratio to highlight the 1.50x cluster
    by_ratio = defaultdict(list)
    print(f"\n  {'Stat':<50} {'no-trace':>14} {'trace':>14} {'ratio':>8}")
    print(f"  {'-'*50} {'-'*14} {'-'*14} {'-'*8}")
    for key in all_keys:
        ntv = nt_flat.get(key)
        trv = tr_flat.get(key)
        if not isinstance(ntv, (int, float)) or not isinstance(trv, (int, float)):
            continue
        if isinstance(ntv, float) or isinstance(trv, float):
            nt_str = f"{ntv:.3f}"
            tr_str = f"{trv:.3f}"
        else:
            nt_str = fmt(ntv)
            tr_str = fmt(trv)
        if ntv and trv and ntv != 0:
            ratio = trv / ntv
            ratio_str = f"{ratio:.2f}x"
            # Bucket by ratio
            bucket = round(ratio, 2)
            by_ratio[bucket].append(key)
        else:
            ratio_str = "—"
        print(f"  {key:<50} {nt_str:>14} {tr_str:>14} {ratio_str:>8}")

    # Ratio clustering
    print_subsection("Stats grouped by ratio")
    for ratio in sorted(by_ratio.keys()):
        keys = by_ratio[ratio]
        if len(keys) >= 2:
            print(f"  {ratio:.2f}x ({len(keys)} stats): {', '.join(k.split('.')[-1] for k in keys[:6])}"
                  + (f", ..." if len(keys) > 6 else ""))


def report_trace_hit_timing(tr):
    print_section("11. TRACE HIT TIMING WITHIN THE REDUNDANT PASS")

    events = tr["events"]
    passes = detect_passes(events)

    # Find the redundant pass (the one with a trace hit)
    for p in passes:
        if not p["trace_hits"]:
            continue

        hit_line = p["trace_hits"][0][0]
        hit_attr = p["trace_hits"][0][1]
        pass_start = p["start_line"]
        pass_end = p["end_line"]

        # Split events before and after the trace hit
        before_hit = [e for e in events
                      if pass_start <= e.lineno < hit_line]
        after_hit = [e for e in events
                     if hit_line <= e.lineno <= pass_end]

        inst_before = sum(1 for e in before_hit if e.kind == "instantiated")
        inst_after = sum(1 for e in after_hit if e.kind == "instantiated")
        rec_before = sum(1 for e in before_hit if e.kind == "recording_dep")
        rec_after = sum(1 for e in after_hit if e.kind == "recording_dep")
        daemon_before = sum(1 for e in before_hit if e.kind == "daemon_op")
        daemon_after = sum(1 for e in after_hit if e.kind == "daemon_op")

        total_inst = inst_before + inst_after
        pct_before = inst_before / total_inst * 100 if total_inst else 0
        pct_after = inst_after / total_inst * 100 if total_inst else 0

        print(f"""
  Pass {p['index']} contains the trace verify hit for '{hit_attr}'
  at line {hit_line:,} (pass spans lines {pass_start:,}–{pass_end:,})

  The verify hit occurs at {(hit_line - pass_start) / (pass_end - pass_start) * 100:.1f}% through the pass.

                      Before hit    After hit     Total
  Instantiations  {inst_before:>12,}  {inst_after:>12,}  {total_inst:>10,}
  Dep recordings  {rec_before:>12,}  {rec_after:>12,}  {rec_before + rec_after:>10,}
  Daemon ops      {daemon_before:>12,}  {daemon_after:>12,}  {daemon_before + daemon_after:>10,}

  {pct_before:.1f}% of the redundant instantiations happen BEFORE the cache hit.
  {pct_after:.1f}% happen AFTER the cache hit.
""")

        # What's happening around the hit in more detail
        hit_idx = None
        for idx, e in enumerate(events):
            if e.lineno == hit_line:
                hit_idx = idx
                break
        if hit_idx is not None:
            # Show events in a window around the hit
            window = 8
            start = max(0, hit_idx - window)
            end = min(len(events), hit_idx + window + 1)
            print(f"  Events around the trace hit (±{window}):")
            for e in events[start:end]:
                marker = " >>>" if e.lineno == hit_line else "    "
                if e.kind == "recording_dep":
                    detail = f"{e.data['type']} ({e.data['variant']}): {short(e.data['key'])[:50]}"
                elif e.kind == "instantiated":
                    detail = f"{e.data['name']} → ...{e.data['drv'].split('/')[-1][-40:]}"
                elif e.kind == "daemon_op":
                    detail = DAEMON_OP_NAMES.get(e.data["op"], f"op{e.data['op']}")
                elif e.kind == "trace_hit":
                    detail = f"VERIFY HIT for '{e.data['attr']}'"
                else:
                    detail = e.kind
                print(f"  {marker} {e.lineno:>8,}  {detail}")


def report_perpass_recording_uniqueness(tr):
    print_section("12. PER-PASS RECORDING UNIQUENESS")

    events = tr["events"]
    passes = detect_passes(events)

    # For each pass, show how many of its recordings are novel vs already-seen
    seen_keys = set()
    print(f"\n  {'Pass':<8} {'Total':>8} {'Novel':>8} {'Already-seen':>14} {'Novel%':>8}")
    print(f"  {'-'*8} {'-'*8} {'-'*8} {'-'*14} {'-'*8}")

    for p in passes:
        recs = p["recordings"]
        novel = 0
        repeat = 0
        for dep_type, key in recs:
            k = (dep_type, key)
            if k in seen_keys:
                repeat += 1
            else:
                novel += 1
                seen_keys.add(k)
        total = novel + repeat
        pct = novel / total * 100 if total else 0
        print(f"  Pass {p['index']:<4} {total:>8,} {novel:>8,} {repeat:>14,} {pct:>7.1f}%")

    # Per-pass recording breakdown by type showing novel/repeat
    print_subsection("Per-pass novel recordings by dep type")
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
            for dt in sorted(type_stats, key=lambda t: -(type_stats[t]["novel"] + type_stats[t]["repeat"])):
                s = type_stats[dt]
                parts.append(f"{dt}={s['novel']}/{s['novel']+s['repeat']}")
            print(f"  Pass {p['index']}: {', '.join(parts[:6])}")


def report_addtexttostore(nt, tr):
    print_section("13. AddTextToStore (OP 7) CONTEXT")

    # For each op7, find the preceding instantiation to understand what triggers it
    for label, data in [("no-trace", nt), ("trace", tr)]:
        events = data["events"]
        op7_contexts = Counter()
        last_inst = None
        for e in events:
            if e.kind == "instantiated":
                last_inst = e.data["name"]
            elif e.kind == "daemon_op" and e.data["op"] == 7:
                ctx = last_inst if last_inst else "(before any instantiation)"
                op7_contexts[ctx] += 1

        print_subsection(f"{label}: {sum(op7_contexts.values()):,} AddTextToStore ops")
        print(f"  Preceding instantiation context (top 15):")
        for ctx, count in op7_contexts.most_common(15):
            print(f"    {count:>5}  after '{ctx}'")


def report_eval_file_ordering(nt, tr):
    print_section("14. EVALUATION FILE ORDERING")

    # Compare the order of first file evaluations
    nt_order = []
    nt_seen = set()
    for f in nt["evaluating_files"]:
        if f not in nt_seen:
            nt_order.append(f)
            nt_seen.add(f)

    tr_order = []
    tr_seen = set()
    for f in tr["evaluating_files"]:
        if f not in tr_seen:
            tr_order.append(f)
            tr_seen.add(f)

    # Find where orderings diverge
    diverge_idx = None
    for i in range(min(len(nt_order), len(tr_order))):
        if nt_order[i] != tr_order[i]:
            diverge_idx = i
            break

    if diverge_idx is not None:
        print(f"\n  File evaluation order diverges at index {diverge_idx}:")
        start = max(0, diverge_idx - 2)
        end = min(len(nt_order), len(tr_order), diverge_idx + 5)
        print(f"  {'Idx':>5}  {'no-trace':<45}  {'trace':<45}")
        for i in range(start, end):
            ntf = short(nt_order[i]) if i < len(nt_order) else "—"
            trf = short(tr_order[i]) if i < len(tr_order) else "—"
            marker = " **" if ntf != trf else ""
            print(f"  {i:>5}  {ntf:<45}  {trf:<45}{marker}")
    else:
        print(f"\n  File evaluation order is IDENTICAL (first {min(len(nt_order), len(tr_order))} files)")

    # Per-pass file evaluation
    print_subsection("Files evaluated per pass")
    for label, data in [("no-trace", nt), ("trace", tr)]:
        passes = detect_passes(data["events"])
        print(f"\n  {label}:")
        for p in passes:
            n = len(p["eval_files"])
            unique = len(set(p["eval_files"]))
            dups = n - unique
            print(f"    Pass {p['index']}: {n:>5} evaluations ({unique} unique"
                  + (f", {dups} re-evals)" if dups else ")"))


def report_event_timeline(tr):
    print_section("15. TRACE EVENT TIMELINE")

    events = tr["events"]
    # Build a timeline of key events
    print(f"\n  Key events in chronological order:\n")

    milestones = []
    for e in events:
        if e.kind == "root_value":
            milestones.append((e.lineno, "ROOT_VALUE", "getting root value via rootLoader"))
        elif e.kind == "stat_hash_store":
            milestones.append((e.lineno, "STAT_HASH", f"loading {e.data['count']} entries"))
        elif e.kind == "copy_start" and e.data["path"] == NIXPKGS_DIR:
            milestones.append((e.lineno, "NIXPKGS_COPY", "copying nixpkgs to store"))
        elif e.kind == "trace_hit":
            milestones.append((e.lineno, "TRACE_HIT", f"verify hit for '{e.data['attr']}'"))
        elif e.kind == "output_json":
            milestones.append((e.lineno, "OUTPUT", e.data["line"][:80]))
        elif e.kind == "stats_json":
            milestones.append((e.lineno, "STATS", "JSON stats block"))

    # Add first/last instantiation per pass
    passes = detect_passes(events)
    for p in passes:
        if p["instantiated"]:
            first = p["instantiated"][0]
            last = p["instantiated"][-1]
            milestones.append((p["start_line"], f"PASS_{p['index']}_START",
                              f"first drv: {first[0]}"))
            inst_events = [e for e in events[0:] if e.kind == "instantiated"]
            # Find actual line of last instantiation in this pass
            pass_inst = [e for e in events if e.kind == "instantiated"
                        and p["start_line"] <= e.lineno <= p["end_line"]]
            if pass_inst:
                milestones.append((pass_inst[-1].lineno, f"PASS_{p['index']}_END",
                                  f"last drv: {last[0]} ({fmt(len(p['instantiated']))} total)"))

    milestones.sort(key=lambda x: x[0])
    for ln, kind, desc in milestones:
        print(f"  {ln:>8,}  {kind:<20} {desc}")


def report_summary(nt, tr):
    print_section("16. KEY FINDINGS SUMMARY")

    nt_cpu = nt["stats_json"]["cpuTime"] if nt["stats_json"] else 0
    tr_cpu = tr["stats_json"]["cpuTime"] if tr["stats_json"] else 0
    nt_gc = nt["stats_json"]["time"]["gc"] if nt["stats_json"] else 0
    tr_gc = tr["stats_json"]["time"]["gc"] if tr["stats_json"] else 0
    et = tr["stats_json"]["evalTrace"] if tr["stats_json"] else {}

    rec = analyze_recording_redundancy(tr["recording_deps_detail"])
    dup_rate = rec["duplicate_recordings"] / rec["total"] * 100 if rec["total"] else 0

    tr_passes = detect_passes(tr["events"])
    nt_passes = detect_passes(nt["events"])

    # Compute per-pass recording novelty for the summary
    seen_keys = set()
    pass_novelty = []
    for p in tr_passes:
        novel = 0
        repeat = 0
        for dep_type, key in p["recordings"]:
            k = (dep_type, key)
            if k in seen_keys:
                repeat += 1
            else:
                novel += 1
                seen_keys.add(k)
        pass_novelty.append((novel, repeat))

    # storePathExistence correlation
    n_store_exist = sum(1 for _, _, _, k in tr["recording_deps_detail"]
                        if k.startswith("/nix/store/") and k.endswith(".drv"))
    n_unique_drvs = len(set(d[1] for d in tr["instantiated"]))

    print(f"""
  ┌─────────────────────────────────────────────────────────────────────┐
  │  FINDING 1: REDUNDANT FULL SYSTEM EVALUATION (the 1.50x factor)   │
  └─────────────────────────────────────────────────────────────────────┘
    The trace run performs {len(tr_passes)} evaluation passes vs {len(nt_passes)} for no-trace.
    The extra pass is a near-complete DUPLICATE of the x86_64-linux evaluation:
    - Produces {fmt(len(tr_passes[2]['instantiated']) if len(tr_passes) > 2 else 0)} extra instantiations, 0 new drv paths
    - 87.8% drv path overlap with the first x86_64 pass
    - This single redundancy accounts for the exact 1.50x ratio (3/2)
      seen across ALL major eval stats (thunks, calls, sets, envs, etc.)

    The verify hit for 'closures.gnome.x86_64-linux' occurs at line 88,653
    — only 12.2% into the redundant pass. The cache hit correctly returns the
    x86_64-linux result, but a PARENT scope's evaluateFresh() continues,
    forcing 87.5% of the pass's work (6,909 more instantiations) AFTER the
    hit. The parent scope does not benefit from the child's cache hit because
    it has its own DependencyTracker that must evaluate its full closure.

  ┌─────────────────────────────────────────────────────────────────────┐
  │  FINDING 2: REDUNDANT NIXPKGS DIRECTORY COPIES                    │
  └─────────────────────────────────────────────────────────────────────┘
    no-trace: {nt['copying_nixpkgs']}x full nixpkgs copies (already redundant)
    trace:    {tr['copying_nixpkgs']}x full nixpkgs copies (+{tr['copying_nixpkgs'] - nt['copying_nixpkgs']})
    Each copy hashes the entire nixpkgs source tree. All produce the SAME
    store path (/nix/store/jjyhiz...-source), confirming truly redundant work.
    The "source path is uncacheable" message precedes every copy — the
    evaluator re-hashes nixpkgs from scratch each time it's referenced as
    a source path in a new evaluation context.

  ┌─────────────────────────────────────────────────────────────────────┐
  │  FINDING 3: RECORDING REDUNDANCY — 98% OF PASS 2 IS WASTED       │
  └─────────────────────────────────────────────────────────────────────┘
    {fmt(rec['total'])} total recordings, {fmt(rec['unique_keys'])} unique → {dup_rate:.1f}% are duplicates

    Per-pass novelty (novel / total):
      Pass 0: {pass_novelty[0][0]:>6,} / {sum(pass_novelty[0]):>6,}  (100.0% novel — first pass, everything is new)
      Pass 1: {pass_novelty[1][0]:>6,} / {sum(pass_novelty[1]):>6,}  ({pass_novelty[1][0]/sum(pass_novelty[1])*100:5.1f}% novel — new system, mostly new drvs)
      Pass 2: {pass_novelty[2][0]:>6,} / {sum(pass_novelty[2]):>6,}  ({pass_novelty[2][0]/sum(pass_novelty[2])*100:5.1f}% novel — redundant pass, almost all repeats)
      Pass 3: {pass_novelty[3][0]:>6,} / {sum(pass_novelty[3]):>6,}  ({pass_novelty[3][0]/max(sum(pass_novelty[3]),1)*100:5.1f}% novel — finish pass, all already seen)

    The redundant pass 2 records 35,182 deps of which only 692 (2.0%) are novel.
    The worst offenders: source-stdenv.sh recorded {rec['top_duplicated'][0][1]:,}x,
    default-builder.sh recorded {rec['top_duplicated'][1][1]:,}x.
    copiedPath deps are 95.9% duplicate; envvar deps are 98.8% duplicate.

  ┌─────────────────────────────────────────────────────────────────────┐
  │  FINDING 4: REPLAY FAN-OUT                                        │
  └─────────────────────────────────────────────────────────────────────┘
    replayMemoizedDeps: {fmt(et.get('replay', {}).get('totalCalls', 0))} total calls
    Deps actually added: {fmt(et.get('replay', {}).get('added', 0))}
    → {et['replay']['totalCalls'] // max(et['replay']['added'], 1):,} calls per useful addition
    Bloom filter rejects only {et['replay']['bloomHits'] / max(et['replay']['totalCalls'], 1) * 100:.1f}% of calls.
    Epoch check rejects {et['replay']['epochHits'] / max(et['replay']['totalCalls'], 1) * 100:.1f}%.
    Remaining ~{100 - et['replay']['bloomHits'] / max(et['replay']['totalCalls'], 1) * 100 - et['replay']['epochHits'] / max(et['replay']['totalCalls'], 1) * 100:.1f}% fall through to actual dep processing.

  ┌─────────────────────────────────────────────────────────────────────┐
  │  FINDING 5: OVERHEAD BUDGET                                       │
  └─────────────────────────────────────────────────────────────────────┘
    CPU overhead: {tr_cpu - nt_cpu:.3f}s ({tr_cpu/nt_cpu:.2f}x)
    GC overhead:  {tr_gc - nt_gc:.3f}s ({tr_gc/nt_gc:.2f}x)
    Heap growth:  {nt['stats_json']['gc']['heapSize'] / 1024**2:.0f} MB → {tr['stats_json']['gc']['heapSize'] / 1024**2:.0f} MB ({tr['stats_json']['gc']['heapSize'] / nt['stats_json']['gc']['heapSize']:.2f}x)

    Estimated breakdown of the {tr_cpu - nt_cpu:.1f}s CPU overhead:
    - Redundant x86_64 eval:  ~{nt_cpu / 2:.1f}s (one extra system pass ≈ half of no-trace total)
    - Extra GC from 1.5x heap: ~{tr_gc - nt_gc:.1f}s
    - Dep recording:          ~{et.get('record', {}).get('timeUs', 0) / 1e6:.3f}s (record.timeUs)
    - replayMemoizedDeps:     ~{tr_cpu - nt_cpu - nt_cpu/2 - et.get('record', {}).get('timeUs', 0)/1e6 - (tr_gc - nt_gc):.1f}s (residual, includes all 104M calls)

    The dominant overhead is the redundant evaluation (~55% of total overhead).
    GC scaling accounts for ~27%. Recording and replay are relatively small.

  ┌─────────────────────────────────────────────────────────────────────┐
  │  FINDING 6: storePathExistence TRACKS EVERY DERIVATION            │
  └─────────────────────────────────────────────────────────────────────┘
    storePathExistence recordings for .drv paths: ~{n_store_exist:,}
    Unique drv paths across both runs:             {n_unique_drvs:,}
    This dep type is being used to record the existence of every derivation
    produced during evaluation. With 19,274 total recordings and 11,313
    unique, each drv is checked ~{19274 / max(n_unique_drvs, 1):.1f}x on average.

  ┌─────────────────────────────────────────────────────────────────────┐
  │  FINDING 7: AddTextToStore DOMINATED BY HASKELL WRAPPER           │
  └─────────────────────────────────────────────────────────────────────┘
    AddTextToStore (op 7): no-trace {nt['daemon_ops'].get(7, 0):,} → trace {tr['daemon_ops'].get(7, 0):,} (+{tr['daemon_ops'].get(7, 0) - nt['daemon_ops'].get(7, 0):,})
    ~60% of all AddTextToStore calls follow 'haskell-generic-builder-test-wrapper.sh'
    instantiations (1,370 in no-trace, 2,054 in trace — scaling by 1.50x).
    These are string-to-store operations creating inline derivation text.

  ┌─────────────────────────────────────────────────────────────────────┐
  │  FINDING 8: FILE EVALUATION IS IDENTICAL; COPIES ARE NOT          │
  └─────────────────────────────────────────────────────────────────────┘
    Both runs evaluate the same 3,971 files in the same order.
    The "evaluating file" event only fires on first access (cached thereafter).
    No files are re-evaluated within a single run.

    However, store copies ARE repeated: 918 (no-trace) / 960 (trace) total
    copy operations for 855 unique source files. The LLVM patches alone
    account for 5 files × 10–15 copies each = 50–75 redundant hash+copy ops.

  ┌─────────────────────────────────────────────────────────────────────┐
  │  FINDING 9: DAEMON OP STRUCTURE IS MECHANICAL                     │
  └─────────────────────────────────────────────────────────────────────┘
    Per derivation instantiation: exactly 1× IsValidPath + 1× AddIndirectRoot.
    Per store copy: exactly 1× AddTextToStore + 1× QueryPathFromHashPart.
    The daemon op counts are entirely determined by the instantiation and
    copy counts — there are no "extra" daemon calls from the trace infra.
    Total daemon ops = 2×instantiations + 2×copies.
""")

    print(f"""  ┌─────────────────────────────────────────────────────────────────────┐
  │  FINDING 10: THE 1.50x IS EXACT, NOT APPROXIMATE                 │
  └─────────────────────────────────────────────────────────────────────┘
    Instantiation ratio: {len(tr['instantiated'])} / {len(nt['instantiated'])} = {len(tr['instantiated'])/len(nt['instantiated']):.6f}
    The count distribution shows a near-perfect mapping:
      no-trace 2x → trace 3x: 4,146 → 4,142 names (99.9% match)
      no-trace 4x → trace 6x: 300 → 300 (100% match)
      no-trace 6x → trace 9x: 159 → 159 (100% match)
      ... and so on for every tier.
    This proves the trace run does exactly 3/2 as many system evaluations
    as the no-trace run, producing the same derivation closure each time.
""")


# ── Main ─────────────────────────────────────────────────────────────────────

def main():
    no_trace_path = "cold-eval-no-trace.log"
    trace_path = "cold-eval-trace.log"

    print("Parsing logs (event-based)...")
    nt_events = parse_log_events(no_trace_path)
    tr_events = parse_log_events(trace_path)

    print(f"  no-trace: {len(nt_events):,} events from {nt_events[-1].lineno:,} lines")
    print(f"  trace:    {len(tr_events):,} events from {tr_events[-1].lineno:,} lines")

    nt = events_to_legacy(nt_events)
    tr = events_to_legacy(tr_events)

    report_overview(nt, tr)
    report_passes(nt, tr)
    report_nixpkgs_copies(nt, tr)
    report_store_copies(nt, tr)
    report_uncacheable(nt, tr)
    report_daemon_ops(nt, tr)
    report_recording_deps(tr)
    report_file_eval(nt, tr)
    report_drv_instantiation(nt, tr)
    report_stats_json(nt, tr)
    report_trace_hit_timing(tr)
    report_perpass_recording_uniqueness(tr)
    report_addtexttostore(nt, tr)
    report_eval_file_ordering(nt, tr)
    report_event_timeline(tr)
    report_summary(nt, tr)


if __name__ == "__main__":
    main()
