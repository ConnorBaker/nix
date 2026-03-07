"""Eval Trace Soundness & Precision Analyzer.

Compares benchmark runs produced by generate-runs.py to verify:
- Soundness: non-reference eval.json matches reference byte-for-byte
- Precision: cache hit rates across runs
- Performance: CPU time and GC metrics across run types
"""

from __future__ import annotations

import argparse
import json
import statistics
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from rich.console import Console
from rich.panel import Panel
from rich.table import Table
from rich.text import Text


# -- Data model ---------------------------------------------------------------


@dataclass
class RunData:
    stats: dict
    eval_raw: str | None


@dataclass
class Run:
    name: str
    path: Path


@dataclass
class Metric:
    label: str
    keys: tuple[str, ...]
    fmt: str = ".0f"
    scale: float = 1.0


# -- Metrics (shared across all render functions) -----------------------------

EVAL_TRACE_METRICS: list[Metric] = [
    Metric("hits", ("evalTrace", "hits")),
    Metric("misses", ("evalTrace", "misses")),
    Metric("verify.count", ("evalTrace", "verify", "count")),
    Metric("verify.passed", ("evalTrace", "verify", "passed")),
    Metric("verify.failed", ("evalTrace", "verify", "failed")),
    Metric("recovery.attempts", ("evalTrace", "recovery", "attempts")),
    Metric("recovery.directHash.hits", ("evalTrace", "recovery", "directHash", "hits")),
    Metric(
        "recovery.structVariant.hits",
        ("evalTrace", "recovery", "structVariant", "hits"),
    ),
    Metric("recovery.failures", ("evalTrace", "recovery", "failures")),
    Metric("record.count", ("evalTrace", "record", "count")),
    Metric("loadTrace.count", ("evalTrace", "loadTrace", "count")),
    Metric("depTracker.scopes", ("evalTrace", "depTracker", "scopes")),
    Metric("depTracker.ownDepsTotal", ("evalTrace", "depTracker", "ownDepsTotal")),
]

PERF_METRICS: list[Metric] = [
    Metric("cpuTime (s)", ("cpuTime",), fmt=".2f"),
    Metric("nrThunks", ("nrThunks",)),
    Metric("nrFunctionCalls", ("nrFunctionCalls",)),
    Metric("nrPrimOpCalls", ("nrPrimOpCalls",)),
    Metric("gc.heapSize (MB)", ("gc", "heapSize"), fmt=".2f", scale=1 / (1024 * 1024)),
    Metric(
        "gc.totalBytes (MB)", ("gc", "totalBytes"), fmt=".2f", scale=1 / (1024 * 1024)
    ),
    Metric("gc.cycles", ("gc", "cycles")),
    Metric("time.gc (s)", ("time", "gc"), fmt=".2f"),
]


# -- Helpers ------------------------------------------------------------------


def short(commit: str) -> str:
    return commit[:12]


def pct(num: float, den: float) -> str:
    return f"{100 * num / den:.1f}%" if den else "N/A"


def ratio_str(val: float, ref: float) -> str:
    return f"{val / ref:.2f}x" if ref else "N/A"


def styled_pass_fail(ok: bool) -> Text:
    return Text("PASS", style="bold green") if ok else Text("FAIL", style="bold red")


def get_nested(d: dict[str, Any], *keys: str, default: Any = None) -> Any:
    for k in keys:
        if not isinstance(d, dict):
            return default
        d = d.get(k, default)
        if d is default:
            return default
    return d


def get_metric(stats: dict, metric: Metric) -> float | None:
    val = get_nested(stats, *metric.keys, default=None)
    if val is None:
        return None
    return float(val) * metric.scale


def get_cache_stats(stats: dict) -> tuple[int, int, float]:
    et = stats.get("evalTrace", {})
    hits = et.get("hits", 0)
    misses = et.get("misses", 0)
    total = hits + misses
    return hits, misses, (hits / total if total else 0.0)


# -- Discovery ----------------------------------------------------------------


def is_commit_hash(name: str) -> bool:
    return len(name) == 40 and all(c in "0123456789abcdef" for c in name)


def has_commit_dirs(path: Path) -> bool:
    return path.is_dir() and any(
        is_commit_hash(p.name) for p in path.iterdir() if p.is_dir()
    )


def discover_runs(nix_path: Path) -> list[Run]:
    """Discover run directories under nix_path.

    A run directory contains commit-hash-named subdirectories (possibly nested
    under a run-number directory).
    """
    runs = []
    for entry in sorted(nix_path.iterdir()):
        if not entry.is_dir() or entry.is_symlink() or entry.name.startswith("."):
            continue
        if has_commit_dirs(entry):
            runs.append(Run(entry.name, entry))
        else:
            for sub in sorted(entry.iterdir()):
                if sub.is_dir() and has_commit_dirs(sub):
                    runs.append(Run(f"{entry.name}/{sub.name}", sub))
    return runs


def discover_commits(runs: list[Run]) -> set[str]:
    """Collect all commit hashes across all runs."""
    commits: set[str] = set()
    for run in runs:
        for p in run.path.iterdir():
            if p.is_dir() and is_commit_hash(p.name):
                commits.add(p.name)
    return commits


def order_commits(nixpkgs_path: Path, candidates: set[str]) -> list[str]:
    """Return candidates in git history order (newest first)."""
    result = subprocess.run(
        ["git", "-C", str(nixpkgs_path), "log", "--format=%H", "master"],
        capture_output=True,
        text=True,
        check=True,
    )
    ordered = []
    remaining = set(candidates)
    for line in result.stdout.strip().splitlines():
        if line in remaining:
            ordered.append(line)
            remaining.discard(line)
            if not remaining:
                break
    # Append any not found in git log (shouldn't happen normally)
    ordered.extend(sorted(remaining))
    return ordered


# -- Data loading -------------------------------------------------------------


def load_run_data(run_path: Path, commit: str) -> RunData | None:
    stats_file = run_path / commit / "stats.json"
    if not stats_file.exists():
        return None
    with open(stats_file) as f:
        stats = json.load(f)
    eval_file = run_path / commit / "eval.json"
    eval_raw = eval_file.read_text() if eval_file.exists() else None
    return RunData(stats=stats, eval_raw=eval_raw)


def load_all_data(
    runs: list[Run],
    commits: list[str],
) -> dict[str, dict[str, RunData | None]]:
    """Returns {commit: {run_name: RunData | None}}."""
    return {c: {run.name: load_run_data(run.path, c) for run in runs} for c in commits}


# -- Checks -------------------------------------------------------------------


def json_diff(a: object, b: object, path: str = "") -> list[str]:
    """Recursively diff two JSON values."""
    if a == b:
        return []
    if type(a) is not type(b):
        return [f"{path or '<root>'}: type {type(a).__name__} vs {type(b).__name__}"]
    if isinstance(a, dict) and isinstance(b, dict):
        diffs = []
        for k in sorted(set(a) | set(b)):
            cp = f"{path}.{k}" if path else k
            if k not in a:
                diffs.append(f"{cp}: added")
            elif k not in b:
                diffs.append(f"{cp}: removed")
            else:
                diffs.extend(json_diff(a[k], b[k], cp))
        return diffs
    if isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            return [f"{path or '<root>'}: length {len(a)} vs {len(b)}"]
        diffs = []
        for i, (x, y) in enumerate(zip(a, b)):
            diffs.extend(json_diff(x, y, f"{path}[{i}]"))
        return diffs
    return [f"{path or '<root>'}: {a!r} vs {b!r}"]


def eval_diff_detail(
    ref_raw: str | None, other_raw: str | None, max_diffs: int = 5
) -> str:
    if ref_raw is None or other_raw is None:
        return " (one side missing)"
    try:
        ref_obj = json.loads(ref_raw)
        other_obj = json.loads(other_raw)
    except json.JSONDecodeError:
        if len(ref_raw) != len(other_raw):
            return f" (size: {len(ref_raw)} vs {len(other_raw)})"
        return ""
    diffs = json_diff(ref_obj, other_obj)
    if not diffs:
        return ""
    shown = diffs[:max_diffs]
    extra = f" ... and {len(diffs) - max_diffs} more" if len(diffs) > max_diffs else ""
    return " (" + "; ".join(shown) + extra + ")"


def check_soundness(
    commit: str,
    run_data: dict[str, RunData | None],
    ref_name: str,
) -> list[str]:
    ref = run_data.get(ref_name)
    if ref is None:
        return [f"{short(commit)}: no reference data"]
    errors = []
    for name, data in run_data.items():
        if name == ref_name or data is None:
            continue
        if ref.eval_raw != data.eval_raw:
            detail = eval_diff_detail(ref.eval_raw, data.eval_raw)
            errors.append(f"{short(commit)}: {name} differs from {ref_name}{detail}")
    return errors


# -- Rendering ----------------------------------------------------------------


def runs_with_eval_trace(
    run_names: list[str],
    all_data: dict[str, dict[str, RunData | None]],
) -> list[str]:
    """Return run names that have evalTrace stats in at least one commit."""
    result = []
    for name in run_names:
        for runs in all_data.values():
            data = runs.get(name)
            if data and data.stats.get("evalTrace"):
                result.append(name)
                break
    return result


def render_summary(
    console: Console,
    run_names: list[str],
    ref_name: str,
    all_data: dict[str, dict[str, RunData | None]],
    issues: dict[str, list[str]],
):
    console.print(Panel("[bold]Summary[/bold]", expand=False))
    t = Table(show_header=True, header_style="bold")
    t.add_column("Commit")
    t.add_column("Sound")
    for name in run_names:
        t.add_column(f"CPU {name} (s)", justify="right")
    for name in run_names:
        if name != ref_name:
            t.add_column(f"{name}/{ref_name}", justify="right")

    for commit, runs in all_data.items():
        row: list[str | Text] = [short(commit)]
        row.append(styled_pass_fail(len(issues.get(commit, [])) == 0))
        cpu: dict[str, float] = {}
        for name in run_names:
            data = runs.get(name)
            if data:
                val = get_nested(data.stats, "cpuTime", default=0)
                cpu[name] = val
                row.append(f"{val:.2f}")
            else:
                row.append("N/A")
        for name in run_names:
            if name != ref_name:
                row.append(ratio_str(cpu.get(name, 0), cpu.get(ref_name, 0)))
        t.add_row(*row)

    console.print(t)
    console.print()


def render_soundness(console: Console, issues: dict[str, list[str]]):
    console.print(Panel("[bold]Soundness[/bold]", expand=False))
    all_errors = [e for errs in issues.values() for e in errs]
    if not all_errors:
        console.print(Text("All outputs match reference.", style="bold green"))
    else:
        for err in all_errors:
            console.print(Text(f"  FAIL: {err}", style="bold red"))
    console.print()


def render_cache(
    console: Console,
    run_names: list[str],
    all_data: dict[str, dict[str, RunData | None]],
):
    cache_runs = runs_with_eval_trace(run_names, all_data)
    if not cache_runs:
        return

    console.print(Panel("[bold]Cache Effectiveness[/bold]", expand=False))
    t = Table(show_header=True, header_style="bold")
    t.add_column("Commit")
    for name in cache_runs:
        t.add_column(f"{name} hits", justify="right")
        t.add_column(f"{name} misses", justify="right")
        t.add_column(f"{name} rate", justify="right")

    totals: dict[str, tuple[int, int]] = {n: (0, 0) for n in cache_runs}

    for commit, runs in all_data.items():
        row: list[str | Text] = [short(commit)]
        for name in cache_runs:
            data = runs.get(name)
            if data is None:
                row.extend(["N/A"] * 3)
                continue
            hits, misses, _ = get_cache_stats(data.stats)
            th, tm = totals[name]
            totals[name] = (th + hits, tm + misses)
            row.append(str(hits))
            row.append(Text(str(misses), style="red" if misses else ""))
            row.append(pct(hits, hits + misses))
        t.add_row(*row)

    total_row: list[str | Text] = [Text("TOTAL", style="bold")]
    for name in cache_runs:
        th, tm = totals[name]
        total_row.append(Text(str(th), style="bold"))
        total_row.append(Text(str(tm), style="bold red" if tm else "bold"))
        total_row.append(Text(pct(th, th + tm), style="bold"))
    t.add_row(*total_row, end_section=True)

    console.print(t)
    console.print()


def render_eval_trace_metrics(
    console: Console,
    run_names: list[str],
    all_data: dict[str, dict[str, RunData | None]],
):
    trace_runs = runs_with_eval_trace(run_names, all_data)
    if not trace_runs:
        return

    console.print(Panel("[bold]Eval Trace Metrics[/bold]", expand=False))
    t = Table(show_header=True, header_style="bold")
    t.add_column("Commit")
    t.add_column("Metric")
    for name in trace_runs:
        t.add_column(name, justify="right")

    for commit, runs in all_data.items():
        for i, metric in enumerate(EVAL_TRACE_METRICS):
            row: list[str] = [short(commit) if i == 0 else "", metric.label]
            for name in trace_runs:
                data = runs.get(name)
                val = get_metric(data.stats, metric) if data else None
                row.append(f"{val:{metric.fmt}}" if val is not None else "0")
            t.add_row(*row, end_section=(i == len(EVAL_TRACE_METRICS) - 1))

    console.print(t)
    console.print()


def render_performance(
    console: Console,
    run_names: list[str],
    ref_name: str,
    all_data: dict[str, dict[str, RunData | None]],
):
    console.print(Panel("[bold]Performance (mean across commits)[/bold]", expand=False))

    values: dict[str, dict[str, list[float]]] = {
        name: {m.label: [] for m in PERF_METRICS} for name in run_names
    }
    for runs in all_data.values():
        for name in run_names:
            data = runs.get(name)
            if data is None:
                continue
            for m in PERF_METRICS:
                val = get_metric(data.stats, m)
                if val is not None:
                    values[name][m.label].append(val)

    t = Table(show_header=True, header_style="bold")
    t.add_column("Metric")
    for name in run_names:
        t.add_column(f"{name} (mean)", justify="right")
    for name in run_names:
        if name != ref_name:
            t.add_column(f"{name}/{ref_name}", justify="right")

    for m in PERF_METRICS:
        row: list[str] = [m.label]
        means: dict[str, float] = {}
        for name in run_names:
            vals = values[name][m.label]
            if vals:
                mean = statistics.mean(vals)
                means[name] = mean
                row.append(f"{mean:{m.fmt}}")
            else:
                row.append("N/A")
        for name in run_names:
            if name != ref_name:
                row.append(ratio_str(means.get(name, 0), means.get(ref_name, 0)))
        t.add_row(*row)

    console.print(t)
    console.print()


def render_regressions(
    console: Console,
    run_names: list[str],
    all_data: dict[str, dict[str, RunData | None]],
):
    console.print(Panel("[bold]Regressions[/bold]", expand=False))

    any_regression = False
    regression_commits: set[str] = set()

    # -- CPU time outliers (per run) --
    cpu_flags: list[tuple[str, str, float, float, float]] = []
    for name in run_names:
        times: dict[str, float] = {}
        for commit, run_data in all_data.items():
            data = run_data.get(name)
            if data is None:
                continue
            cpu = get_nested(data.stats, "cpuTime", default=None)
            if cpu is not None:
                times[commit] = cpu

        if len(times) < 2:
            continue
        med = statistics.median(times.values())
        threshold = med * 0.2
        for commit, cpu in times.items():
            if abs(cpu - med) > threshold:
                deviation = (cpu - med) / med * 100
                cpu_flags.append((commit, name, cpu, med, deviation))
                regression_commits.add(commit)

    if cpu_flags:
        any_regression = True
        t = Table(title="CPU Time Outliers", show_header=True, header_style="bold")
        t.add_column("Commit")
        t.add_column("Run")
        t.add_column("cpuTime (s)", justify="right")
        t.add_column("Median (s)", justify="right")
        t.add_column("Deviation", justify="right")
        for commit, name, cpu, med, deviation in cpu_flags:
            style = "red" if deviation > 0 else "green"
            t.add_row(
                short(commit),
                name,
                f"{cpu:.2f}",
                f"{med:.2f}",
                Text(f"{deviation:+.0f}%", style=style),
            )
        console.print(t)
        console.print()

    # -- Cache misses --
    miss_flags: list[tuple[str, str, int, int, float]] = []
    for commit, run_data in all_data.items():
        for name in run_names:
            data = run_data.get(name)
            if data is None or not data.stats.get("evalTrace"):
                continue
            hits, misses, rate = get_cache_stats(data.stats)
            if misses > 0:
                miss_flags.append((commit, name, hits, misses, rate))
                regression_commits.add(commit)

    if miss_flags:
        any_regression = True
        t = Table(title="Cache Misses", show_header=True, header_style="bold")
        t.add_column("Commit")
        t.add_column("Run")
        t.add_column("Hits", justify="right")
        t.add_column("Misses", justify="right")
        t.add_column("Hit Rate", justify="right")
        for commit, name, hits, misses, rate in miss_flags:
            t.add_row(
                short(commit),
                name,
                str(hits),
                Text(str(misses), style="red"),
                pct(hits, hits + misses),
            )
        console.print(t)
        console.print()

    if not any_regression:
        console.print(Text("No regressions detected.", style="bold green"))
        console.print()
        return

    # -- compare-logs commands for regression commits --
    runs_csv = ",".join(run_names)
    console.print(Panel("[bold]Investigate with compare-logs[/bold]", expand=False))
    for commit in all_data:
        if commit not in regression_commits:
            continue
        console.print(f"[dim]# {short(commit)}[/dim]")
        console.print(
            f"nix run .#compare-logs -- --commit {commit} --runs {runs_csv}",
            soft_wrap=True,
        )
        console.print()
    console.print()


# -- Main ---------------------------------------------------------------------


def main():
    parser = argparse.ArgumentParser(
        prog="compare-runs",
        description="Eval Trace Soundness & Precision Analyzer",
    )
    parser.add_argument(
        "--nix",
        type=Path,
        default=Path.cwd(),
        help="Path to nix source dir (default: cwd)",
    )
    parser.add_argument(
        "--nixpkgs",
        type=Path,
        default=Path.home() / "nixpkgs",
        help="Path to nixpkgs checkout for commit ordering (default: ~/nixpkgs)",
    )
    parser.add_argument(
        "--reference",
        type=str,
        default="reference",
        help="Run name to use as reference for soundness checks (default: reference)",
    )
    parser.add_argument(
        "--runs",
        type=str,
        default=None,
        help="Comma-separated run names to include (default: auto-discover)",
    )
    parser.add_argument(
        "--commits",
        nargs="*",
        default=None,
        help="Specific commits to analyze (default: auto-discover)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="Save report to file (.html for rich markup, otherwise plain text)",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Show per-commit eval trace metric tables",
    )
    args = parser.parse_args()

    # Discover runs
    all_runs = discover_runs(args.nix)
    if not all_runs:
        print("No run directories found. Check --nix path.", file=sys.stderr)
        sys.exit(1)

    if args.runs:
        requested = {r.strip() for r in args.runs.split(",")}
        all_runs = [r for r in all_runs if r.name in requested]
        if not all_runs:
            available = [r.name for r in discover_runs(args.nix)]
            print(
                f"None of the requested runs found. Available: {available}",
                file=sys.stderr,
            )
            sys.exit(1)

    run_names = [r.name for r in all_runs]
    ref_name = args.reference if args.reference in run_names else run_names[0]

    # Discover and order commits
    all_commit_hashes = discover_commits(all_runs)
    if args.commits:
        commits = [c for c in args.commits if c in all_commit_hashes]
    else:
        commits = order_commits(args.nixpkgs, all_commit_hashes)

    if not commits:
        print("No commits found.", file=sys.stderr)
        sys.exit(1)

    # Load data
    all_data = load_all_data(all_runs, commits)

    # Soundness checks
    issues: dict[str, list[str]] = {}
    for commit, runs in all_data.items():
        errs = check_soundness(commit, runs, ref_name)
        if errs:
            issues[commit] = errs

    has_soundness_failure = any(issues.values())
    has_precision_issue = any(
        get_cache_stats(data.stats)[1] > 0
        for runs in all_data.values()
        for data in runs.values()
        if data and data.stats.get("evalTrace")
    )

    # Render
    console = Console(record=True)

    console.print(
        Panel(
            f"[bold]Eval Trace Report[/bold] — {len(commits)} commits, "
            f"runs: {', '.join(run_names)}, ref: {ref_name}",
            expand=False,
        )
    )
    console.print()

    render_summary(console, run_names, ref_name, all_data, issues)
    render_soundness(console, issues)
    render_cache(console, run_names, all_data)
    if args.verbose:
        render_eval_trace_metrics(console, run_names, all_data)
    render_performance(console, run_names, ref_name, all_data)
    render_regressions(console, run_names, all_data)

    if args.output:
        if args.output.suffix == ".html":
            args.output.write_text(console.export_html())
        else:
            args.output.write_text(console.export_text())

    if has_soundness_failure:
        sys.exit(1)
    elif has_precision_issue:
        sys.exit(2)


if __name__ == "__main__":
    main()
