"""`sv` subcommand — `byDepKeySet` SV telemetry analyses (D1-D7)."""

from __future__ import annotations

import sys
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path

from cyclopts import App
from rich.console import Console

from ..cliutil import DEFAULT_NIX, DEFAULT_NIXPKGS, DatasetArgs, report_dataset_issues
from ..dataframe import Record
from ..models import ByDepKeySetEntry
from ..render import export_console, fmt_int, pct, section, simple_table


@dataclass
class SvAgg:
    """Aggregated telemetry for one depKeySetId across all commits."""

    tried: int = 0
    succeeded: int = 0
    aborted_early: int = 0
    hash_mismatch: int = 0
    avg_deps_sum: float = 0.0
    avg_us_sum: float = 0.0
    commits: int = 0
    both_set_count: int = 0
    earlier_hash_mismatch_count: int = 0
    earlier_hash_mismatch_saved_deps: int = 0
    hash_mismatch_only_count: int = 0
    hash_mismatch_only_saved_deps: int = 0
    estimated_early_exit_saved_us: float = 0.0


def _sv_entries(rec: Record) -> list[ByDepKeySetEntry]:
    return rec.et.struct_variant.by_dep_key_set if rec.et is not None else []


def aggregate(records: Iterable[Record]) -> dict[int, SvAgg]:
    agg: dict[int, SvAgg] = {}
    for rec in records:
        for entry in _sv_entries(rec):
            slot = agg.setdefault(entry.dep_key_set_id, SvAgg())
            slot.tried += entry.tried
            slot.succeeded += entry.succeeded
            slot.aborted_early += entry.aborted_early
            slot.hash_mismatch += entry.hash_mismatch
            slot.avg_deps_sum += entry.avg_deps
            slot.avg_us_sum += entry.avg_us
            slot.commits += 1
            slot.both_set_count += entry.both_set_count
            slot.earlier_hash_mismatch_count += entry.earlier_hash_mismatch_count
            slot.earlier_hash_mismatch_saved_deps += entry.earlier_hash_mismatch_saved_deps
            slot.hash_mismatch_only_count += entry.hash_mismatch_only_count
            slot.hash_mismatch_only_saved_deps += entry.hash_mismatch_only_saved_deps
            saved_deps = (
                entry.earlier_hash_mismatch_saved_deps + entry.hash_mismatch_only_saved_deps
            )
            if entry.avg_deps > 0:
                slot.estimated_early_exit_saved_us += saved_deps * (entry.avg_us / entry.avg_deps)
    return agg


def _render(console: Console, run_name: str, agg: dict[int, SvAgg]) -> None:
    if not agg:
        return
    section(console, f"{run_name}: byDepKeySet aggregates")

    totals_tried = sum(s.tried for s in agg.values())
    totals_succ = sum(s.succeeded for s in agg.values())
    totals_abort = sum(s.aborted_early for s in agg.values())
    totals_mism = sum(s.hash_mismatch for s in agg.values())
    totals_both_set = sum(s.both_set_count for s in agg.values())
    totals_earlier_hash = sum(s.earlier_hash_mismatch_count for s in agg.values())
    totals_earlier_saved_deps = sum(s.earlier_hash_mismatch_saved_deps for s in agg.values())
    totals_mismatch_only = sum(s.hash_mismatch_only_count for s in agg.values())
    totals_mismatch_only_saved_deps = sum(s.hash_mismatch_only_saved_deps for s in agg.values())
    totals_estimated_saved_us = sum(s.estimated_early_exit_saved_us for s in agg.values())

    # D6: abort-outcome global breakdown.
    console.print(
        simple_table(
            ["Outcome", "Count", "Share of tried"],
            [
                ["succeeded", fmt_int(totals_succ), pct(totals_succ, totals_tried)],
                ["abortedEarly", fmt_int(totals_abort), pct(totals_abort, totals_tried)],
                ["hashMismatch", fmt_int(totals_mism), pct(totals_mism, totals_tried)],
                ["tried total", fmt_int(totals_tried), "100.0%"],
            ],
            title="D6: abort-outcome global",
        )
    )

    # D7: early-exit opportunity from mismatch telemetry.
    console.print(
        simple_table(
            ["Signal", "Count / estimate", "Share"],
            [
                [
                    "both hash-mismatch and resolve-fail observed",
                    fmt_int(totals_both_set),
                    pct(totals_both_set, totals_tried),
                ],
                [
                    "hash mismatch earlier than resolve-fail",
                    fmt_int(totals_earlier_hash),
                    pct(totals_earlier_hash, totals_both_set),
                ],
                [
                    "saved deps before resolve-fail",
                    fmt_int(totals_earlier_saved_deps),
                    "",
                ],
                [
                    "hash-mismatch-only candidates",
                    fmt_int(totals_mismatch_only),
                    pct(totals_mismatch_only, totals_tried),
                ],
                [
                    "saved deps on mismatch-only candidates",
                    fmt_int(totals_mismatch_only_saved_deps),
                    "",
                ],
                [
                    "estimated early-exit saved time",
                    f"{totals_estimated_saved_us / 1_000_000:.2f}s",
                    "",
                ],
            ],
            title="D7: early-exit opportunity (requires mismatch telemetry)",
        )
    )

    # D1: bucket-success histogram.
    bins: list[tuple[float, float]] = [
        (0.0, 0.001),
        (0.001, 0.1),
        (0.1, 0.25),
        (0.25, 0.5),
        (0.5, 1.0),
        (1.0, 1.01),
    ]
    bin_counts = [0] * len(bins)
    for stats in agg.values():
        if stats.tried == 0:
            continue
        rate = stats.succeeded / stats.tried
        for i, (lo, hi) in enumerate(bins):
            if lo <= rate < hi or (i == len(bins) - 1 and rate >= lo):
                bin_counts[i] += 1
                break
    console.print(
        simple_table(
            ["rate", "buckets"],
            [
                [f"{lo:.3f} <= r < {hi:.3f}" if hi <= 1.0 else f">= {lo:.3f}", fmt_int(n)]
                for (lo, hi), n in zip(bins, bin_counts, strict=True)
            ],
            title="D1: success-rate histogram (per DepKeySet bucket)",
        )
    )

    # D2: Pareto of tries.
    sorted_by_tries = sorted(agg.items(), key=lambda kv: -kv[1].tried)
    top_n = 10
    console.print(
        simple_table(
            [
                "depKeySetId",
                "tried",
                "succeeded",
                "success %",
                "hashMismatch",
                "abortedEarly",
                "early-exit saved deps",
                "est saved s",
                "share of all tries",
            ],
            [
                [
                    str(dks),
                    fmt_int(stats.tried),
                    fmt_int(stats.succeeded),
                    pct(stats.succeeded, stats.tried),
                    fmt_int(stats.hash_mismatch),
                    fmt_int(stats.aborted_early),
                    fmt_int(
                        stats.earlier_hash_mismatch_saved_deps + stats.hash_mismatch_only_saved_deps
                    ),
                    f"{stats.estimated_early_exit_saved_us / 1_000_000:.2f}",
                    pct(stats.tried, totals_tried),
                ]
                for dks, stats in sorted_by_tries[:top_n]
            ],
            title=f"D2: top-{top_n} buckets by tries",
        )
    )

    # D3: persistent-fail summary.
    persistent_fail = sum(1 for s in agg.values() if s.tried > 0 and s.succeeded == 0)
    ever_succeeded = sum(1 for s in agg.values() if s.succeeded > 0)
    console.print(
        simple_table(
            ["Category", "Buckets"],
            [
                ["persistent-fail (tried > 0, succeeded == 0)", fmt_int(persistent_fail)],
                ["ever-succeeded (succeeded > 0)", fmt_int(ever_succeeded)],
                ["total buckets", fmt_int(len(agg))],
            ],
            title="D3: persistence summary",
        )
    )


def register(app: App) -> None:
    @app.command
    def sv(
        nix: Path = DEFAULT_NIX,
        nixpkgs: Path = DEFAULT_NIXPKGS,
        nixpkgs_branch: str = "master",
        nixpkgs_base: str | None = None,
        runs: str | None = None,
        output: Path | None = None,
    ) -> int:
        """Aggregate `byDepKeySet` SV telemetry across runs (D1-D7).

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
            _render(console, run_name, aggregate(ds.records_for(run_name)))
        if ds_args.output is not None:
            export_console(console, ds_args.output)
        return 0
