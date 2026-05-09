"""`export` subcommand — dump the tidy per-(run, commit) dataframe.

Plan-doc §1 observation: every ad-hoc analysis begins with "walk the
runs, flatten stats.json, dump to /tmp/*.json".  This subcommand does
that once so follow-up work can happen in whatever dataframe tool the
user prefers (pandas, polars, duckdb, awk).
"""

from __future__ import annotations

import contextlib
import csv
import json
import sys
from collections.abc import Generator
from pathlib import Path
from typing import Annotated, Literal, TextIO

from cyclopts import App, Parameter

from ..cliutil import DEFAULT_NIX, DEFAULT_NIXPKGS, DatasetArgs, report_dataset_issues
from ..dataframe import tidy_rows

_COMMON_ORDER: tuple[str, ...] = (
    "run_name",
    "commit",
    "proc_idx",
    "mode",
    "present",
    "wall",
    "cpu",
)


def _fieldnames(rows: list[dict[str, object]]) -> list[str]:
    """Every column seen across all rows, common ones first."""
    extra: set[str] = set()
    for row in rows:
        extra.update(row.keys())
    extra.difference_update(_COMMON_ORDER)
    return [*_COMMON_ORDER, *sorted(extra)]


@contextlib.contextmanager
def _output(path: Path) -> Generator[TextIO]:
    """Open `path` for writing, or yield stdout if `path == Path('-')`."""
    if str(path) == "-":
        yield sys.stdout
        return
    with path.open("w", newline="") as f:
        yield f


def _write_csv(rows: list[dict[str, object]], handle: TextIO) -> None:
    w = csv.DictWriter(handle, fieldnames=_fieldnames(rows), extrasaction="ignore")
    w.writeheader()
    for row in rows:
        w.writerow(row)


def _write_jsonl(rows: list[dict[str, object]], handle: TextIO) -> None:
    for row in rows:
        handle.write(json.dumps(row))
        handle.write("\n")


def _write_json(rows: list[dict[str, object]], handle: TextIO) -> None:
    json.dump(rows, handle, indent=2)
    handle.write("\n")


_WRITERS = {"csv": _write_csv, "json": _write_json, "jsonl": _write_jsonl}


def register(app: App) -> None:
    @app.command
    def export(
        *,
        out: Annotated[Path, Parameter(allow_leading_hyphen=True)],
        nix: Path = DEFAULT_NIX,
        nixpkgs: Path = DEFAULT_NIXPKGS,
        nixpkgs_branch: str = "master",
        nixpkgs_base: str | None = None,
        runs: str | None = None,
        format_: Literal["csv", "json", "jsonl"] = "csv",
    ) -> int:
        """Export the per-(run, commit) dataframe as CSV / JSON / JSONL.

        Parameters
        ----------
        out: Output file (use - for stdout).
        nix: Nix checkout containing eval-trace-bench-results.
        nixpkgs: Nixpkgs checkout used to order commits by git history.
        nixpkgs_branch: Branch to walk for commit ordering.
        nixpkgs_base: Optional nixpkgs commit/ref to use as the newest commit.
        runs: Comma-separated run names (default: auto-discover).
        format_: Output format.
        """
        ds_args = DatasetArgs.from_strings(
            nix=nix,
            nixpkgs=nixpkgs,
            nixpkgs_branch=nixpkgs_branch,
            nixpkgs_base=nixpkgs_base,
            runs=runs,
            output=None,
        )
        ds = ds_args.load()
        if report_dataset_issues(
            ds,
            sys.stderr,
            requested_runs=ds_args.runs,
            include_records=False,
        ):
            return 1
        if not ds.runs:
            print("No run directories found.", file=sys.stderr)
            return 1
        rows = tidy_rows(ds.records)
        with _output(out) as handle:
            _WRITERS[format_](rows, handle)
        if str(out) != "-":
            print(f"wrote {len(rows)} rows to {out}", file=sys.stderr)
        return 0
