"""`pairwise` subcommand — same-commit A/B delta (G1-G2).

For each commit present in both `--baseline` and `--target` runs,
compute wall / CPU deltas and per-component SV / recovery / verify /
record / loadTrace sub-timer deltas.  Summarise as total, mean, and
distribution.
"""

from __future__ import annotations

import statistics
import sys
from collections import Counter
from collections.abc import Callable
from pathlib import Path

from cyclopts import App
from rich.console import Console
from rich.text import Text

from ..classify import OutcomeClass, classify_eval_trace
from ..cliutil import DEFAULT_NIX, DEFAULT_NIXPKGS, DatasetArgs, report_record_issues
from ..dataframe import Record
from ..models import EvalTraceStats
from ..render import (
    delta_text,
    export_console,
    fmt_int,
    pct,
    ratio_str,
    section,
    short_commit,
    simple_table,
)


def _direct_hash_total_us(et: EvalTraceStats) -> int:
    return et.dep_hash.recovery_recompute_us + et.recovery.direct_hash.time_us


def _recovery_unattributed_us(et: EvalTraceStats) -> int:
    accounted = (
        et.recovery.git_identity.time_us
        + _direct_hash_total_us(et)
        + et.recovery.lookups.scan_history.time_us
        + et.recovery.struct_variant.time_us
    )
    return et.recovery.time_us - accounted


# (label, accessor) — deltas in microseconds on the eval-trace subtree.
DELTA_KEYS: list[tuple[str, Callable[[EvalTraceStats], int]]] = [
    ("sv.depResolveUs", lambda et: et.dep_hash.struct_variant_dep_resolve_us),
    ("sv.loadKeySetUs", lambda et: et.dep_hash.struct_variant_load_key_set_us),
    ("sv.hashUs", lambda et: et.dep_hash.struct_variant_hash_us),
    ("recovery.timeUs", lambda et: et.recovery.time_us),
    ("recovery.directHash.recomputeUs", lambda et: et.dep_hash.recovery_recompute_us),
    ("recovery.directHash.stageUs", lambda et: et.recovery.direct_hash.time_us),
    ("recovery.directHash.totalUs", _direct_hash_total_us),
    ("recovery.lookup.directHashUs", lambda et: et.recovery.lookups.direct_hash.time_us),
    ("recovery.gitIdentity.timeUs", lambda et: et.recovery.git_identity.time_us),
    ("recovery.lookup.gitIdentityUs", lambda et: et.recovery.lookups.git_identity.time_us),
    ("recovery.lookup.scanHistoryUs", lambda et: et.recovery.lookups.scan_history.time_us),
    ("recovery.implicitGuard.timeUs", lambda et: et.recovery.acceptance.implicit_guard_time_us),
    ("recovery.structVariant.stageUs", lambda et: et.recovery.struct_variant.time_us),
    ("recovery.unattributedUs~", _recovery_unattributed_us),
    ("verify.timeUs", lambda et: et.verify.time_us),
    ("verifyTrace.timeUs", lambda et: et.verify_trace.time_us),
    ("record.timeUs", lambda et: et.record.time_us),
    ("record.hashUs", lambda et: et.record.hash_us),
    ("record.serializeKeysUs", lambda et: et.record.serialize_keys_us),
    ("record.serializeValuesUs", lambda et: et.record.serialize_values_us),
    ("record.flushUs", lambda et: et.record.flush_us),
    ("loadTrace.timeUs", lambda et: et.load_trace.time_us),
    ("loadKeySet.timeUs", lambda et: et.load_key_set.time_us),
]


def _render_g1(
    console: Console,
    pairs: list[tuple[str, Record, Record]],
    baseline: str,
    target: str,
) -> None:
    """G1 — same-commit wall/CPU delta."""
    section(console, f"Per-commit delta: {target} vs {baseline} (G1)")
    wall_deltas: list[float] = []
    cpu_deltas: list[float] = []
    rows: list[list[str | Text]] = []
    for commit, base, tgt in pairs:
        row: list[str | Text] = [short_commit(commit)]
        wall_b, wall_t = base.wall, tgt.wall
        row.append(f"{wall_b:.2f}" if wall_b is not None else "—")
        row.append(f"{wall_t:.2f}" if wall_t is not None else "—")
        if wall_b is not None and wall_t is not None:
            row.append(delta_text(wall_t - wall_b))
            row.append(ratio_str(wall_t, wall_b))
            wall_deltas.append(wall_t - wall_b)
        else:
            row.extend(("—", "—"))
        if base.cpu is not None and tgt.cpu is not None:
            row.append(delta_text(tgt.cpu - base.cpu))
            cpu_deltas.append(tgt.cpu - base.cpu)
        else:
            row.append("—")
        rows.append(row)

    if wall_deltas:
        for label, reduce in (("total", sum), ("mean", statistics.mean)):
            rows.append(
                [
                    Text(label, style="bold"),
                    "",
                    "",
                    Text(f"{reduce(wall_deltas):+.2f}s", style="bold"),
                    "",
                    Text(f"{reduce(cpu_deltas):+.2f}s", style="bold") if cpu_deltas else "",
                ]
            )

    console.print(
        simple_table(
            [
                "Commit",
                f"wall {baseline} (s)",
                f"wall {target} (s)",
                "wall Δ",
                "wall ratio",
                "cpu Δ (s)",
            ],
            rows,
        )
    )


def _percentile(values: list[float], quantile: float) -> float:
    ordered = sorted(values)
    if not ordered:
        raise ValueError("percentile requires at least one value")
    if len(ordered) == 1:
        return ordered[0]
    pos = (len(ordered) - 1) * quantile
    lower = int(pos)
    upper = min(lower + 1, len(ordered) - 1)
    weight = pos - lower
    return ordered[lower] * (1 - weight) + ordered[upper] * weight


def _ratio_text(value: float) -> Text:
    return Text(f"{value:.3f}x", style="red" if value > 1 else "green" if value < 1 else "")


def _delta_seconds_text(value: float) -> Text:
    return Text(f"{value:+.2f}s", style="red" if value > 0 else "green" if value < 0 else "")


def _render_g4(
    console: Console,
    pairs: list[tuple[str, Record, Record]],
    baseline: str,
    target: str,
) -> None:
    """G4 — paired wall-time distribution robust to noisy machines."""
    section(console, f"Paired wall-time distribution: {target} vs {baseline} (G4)")
    ratios: list[float] = []
    deltas: list[float] = []
    baseline_total = 0.0
    target_total = 0.0
    for _, base, tgt in pairs:
        if base.wall is None or tgt.wall is None or base.wall <= 0:
            continue
        baseline_total += base.wall
        target_total += tgt.wall
        ratios.append(tgt.wall / base.wall)
        deltas.append(tgt.wall - base.wall)

    if not ratios:
        console.print("  [dim](no commits with wall times on both sides)[/dim]")
        return

    def count_where(pred: Callable[[float], bool]) -> int:
        return sum(1 for ratio in ratios if pred(ratio))

    rows: list[list[str | Text]] = [
        ["matched commits", fmt_int(len(ratios))],
        [f"{baseline} total wall", f"{baseline_total:.2f}s"],
        [f"{target} total wall", f"{target_total:.2f}s"],
        ["total wall delta", _delta_seconds_text(target_total - baseline_total)],
        ["mean ratio", _ratio_text(statistics.mean(ratios))],
        ["p25 ratio", _ratio_text(_percentile(ratios, 0.25))],
        ["median ratio", _ratio_text(statistics.median(ratios))],
        ["p75 ratio", _ratio_text(_percentile(ratios, 0.75))],
        ["median delta", _delta_seconds_text(statistics.median(deltas))],
    ]
    console.print(simple_table(["Metric", "Value"], rows))

    band_rows: list[list[str | Text]] = []
    bands: list[tuple[str, Callable[[float], bool], str]] = [
        (f"{target} faster by >5%", lambda r: r < 0.95, "green"),
        (f"{target} faster by 1-5%", lambda r: 0.95 <= r < 0.99, "green"),
        ("within +/-1%", lambda r: 0.99 <= r <= 1.01, ""),
        (f"{target} slower by 1-5%", lambda r: 1.01 < r <= 1.05, "red"),
        (f"{target} slower by >5%", lambda r: r > 1.05, "red"),
    ]
    for label, pred, style in bands:
        count = count_where(pred)
        count_text = Text(fmt_int(count), style=style)
        band_rows.append([label, count_text, pct(count, len(ratios))])
    console.print(simple_table(["Band", "Commits", "Share"], band_rows))


def _render_g3(
    console: Console,
    pairs: list[tuple[str, Record, Record]],
    baseline: str,
    target: str,
) -> None:
    """G3 — recovery-outcome delta matrix.

    For every commit that has eval-trace stats on both sides, bucket
    `(baseline_class, target_class)` — a nonzero off-diagonal cell
    means the change moved commits between classes (hit↔miss), which
    can be neutral-wall-but-not-neutral-correctness (path-dependent
    cache state).
    """
    section(console, f"Recovery-outcome delta: {target} vs {baseline} (G3)")
    classes = list(OutcomeClass)
    matrix: Counter[tuple[OutcomeClass, OutcomeClass]] = Counter()
    base_totals: Counter[OutcomeClass] = Counter()
    tgt_totals: Counter[OutcomeClass] = Counter()
    for _, base, tgt in pairs:
        if base.et is None or tgt.et is None:
            continue
        bc = classify_eval_trace(base.et)
        tc = classify_eval_trace(tgt.et)
        matrix[(bc, tc)] += 1
        base_totals[bc] += 1
        tgt_totals[tc] += 1
    if not matrix:
        console.print("  [dim](no commits with stats on both sides)[/dim]")
        return

    # Keep only classes that appear somewhere.
    active = [c for c in classes if base_totals[c] or tgt_totals[c]]
    headers = [f"{baseline} \\ {target}", *[c.value for c in active], "total", "unchanged"]
    rows: list[list[str | Text]] = []
    off_diag_total = 0
    for bc in active:
        row: list[str | Text] = [bc.value]
        for tc in active:
            n = matrix.get((bc, tc), 0)
            cell: str | Text = str(n) if n else "—"
            if n and bc != tc:
                cell = Text(str(n), style="red")
                off_diag_total += n
            row.append(cell)
        row.append(fmt_int(base_totals[bc]))
        row.append(fmt_int(matrix.get((bc, bc), 0)))
        rows.append(row)
    # Target-totals footer row.
    footer: list[str | Text] = [Text(f"total ({target})", style="bold")]
    for tc in active:
        footer.append(Text(fmt_int(tgt_totals[tc]), style="bold"))
    footer.append(Text(fmt_int(sum(base_totals.values())), style="bold"))
    footer.append("")
    rows.append(footer)
    console.print(simple_table(headers, rows))
    console.print(
        f"  [bold]{off_diag_total}[/bold] commits shifted class between {baseline} and {target}."
    )


def _render_g2(
    console: Console,
    pairs: list[tuple[str, Record, Record]],
    baseline: str,
    target: str,
) -> None:
    """G2 — per-component deltas (mean + sum)."""
    section(console, f"Per-component delta (mean Δ ms, sum Δ s): {target} vs {baseline} (G2)")
    rows: list[list[str | Text]] = []
    for label, accessor in DELTA_KEYS:
        base_vals: list[float] = []
        tgt_vals: list[float] = []
        for _, base, tgt in pairs:
            if base.et is None or tgt.et is None:
                continue
            base_vals.append(float(accessor(base.et)))
            tgt_vals.append(float(accessor(tgt.et)))
        if not base_vals:
            continue
        bm = statistics.mean(base_vals) / 1000
        tm = statistics.mean(tgt_vals) / 1000
        sum_delta_s = (sum(tgt_vals) - sum(base_vals)) / 1_000_000
        rows.append(
            [
                label,
                f"{bm:.1f}",
                f"{tm:.1f}",
                Text(
                    f"{tm - bm:+.1f}",
                    style="red" if tm > bm else "green" if tm < bm else "",
                ),
                Text(
                    f"{sum_delta_s:+.2f}",
                    style="red" if sum_delta_s > 0 else "green" if sum_delta_s < 0 else "",
                ),
            ]
        )
    console.print(
        simple_table(
            [
                "Component",
                f"{baseline} mean ms",
                f"{target} mean ms",
                "Δ mean ms",
                "Δ sum s",
            ],
            rows,
        )
    )


def register(app: App) -> None:
    @app.command
    def pairwise(
        *,
        baseline: str,
        target: str,
        nix: Path = DEFAULT_NIX,
        nixpkgs: Path = DEFAULT_NIXPKGS,
        nixpkgs_branch: str = "master",
        nixpkgs_base: str | None = None,
        runs: str | None = None,
        output: Path | None = None,
    ) -> int:
        """Same-commit A/B deltas (G1-G2).

        Parameters
        ----------
        baseline: Baseline run name.
        target: Target run name.
        nix: Nix checkout containing eval-trace-bench-results.
        nixpkgs: Nixpkgs checkout used to order commits by git history.
        nixpkgs_branch: Branch to walk for commit ordering.
        nixpkgs_base: Optional nixpkgs commit/ref to use as the newest commit.
        runs: Comma-separated run names (default: auto-discover).
        output: Save report (.html for rich markup, else plain text).
        """
        ds_args = DatasetArgs.from_strings(
            nix=nix,
            nixpkgs=nixpkgs,
            nixpkgs_branch=nixpkgs_branch,
            nixpkgs_base=nixpkgs_base,
            runs=runs,
            output=output,
        )
        ds = ds_args.load()
        if baseline not in ds.run_names or target not in ds.run_names:
            print(f"missing runs. discovered: {ds.run_names}", file=sys.stderr)
            return 1
        pair_runs = {baseline, target}
        relevant = [rec for rec in ds.incomplete_records if rec.run_name in pair_runs]
        if report_record_issues(relevant, sys.stderr):
            return 1

        pairs: list[tuple[str, Record, Record]] = []
        for commit in ds.commits:
            recs = ds.by_commit_map.get(commit, {})
            base = recs.get(baseline)
            tgt = recs.get(target)
            if base is None or tgt is None or base.data is None or tgt.data is None:
                continue
            pairs.append((commit, base, tgt))

        console = Console(record=True)
        _render_g1(console, pairs, baseline, target)
        _render_g4(console, pairs, baseline, target)
        _render_g2(console, pairs, baseline, target)
        _render_g3(console, pairs, baseline, target)
        if ds_args.output is not None:
            export_console(console, ds_args.output)
        return 0
