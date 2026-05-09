"""`simulate` subcommand — replay `byDepKeySet` history under alternate policies.

Implements plan-doc D4 (OPP-1), D5 (winner-rank retrospective), plus
additional policies not in the plan but useful for the same ceiling
analysis: top-K prefix, success-weighted, per-commit time budget, and
combined (AND-composed) policies.

The replay mechanics live in `simulation.py`; this file just parses
policy specs off the CLI, fans them out across every run, and
pretty-prints the table.
"""

from __future__ import annotations

import sys
from collections.abc import Callable
from pathlib import Path

from cyclopts import App
from rich.console import Console
from rich.text import Text

from ..cliutil import DEFAULT_NIX, DEFAULT_NIXPKGS, DatasetArgs, report_dataset_issues
from ..dataframe import Record
from ..render import export_console, fmt_int, pct, section, simple_table
from ..simulation import (
    OPP1,
    Combined,
    NeverSkip,
    Policy,
    SuccessWeighted,
    TimeBudget,
    TopKPrefix,
    extract_events,
    run_simulation,
    winner_ranks,
)


def _parse_success(arg: str) -> Policy:
    parts = arg.split(":")
    rate = float(parts[0])
    warmup = int(parts[1]) if len(parts) > 1 else 5
    return SuccessWeighted(rate=rate, warmup=warmup)


# Each entry: (prefix, factory taking the suffix after the prefix).
_PREFIX_PARSERS: list[tuple[str, Callable[[str], Policy]]] = [
    ("opp1:", lambda a: OPP1(threshold=int(a))),
    ("top-", lambda a: TopKPrefix(k=int(a))),
    ("budget:", lambda a: TimeBudget(budget_ms=float(a))),
    ("success:", _parse_success),
]


def _parse_policy(spec: str) -> Policy:
    """Parse a single policy spec.

    Accepted forms:
      never-skip
      opp1:T                 -> OPP-1 with threshold T
      top-K                  -> try first K buckets each commit
      budget:MS              -> per-commit time budget in ms
      success:RATE[:WARMUP]  -> Laplace-smoothed success-rate floor
      combo(spec1+spec2+…)   -> AND of component policies
    """
    spec = spec.strip()
    if spec == "never-skip":
        return NeverSkip()
    if spec.startswith("combo(") and spec.endswith(")"):
        inner = spec[len("combo(") : -1]
        children = [_parse_policy(s) for s in inner.split("+") if s]
        return Combined(children=children, label=spec)
    for prefix, factory in _PREFIX_PARSERS:
        if spec.startswith(prefix):
            return factory(spec.removeprefix(prefix))
    raise SystemExit(f"unknown policy spec: {spec!r}")


_DEFAULT_POLICY_BATTERY: list[str] = [
    "never-skip",
    "opp1:3",
    "opp1:5",
    "opp1:10",
    "opp1:20",
    "top-1",
    "top-3",
    "top-10",
    "success:0.01",
    "success:0.05",
    "budget:500",
    "budget:2000",
    "combo(opp1:10+top-10)",
]


def _parse_policy_list(policy_arg: str | None) -> list[Policy]:
    specs: list[str] = []
    if policy_arg:
        specs.extend(s.strip() for s in policy_arg.split(","))
    if not specs:
        specs = _DEFAULT_POLICY_BATTERY
    return [_parse_policy(s) for s in specs]


def _render_policies(
    console: Console, run_name: str, records: list[Record], policies: list[Policy]
) -> None:
    events = extract_events(records)
    if not events:
        return
    section(console, f"{run_name}: SV policy simulation")
    rows: list[list[str | Text]] = []
    for policy in policies:
        result = run_simulation(policy, events)
        net_s = (result.saved_us - result.lost_us) / 1_000_000
        # Style the net cell green if it would save wall, red if it
        # would lose wall, neutral if neither.
        net_style = "green" if net_s > 0 else "red" if net_s < 0 else ""
        rows.append(
            [
                policy.name,
                fmt_int(result.saved_tries),
                f"{result.saved_us / 1_000_000:.2f}s",
                fmt_int(result.missed_wins),
                f"{result.lost_us / 1_000_000:.2f}s",
                fmt_int(result.tried_bucket_events),
                fmt_int(result.skipped_bucket_events),
                Text(f"{net_s:+.2f}s", style=net_style) if net_style else f"{net_s:+.2f}s",
            ]
        )
    console.print(
        simple_table(
            [
                "policy",
                "saved tries",
                "saved wall",
                "missed wins",
                "lost wall",
                "tried events",
                "skipped events",
                "net",
            ],
            rows,
        )
    )


def _render_winner_ranks(console: Console, run_name: str, records: list[Record]) -> None:
    """D5: where does the winning bucket land within each commit?"""
    summary = winner_ranks(records)
    if summary.total_commits_with_wins == 0:
        return
    console.print(
        simple_table(
            ["Metric", "Value"],
            [
                ["commits with a SV win", fmt_int(summary.total_commits_with_wins)],
                ["mean winner rank", f"{summary.mean_rank:.2f}"],
                ["median winner rank", f"{summary.median_rank:.0f}"],
                [
                    "winner in top-3",
                    pct(
                        int(summary.top_3_share * summary.total_commits_with_wins),
                        summary.total_commits_with_wins,
                    ),
                ],
                [
                    "winner in top-10",
                    pct(
                        int(summary.top_10_share * summary.total_commits_with_wins),
                        summary.total_commits_with_wins,
                    ),
                ],
            ],
            title=f"{run_name}: winner-rank retrospective (D5)",
        )
    )


def register(app: App) -> None:
    @app.command
    def simulate(
        nix: Path = DEFAULT_NIX,
        nixpkgs: Path = DEFAULT_NIXPKGS,
        nixpkgs_branch: str = "master",
        nixpkgs_base: str | None = None,
        runs: str | None = None,
        output: Path | None = None,
        policy: str | None = None,
        winner_ranks_: bool = False,
    ) -> int:
        """Replay byDepKeySet history under alternate policies.

        Parameters
        ----------
        nix: Nix checkout containing eval-trace-bench-results.
        nixpkgs: Nixpkgs checkout used to order commits by git history.
        nixpkgs_branch: Branch to walk for commit ordering.
        nixpkgs_base: Optional nixpkgs commit/ref to use as the newest commit.
        runs: Comma-separated run names (default: auto-discover).
        output: Save report (.html for rich markup, else plain text).
        policy: Comma-separated policy specs (never-skip, opp1:T, top-K,
            budget:MS, success:RATE[:WARMUP], combo(spec1+spec2+…)).
            Default: headline battery.
        winner_ranks_: Also emit the D5 winner-rank retrospective table.
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
        policies = _parse_policy_list(policy)
        console = Console(record=True)
        for run_name in ds.run_names:
            records = ds.records_for(run_name)
            _render_policies(console, run_name, records, policies)
            if winner_ranks_:
                _render_winner_ranks(console, run_name, records)
        if ds_args.output is not None:
            export_console(console, ds_args.output)
        return 0
