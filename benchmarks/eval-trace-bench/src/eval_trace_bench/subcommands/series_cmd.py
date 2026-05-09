"""`series` subcommand — per-run time-series over processing order (A1-A5)."""

from __future__ import annotations

import statistics
import sys
from collections import defaultdict
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from pathlib import Path

from cyclopts import App
from rich.console import Console
from rich.panel import Panel

from ..classify import OutcomeClass, classify_eval_trace
from ..cliutil import DEFAULT_NIX, DEFAULT_NIXPKGS, DatasetArgs, report_dataset_issues
from ..dataframe import Record
from ..models import EvalTraceStats
from ..render import export_console, fmt_int, section, simple_table


@dataclass(frozen=True)
class Bucket:
    start: int
    end: int  # exclusive
    records: list[Record]


def _bucketize(records: list[Record], window: int) -> list[Bucket]:
    records = sorted(records, key=lambda r: r.proc_idx)
    if not records:
        return []
    max_idx = records[-1].proc_idx
    buckets: list[Bucket] = []
    for start in range(0, max_idx + 1, window):
        end = start + window
        bucket = [r for r in records if start <= r.proc_idx < end]
        if bucket:
            buckets.append(Bucket(start, end, bucket))
    return buckets


def _pearson(xs: Sequence[float], ys: Sequence[float]) -> float:
    if len(xs) < 2 or len(xs) != len(ys):
        return 0.0
    mx, my = statistics.mean(xs), statistics.mean(ys)
    num = sum((x - mx) * (y - my) for x, y in zip(xs, ys, strict=True))
    dx = sum((x - mx) ** 2 for x in xs) ** 0.5
    dy = sum((y - my) ** 2 for y in ys) ** 0.5
    return num / (dx * dy) if dx and dy else 0.0


def _linear_fit(xs: Sequence[float], ys: Sequence[float]) -> tuple[float, float]:
    if len(xs) < 2:
        return (0.0, 0.0)
    mx, my = statistics.mean(xs), statistics.mean(ys)
    denom = sum((x - mx) ** 2 for x in xs)
    if denom == 0:
        return (0.0, my)
    slope = sum((x - mx) * (y - my) for x, y in zip(xs, ys, strict=True)) / denom
    return (slope, my - slope * mx)


def _wall_trend(console: Console, records: list[Record], run_name: str) -> None:
    """A1: slope / intercept / Pearson r of cold_wall vs proc_idx."""
    xs: list[float] = []
    ys: list[float] = []
    for r in records:
        if r.wall is not None:
            xs.append(float(r.proc_idx))
            ys.append(float(r.wall))
    if len(xs) < 2:
        return
    slope, intercept = _linear_fit(xs, ys)
    r_val = _pearson(xs, ys)
    console.print(
        Panel(
            f"[bold]{run_name}: wall trend over processing order (A1)[/bold]\n"
            f"  n={len(xs)}  slope={slope:+.4f} s/commit  "
            f"intercept={intercept:.2f} s  Pearson r={r_val:+.3f}",
            expand=False,
        )
    )


ColumnExtractor = Callable[[Record], float | None]


def _wall_col(rec: Record) -> float | None:
    return rec.wall


def _et_col(fn: Callable[[EvalTraceStats], float]) -> ColumnExtractor:
    def accessor(rec: Record) -> float | None:
        return float(fn(rec.et)) if rec.et is not None else None

    return accessor


def _bucketed_table(
    console: Console,
    title: str,
    buckets: list[Bucket],
    cols: list[tuple[str, ColumnExtractor]],
) -> None:
    rows: list[list[str]] = []
    for b in buckets:
        row: list[str] = [f"{b.start}-{b.end - 1}", str(len(b.records))]
        for _, extractor in cols:
            vs = [v for v in (extractor(r) for r in b.records) if v is not None]
            row.append(f"{statistics.mean(vs):.1f}" if vs else "—")
        rows.append(row)
    console.print(
        simple_table(
            ["Bucket", "n", *(h for h, _ in cols)],
            rows,
            title=title,
        )
    )


# (header, numerator extractor, denominator extractor, format string) —
# per-bucket ratio column for the C-series view.
RatioColumn = tuple[str, ColumnExtractor, ColumnExtractor, str]


def _bucketed_ratio_table(
    console: Console,
    title: str,
    buckets: list[Bucket],
    cols: list[RatioColumn],
) -> None:
    """Per-bucket sum(num)/sum(den) ratios.  "—" when denominator is zero."""
    rows: list[list[str]] = []
    for b in buckets:
        row: list[str] = [f"{b.start}-{b.end - 1}", str(len(b.records))]
        for _, num_fn, den_fn, fmt in cols:
            num = sum(v for v in (num_fn(r) for r in b.records) if v is not None)
            den = sum(v for v in (den_fn(r) for r in b.records) if v is not None)
            row.append(f"{num / den:{fmt}}" if den else "—")
        rows.append(row)
    console.print(
        simple_table(
            ["Bucket", "n", *(h for h, _, _, _ in cols)],
            rows,
            title=title,
        )
    )


def _render_run(
    console: Console,
    records: list[Record],
    run_name: str,
    window: int,
    split_by_class: bool,
) -> None:
    buckets = _bucketize(records, window)
    if not buckets:
        return
    _wall_trend(console, records, run_name)

    # A2 — SV activity per bucket.
    _bucketed_table(
        console,
        f"{run_name}: per-bucket SV activity (A2)",
        buckets,
        [
            ("mean wall (s)", _wall_col),
            ("mean cand", _et_col(lambda et: et.dep_hash.struct_variant_candidates)),
            (
                "mean dep-resolve",
                _et_col(lambda et: et.dep_hash.struct_variant_deps_resolved),
            ),
            ("hits", _et_col(lambda et: et.hits)),
            ("misses", _et_col(lambda et: et.misses)),
        ],
    )

    # A3 — SV time decomposition per bucket.
    _bucketed_table(
        console,
        f"{run_name}: per-bucket SV time decomposition (A3) [us, mean]",
        buckets,
        [
            ("depResolveUs", _et_col(lambda et: et.dep_hash.struct_variant_dep_resolve_us)),
            ("loadKeySetUs", _et_col(lambda et: et.dep_hash.struct_variant_load_key_set_us)),
            ("hashUs", _et_col(lambda et: et.dep_hash.struct_variant_hash_us)),
        ],
    )

    # C1-C3 — per-bucket per-call costs: ns/resolve, us/load, L1 hit-rate.
    # Flat-across-buckets = per-lookup cost bounded; growing = regression.
    _bucketed_ratio_table(
        console,
        f"{run_name}: per-call costs (C1-C3)",
        buckets,
        [
            (
                "ns/sv-resolve (C1)",
                # depResolveUs is already in microseconds; × 1000 -> ns
                _et_col(lambda et: float(et.dep_hash.struct_variant_dep_resolve_us) * 1000),
                _et_col(lambda et: float(et.dep_hash.struct_variant_deps_resolved)),
                ".0f",
            ),
            (
                "us/loadTrace (C2)",
                _et_col(lambda et: float(et.load_trace.time_us)),
                _et_col(lambda et: float(et.load_trace.count)),
                ".1f",
            ),
            (
                "us/loadKeySet",
                _et_col(lambda et: float(et.load_key_set.time_us)),
                _et_col(lambda et: float(et.load_key_set.count)),
                ".1f",
            ),
            (
                "L1 hit% (C3)",
                _et_col(lambda et: float(et.dep_hash.cache_hits) * 100),
                _et_col(lambda et: float(et.dep_hash.cache_hits + et.dep_hash.cache_misses)),
                ".1f",
            ),
            (
                "keySet hit%",
                _et_col(lambda et: float(et.load_key_set.cache_hits) * 100),
                _et_col(
                    lambda et: float(et.load_key_set.cache_hits + et.load_key_set.cache_misses)
                ),
                ".1f",
            ),
        ],
    )

    # A4 — outlier table (top-10 by wall).
    present = sorted(
        (r for r in records if r.wall is not None),
        key=lambda r: r.wall or 0.0,
        reverse=True,
    )
    topn = present[:10]
    if topn:
        rows = [
            [
                r.commit[:12],
                str(r.proc_idx),
                f"{r.wall:.2f}" if r.wall is not None else "—",
                fmt_int(r.et.dep_hash.struct_variant_candidates) if r.et else "—",
                fmt_int(r.et.dep_hash.struct_variant_deps_resolved) if r.et else "—",
                fmt_int(r.et.recovery.attempts) if r.et else "—",
                fmt_int(r.et.recovery.failures) if r.et else "—",
            ]
            for r in topn
        ]
        console.print(
            simple_table(
                [
                    "Commit",
                    "proc_idx",
                    "wall (s)",
                    "sv.cand",
                    "sv.depsResolved",
                    "recovery.attempts",
                    "recovery.failures",
                ],
                rows,
                title=f"{run_name}: top-10 slowest commits (A4)",
            )
        )

    # A5 — split by outcome class.
    if split_by_class:
        section(console, f"{run_name}: bucketed by outcome class (A5)")
        by_class: dict[OutcomeClass, list[Record]] = defaultdict(list)
        for rec in records:
            if rec.et is None:
                continue
            by_class[classify_eval_trace(rec.et)].append(rec)
        for cls in OutcomeClass:
            bucket = by_class.get(cls, [])
            if not bucket:
                continue
            _bucketed_table(
                console,
                f"{run_name} / {cls.value}",
                _bucketize(bucket, window),
                [("mean wall (s)", _wall_col)],
            )


def register(app: App) -> None:
    @app.command
    def series(
        nix: Path = DEFAULT_NIX,
        nixpkgs: Path = DEFAULT_NIXPKGS,
        nixpkgs_branch: str = "master",
        nixpkgs_base: str | None = None,
        runs: str | None = None,
        output: Path | None = None,
        bucket_window: int = 50,
        split_by_class: bool = False,
    ) -> int:
        """Per-run time-series over processing order (A1-A5).

        Parameters
        ----------
        nix: Nix checkout containing eval-trace-bench-results.
        nixpkgs: Nixpkgs checkout used to order commits by git history.
        nixpkgs_branch: Branch to walk for commit ordering.
        nixpkgs_base: Optional nixpkgs commit/ref to use as the newest commit.
        runs: Comma-separated run names (default: auto-discover).
        output: Save report (.html for rich markup, else plain text).
        bucket_window: Processing-order bucket width.
        split_by_class: Also emit per-outcome-class sub-tables (A5).
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
        if report_dataset_issues(ds, sys.stderr, requested_runs=ds_args.runs):
            return 1
        if not ds.runs:
            print("No run directories found.", file=sys.stderr)
            return 1
        console = Console(record=True)
        for run_name in ds.run_names:
            _render_run(
                console,
                ds.records_for(run_name),
                run_name,
                bucket_window,
                split_by_class,
            )
        if ds_args.output is not None:
            export_console(console, ds_args.output)
        return 0
