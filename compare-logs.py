"""Analyze and compare Nix evaluation debug logs (--debug output).

Supports one or more log files. With two or more, produces comparison tables
with deltas. Focuses on:
- Evaluation stats and performance comparison
- Eval trace cache behavior (hits, misses, verify, recovery)
- Evaluation pass structure
- Dependency recording overhead and redundancy
- Trace hit context and timeline

Usage via flake:
    nix run .#compare-logs -- --commit HASH [--runs r1,r2,...] [--nix PATH]
    nix run .#compare-logs -- [--labels l1,l2,...] LOG1 [LOG2 ...]
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from rich.console import Console
from rich.panel import Panel
from rich.table import Table
from rich.text import Text

# -- Constants ----------------------------------------------------------------

DAEMON_OP_NAMES: dict[int, str] = {
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


# -- Data model ---------------------------------------------------------------


@dataclass
class LogEvent:
    lineno: int
    kind: str
    data: dict[str, Any] = field(default_factory=dict)


@dataclass
class PassData:
    index: int
    start_line: int
    end_line: int
    instantiated: list[tuple[str, str]]
    drv_paths: set[str]
    eval_files: list[str]
    copies: list[str]
    daemon_ops: Counter[int]
    recordings: list[tuple[str, str]]
    trace_hits: list[tuple[int, str]]
    uncacheable: list[str]
    event_count: int


@dataclass
class LogSummary:
    copying_to_store: list[str]
    copying_nixpkgs: int
    daemon_ops: Counter[int]
    evaluating_files: list[str]
    instantiated: list[tuple[str, str]]
    recording_deps: Counter[str]
    recording_deps_detail: list[tuple[str, str, str, str]]
    stats_json: dict[str, Any] | None
    total_lines: int
    events: list[LogEvent]
    copy_done: list[tuple[str, str, str]]
    uncacheable: list[str]
    trace_hits: list[tuple[int, str]]
    passes: list[PassData]


# -- Config (set in main) ----------------------------------------------------

_nixpkgs_dir: str | None = None
_nixpkgs_prefix: str | None = None


# -- Helpers ------------------------------------------------------------------


def fmt(n: int | float) -> str:
    if isinstance(n, float):
        return f"{n:,.3f}"
    return f"{n:,}"


def short(path: str) -> str:
    if _nixpkgs_prefix:
        path = path.replace(_nixpkgs_prefix, "nixpkgs/")
    if _nixpkgs_dir:
        path = path.replace(_nixpkgs_dir, "nixpkgs")
    return path


def section(console: Console, title: str) -> None:
    console.print(Panel(f"[bold]{title}[/bold]", expand=False))


def subsection(console: Console, title: str) -> None:
    console.print(f"\n[bold dim]{title}[/bold dim]")


def ratio_str(a: float, b: float) -> str:
    if b == 0:
        return "\u2014"
    return f"{a / b:.2f}x"


# -- Parsing ------------------------------------------------------------------


def parse_log_events(path: str) -> list[LogEvent]:
    events: list[LogEvent] = []
    json_lines: list[str] = []
    in_json = False
    json_depth = 0

    with open(path) as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.rstrip("\n")

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

            m = re.match(r"copying '(.+?)' to the store\.\.\.", line)
            if m:
                events.append(LogEvent(lineno, "copy_start", {"path": m.group(1)}))
                continue

            m = re.match(r"copied '(.+?)' to '(.+?)' \(hash '(.+?)'\)", line)
            if m:
                events.append(
                    LogEvent(
                        lineno,
                        "copy_done",
                        {
                            "src": m.group(1),
                            "dst": m.group(2),
                            "hash": m.group(3),
                        },
                    )
                )
                continue

            m = re.match(r"source path '(.+?)' is uncacheable", line)
            if m:
                events.append(LogEvent(lineno, "uncacheable", {"path": m.group(1)}))
                continue

            m = re.match(r"performing daemon worker op: (\d+)", line)
            if m:
                events.append(LogEvent(lineno, "daemon_op", {"op": int(m.group(1))}))
                continue

            m = re.match(r"evaluating file '(.+?)'", line)
            if m:
                events.append(LogEvent(lineno, "eval_file", {"path": m.group(1)}))
                continue

            m = re.match(r"instantiated '(.+?)' -> '(.+?)'", line)
            if m:
                events.append(
                    LogEvent(
                        lineno,
                        "instantiated",
                        {"name": m.group(1), "drv": m.group(2)},
                    )
                )
                continue

            m = re.match(
                r"recording (\w+) \((\w+)\) dep:"
                r" input='(.*?)' key='(.*?)'",
                line,
            )
            if m:
                events.append(
                    LogEvent(
                        lineno,
                        "recording_dep",
                        {
                            "type": m.group(1),
                            "variant": m.group(2),
                            "input": m.group(3),
                            "key": m.group(4),
                        },
                    )
                )
                continue

            m = re.match(r"trace verify hit for '(.+?)'", line)
            if m:
                events.append(LogEvent(lineno, "trace_hit", {"attr": m.group(1)}))
                continue

            m = re.match(r"stat hash store: loading (\d+) entries", line)
            if m:
                events.append(
                    LogEvent(
                        lineno,
                        "stat_hash_store",
                        {"count": int(m.group(1))},
                    )
                )
                continue

            if line.startswith("getting root value"):
                events.append(LogEvent(lineno, "root_value"))
                continue

            if line.startswith('{"'):
                events.append(LogEvent(lineno, "output_json", {"line": line}))
                continue

            if line.startswith("warning:"):
                events.append(LogEvent(lineno, "warning", {"msg": line}))
                continue

    return events


# -- Aggregation --------------------------------------------------------------


def detect_passes(events: list[LogEvent]) -> list[PassData]:
    boundaries = [0]
    if _nixpkgs_dir:
        for i, e in enumerate(events):
            if e.kind == "copy_start" and e.data["path"] == _nixpkgs_dir:
                boundaries.append(i)
    boundaries.append(len(events))

    passes: list[PassData] = []
    for j in range(len(boundaries) - 1):
        start = boundaries[j]
        end = boundaries[j + 1]
        span = events[start:end]

        drvs = [
            (e.data["name"], e.data["drv"]) for e in span if e.kind == "instantiated"
        ]
        evals = [e.data["path"] for e in span if e.kind == "eval_file"]
        copies = [e.data["path"] for e in span if e.kind == "copy_start"]
        daemon: Counter[int] = Counter(
            e.data["op"] for e in span if e.kind == "daemon_op"
        )
        recs = [
            (e.data["type"], e.data["key"]) for e in span if e.kind == "recording_dep"
        ]
        hits = [(e.lineno, e.data["attr"]) for e in span if e.kind == "trace_hit"]
        uncache = [e.data["path"] for e in span if e.kind == "uncacheable"]

        passes.append(
            PassData(
                index=j,
                start_line=span[0].lineno if span else 0,
                end_line=span[-1].lineno if span else 0,
                instantiated=drvs,
                drv_paths={d[1] for d in drvs},
                eval_files=evals,
                copies=copies,
                daemon_ops=daemon,
                recordings=recs,
                trace_hits=hits,
                uncacheable=uncache,
                event_count=len(span),
            )
        )

    return passes


def aggregate_events(events: list[LogEvent]) -> LogSummary:
    copying_to_store: list[str] = []
    copying_nixpkgs = 0
    daemon_ops: Counter[int] = Counter()
    evaluating_files: list[str] = []
    instantiated: list[tuple[str, str]] = []
    recording_deps: Counter[str] = Counter()
    recording_deps_detail: list[tuple[str, str, str, str]] = []
    stats_json: dict[str, Any] | None = None
    copy_done: list[tuple[str, str, str]] = []
    uncacheable: list[str] = []
    trace_hits: list[tuple[int, str]] = []

    for e in events:
        if e.kind == "copy_start":
            p = e.data["path"]
            copying_to_store.append(p)
            if _nixpkgs_dir and p == _nixpkgs_dir:
                copying_nixpkgs += 1
        elif e.kind == "copy_done":
            copy_done.append((e.data["src"], e.data["dst"], e.data["hash"]))
        elif e.kind == "uncacheable":
            uncacheable.append(e.data["path"])
        elif e.kind == "daemon_op":
            daemon_ops[e.data["op"]] += 1
        elif e.kind == "eval_file":
            evaluating_files.append(e.data["path"])
        elif e.kind == "instantiated":
            instantiated.append((e.data["name"], e.data["drv"]))
        elif e.kind == "recording_dep":
            label = f"{e.data['type']} ({e.data['variant']})"
            recording_deps[label] += 1
            recording_deps_detail.append(
                (
                    e.data["type"],
                    e.data["variant"],
                    e.data["input"],
                    e.data["key"],
                )
            )
        elif e.kind == "stats_json":
            stats_json = e.data["json"]
        elif e.kind == "trace_hit":
            trace_hits.append((e.lineno, e.data["attr"]))

    return LogSummary(
        copying_to_store=copying_to_store,
        copying_nixpkgs=copying_nixpkgs,
        daemon_ops=daemon_ops,
        evaluating_files=evaluating_files,
        instantiated=instantiated,
        recording_deps=recording_deps,
        recording_deps_detail=recording_deps_detail,
        stats_json=stats_json,
        total_lines=events[-1].lineno if events else 0,
        events=events,
        copy_done=copy_done,
        uncacheable=uncacheable,
        trace_hits=trace_hits,
        passes=detect_passes(events),
    )


# -- Analysis helpers ---------------------------------------------------------


def analyze_recording_redundancy(
    details: list[tuple[str, str, str, str]],
) -> tuple[int, int, list[tuple[tuple[str, str], int]]]:
    key_counter: Counter[tuple[str, str]] = Counter((d[0], d[3]) for d in details)
    return len(details), len(key_counter), key_counter.most_common(30)


def flat_stats(d: dict[str, Any], prefix: str = "") -> dict[str, int | float]:
    out: dict[str, int | float] = {}
    for k, v in d.items():
        key = f"{prefix}.{k}" if prefix else k
        if isinstance(v, dict):
            out.update(flat_stats(v, key))
        elif isinstance(v, (int, float)):
            out[key] = v
    return out


# -- Type aliases for report signatures --------------------------------------

type LogEntries = list[tuple[str, LogSummary]]


# -- Reports ------------------------------------------------------------------


def report_overview(console: Console, logs: LogEntries) -> None:
    section(console, "1. Overview")
    labels = [lbl for lbl, _ in logs]
    ref_label = labels[0]

    t = Table(show_header=True, header_style="bold")
    t.add_column("Metric")
    for lbl in labels:
        t.add_column(lbl, justify="right")
    if len(labels) > 1:
        for lbl in labels[1:]:
            t.add_column(f"{lbl}/{ref_label}", justify="right")

    rows: list[tuple[str, list[int]]] = [
        ("Total log lines", [d.total_lines for _, d in logs]),
        ("Files evaluated", [len(d.evaluating_files) for _, d in logs]),
        ("Derivations instantiated", [len(d.instantiated) for _, d in logs]),
        (
            "Unique drv paths",
            [len({drv for _, drv in d.instantiated}) for _, d in logs],
        ),
        ("Total store copies", [len(d.copying_to_store) for _, d in logs]),
        ("Uncacheable source paths", [len(d.uncacheable) for _, d in logs]),
        ("Total daemon ops", [sum(d.daemon_ops.values()) for _, d in logs]),
        ("Dep recordings", [sum(d.recording_deps.values()) for _, d in logs]),
        ("Trace cache hits", [len(d.trace_hits) for _, d in logs]),
    ]

    for label, values in rows:
        row: list[str | Text] = [label, *(fmt(v) for v in values)]
        if len(labels) > 1:
            for v in values[1:]:
                row.append(ratio_str(float(v), float(values[0])))
        t.add_row(*row)

    # Log line overhead attribution
    if len(labels) > 1:
        ref_lines = [d.total_lines for _, d in logs][0]
        ref_recs = [sum(d.recording_deps.values()) for _, d in logs][0]
        for i, (lbl, data) in enumerate(logs[1:], 1):
            delta_lines = data.total_lines - ref_lines
            delta_recs = sum(data.recording_deps.values()) - ref_recs
            if delta_lines != 0 and delta_recs != 0:
                attribution = delta_recs / delta_lines * 100 if delta_lines else 0.0
                t.add_row(
                    Text(f"  log line delta vs {ref_label}", style="dim"),
                    *([""] * len(labels)),
                    *[""] * (i - 1),
                    Text(
                        f"{delta_lines:+,} lines ({attribution:.0f}% from dep recording)",
                        style="dim",
                    ),
                    *[""] * (len(labels) - 1 - i),
                )

    console.print(t)


def report_stats_json(console: Console, logs: LogEntries) -> None:
    section(console, "2. Evaluation Stats")

    stats_logs = [(lbl, d) for lbl, d in logs if d.stats_json is not None]
    if not stats_logs:
        console.print("  [dim](no stats found)[/dim]")
        return

    labels = [lbl for lbl, _ in stats_logs]
    flat_all = {
        lbl: flat_stats(d.stats_json)
        for lbl, d in stats_logs
        if d.stats_json is not None
    }
    all_keys = sorted(set().union(*(f.keys() for f in flat_all.values())))

    t = Table(show_header=True, header_style="bold")
    t.add_column("Stat")
    for lbl in labels:
        t.add_column(lbl, justify="right")
    if len(labels) > 1:
        for lbl in labels[1:]:
            t.add_column(f"{lbl}/{labels[0]}", justify="right")

    for key in all_keys:
        values = [flat_all[lbl].get(key) for lbl in labels]
        if any(v is None for v in values):
            continue

        row: list[str] = [key]
        for v in values:
            if isinstance(v, float):
                row.append(f"{v:.3f}")
            else:
                row.append(fmt(v) if v is not None else "\u2014")

        if len(labels) > 1 and values[0] is not None:
            for v in values[1:]:
                if v is not None and values[0] != 0:
                    row.append(f"{float(v) / float(values[0]):.2f}x")
                else:
                    row.append("\u2014")

        t.add_row(*row)

    console.print(t)


def report_eval_trace_summary(console: Console, logs: LogEntries) -> None:
    """Focused eval-trace cache behavior summary."""
    stats_logs = [(lbl, d) for lbl, d in logs if d.stats_json is not None]
    has_trace = any(
        d.stats_json is not None and d.stats_json.get("evalTrace")
        for _, d in stats_logs
    )
    if not has_trace:
        return

    section(console, "3. Eval Trace Cache Summary")

    trace_keys = [
        ("hits", ("evalTrace", "hits")),
        ("misses", ("evalTrace", "misses")),
        ("verify.count", ("evalTrace", "verify", "count")),
        ("verify.passed", ("evalTrace", "verify", "passed")),
        ("verify.failed", ("evalTrace", "verify", "failed")),
        ("verify.depsChecked", ("evalTrace", "verify", "depsChecked")),
        ("verify.timeUs", ("evalTrace", "verify", "timeUs")),
        ("verifyTrace.timeUs", ("evalTrace", "verifyTrace", "timeUs")),
        ("recovery.attempts", ("evalTrace", "recovery", "attempts")),
        ("recovery.directHash.hits", ("evalTrace", "recovery", "directHash", "hits")),
        (
            "recovery.structVariant.hits",
            ("evalTrace", "recovery", "structVariant", "hits"),
        ),
        ("recovery.failures", ("evalTrace", "recovery", "failures")),
        ("recovery.timeUs", ("evalTrace", "recovery", "timeUs")),
        ("record.count", ("evalTrace", "record", "count")),
        ("record.timeUs", ("evalTrace", "record", "timeUs")),
        ("loadTrace.count", ("evalTrace", "loadTrace", "count")),
        ("loadTrace.timeUs", ("evalTrace", "loadTrace", "timeUs")),
        ("db.initTimeUs", ("evalTrace", "db", "initTimeUs")),
        ("replay.totalCalls", ("evalTrace", "replay", "totalCalls")),
        ("replay.bloomHits", ("evalTrace", "replay", "bloomHits")),
        ("replay.epochHits", ("evalTrace", "replay", "epochHits")),
        ("replay.added", ("evalTrace", "replay", "added")),
        ("depTracker.scopes", ("evalTrace", "depTracker", "scopes")),
        ("depTracker.ownDepsTotal", ("evalTrace", "depTracker", "ownDepsTotal")),
        ("depTracker.ownDepsMax", ("evalTrace", "depTracker", "ownDepsMax")),
    ]

    labels = [lbl for lbl, _ in stats_logs]
    t = Table(show_header=True, header_style="bold")
    t.add_column("Metric")
    for lbl in labels:
        t.add_column(lbl, justify="right")

    for name, keys in trace_keys:
        values: list[int | float | None] = []
        for _, data in stats_logs:
            v: Any = data.stats_json
            for k in keys:
                if isinstance(v, dict):
                    v = v.get(k)
                else:
                    v = None
                    break
            values.append(v if isinstance(v, (int, float)) else None)

        if all(v is None or v == 0 for v in values):
            continue

        row: list[str] = [name]
        for v in values:
            if v is None:
                row.append("\u2014")
            elif name.endswith("TimeUs") or name.endswith("timeUs"):
                row.append(f"{v / 1000:.1f}ms")
            else:
                row.append(fmt(v))
        t.add_row(*row)

    console.print(t)


def report_passes(console: Console, logs: LogEntries) -> None:
    section(console, "4. Evaluation Pass Structure")

    if not _nixpkgs_dir:
        console.print("[dim]  (pass detection requires --nixpkgs)[/dim]")
        return

    # Check if pass structure is identical across runs
    pass_summaries = []
    for _label, data in logs:
        summary = [
            (len(p.instantiated), len(p.drv_paths), len(p.eval_files), len(p.copies))
            for p in data.passes
        ]
        pass_summaries.append(summary)

    identical_structure = len(pass_summaries) > 1 and all(
        s == pass_summaries[0] for s in pass_summaries[1:]
    )

    if identical_structure:
        # Show one representative, note which runs share it
        labels_with_passes = [lbl for lbl, d in logs if len(d.passes) > 1]
        labels_without = [lbl for lbl, d in logs if len(d.passes) <= 1]

        if labels_with_passes:
            console.print(
                f"\n[dim]Pass structure identical across:"
                f" {', '.join(labels_with_passes)}[/dim]"
            )
            # Show first run's passes as representative
            label, data = next((lbl, d) for lbl, d in logs if len(d.passes) > 1)
            _render_passes_for_label(console, label, data)
        for lbl in labels_without:
            data = next(d for lab, d in logs if lab == lbl)
            _render_passes_for_label(console, lbl, data)
    else:
        for label, data in logs:
            _render_passes_for_label(console, label, data)


def _render_passes_for_label(console: Console, label: str, data: LogSummary) -> None:
    passes = data.passes
    subsection(
        console,
        f"{label}: {len(passes)} passes (split by nixpkgs dir copies)",
    )

    for p in passes:
        t = Table(
            title=(
                f"Pass {p.index}  (lines {p.start_line:,}"
                f"\u2013{p.end_line:,}, {p.event_count:,} events)"
            ),
            show_header=False,
            box=None,
            padding=(0, 2),
        )
        t.add_column("Key", style="bold")
        t.add_column("Value")
        t.add_row(
            "Instantiated",
            f"{fmt(len(p.instantiated))} ({fmt(len(p.drv_paths))} unique drv paths)",
        )
        t.add_row("Files evaluated", fmt(len(p.eval_files)))
        t.add_row(
            "Store copies",
            f"{fmt(len(p.copies))} (uncacheable: {len(p.uncacheable)})",
        )
        t.add_row("Daemon ops", fmt(sum(p.daemon_ops.values())))
        if p.recordings:
            t.add_row("Dep recordings", fmt(len(p.recordings)))
        for ln, attr in p.trace_hits:
            t.add_row("Trace hit", f"line {ln:,}: '{attr}'")
        if p.instantiated:
            last = p.instantiated[-1]
            drv_tail = last[1].split("/")[-1]
            t.add_row("Last drv", f"{last[0]} \u2192 ...{drv_tail}")
        console.print(t)


def report_recording_deps(console: Console, logs: LogEntries) -> None:
    runs_with_deps = [(lbl, d) for lbl, d in logs if d.recording_deps]
    if not runs_with_deps:
        return

    section(console, "5. Dependency Recording")

    # Check if dep data is identical across runs with recordings
    dep_summaries = [sorted(d.recording_deps.items()) for _, d in runs_with_deps]
    identical = len(dep_summaries) > 1 and all(
        s == dep_summaries[0] for s in dep_summaries[1:]
    )

    if identical and len(runs_with_deps) > 1:
        labels_str = ", ".join(lbl for lbl, _ in runs_with_deps)
        console.print(f"\n[dim]Identical across: {labels_str}[/dim]")
        runs_with_deps = runs_with_deps[:1]

    for label, data in runs_with_deps:
        # Type breakdown
        subsection(console, f"{label}: dep type breakdown")
        t = Table(show_header=True, header_style="bold")
        t.add_column("Dep type")
        t.add_column("Count", justify="right")
        for dep_type, count in data.recording_deps.most_common():
            t.add_row(dep_type, fmt(count))
        console.print(t)

        # Redundancy
        total, unique_keys, _top = analyze_recording_redundancy(
            data.recording_deps_detail
        )
        dup = total - unique_keys
        dup_rate = dup / total * 100 if total else 0.0
        console.print(
            f"\n  {fmt(total)} total, {fmt(unique_keys)} unique,"
            f" {dup_rate:.1f}% duplicate"
        )

        # By dep type: unique vs duplicate
        by_type: dict[str, dict[str, Any]] = defaultdict(
            lambda: {"total": 0, "keys": Counter()}
        )
        for dep_type, _variant, _inp, key in data.recording_deps_detail:
            by_type[dep_type]["total"] += 1
            by_type[dep_type]["keys"][(dep_type, key)] += 1

        t = Table(show_header=True, header_style="bold")
        t.add_column("Dep type")
        t.add_column("Total", justify="right")
        t.add_column("Unique", justify="right")
        t.add_column("Dup", justify="right")
        t.add_column("Dup%", justify="right")
        for dep_type in sorted(by_type, key=lambda x: -by_type[x]["total"]):
            info = by_type[dep_type]
            unique_ct = len(info["keys"])
            dup_ct = info["total"] - unique_ct
            dup_pct = dup_ct / info["total"] * 100 if info["total"] else 0.0
            t.add_row(
                dep_type,
                fmt(info["total"]),
                fmt(unique_ct),
                fmt(dup_ct),
                f"{dup_pct:.1f}%",
            )
        console.print(t)

        # Per-pass recording distribution
        if len(data.passes) > 1:
            subsection(console, f"{label}: recordings per pass")
            for p in data.passes:
                n = len(p.recordings)
                if n:
                    type_counts: Counter[str] = Counter(r[0] for r in p.recordings)
                    breakdown = ", ".join(
                        f"{k}={v}" for k, v in type_counts.most_common(5)
                    )
                    console.print(
                        f"  Pass {p.index}: {fmt(n):>8} recordings  ({breakdown})"
                    )


def report_perpass_recording_uniqueness(console: Console, logs: LogEntries) -> None:
    has_any = False
    for _label, data in logs:
        if len(data.passes) > 1 and any(p.recordings for p in data.passes):
            has_any = True
            break
    if not has_any:
        return

    section(console, "6. Per-Pass Recording Uniqueness")

    for label, data in logs:
        if len(data.passes) <= 1:
            continue
        has_recordings = any(p.recordings for p in data.passes)
        if not has_recordings:
            continue

        subsection(console, label)
        t = Table(show_header=True, header_style="bold")
        t.add_column("Pass", justify="right")
        t.add_column("Total", justify="right")
        t.add_column("Novel", justify="right")
        t.add_column("Already-seen", justify="right")
        t.add_column("Novel%", justify="right")

        seen_keys: set[tuple[str, str]] = set()
        for p in data.passes:
            novel = 0
            repeat = 0
            for dep_type, key in p.recordings:
                k = (dep_type, key)
                if k in seen_keys:
                    repeat += 1
                else:
                    novel += 1
                    seen_keys.add(k)
            total = novel + repeat
            pct_val = novel / total * 100 if total else 0.0
            t.add_row(
                str(p.index),
                fmt(total),
                fmt(novel),
                fmt(repeat),
                f"{pct_val:.1f}%",
            )
        console.print(t)


def report_trace_hit_timing(console: Console, logs: LogEntries) -> None:
    has_hits = any(data.trace_hits for _, data in logs)
    if not has_hits:
        return

    section(console, "7. Trace Hit Context")

    for label, data in logs:
        if not data.trace_hits:
            continue

        events = data.events

        for p in data.passes:
            if not p.trace_hits:
                continue

            hit_line = p.trace_hits[0][0]
            hit_attr = p.trace_hits[0][1]

            before_hit = [e for e in events if p.start_line <= e.lineno < hit_line]
            after_hit = [e for e in events if hit_line <= e.lineno <= p.end_line]

            inst_before = sum(1 for e in before_hit if e.kind == "instantiated")
            inst_after = sum(1 for e in after_hit if e.kind == "instantiated")
            daemon_before = sum(1 for e in before_hit if e.kind == "daemon_op")
            daemon_after = sum(1 for e in after_hit if e.kind == "daemon_op")

            pass_span = p.end_line - p.start_line
            hit_pct = (hit_line - p.start_line) / pass_span * 100 if pass_span else 0.0

            console.print(
                f"\n  [{label}] Pass {p.index}:"
                f" trace verify hit for '{hit_attr}'"
                f" at line {hit_line:,}"
                f" ({hit_pct:.1f}% through pass)"
            )

            t = Table(show_header=True, header_style="bold", box=None)
            t.add_column("Metric")
            t.add_column("Before hit", justify="right")
            t.add_column("After hit", justify="right")
            t.add_column("Total", justify="right")
            t.add_row(
                "Instantiations",
                fmt(inst_before),
                fmt(inst_after),
                fmt(inst_before + inst_after),
            )
            t.add_row(
                "Daemon ops",
                fmt(daemon_before),
                fmt(daemon_after),
                fmt(daemon_before + daemon_after),
            )
            console.print(t)

            # All trace hits for this pass
            if len(p.trace_hits) > 1:
                subsection(console, "All trace hits:")
                for ln, attr in p.trace_hits:
                    console.print(f"    line {ln:>8,}: '{attr}'")


def report_daemon_ops(console: Console, logs: LogEntries) -> None:
    section(console, "8. Daemon Worker Operations")

    labels = [lbl for lbl, _ in logs]
    all_ops = sorted(set().union(*(d.daemon_ops.keys() for _, d in logs)))

    # Check if identical
    op_summaries = [sorted(d.daemon_ops.items()) for _, d in logs]
    identical = len(op_summaries) > 1 and all(
        s == op_summaries[0] for s in op_summaries[1:]
    )
    if identical:
        console.print("\n[dim]Identical across all runs[/dim]")
        labels = labels[:1]
        logs = logs[:1]

    t = Table(show_header=True, header_style="bold")
    t.add_column("Op", justify="right")
    t.add_column("Name")
    for lbl in labels:
        t.add_column(lbl, justify="right")

    for op in all_ops:
        values = [d.daemon_ops.get(op, 0) for _, d in logs]
        name = DAEMON_OP_NAMES.get(op, f"Unknown({op})")
        row: list[str] = [str(op), name, *(fmt(v) for v in values)]
        t.add_row(*row)

    console.print(t)


def report_event_timeline(console: Console, logs: LogEntries) -> None:
    section(console, "9. Event Timeline")

    for label, data in logs:
        subsection(
            console,
            f"{label}: key events in chronological order",
        )

        milestones: list[tuple[int, str, str]] = []
        for e in data.events:
            if e.kind == "root_value":
                milestones.append(
                    (e.lineno, "ROOT_VALUE", "getting root value via rootLoader")
                )
            elif e.kind == "stat_hash_store":
                milestones.append(
                    (e.lineno, "STAT_HASH", f"loading {e.data['count']} entries")
                )
            elif (
                e.kind == "copy_start"
                and _nixpkgs_dir
                and e.data["path"] == _nixpkgs_dir
            ):
                milestones.append(
                    (e.lineno, "NIXPKGS_COPY", "copying nixpkgs to store")
                )
            elif e.kind == "trace_hit":
                milestones.append(
                    (e.lineno, "TRACE_HIT", f"verify hit for '{e.data['attr']}'")
                )

        for p in data.passes:
            if p.instantiated:
                first = p.instantiated[0]
                last = p.instantiated[-1]
                milestones.append(
                    (p.start_line, f"PASS_{p.index}_START", f"first drv: {first[0]}")
                )
                pass_inst = [
                    e
                    for e in data.events
                    if e.kind == "instantiated"
                    and p.start_line <= e.lineno <= p.end_line
                ]
                if pass_inst:
                    milestones.append(
                        (
                            pass_inst[-1].lineno,
                            f"PASS_{p.index}_END",
                            f"last drv: {last[0]} ({fmt(len(p.instantiated))} total)",
                        )
                    )

        milestones.sort(key=lambda x: x[0])
        t = Table(show_header=True, header_style="bold", box=None)
        t.add_column("Line", justify="right")
        t.add_column("Event")
        t.add_column("Detail")
        for ln, kind, desc in milestones:
            t.add_row(fmt(ln), kind, desc)
        console.print(t)


# -- Main ---------------------------------------------------------------------


def main() -> None:
    global _nixpkgs_dir, _nixpkgs_prefix  # noqa: PLW0603

    parser = argparse.ArgumentParser(
        prog="compare-logs",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "log_files",
        nargs="*",
        metavar="LOG",
        help="One or more Nix --debug log files to analyze",
    )
    parser.add_argument(
        "--nix",
        type=Path,
        default=Path.cwd(),
        help="Path to nix source dir with run data (default: cwd)",
    )
    parser.add_argument(
        "--commit",
        type=str,
        default=None,
        help="Commit hash to analyze (resolves debug.log/stats.json from run dirs)",
    )
    parser.add_argument(
        "--runs",
        type=str,
        default=None,
        help="Comma-separated run names to compare (default: auto-discover)",
    )
    parser.add_argument(
        "--nixpkgs",
        metavar="PATH",
        default=str(Path.home() / "nixpkgs"),
        help="Path to nixpkgs directory (default: ~/nixpkgs)",
    )
    parser.add_argument(
        "--labels",
        type=str,
        default=None,
        help="Comma-separated labels for log files (default: file stems)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="Save report to file (.html for rich markup, otherwise plain text)",
    )
    args = parser.parse_args()

    log_files: list[str] = []
    labels: list[str] = []
    stats_overrides: dict[str, dict[str, Any]] = {}

    if args.commit:
        # Resolve logs and stats from run directory structure
        commit = args.commit
        nix_path = args.nix
        if args.runs:
            run_names = [r.strip() for r in args.runs.split(",")]
        else:
            # Auto-discover runs that have this commit
            run_names = []
            for entry in sorted(nix_path.iterdir()):
                if (
                    not entry.is_dir()
                    or entry.is_symlink()
                    or entry.name.startswith(".")
                ):
                    continue
                if (entry / commit / "debug.log").exists():
                    run_names.append(entry.name)
                else:
                    for sub in sorted(entry.iterdir()):
                        if sub.is_dir() and (sub / commit / "debug.log").exists():
                            run_names.append(f"{entry.name}/{sub.name}")
            if not run_names:
                print(
                    f"Error: no runs found for commit {commit} under {nix_path}",
                    file=sys.stderr,
                )
                sys.exit(1)

        for name in run_names:
            run_dir = nix_path / Path(name) / commit
            debug_log = run_dir / "debug.log"
            if not debug_log.exists():
                print(
                    f"Error: debug.log not found: {debug_log}",
                    file=sys.stderr,
                )
                sys.exit(1)
            log_files.append(str(debug_log))
            labels.append(name)
            stats_file = run_dir / "stats.json"
            if stats_file.exists():
                with open(stats_file) as f:
                    stats_overrides[name] = json.load(f)
    else:
        if not args.log_files:
            parser.error("LOG files are required unless --commit is used")
        log_files = args.log_files
        for path in log_files:
            if not Path(path).is_file():
                print(
                    f"Error: log file not found: {path}",
                    file=sys.stderr,
                )
                sys.exit(1)
        if args.labels:
            labels = [label.strip() for label in args.labels.split(",")]
            if len(labels) != len(log_files):
                print(
                    f"Error: {len(labels)} labels provided for"
                    f" {len(log_files)} log files",
                    file=sys.stderr,
                )
                sys.exit(1)
        else:
            labels = [Path(p).stem for p in log_files]

    if args.nixpkgs:
        nixpkgs = args.nixpkgs.rstrip("/")
        _nixpkgs_dir = nixpkgs
        _nixpkgs_prefix = nixpkgs + "/"

    console = Console(record=True)

    console.print("Parsing logs...")
    logs: LogEntries = []
    for path, label in zip(log_files, labels):
        events = parse_log_events(path)
        if events:
            console.print(
                f"  {label}: {len(events):,} events from {events[-1].lineno:,} lines"
            )
        else:
            console.print(f"  {label}: [dim](empty)[/dim]")
        data = aggregate_events(events)
        if label in stats_overrides and data.stats_json is None:
            data.stats_json = stats_overrides[label]
        logs.append((label, data))

    report_overview(console, logs)
    report_stats_json(console, logs)
    report_eval_trace_summary(console, logs)
    report_passes(console, logs)
    report_recording_deps(console, logs)
    report_perpass_recording_uniqueness(console, logs)
    report_trace_hit_timing(console, logs)
    report_daemon_ops(console, logs)
    report_event_timeline(console, logs)

    if args.output:
        if args.output.suffix == ".html":
            args.output.write_text(console.export_html())
        else:
            args.output.write_text(console.export_text())


if __name__ == "__main__":
    main()
