"""`classify` subcommand — commit-outcome partitioning (B1-B4).

Each commit is bucketed by recovery outcome into one of four classes
(hit-only, partial-miss, full-miss, no-recovery) and summary metrics
are reported per-class.  Per-run, so a cold run's partition is
distinct from a hot run's partition.
"""

from __future__ import annotations

import statistics
import sys
from collections import defaultdict
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

from cyclopts import App
from rich.console import Console

from ..classify import OutcomeClass, classify_eval_trace
from ..cliutil import DEFAULT_NIX, DEFAULT_NIXPKGS, DatasetArgs, report_dataset_issues
from ..dataframe import Record
from ..models import EvalTraceStats
from ..render import export_console, fmt_int, pct, simple_table


def _outcome(rec: Record) -> OutcomeClass:
    return classify_eval_trace(rec.et) if rec.et is not None else OutcomeClass.MISSING


def _mean(vs: list[float]) -> float:
    return statistics.mean(vs) if vs else 0.0


def _mean_ms(records: list[Record], accessor: Callable[[EvalTraceStats], int]) -> str:
    vals = [accessor(r.et) for r in records if r.et is not None]
    vals = [v for v in vals if v > 0]
    return f"{statistics.mean(vals) / 1000:.1f}" if vals else "—"


@dataclass(frozen=True)
class MissCostColumn:
    header: str
    accessor: Callable[[EvalTraceStats], int]


MISS_COST_COLS: list[MissCostColumn] = [
    MissCostColumn("verify ms", lambda et: et.verify.time_us),
    MissCostColumn("verifyTrace ms", lambda et: et.verify_trace.time_us),
    MissCostColumn("recovery ms", lambda et: et.recovery.time_us),
    MissCostColumn("  dh ms", lambda et: et.recovery.direct_hash.time_us),
    MissCostColumn("  sv ms", lambda et: et.recovery.struct_variant.time_us),
    MissCostColumn("record ms", lambda et: et.record.time_us),
    MissCostColumn("loadTrace ms", lambda et: et.load_trace.time_us),
]


def _bucketize(records: list[Record]) -> dict[OutcomeClass, list[Record]]:
    buckets: dict[OutcomeClass, list[Record]] = defaultdict(list)
    for rec in records:
        buckets[_outcome(rec)].append(rec)
    return buckets


def _render_recovery_summary(console: Console, records: list[Record], run_name: str) -> None:
    """B0: per-class hit/miss and recovery totals."""
    buckets = _bucketize(records)
    rows: list[list[str | int]] = []
    total_commits = total_hits = total_misses = total_attempts = total_failures = 0

    for cls in (
        OutcomeClass.HIT_ONLY,
        OutcomeClass.PARTIAL_MISS,
        OutcomeClass.FULL_MISS,
        OutcomeClass.NO_RECOVERY,
    ):
        bucket = buckets.get(cls, [])
        if not bucket:
            continue
        hits = sum(rec.et.hits for rec in bucket if rec.et is not None)
        misses = sum(rec.et.misses for rec in bucket if rec.et is not None)
        attempts = sum(rec.et.recovery.attempts for rec in bucket if rec.et is not None)
        failures = sum(rec.et.recovery.failures for rec in bucket if rec.et is not None)
        total_commits += len(bucket)
        total_hits += hits
        total_misses += misses
        total_attempts += attempts
        total_failures += failures
        rows.append(
            [
                cls.value,
                fmt_int(len(bucket)),
                fmt_int(hits),
                fmt_int(misses),
                pct(hits, hits + misses),
                fmt_int(attempts),
                fmt_int(failures),
                pct(failures, attempts),
            ]
        )

    if not rows:
        return

    rows.append(
        [
            "TOTAL",
            fmt_int(total_commits),
            fmt_int(total_hits),
            fmt_int(total_misses),
            pct(total_hits, total_hits + total_misses),
            fmt_int(total_attempts),
            fmt_int(total_failures),
            pct(total_failures, total_attempts),
        ]
    )
    console.print(
        simple_table(
            [
                "Class",
                "Commits",
                "Hits",
                "Misses",
                "Hit rate",
                "Recovery attempts",
                "Recovery failures",
                "Failure rate",
            ],
            rows,
            title=f"{run_name}: miss/recovery coverage (B0)",
        )
    )


def _render_class_partition(console: Console, records: list[Record], run_name: str) -> None:
    """B1 + B4: per-class summary (count, walls, CPU, miss-cost breakdown)."""
    buckets = _bucketize(records)

    hit_only = buckets.get(OutcomeClass.HIT_ONLY, [])
    wall_hit_only = [rec.wall for rec in hit_only if rec.wall is not None]
    hit_median = statistics.median(wall_hit_only) if wall_hit_only else 0.0

    rows_b1: list[list[str]] = []
    rows_b4: list[list[str]] = []
    for cls in OutcomeClass:
        bucket = buckets.get(cls, [])
        if not bucket:
            continue
        walls = [rec.wall for rec in bucket if rec.wall is not None]
        cpus = [rec.cpu for rec in bucket if rec.cpu is not None]
        wall_mean = _mean(walls)
        rows_b1.append(
            [
                cls.value,
                fmt_int(len(bucket)),
                f"{wall_mean:.2f}" if walls else "N/A",
                f"{_mean(cpus):.2f}" if cpus else "N/A",
                f"{wall_mean - hit_median:+.2f}" if walls and wall_hit_only else "—",
            ]
        )
        rows_b4.append([cls.value, *[_mean_ms(bucket, col.accessor) for col in MISS_COST_COLS]])

    console.print(
        simple_table(
            [
                "Class",
                "Commits",
                "Wall mean (s)",
                "CPU mean (s)",
                "Miss cost (s, wall - median(HIT))",
            ],
            rows_b1,
            title=f"{run_name}: outcome classes (B1)",
        )
    )
    console.print(
        simple_table(
            ["Class", *[col.header for col in MISS_COST_COLS]],
            rows_b4,
            title=f"{run_name}: miss-cost decomposition (B4)",
        )
    )


def _render_failure_count_breakdown(console: Console, records: list[Record], run_name: str) -> None:
    bucket: dict[int, list[Record]] = defaultdict(list)
    for rec in records:
        if rec.et is None:
            continue
        bucket[rec.et.recovery.failures].append(rec)
    if not bucket:
        return

    rows: list[list[str]] = []
    for n in sorted(bucket):
        items = bucket[n]
        walls = [r.wall for r in items if r.wall is not None]
        thunks = [r.stats.nr_thunks for r in items if r.stats is not None]
        rows.append(
            [
                str(n),
                fmt_int(len(items)),
                f"{_mean(walls):.2f}" if walls else "N/A",
                f"{_mean([float(t) for t in thunks]):.0f}" if thunks else "N/A",
            ]
        )
    console.print(
        simple_table(
            ["fails", "commits", "wall mean (s)", "nrThunks mean"],
            rows,
            title=f"{run_name}: per-failure-count breakdown (B2)",
        )
    )


def _render_hit_path(console: Console, records: list[Record], run_name: str) -> None:
    """B3: on hit-only commits, distribution of which path served the hit."""
    hit_only = [r for r in records if _outcome(r) == OutcomeClass.HIT_ONLY]
    if not hit_only:
        return
    primary = dh = sv = git = history = 0
    for rec in hit_only:
        if rec.et is None:
            continue
        r = rec.et.recovery
        if r.attempts == 0 and rec.et.hits > 0:
            primary += 1
        if r.direct_hash.hits > 0:
            dh += 1
        if r.struct_variant.hits > 0:
            sv += 1
        if r.git_identity_hits > 0:
            git += 1
        if r.history_bootstraps > 0:
            history += 1
    total = len(hit_only)
    rows = [
        [name, fmt_int(count), pct(count, total)]
        for name, count in (
            ("primary cache", primary),
            ("DirectHash recovery", dh),
            ("StructVariant recovery", sv),
            ("GitIdentity recovery", git),
            ("history bootstrap", history),
        )
    ]
    console.print(
        simple_table(
            ["Path", "Commits", "Share"],
            rows,
            title=f"{run_name}: hit-path distribution (B3)",
        )
    )


def register(app: App) -> None:
    @app.command
    def classify(
        nix: Path = DEFAULT_NIX,
        nixpkgs: Path = DEFAULT_NIXPKGS,
        nixpkgs_branch: str = "master",
        nixpkgs_base: str | None = None,
        runs: str | None = None,
        output: Path | None = None,
    ) -> int:
        """Per-run commit-outcome partitioning (B1-B4).

        Parameters
        ----------
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
        if report_dataset_issues(ds, sys.stderr, requested_runs=ds_args.runs):
            return 1
        if not ds.runs:
            print("No run directories found.", file=sys.stderr)
            return 1
        console = Console(record=True)
        for run_name in ds.run_names:
            records = ds.records_for(run_name)
            _render_recovery_summary(console, records, run_name)
            _render_class_partition(console, records, run_name)
            _render_failure_count_breakdown(console, records, run_name)
            _render_hit_path(console, records, run_name)
        if ds_args.output is not None:
            export_console(console, ds_args.output)
        return 0
