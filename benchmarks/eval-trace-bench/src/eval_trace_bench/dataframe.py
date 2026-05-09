"""Per-(run, commit) record loader — the shared backbone of every analysis.

A single place that walks
`{nix_root}/eval-trace-bench-results/{run}/{commit}/` trees, loads
stats.json / timing.json / eval.json, attaches processing order, and
yields one `Record` per (run, commit).
"""

from __future__ import annotations

from collections.abc import Iterable
from dataclasses import dataclass, field
from functools import cached_property
from pathlib import Path
from typing import Any

from .discovery import (
    Run,
    classify_run_mode,
    discover_commits,
    discover_runs,
    manifest_commit_order,
    order_commits,
    select_runs,
)
from .layout import existing_results_root
from .models import EvalTraceStats, RunStats
from .stats import RunData, flatten, load_run_data_result


@dataclass(frozen=True)
class Record:
    """One (run, commit) row."""

    run_name: str
    commit: str
    proc_idx: int
    mode: str | None
    data: RunData | None
    load_error: str | None = None

    @property
    def present(self) -> bool:
        return self.data is not None

    @property
    def stats(self) -> RunStats | None:
        return self.data.stats if self.data else None

    @property
    def et(self) -> EvalTraceStats | None:
        return self.data.stats.eval_trace if self.data else None

    @property
    def wall(self) -> float | None:
        return self.data.wall_time if self.data else None

    @property
    def cpu(self) -> float | None:
        return self.data.stats.cpu_time if self.data else None


@dataclass
class Dataset:
    """Tidy view over a set of runs × commits."""

    runs: list[Run]
    commits: list[str]  # newest-first processing order
    records: list[Record] = field(default_factory=list[Record])

    @property
    def run_names(self) -> list[str]:
        return [r.name for r in self.runs]

    @cached_property
    def by_commit_map(self) -> dict[str, dict[str, Record]]:
        out: dict[str, dict[str, Record]] = {c: {} for c in self.commits}
        for rec in self.records:
            out.setdefault(rec.commit, {})[rec.run_name] = rec
        return out

    @cached_property
    def by_run_map(self) -> dict[str, list[Record]]:
        out: dict[str, list[Record]] = {r.name: [] for r in self.runs}
        for rec in self.records:
            out.setdefault(rec.run_name, []).append(rec)
        for v in out.values():
            v.sort(key=lambda r: r.proc_idx)
        return out

    def records_for(self, run_name: str) -> list[Record]:
        return self.by_run_map.get(run_name, [])

    @property
    def incomplete_records(self) -> list[Record]:
        return [r for r in self.records if r.data is None]


def load_dataset(
    nix_root: Path,
    *,
    source_repo: Path | None = None,
    source_branch: str = "master",
    source_base: str | None = None,
    runs: list[str] | None = None,
    commits: list[str] | None = None,
) -> Dataset:
    """Load every (run, commit) generated below `nix_root/`.

    `source_repo` is used to order commits via `git log`.  Missing or
    non-git repos fall through to deterministic alphabetical order.  If
    the grouped results directory does not exist yet, the loader falls
    back to the legacy top-level run layout.
    """
    data_root = existing_results_root(nix_root)
    all_runs = discover_runs(data_root)
    selected = select_runs(all_runs, runs)
    if commits is not None:
        ordered = list(dict.fromkeys(commits))
    else:
        candidates = discover_commits(selected)
        ordered = manifest_commit_order(selected, candidates) or order_commits(
            source_repo or nix_root,
            candidates,
            branch=source_branch,
            base=source_base,
        )
    records: list[Record] = []
    for run in selected:
        mode = classify_run_mode(run.name)
        for idx, commit in enumerate(ordered):
            load = load_run_data_result(run.path, commit)
            records.append(
                Record(
                    run_name=run.name,
                    commit=commit,
                    proc_idx=idx,
                    mode=mode,
                    data=load.data,
                    load_error=load.error,
                )
            )
    return Dataset(runs=selected, commits=ordered, records=records)


def tidy_rows(records: Iterable[Record]) -> list[dict[str, Any]]:
    """Return a list-of-dict view suitable for CSV / JSON export.

    Every flat dotted-key counter + proc_idx + wall + cpu is projected
    for each (run, commit) pair.  Preserves unknown keys emitted by
    newer Nix binaries via `RunStats.raw`.
    """
    rows: list[dict[str, Any]] = []
    for rec in records:
        row: dict[str, Any] = {
            "run_name": rec.run_name,
            "commit": rec.commit,
            "proc_idx": rec.proc_idx,
            "mode": rec.mode,
            "present": rec.present,
            "wall": rec.wall,
            "cpu": rec.cpu,
        }
        if rec.data is not None:
            row.update(flatten(rec.data.stats.raw))
        rows.append(row)
    return rows
