"""`logs` subcommand — analyse one or more `nix --debug` log files.

Inputs can be provided either as raw log paths (legacy style) or as
`--commit HASH --runs r1,r2,...`, which pulls the logs from the
`eval-trace-bench generate` results directory structure.
"""

from __future__ import annotations

import json
import math
import sys
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any

from cyclopts import App
from rich.console import Console
from rich.text import Text

from ..cliutil import DEFAULT_NIX, DEFAULT_NIXPKGS
from ..discovery import discover_runs
from ..layout import existing_results_root
from ..logparse import (
    DAEMON_OP_NAMES,
    LogSummary,
    aggregate_events,
    analyze_recording_redundancy,
    parse_log_events,
)
from ..render import (
    MetricRow,
    export_console,
    fmt_int,
    metric_table,
    section,
    simple_table,
    subsection,
)
from ..stats import flatten

LogEntries = list[tuple[str, LogSummary]]


# -- Reports ----------------------------------------------------------------


def report_overview(console: Console, logs: LogEntries) -> None:
    section(console, "1. Overview")
    labels = [lbl for lbl, _ in logs]
    ref = labels[0]

    # Each tuple: (label, [values-by-log-entry]).
    row_defs: list[tuple[str, list[int]]] = [
        ("Total log lines", [d.total_lines for _, d in logs]),
        ("Files evaluated", [len(d.evaluating_files) for _, d in logs]),
        ("Derivations instantiated", [len(d.instantiated) for _, d in logs]),
        ("Unique drv paths", [len({drv for _, drv in d.instantiated}) for _, d in logs]),
        ("Total store copies", [len(d.copying_to_store) for _, d in logs]),
        ("Uncacheable source paths", [len(d.uncacheable) for _, d in logs]),
        ("Total daemon ops", [sum(d.daemon_ops.values()) for _, d in logs]),
        ("Dep recordings", [sum(d.recording_deps.values()) for _, d in logs]),
        ("Trace cache hits", [len(d.trace_hits) for _, d in logs]),
    ]
    rows = [
        MetricRow(label, dict(zip(labels, (float(v) for v in values), strict=True)))
        for label, values in row_defs
    ]
    console.print(
        metric_table(
            rows,
            labels,
            ratios_against=ref if len(labels) > 1 else None,
            fmt=lambda v: fmt_int(int(v)),
        )
    )


def report_stats_json(console: Console, logs: LogEntries) -> None:
    """Full counter dump with no ellipsis truncation (plan H1)."""
    section(console, "2. Evaluation stats")

    stats_logs = [(lbl, d) for lbl, d in logs if d.stats_json is not None]
    if not stats_logs:
        console.print("  [dim](no stats found)[/dim]")
        return

    labels = [lbl for lbl, _ in stats_logs]
    flat_all: dict[str, dict[str, int | float]] = {}
    for lbl, d in stats_logs:
        if d.stats_json is not None:
            flat_all[lbl] = flatten(d.stats_json)
    all_keys_set: set[str] = set()
    for f in flat_all.values():
        all_keys_set.update(f.keys())

    rows: list[MetricRow] = []
    for key in sorted(all_keys_set):
        values = {lbl: flat_all[lbl].get(key) for lbl in labels}
        if any(v is None for v in values.values()):
            continue
        rows.append(
            MetricRow(key, {lbl: float(v) if v is not None else None for lbl, v in values.items()})
        )

    def _fmt(v: float) -> str:
        return f"{v:.3f}" if not v.is_integer() else fmt_int(int(v))

    console.print(
        metric_table(
            rows,
            labels,
            ratios_against=labels[0] if len(labels) > 1 else None,
            fmt=_fmt,
        )
    )


# Paths into stats.json emitted by `src/libexpr/eval.cc`; keyed under
# `evalTrace.*`.  Kept as a single table so section 3 is one `metric_table`
# call instead of ten branches.
_TRACE_KEYS: list[tuple[str, tuple[str, ...]]] = [
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
    ("recovery.structVariant.hits", ("evalTrace", "recovery", "structVariant", "hits")),
    ("recovery.gitIdentity.attempts", ("evalTrace", "recovery", "gitIdentity", "attempts")),
    ("recovery.gitIdentity.candidates", ("evalTrace", "recovery", "gitIdentity", "candidates")),
    ("recovery.gitIdentity.rejected", ("evalTrace", "recovery", "gitIdentity", "rejected")),
    ("recovery.gitIdentityHits", ("evalTrace", "recovery", "gitIdentityHits")),
    ("recovery.historyBootstraps", ("evalTrace", "recovery", "historyBootstraps")),
    ("recovery.failures", ("evalTrace", "recovery", "failures")),
    ("recovery.timeUs", ("evalTrace", "recovery", "timeUs")),
    ("recovery.lookup.directHashUs", ("evalTrace", "recovery", "lookups", "directHash", "timeUs")),
    (
        "recovery.lookup.gitIdentityUs",
        ("evalTrace", "recovery", "lookups", "gitIdentity", "timeUs"),
    ),
    (
        "recovery.lookup.scanHistoryUs",
        ("evalTrace", "recovery", "lookups", "scanHistory", "timeUs"),
    ),
    (
        "recovery.implicitGuard.timeUs",
        ("evalTrace", "recovery", "acceptance", "implicitGuardTimeUs"),
    ),
    ("record.count", ("evalTrace", "record", "count")),
    ("record.timeUs", ("evalTrace", "record", "timeUs")),
    ("record.hashUs", ("evalTrace", "record", "hashUs")),
    ("record.serializeKeysUs", ("evalTrace", "record", "serializeKeysUs")),
    ("record.serializeValuesUs", ("evalTrace", "record", "serializeValuesUs")),
    ("record.flushUs", ("evalTrace", "record", "flushUs")),
    ("loadTrace.count", ("evalTrace", "loadTrace", "count")),
    ("loadTrace.timeUs", ("evalTrace", "loadTrace", "timeUs")),
    ("loadKeySet.count", ("evalTrace", "loadKeySet", "count")),
    ("loadKeySet.cacheHits", ("evalTrace", "loadKeySet", "cacheHits")),
    ("loadKeySet.cacheMisses", ("evalTrace", "loadKeySet", "cacheMisses")),
    ("loadKeySet.timeUs", ("evalTrace", "loadKeySet", "timeUs")),
    ("db.initTimeUs", ("evalTrace", "db", "initTimeUs")),
    ("replay.totalCalls", ("evalTrace", "replay", "totalCalls")),
    ("replay.bloomHits", ("evalTrace", "replay", "bloomHits")),
    ("replay.epochHits", ("evalTrace", "replay", "epochHits")),
    ("replay.added", ("evalTrace", "replay", "added")),
    ("depTracker.scopes", ("evalTrace", "depTracker", "scopes")),
    ("depTracker.ownDepsTotal", ("evalTrace", "depTracker", "ownDepsTotal")),
    ("depTracker.ownDepsMax", ("evalTrace", "depTracker", "ownDepsMax")),
    ("depHash.cacheHits", ("evalTrace", "depHash", "cacheHits")),
    ("depHash.cacheMisses", ("evalTrace", "depHash", "cacheMisses")),
    ("depHash.contentUs", ("evalTrace", "depHash", "contentUs")),
    ("depHash.structuredJsonUs", ("evalTrace", "depHash", "structuredJsonUs")),
    ("depHash.structuredNixUs", ("evalTrace", "depHash", "structuredNixUs")),
    ("depHash.structuredTomlUs", ("evalTrace", "depHash", "structuredTomlUs")),
    ("depHash.structuredOuterUs", ("evalTrace", "depHash", "structuredOuterUs")),
    ("depHash.gitIdentityUs", ("evalTrace", "depHash", "gitIdentityUs")),
    ("depHash.structVariantCandidates", ("evalTrace", "depHash", "structVariantCandidates")),
    ("depHash.structVariantDepsResolved", ("evalTrace", "depHash", "structVariantDepsResolved")),
    ("depHash.structVariantDepResolveUs", ("evalTrace", "depHash", "structVariantDepResolveUs")),
    ("depHash.structVariantLoadKeySetUs", ("evalTrace", "depHash", "structVariantLoadKeySetUs")),
    ("depHash.structVariantHashUs", ("evalTrace", "depHash", "structVariantHashUs")),
]


def _lookup_path(obj: dict[str, Any], path: tuple[str, ...]) -> int | float | None:
    current: Any = obj
    for k in path:
        if not isinstance(current, dict):
            return None
        typed: dict[str, Any] = current  # pyright: ignore[reportUnknownVariableType]
        current = typed.get(k)
    return current if isinstance(current, int | float) else None


def report_eval_trace_summary(console: Console, logs: LogEntries) -> None:
    stats_logs = [(lbl, d) for lbl, d in logs if d.stats_json is not None]
    has_trace = any(
        d.stats_json is not None and d.stats_json.get("evalTrace") for _, d in stats_logs
    )
    if not has_trace:
        return

    section(console, "3. Eval-trace cache summary")

    labels = [lbl for lbl, _ in stats_logs]
    rows: list[MetricRow] = []
    for name, path in _TRACE_KEYS:
        values: dict[str, float | None] = {}
        for lbl, data in stats_logs:
            assert data.stats_json is not None
            v = _lookup_path(data.stats_json, path)
            values[lbl] = float(v) if v is not None else None
        if all(v is None or v == 0 for v in values.values()):
            continue
        rows.append(MetricRow(name, values))

    def _fmt(v: float, name: str) -> str:
        return f"{v / 1000:.1f}ms" if name.endswith(("TimeUs", "Us", "timeUs")) else fmt_int(int(v))

    console.print(metric_table(rows, labels, row_fmt=_fmt))


def _render_passes_for_label(console: Console, label: str, data: LogSummary) -> None:
    subsection(console, f"{label}: {len(data.passes)} passes (split by nixpkgs dir copies)")
    for p in data.passes:
        rows: list[list[str | Text]] = [
            [
                "Instantiated",
                f"{fmt_int(len(p.instantiated))} ({fmt_int(len(p.drv_paths))} unique drv paths)",
            ],
            ["Files evaluated", fmt_int(len(p.eval_files))],
            ["Store copies", f"{fmt_int(len(p.copies))} (uncacheable: {len(p.uncacheable)})"],
            ["Daemon ops", fmt_int(sum(p.daemon_ops.values()))],
        ]
        if p.recordings:
            rows.append(["Dep recordings", fmt_int(len(p.recordings))])
        for ln, attr in p.trace_hits:
            rows.append(["Trace hit", f"line {ln:,}: '{attr}'"])
        if p.instantiated:
            last = p.instantiated[-1]
            drv_tail = last[1].split("/")[-1]
            rows.append(["Last drv", f"{last[0]} → ...{drv_tail}"])
        console.print(
            simple_table(
                ["Key", "Value"],
                rows,
                title=f"Pass {p.index}  (lines {p.start_line:,}–{p.end_line:,}, {p.event_count:,} events)",
            )
        )


def report_passes(console: Console, logs: LogEntries, nixpkgs_dir: str | None) -> None:
    section(console, "4. Evaluation pass structure")
    if not nixpkgs_dir:
        console.print("[dim]  (pass detection requires --nixpkgs)[/dim]")
        return

    pass_summaries = [
        [
            (len(p.instantiated), len(p.drv_paths), len(p.eval_files), len(p.copies))
            for p in d.passes
        ]
        for _, d in logs
    ]
    identical = len(pass_summaries) > 1 and all(s == pass_summaries[0] for s in pass_summaries[1:])

    if identical:
        labels_with = [lbl for lbl, d in logs if len(d.passes) > 1]
        labels_without = [lbl for lbl, d in logs if len(d.passes) <= 1]
        if labels_with:
            console.print(f"\n[dim]Pass structure identical across: {', '.join(labels_with)}[/dim]")
            label, data = next((lbl, d) for lbl, d in logs if len(d.passes) > 1)
            _render_passes_for_label(console, label, data)
        for lbl in labels_without:
            data = next(d for lab, d in logs if lab == lbl)
            _render_passes_for_label(console, lbl, data)
    else:
        for label, data in logs:
            _render_passes_for_label(console, label, data)


def report_recording_deps(console: Console, logs: LogEntries) -> None:
    runs_with_deps = [(lbl, d) for lbl, d in logs if d.recording_deps]
    if not runs_with_deps:
        return
    section(console, "5. Dependency recording")
    dep_summaries = [sorted(d.recording_deps.items()) for _, d in runs_with_deps]
    identical = len(dep_summaries) > 1 and all(s == dep_summaries[0] for s in dep_summaries[1:])
    if identical and len(runs_with_deps) > 1:
        labels_str = ", ".join(lbl for lbl, _ in runs_with_deps)
        console.print(f"\n[dim]Identical across: {labels_str}[/dim]")
        runs_with_deps = runs_with_deps[:1]

    for label, data in runs_with_deps:
        subsection(console, f"{label}: dep type breakdown")
        console.print(
            simple_table(
                ["Dep type", "Count"],
                [
                    [dep_type, fmt_int(count)]
                    for dep_type, count in data.recording_deps.most_common()
                ],
            )
        )

        total, unique_keys, _ = analyze_recording_redundancy(data.recording_deps_detail)
        dup = total - unique_keys
        dup_rate = dup / total * 100 if total else 0.0
        console.print(
            f"\n  {fmt_int(total)} total, {fmt_int(unique_keys)} unique, {dup_rate:.1f}% duplicate"
        )

        by_type: dict[str, dict[str, Any]] = defaultdict(lambda: {"total": 0, "keys": Counter()})
        for dep_type, _variant, _inp, key in data.recording_deps_detail:
            by_type[dep_type]["total"] += 1
            keys_counter: Counter[tuple[str, str, str, str]] = by_type[dep_type]["keys"]
            keys_counter[(dep_type, _variant, _inp, key)] += 1
        dup_rows: list[list[str]] = []
        for dep_type in sorted(by_type, key=lambda x: -by_type[x]["total"]):
            info = by_type[dep_type]
            total_t = int(info["total"])
            keys_counter = info["keys"]
            unique_ct = len(keys_counter)
            dup_ct = total_t - unique_ct
            dup_pct = dup_ct / total_t * 100 if total_t else 0.0
            dup_rows.append(
                [dep_type, fmt_int(total_t), fmt_int(unique_ct), fmt_int(dup_ct), f"{dup_pct:.1f}%"]
            )
        console.print(simple_table(["Dep type", "Total", "Unique", "Dup", "Dup%"], dup_rows))

        if len(data.passes) > 1:
            subsection(console, f"{label}: recordings per pass")
            for p in data.passes:
                n = len(p.recordings)
                if n:
                    type_counts: Counter[str] = Counter(r[0] for r in p.recordings)
                    breakdown = ", ".join(f"{k}={v}" for k, v in type_counts.most_common(5))
                    console.print(f"  Pass {p.index}: {fmt_int(n):>8} recordings  ({breakdown})")


def report_perpass_recording_uniqueness(console: Console, logs: LogEntries) -> None:
    has_any = any(len(d.passes) > 1 and any(p.recordings for p in d.passes) for _, d in logs)
    if not has_any:
        return
    section(console, "6. Per-pass recording uniqueness")
    for label, data in logs:
        if len(data.passes) <= 1 or not any(p.recordings for p in data.passes):
            continue
        subsection(console, label)
        seen_keys: set[tuple[str, str, str, str]] = set()
        rows: list[list[str]] = []
        for p in data.passes:
            novel = repeat = 0
            for recording in p.recordings:
                if recording in seen_keys:
                    repeat += 1
                else:
                    novel += 1
                    seen_keys.add(recording)
            total = novel + repeat
            pct_val = novel / total * 100 if total else 0.0
            rows.append(
                [str(p.index), fmt_int(total), fmt_int(novel), fmt_int(repeat), f"{pct_val:.1f}%"]
            )
        console.print(simple_table(["Pass", "Total", "Novel", "Already-seen", "Novel%"], rows))


def report_trace_hit_timing(console: Console, logs: LogEntries) -> None:
    if not any(data.trace_hits for _, data in logs):
        return
    section(console, "7. Trace hit context")
    for label, data in logs:
        if not data.trace_hits:
            continue
        events = data.events
        for p in data.passes:
            if not p.trace_hits:
                continue
            hit_line, hit_attr = p.trace_hits[0]
            before = [e for e in events if p.start_line <= e.lineno < hit_line]
            after = [e for e in events if hit_line <= e.lineno <= p.end_line]
            inst_b = sum(1 for e in before if e.kind == "instantiated")
            inst_a = sum(1 for e in after if e.kind == "instantiated")
            d_b = sum(1 for e in before if e.kind == "daemon_op")
            d_a = sum(1 for e in after if e.kind == "daemon_op")
            pass_span = p.end_line - p.start_line
            hit_pct = (hit_line - p.start_line) / pass_span * 100 if pass_span else 0.0
            console.print(
                f"\n  [{label}] Pass {p.index}: trace verify hit for '{hit_attr}' "
                f"at line {hit_line:,} ({hit_pct:.1f}% through pass)"
            )
            console.print(
                simple_table(
                    ["Metric", "Before hit", "After hit", "Total"],
                    [
                        [
                            "Instantiations",
                            fmt_int(inst_b),
                            fmt_int(inst_a),
                            fmt_int(inst_b + inst_a),
                        ],
                        ["Daemon ops", fmt_int(d_b), fmt_int(d_a), fmt_int(d_b + d_a)],
                    ],
                )
            )
            if len(p.trace_hits) > 1:
                subsection(console, "All trace hits:")
                for ln, attr in p.trace_hits:
                    console.print(f"    line {ln:>8,}: '{attr}'")


def report_daemon_ops(console: Console, logs: LogEntries) -> None:
    section(console, "8. Daemon worker operations")
    labels = [lbl for lbl, _ in logs]
    all_ops_set: set[int] = set()
    for _, d in logs:
        all_ops_set.update(d.daemon_ops.keys())
    all_ops = sorted(all_ops_set)

    op_summaries = [sorted(d.daemon_ops.items()) for _, d in logs]
    identical = len(op_summaries) > 1 and all(s == op_summaries[0] for s in op_summaries[1:])
    if identical:
        console.print("\n[dim]Identical across all runs[/dim]")
        labels = labels[:1]
        logs = logs[:1]

    headers = ["Op", "Name", *labels]
    rows = [
        [
            str(op),
            DAEMON_OP_NAMES.get(op, f"Unknown({op})"),
            *(fmt_int(d.daemon_ops.get(op, 0)) for _, d in logs),
        ]
        for op in all_ops
    ]
    console.print(simple_table(headers, rows))


def report_event_timeline(console: Console, logs: LogEntries, nixpkgs_dir: str | None) -> None:
    section(console, "9. Event timeline")
    for label, data in logs:
        subsection(console, f"{label}: key events in chronological order")
        milestones: list[tuple[int, str, str]] = []
        for e in data.events:
            match e.kind:
                case "root_value":
                    milestones.append((e.lineno, "ROOT_VALUE", "getting root value via rootLoader"))
                case "stat_hash_store":
                    milestones.append((e.lineno, "STAT_HASH", f"loading {e.data['count']} entries"))
                case "copy_start" if nixpkgs_dir and e.data["path"] == nixpkgs_dir:
                    milestones.append((e.lineno, "NIXPKGS_COPY", "copying nixpkgs to store"))
                case "trace_hit":
                    milestones.append((e.lineno, "TRACE_HIT", f"verify hit for '{e.data['attr']}'"))
                case _:
                    pass

        for p in data.passes:
            if p.instantiated:
                first = p.instantiated[0]
                last = p.instantiated[-1]
                milestones.append((p.start_line, f"PASS_{p.index}_START", f"first drv: {first[0]}"))
                pass_inst = [
                    e
                    for e in data.events
                    if e.kind == "instantiated" and p.start_line <= e.lineno <= p.end_line
                ]
                if pass_inst:
                    milestones.append(
                        (
                            pass_inst[-1].lineno,
                            f"PASS_{p.index}_END",
                            f"last drv: {last[0]} ({fmt_int(len(p.instantiated))} total)",
                        )
                    )
        milestones.sort(key=lambda x: x[0])
        console.print(
            simple_table(
                ["Line", "Event", "Detail"],
                [[fmt_int(ln), kind, desc] for ln, kind, desc in milestones],
            )
        )


def report_eval_trace_kv(console: Console, logs: LogEntries) -> None:
    """Structured eval-trace/* debug-line summaries (plan E1-E3)."""
    if not any(d.eval_trace_events for _, d in logs):
        return
    section(console, "10. eval-trace/* structured events")
    for label, data in logs:
        if not data.eval_trace_events:
            continue
        subsection(console, label)
        console.print(
            simple_table(
                ["Subsystem", "Events"],
                [
                    [sub, fmt_int(len(events))]
                    for sub, events in sorted(data.eval_trace_events.items())
                ],
            )
        )

        recovery = data.eval_trace_events.get("recovery", [])
        if not recovery:
            continue
        for field, title in (
            ("reason", f"{label}: SV abort reasons"),
            ("kind", f"{label}: SV abort CQK kinds"),
        ):
            counts: Counter[str] = Counter(kv[field] for kv in recovery if field in kv)
            if not counts:
                continue
            subsection(console, title)
            console.print(
                simple_table(
                    [field, "Count"], [[name, fmt_int(n)] for name, n in counts.most_common()]
                )
            )


# -- E3 / E4 / E5 / E6: structured recovery-line slicing --------------------


def _normalise_sv_abort_key(key: str) -> str:
    """Collapse the `<path>@<format><suffix>` form so small path variants
    don't drown out the "which files dominate" signal.

    The `@<format>` tail is the shape suffix Nix attaches (e.g.
    `@jsonValue`, `@keys`).  Keep it attached so identical files with
    different shapes don't collapse into one.
    """
    return key


def _distribution(vs: list[float]) -> dict[str, float]:
    """min/p25/p50/p75/p95/max/mean for a list of numbers."""
    if not vs:
        return {}
    s = sorted(vs)
    n = len(s)

    def q(p: float) -> float:
        if n == 1:
            return s[0]
        pos = p * (n - 1)
        lo = math.floor(pos)
        hi = math.ceil(pos)
        if lo == hi:
            return s[lo]
        frac = pos - lo
        return s[lo] * (1.0 - frac) + s[hi] * frac

    return {
        "n": float(n),
        "min": s[0],
        "p25": q(0.25),
        "p50": q(0.5),
        "p75": q(0.75),
        "p95": q(0.95),
        "max": s[-1],
        "mean": sum(s) / n,
    }


def report_top_failing_keys(console: Console, logs: LogEntries, top_n: int = 20) -> None:
    """E3: top attrs/files producing `reason=resolveFailed` SV aborts."""
    if not any(d.sv_aborts for _, d in logs):
        return
    section(console, "11. Top failing keys (E3)")
    for label, data in logs:
        if not data.sv_aborts:
            continue
        subsection(console, label)
        resolve_failed = [a for a in data.sv_aborts if a.reason == "resolveFailed"]
        key_counts: Counter[str] = Counter(_normalise_sv_abort_key(a.key) for a in resolve_failed)
        console.print(
            f"  {fmt_int(len(resolve_failed))} resolveFailed aborts "
            f"({fmt_int(len(key_counts))} unique keys)"
        )
        if not key_counts:
            continue
        console.print(
            simple_table(
                ["key", "count", "share"],
                [
                    [key, fmt_int(n), f"{n / len(resolve_failed) * 100:.1f}%"]
                    for key, n in key_counts.most_common(top_n)
                ],
                title=f"top-{top_n} failing keys",
            )
        )


def report_hash_mismatch_distribution(console: Console, logs: LogEntries) -> None:
    """E4: deps_resolved distribution for SV hash-mismatch aborts."""
    if not any(d.sv_hash_mismatches for _, d in logs):
        return
    section(console, "12. SV hash-mismatch dep-count distribution (E4)")
    rows: list[list[str]] = []
    for label, data in logs:
        if not data.sv_hash_mismatches:
            continue
        dist = _distribution([float(m.deps_resolved) for m in data.sv_hash_mismatches])
        rows.append(
            [
                label,
                fmt_int(int(dist["n"])),
                fmt_int(int(dist["min"])),
                fmt_int(int(dist["p25"])),
                fmt_int(int(dist["p50"])),
                fmt_int(int(dist["p75"])),
                fmt_int(int(dist["p95"])),
                fmt_int(int(dist["max"])),
                f"{dist['mean']:.1f}",
            ]
        )
    console.print(
        simple_table(["label", "n", "min", "p25", "p50", "p75", "p95", "max", "mean"], rows)
    )


def report_recompute_size(console: Console, logs: LogEntries) -> None:
    """E5: distribution of N/M on `recomputed N/M dep hashes` lines."""
    if not any(d.recompute_samples for _, d in logs):
        return
    section(console, "13. DirectHash recompute size (E5)")
    for label, data in logs:
        if not data.recompute_samples:
            continue
        subsection(console, label)
        full = sum(1 for s in data.recompute_samples if s.is_full)
        partial = sum(1 for s in data.recompute_samples if not s.is_full)
        total = len(data.recompute_samples)
        m_vals = [float(s.m) for s in data.recompute_samples]
        n_vals = [float(s.n) for s in data.recompute_samples]
        dist_m = _distribution(m_vals)
        dist_n = _distribution(n_vals)
        rows: list[list[str]] = [
            [
                "full (n==m)",
                fmt_int(full),
                f"{full / total * 100:.1f}%" if total else "N/A",
                "—",
                "—",
                "—",
            ],
            [
                "partial (n<m)",
                fmt_int(partial),
                f"{partial / total * 100:.1f}%" if total else "N/A",
                "—",
                "—",
                "—",
            ],
            [
                "N recomputed",
                "—",
                "—",
                fmt_int(int(dist_n["p50"])),
                fmt_int(int(dist_n["max"])),
                f"{dist_n['mean']:.1f}",
            ],
            [
                "M recorded",
                "—",
                "—",
                fmt_int(int(dist_m["p50"])),
                fmt_int(int(dist_m["max"])),
                f"{dist_m['mean']:.1f}",
            ],
        ]
        console.print(
            simple_table(
                ["metric", "count", "share", "p50", "max", "mean"],
                rows,
                title=f"{label}: recompute sizes (total {total} samples)",
            )
        )


def report_recovery_crosscheck(console: Console, logs: LogEntries) -> None:
    """E6: log-grep recovery success counts vs stats counters."""
    if not any(
        d.recovery_successes.git_identity
        or d.recovery_successes.direct_hash
        or d.recovery_successes.struct_variant
        for _, d in logs
    ):
        return
    section(console, "14. Recovery success cross-check (E6)")
    for label, data in logs:
        successes = data.recovery_successes
        if not (successes.git_identity or successes.direct_hash or successes.struct_variant):
            continue
        subsection(console, label)
        stats = data.stats_json if data.stats_json is not None else {}
        counter_git = int(_lookup_path(stats, ("evalTrace", "recovery", "gitIdentityHits")) or 0)
        counter_dh = int(_lookup_path(stats, ("evalTrace", "recovery", "directHash", "hits")) or 0)
        counter_sv = int(
            _lookup_path(stats, ("evalTrace", "recovery", "structVariant", "hits")) or 0
        )
        console.print(
            simple_table(
                ["strategy", "log count", "stats counter", "agree?"],
                [
                    [
                        "GitIdentity",
                        fmt_int(successes.git_identity),
                        fmt_int(counter_git),
                        "✓" if successes.git_identity == counter_git else "✗",
                    ],
                    [
                        "DirectHash",
                        fmt_int(successes.direct_hash),
                        fmt_int(counter_dh),
                        "✓" if successes.direct_hash == counter_dh else "✗",
                    ],
                    [
                        "StructVariant",
                        fmt_int(successes.struct_variant),
                        fmt_int(counter_sv),
                        "✓" if successes.struct_variant == counter_sv else "✗",
                    ],
                ],
            )
        )


def _resolve_from_commit(
    nix_path: Path, commit: str, runs: list[str] | None
) -> tuple[list[str], list[str], dict[str, dict[str, Any]]]:
    data_root = existing_results_root(nix_path)
    if runs:
        run_names = [r.strip() for r in runs]
    else:
        discovered = discover_runs(data_root)
        run_names = [r.name for r in discovered if (r.path / commit / "debug.log").exists()]
    if not run_names:
        raise SystemExit(f"no runs found for commit {commit} under {data_root}")

    log_files: list[str] = []
    labels: list[str] = []
    stats_overrides: dict[str, dict[str, Any]] = {}
    for name in run_names:
        run_dir = data_root / Path(name) / commit
        debug_log = run_dir / "debug.log"
        if not debug_log.exists():
            raise SystemExit(f"debug.log not found: {debug_log}")
        log_files.append(str(debug_log))
        labels.append(name)
        stats_file = run_dir / "stats.json"
        if stats_file.exists():
            payload: Any = json.loads(stats_file.read_text())
            if isinstance(payload, dict):
                stats_overrides[name] = payload  # pyright: ignore[reportUnknownArgumentType, reportUnknownVariableType]
    return log_files, labels, stats_overrides


def _collect_inputs(
    *,
    log_files: list[Path],
    nix: Path,
    commit: str | None,
    runs_spec: str | None,
    labels_spec: str | None,
) -> tuple[list[str], list[str], dict[str, dict[str, Any]]]:
    if commit:
        runs = [r.strip() for r in runs_spec.split(",")] if runs_spec else None
        return _resolve_from_commit(nix, commit, runs)
    if not log_files:
        print("error: LOG files are required unless --commit is used", file=sys.stderr)
        raise SystemExit(2)
    for path in log_files:
        if not path.is_file():
            raise SystemExit(f"log file not found: {path}")
    if labels_spec:
        labels = [lbl.strip() for lbl in labels_spec.split(",")]
        if len(labels) != len(log_files):
            raise SystemExit(f"{len(labels)} labels provided for {len(log_files)} log files")
    else:
        labels = [p.stem for p in log_files]
    return [str(p) for p in log_files], labels, {}


def register(app: App) -> None:
    @app.command
    def logs(
        log_files: list[Path] | None = None,
        *,
        nix: Path = DEFAULT_NIX,
        commit: str | None = None,
        runs: str | None = None,
        nixpkgs: str | None = None,
        labels: str | None = None,
        output: Path | None = None,
    ) -> int:
        """Analyse one or more `nix --debug` logs.

        Parameters
        ----------
        log_files: One or more nix --debug log files.  Either this or
            --commit must be given.
        nix: Nix checkout containing eval-trace-bench-results.
        commit: Commit hash to resolve debug.log/stats.json from run dirs.
        runs: Comma-separated run names (default: auto-discover runs holding this commit).
        nixpkgs: nixpkgs directory (used for pass detection and path shortening).
        labels: Comma-separated labels for log files (default: file stems).
        output: Save report (.html for rich markup, else plain text).
        """
        resolved_files, label_list, stats_overrides = _collect_inputs(
            log_files=log_files or [],
            nix=nix,
            commit=commit,
            runs_spec=runs,
            labels_spec=labels,
        )

        effective_nixpkgs = nixpkgs if nixpkgs is not None else str(DEFAULT_NIXPKGS)
        nixpkgs_dir = effective_nixpkgs.rstrip("/") if effective_nixpkgs else None

        console = Console(record=True)
        console.print("Parsing logs...")
        log_entries: LogEntries = []
        for path, label in zip(resolved_files, label_list, strict=True):
            events = parse_log_events(path)
            if events:
                console.print(f"  {label}: {len(events):,} events from {events[-1].lineno:,} lines")
            else:
                console.print(f"  {label}: [dim](empty)[/dim]")
            summary = aggregate_events(events, nixpkgs_dir)
            if label in stats_overrides and summary.stats_json is None:
                summary.stats_json = stats_overrides[label]
            log_entries.append((label, summary))

        report_overview(console, log_entries)
        report_stats_json(console, log_entries)
        report_eval_trace_summary(console, log_entries)
        report_passes(console, log_entries, nixpkgs_dir)
        report_recording_deps(console, log_entries)
        report_perpass_recording_uniqueness(console, log_entries)
        report_trace_hit_timing(console, log_entries)
        report_daemon_ops(console, log_entries)
        report_event_timeline(console, log_entries, nixpkgs_dir)
        report_eval_trace_kv(console, log_entries)
        report_top_failing_keys(console, log_entries)
        report_hash_mismatch_distribution(console, log_entries)
        report_recompute_size(console, log_entries)
        report_recovery_crosscheck(console, log_entries)

        if output is not None:
            export_console(console, output)
        return 0
