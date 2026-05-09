"""`runs` subcommand — soundness, cache effectiveness, performance across runs."""

from __future__ import annotations

import statistics
import sys
from pathlib import Path

from cyclopts import App
from rich.console import Console
from rich.text import Text

from ..cliutil import DEFAULT_NIX, DEFAULT_NIXPKGS, DatasetArgs, report_dataset_issues
from ..dataframe import Dataset, Record
from ..diff import eval_diff_detail
from ..discovery import expects_cache_misses
from ..metrics import DEP_HASH_US_METRICS, EVAL_TRACE_METRICS, PERF_METRICS
from ..render import (
    MetricRow,
    export_console,
    fmt_int,
    metric_table,
    pass_fail,
    pct,
    ratio_str,
    section,
    short_commit,
    simple_table,
)

# -- Soundness ---------------------------------------------------------------


def check_soundness(commit: str, by_run: dict[str, Record], ref_name: str) -> list[str]:
    ref = by_run.get(ref_name)
    if ref is None or ref.data is None:
        return [f"{short_commit(commit)}: no reference data"]
    errors: list[str] = []
    for name, rec in by_run.items():
        if name == ref_name or rec.data is None:
            continue
        if ref.data.eval_raw != rec.data.eval_raw:
            detail = eval_diff_detail(ref.data.eval_raw, rec.data.eval_raw)
            errors.append(f"{short_commit(commit)}: {name} differs from {ref_name}{detail}")
    return errors


def _runs_with_eval_trace(ds: Dataset) -> list[str]:
    return [
        r.name
        for r in ds.runs
        if any(
            rec.data is not None and (rec.data.hits + rec.data.misses) > 0
            for rec in ds.records_for(r.name)
        )
    ]


# -- Rendering ---------------------------------------------------------------


def render_summary(
    console: Console,
    ds: Dataset,
    ref_name: str,
    issues: dict[str, list[str]],
) -> None:
    """Per-commit: soundness + wall/CPU + ratios against reference.

    This table is too shape-heavy (one or two columns per run plus
    Sound/commit columns) for `metric_table`; we build it directly.
    """
    section(console, "Summary")
    has_wall = any(r.wall is not None for r in ds.records)

    headers: list[str] = ["Commit", "Sound"]
    for name in ds.run_names:
        if has_wall:
            headers.append(f"Wall {name} (s)")
        headers.append(f"CPU {name} (s)")
    headers.extend(f"{name}/{ref_name}" for name in ds.run_names if name != ref_name)

    rows: list[list[str | Text]] = []
    for commit in ds.commits:
        recs = ds.by_commit_map.get(commit, {})
        row: list[str | Text] = [
            short_commit(commit),
            pass_fail(len(issues.get(commit, [])) == 0),
        ]
        wall: dict[str, float] = {}
        cpu: dict[str, float] = {}
        for name in ds.run_names:
            rec = recs.get(name)
            if rec is None or rec.data is None:
                if has_wall:
                    row.append("N/A")
                row.append("N/A")
                continue
            if has_wall:
                if rec.wall is not None:
                    wall[name] = rec.wall
                    row.append(f"{rec.wall:.2f}")
                else:
                    row.append("N/A")
            if rec.cpu is not None:
                cpu[name] = rec.cpu
                row.append(f"{rec.cpu:.2f}")
            else:
                row.append("N/A")
        for name in ds.run_names:
            if name == ref_name:
                continue
            if has_wall and name in wall and ref_name in wall:
                row.append(ratio_str(wall[name], wall[ref_name]))
            else:
                row.append(ratio_str(cpu.get(name, 0.0), cpu.get(ref_name, 0.0)))
        rows.append(row)

    console.print(simple_table(headers, rows))
    console.print()


def render_soundness(console: Console, issues: dict[str, list[str]]) -> None:
    section(console, "Soundness")
    all_errors = [e for errs in issues.values() for e in errs]
    if not all_errors:
        console.print(Text("All outputs match reference.", style="bold green"))
    else:
        for err in all_errors:
            console.print(Text(f"  FAIL: {err}", style="bold red"))
    console.print()


def render_cache(console: Console, ds: Dataset) -> None:
    cache_runs = _runs_with_eval_trace(ds)
    if not cache_runs:
        return
    section(console, "Cache effectiveness")

    headers: list[str] = ["Commit"]
    for name in cache_runs:
        headers += [f"{name} hits", f"{name} misses", f"{name} rate"]

    totals: dict[str, tuple[int, int]] = dict.fromkeys(cache_runs, (0, 0))
    rows: list[list[str | Text]] = []
    for commit in ds.commits:
        recs = ds.by_commit_map.get(commit, {})
        row: list[str | Text] = [short_commit(commit)]
        for name in cache_runs:
            rec = recs.get(name)
            if rec is None or rec.data is None:
                row.extend(["N/A"] * 3)
                continue
            hits = rec.data.hits
            misses = rec.data.misses
            th, tm = totals[name]
            totals[name] = (th + hits, tm + misses)
            row.append(str(hits))
            row.append(Text(str(misses), style="red" if misses else ""))
            row.append(pct(hits, hits + misses))
        rows.append(row)

    total_row: list[str | Text] = [Text("TOTAL", style="bold")]
    for name in cache_runs:
        th, tm = totals[name]
        total_row.append(Text(str(th), style="bold"))
        total_row.append(Text(str(tm), style="bold red" if tm else "bold"))
        total_row.append(Text(pct(th, th + tm), style="bold"))
    rows.append(total_row)

    console.print(simple_table(headers, rows))
    console.print()


def render_eval_trace_metrics(console: Console, ds: Dataset) -> None:
    trace_runs = _runs_with_eval_trace(ds)
    if not trace_runs:
        return
    section(console, "Per-commit eval-trace metrics")

    headers: list[str] = ["Commit", "Metric", *trace_runs]
    rows: list[list[str | Text]] = []
    for commit in ds.commits:
        recs = ds.by_commit_map.get(commit, {})
        for i, metric in enumerate(EVAL_TRACE_METRICS):
            row: list[str | Text] = [short_commit(commit) if i == 0 else "", metric.label]
            for name in trace_runs:
                rec = recs.get(name)
                val = metric.get(rec.stats) if rec else None
                row.append(f"{val:{metric.fmt}}" if val is not None else "0")
            rows.append(row)
    console.print(simple_table(headers, rows))
    console.print()


def render_performance(console: Console, ds: Dataset, ref_name: str) -> None:
    section(console, "Performance (mean across commits)")
    series = [f"{n} (mean)" for n in ds.run_names]
    rows: list[MetricRow] = []

    wall_values: dict[str, list[float]] = {n: [] for n in ds.run_names}
    for rec in ds.records:
        if rec.wall is not None:
            wall_values[rec.run_name].append(rec.wall)
    if any(wall_values[n] for n in ds.run_names):
        rows.append(
            MetricRow(
                "wallTime (s)",
                {f"{n} (mean)": statistics.mean(v) if v else None for n, v in wall_values.items()},
            )
        )

    values: dict[str, dict[str, list[float]]] = {
        n: {m.label: [] for m in PERF_METRICS} for n in ds.run_names
    }
    for rec in ds.records:
        if rec.stats is None:
            continue
        for m in PERF_METRICS:
            v = m.get(rec.stats)
            if v is not None:
                values[rec.run_name][m.label].append(v)

    for m in PERF_METRICS:
        rows.append(
            MetricRow(
                m.label,
                {
                    f"{n} (mean)": statistics.mean(values[n][m.label])
                    if values[n][m.label]
                    else None
                    for n in ds.run_names
                },
            )
        )

    console.print(
        metric_table(
            rows,
            series,
            ratios_against=f"{ref_name} (mean)",
            fmt=lambda v: f"{v:.2f}",
        )
    )
    console.print()


def render_dep_hash_kinds(console: Console, ds: Dataset, ref_name: str) -> None:
    """H2 — dep-hash plus eval-trace sub-stage timing (sum across commits).

    Identifies which canonical query kind or trace-store sub-stage dominates
    verify/recovery/record time. Totals are summed seconds across all commits
    in each run.
    """
    section(console, "Eval-trace sub-stage timing (H2)")
    series = [f"{n} sum (s)" for n in ds.run_names]
    rows: list[MetricRow] = []
    for m in DEP_HASH_US_METRICS:
        sums: dict[str, float] = {n: 0.0 for n in ds.run_names}
        for rec in ds.records:
            if rec.stats is None:
                continue
            v = m.get(rec.stats)
            if v is not None:
                sums[rec.run_name] += v
        if all(v == 0 for v in sums.values()):
            continue
        rows.append(
            MetricRow(
                m.label,
                {f"{n} sum (s)": sums[n] / 1_000_000 if sums[n] else None for n in ds.run_names},
            )
        )
    if not rows:
        console.print("  [dim](no dep-hash timers recorded)[/dim]")
        return
    console.print(
        metric_table(
            rows,
            series,
            ratios_against=f"{ref_name} sum (s)",
            fmt=lambda v: f"{v:.2f}",
        )
    )
    console.print()


def render_recovery_phases(console: Console, ds: Dataset) -> None:
    """Recovery phase totals across commits.

    `recovery.directHash.timeUs` intentionally excludes dep recomputation, so
    direct-hash total time is reported as recompute + direct-hash stage.
    Lookup and guard timers are also shown separately; some are nested inside
    strategy timers, so they are diagnostic breakdowns rather than additive
    phase totals.
    """
    trace_runs = _runs_with_eval_trace(ds)
    if not trace_runs:
        return

    section(console, "Recovery phase totals")
    rows: list[list[str | Text]] = []
    for name in trace_runs:
        attempts = 0
        failures = 0
        git_attempts = 0
        git_candidates = 0
        git_hits = 0
        direct_hits = 0
        sv_hits = 0
        direct_recompute_us = 0
        direct_stage_us = 0
        git_stage_us = 0
        scan_history_us = 0
        sv_stage_us = 0
        recovery_total_us = 0

        for rec in ds.records_for(name):
            if rec.stats is None:
                continue
            et = rec.stats.eval_trace
            attempts += et.recovery.attempts
            failures += et.recovery.failures
            git_attempts += et.recovery.git_identity.attempts
            git_candidates += et.recovery.git_identity.candidates
            git_hits += et.recovery.git_identity_hits
            direct_hits += et.recovery.direct_hash.hits
            sv_hits += et.recovery.struct_variant.hits
            direct_recompute_us += et.dep_hash.recovery_recompute_us
            direct_stage_us += et.recovery.direct_hash.time_us
            git_stage_us += et.recovery.git_identity.time_us
            scan_history_us += et.recovery.lookups.scan_history.time_us
            sv_stage_us += et.recovery.struct_variant.time_us
            recovery_total_us += et.recovery.time_us

        direct_total_us = direct_recompute_us + direct_stage_us
        accounted_us = git_stage_us + direct_total_us + scan_history_us + sv_stage_us
        unattributed_us = recovery_total_us - accounted_us
        rows.append(
            [
                name,
                fmt_int(attempts),
                fmt_int(git_attempts),
                fmt_int(git_candidates),
                fmt_int(git_hits),
                fmt_int(direct_hits),
                fmt_int(sv_hits),
                fmt_int(failures),
                f"{git_stage_us / 1_000_000:.2f}",
                f"{direct_total_us / 1_000_000:.2f}",
                f"{direct_recompute_us / 1_000_000:.2f}",
                f"{direct_stage_us / 1_000_000:.2f}",
                f"{scan_history_us / 1_000_000:.2f}",
                f"{sv_stage_us / 1_000_000:.2f}",
                f"{recovery_total_us / 1_000_000:.2f}",
                Text(
                    f"{unattributed_us / 1_000_000:.2f}",
                    style="red" if unattributed_us > 0 else "green" if unattributed_us < 0 else "",
                ),
            ]
        )

    console.print(
        simple_table(
            [
                "Run",
                "attempts",
                "git attempts",
                "git candidates",
                "git hits",
                "direct hits",
                "sv hits",
                "failures",
                "git s",
                "direct total s",
                "direct recompute s",
                "direct stage s",
                "scan history s",
                "sv stage s",
                "recovery total s",
                "unattributed s~",
            ],
            rows,
        )
    )
    console.print()

    lookup_rows: list[list[str]] = []
    for name in trace_runs:
        latest_count = latest_us = 0
        direct_count = direct_rows = direct_us = 0
        git_lookup_count = git_lookup_rows = git_lookup_us = 0
        scan_count = scan_rows = scan_us = 0
        keyset_count = keyset_hits = keyset_misses = keyset_us = 0
        guard_candidates = guard_checks = guard_failures = guard_us = 0
        record_hash_us = record_keys_us = record_values_us = record_flush_us = 0

        for rec in ds.records_for(name):
            if rec.stats is None:
                continue
            et = rec.stats.eval_trace
            latest_count += et.recovery.lookups.latest_history.count
            latest_us += et.recovery.lookups.latest_history.time_us
            direct_count += et.recovery.lookups.direct_hash.count
            direct_rows += et.recovery.lookups.direct_hash.rows
            direct_us += et.recovery.lookups.direct_hash.time_us
            git_lookup_count += et.recovery.lookups.git_identity.count
            git_lookup_rows += et.recovery.lookups.git_identity.rows
            git_lookup_us += et.recovery.lookups.git_identity.time_us
            scan_count += et.recovery.lookups.scan_history.count
            scan_rows += et.recovery.lookups.scan_history.rows
            scan_us += et.recovery.lookups.scan_history.time_us
            keyset_count += et.load_key_set.count
            keyset_hits += et.load_key_set.cache_hits
            keyset_misses += et.load_key_set.cache_misses
            keyset_us += et.load_key_set.time_us
            guard_candidates += et.recovery.acceptance.implicit_guard_candidates
            guard_checks += et.recovery.acceptance.implicit_guard_checks
            guard_failures += et.recovery.acceptance.implicit_guard_failures
            guard_us += et.recovery.acceptance.implicit_guard_time_us
            record_hash_us += et.record.hash_us
            record_keys_us += et.record.serialize_keys_us
            record_values_us += et.record.serialize_values_us
            record_flush_us += et.record.flush_us

        lookup_rows.append(
            [
                name,
                f"{fmt_int(latest_count)} / {latest_us / 1_000_000:.2f}",
                f"{fmt_int(direct_count)} / {fmt_int(direct_rows)} / {direct_us / 1_000_000:.2f}",
                (
                    f"{fmt_int(git_lookup_count)} / {fmt_int(git_lookup_rows)} / "
                    f"{git_lookup_us / 1_000_000:.2f}"
                ),
                f"{fmt_int(scan_count)} / {fmt_int(scan_rows)} / {scan_us / 1_000_000:.2f}",
                (
                    f"{fmt_int(keyset_count)} / {fmt_int(keyset_hits)} / "
                    f"{fmt_int(keyset_misses)} / {keyset_us / 1_000_000:.2f}"
                ),
                (
                    f"{fmt_int(guard_candidates)} / {fmt_int(guard_checks)} / "
                    f"{fmt_int(guard_failures)} / {guard_us / 1_000_000:.2f}"
                ),
                (
                    f"{record_hash_us / 1_000_000:.2f} / {record_keys_us / 1_000_000:.2f} / "
                    f"{record_values_us / 1_000_000:.2f} / {record_flush_us / 1_000_000:.2f}"
                ),
            ]
        )

    console.print(
        simple_table(
            [
                "Run",
                "latestHistory count/s",
                "direct lookup count/rows/s",
                "git lookup count/rows/s",
                "scanHistory count/rows/s",
                "loadKeySet count/hit/miss/s",
                "implicit guards cand/check/fail/s",
                "record hash/keys/values/flush s",
            ],
            lookup_rows,
        )
    )
    console.print()


def render_regressions(console: Console, ds: Dataset) -> None:
    section(console, "Regressions")
    any_regression = False
    regression_commits: set[str] = set()

    time_flags: list[tuple[str, str, float, float, float, str]] = []
    for name in ds.run_names:
        recs = ds.records_for(name)
        walls: dict[str, float] = {r.commit: r.wall for r in recs if r.wall is not None}
        cpus: dict[str, float] = {r.commit: r.cpu for r in recs if r.cpu is not None}
        if walls:
            times, label = walls, "wallTime"
        elif cpus:
            times, label = cpus, "cpuTime"
        else:
            continue
        if len(times) < 2:
            continue
        med = statistics.median(times.values())
        threshold = med * 0.2
        for commit, t_val in times.items():
            if abs(t_val - med) > threshold:
                deviation = ((t_val - med) / med * 100) if med else 0.0
                time_flags.append((commit, name, t_val, med, deviation, label))
                regression_commits.add(commit)

    if time_flags:
        any_regression = True
        labels = {f[5] for f in time_flags}
        title = (
            "Wall Time Outliers"
            if labels == {"wallTime"}
            else "CPU Time Outliers"
            if labels == {"cpuTime"}
            else "Time Outliers"
        )
        rows: list[list[str | Text]] = [
            [
                short_commit(commit),
                name,
                f"{tv:.2f}",
                f"{med:.2f}",
                Text(f"{deviation:+.0f}%", style="red" if deviation > 0 else "green"),
            ]
            for commit, name, tv, med, deviation, _ in time_flags
        ]
        console.print(
            simple_table(
                ["Commit", "Run", "Time (s)", "Median (s)", "Deviation"], rows, title=title
            )
        )
        console.print()

    miss_flags: list[tuple[str, str, int, int]] = []
    for rec in ds.records:
        if rec.data is None or (rec.data.hits + rec.data.misses) == 0:
            continue
        if rec.data.misses > 0 and not expects_cache_misses(rec.run_name):
            miss_flags.append((rec.commit, rec.run_name, rec.data.hits, rec.data.misses))
            regression_commits.add(rec.commit)

    if miss_flags:
        any_regression = True
        miss_rows: list[list[str | Text]] = [
            [
                short_commit(commit),
                name,
                str(hits),
                Text(str(misses), style="red"),
                pct(hits, hits + misses),
            ]
            for commit, name, hits, misses in miss_flags
        ]
        console.print(
            simple_table(
                ["Commit", "Run", "Hits", "Misses", "Hit rate"],
                miss_rows,
                title="Unexpected cache misses",
            )
        )
        console.print()

    if not any_regression:
        console.print(Text("No regressions detected.", style="bold green"))
        console.print()
        return

    runs_csv = ",".join(ds.run_names)
    section(console, "Investigate with eval-trace-bench logs")
    for commit in ds.commits:
        if commit not in regression_commits:
            continue
        console.print(f"[dim]# {short_commit(commit)}[/dim]")
        console.print(
            f"eval-trace-bench logs --commit {commit} --runs {runs_csv}",
            soft_wrap=True,
        )
        console.print()
    console.print()


def register(app: App) -> None:
    @app.command
    def runs(
        nix: Path = DEFAULT_NIX,
        nixpkgs: Path = DEFAULT_NIXPKGS,
        nixpkgs_branch: str = "master",
        nixpkgs_base: str | None = None,
        runs_: str | None = None,
        output: Path | None = None,
        reference: str = "reference",
        commits: list[str] | None = None,
        verbose: bool = False,
    ) -> int:
        """Compare multiple runs (soundness + cache + performance).

        Parameters
        ----------
        nix: Nix checkout containing eval-trace-bench-results.
        nixpkgs: Nixpkgs checkout used to order commits by git history.
        nixpkgs_branch: Branch to walk for commit ordering.
        nixpkgs_base: Optional nixpkgs commit/ref to use as the newest commit.
        runs_: Comma-separated run names (default: auto-discover).
        output: Save report (.html for rich markup, else plain text).
        reference: Run name used as soundness reference.
        commits: Restrict to specific commits.
        verbose: Show per-commit eval-trace metric tables.
        """
        ds_args = DatasetArgs.from_strings(
            nix=nix,
            nixpkgs=nixpkgs,
            nixpkgs_branch=nixpkgs_branch,
            nixpkgs_base=nixpkgs_base,
            runs=runs_,
            output=output,
        )
        ds = ds_args.load(commits=commits)
        if report_dataset_issues(ds, sys.stderr, requested_runs=ds_args.runs):
            return 1
        if not ds.runs:
            print("No run directories found. Check --nix path.", file=sys.stderr)
            return 1
        if not ds.commits:
            print("No commits found.", file=sys.stderr)
            return 1

        ref_name = reference if reference in ds.run_names else ds.run_names[0]

        issues: dict[str, list[str]] = {}
        for commit in ds.commits:
            errs = check_soundness(commit, ds.by_commit_map.get(commit, {}), ref_name)
            if errs:
                issues[commit] = errs

        has_soundness_failure = any(issues.values())
        has_precision_issue = any(
            rec.data is not None and rec.data.misses > 0 and not expects_cache_misses(rec.run_name)
            for rec in ds.records
        )

        console = Console(record=True)
        section(
            console,
            f"Eval-trace runs report — {len(ds.commits)} commits, "
            f"runs: {', '.join(ds.run_names)}, ref: {ref_name}",
        )
        console.print()
        render_summary(console, ds, ref_name, issues)
        render_soundness(console, issues)
        render_cache(console, ds)
        if verbose:
            render_eval_trace_metrics(console, ds)
            render_dep_hash_kinds(console, ds, ref_name)
            render_recovery_phases(console, ds)
        render_performance(console, ds, ref_name)
        render_regressions(console, ds)

        if ds_args.output is not None:
            export_console(console, ds_args.output)

        if has_soundness_failure:
            return 1
        if has_precision_issue:
            return 2
        return 0
