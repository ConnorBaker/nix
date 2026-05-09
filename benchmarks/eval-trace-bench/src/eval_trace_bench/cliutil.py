"""Shared dataset-loading helpers for subcommands.

Each subcommand that walks the run dataset (everything except
`generate`, `logs`, and `db-inspect`) takes the same dataset flags:
`--nix / --nixpkgs / --nixpkgs-branch / --nixpkgs-base / --runs / --output`.  `--nix`
points at the Nix checkout; generated datasets live below its
`eval-trace-bench-results/` directory.  This module centralises the
Pydantic-like value object that wraps those flags.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import TextIO

from .dataframe import Dataset, Record, load_dataset

# Default paths for CLI argument defaults.  Captured once at import
# time so subcommand signatures don't trigger B008 (function call in
# argument default).
DEFAULT_NIX: Path = Path.cwd()
DEFAULT_NIXPKGS: Path = Path.home() / "nixpkgs"
DEFAULT_NIX_SRC: Path = Path.home() / "nix"
DEFAULT_NIX_CACHE: Path = Path.home() / ".cache/nix"


def _split_csv(value: str | None) -> list[str] | None:
    if not value:
        return None
    parts = [r.strip() for r in value.split(",") if r.strip()]
    return parts or None


@dataclass(frozen=True)
class DatasetArgs:
    """Standard "which runs to load" parameter bundle."""

    nix: Path
    nixpkgs: Path
    nixpkgs_branch: str
    nixpkgs_base: str | None
    runs: list[str] | None
    output: Path | None

    @classmethod
    def from_strings(
        cls,
        *,
        nix: Path,
        nixpkgs: Path,
        nixpkgs_branch: str,
        nixpkgs_base: str | None = None,
        runs: str | None,
        output: Path | None,
    ) -> DatasetArgs:
        return cls(
            nix=nix,
            nixpkgs=nixpkgs,
            nixpkgs_branch=nixpkgs_branch,
            nixpkgs_base=nixpkgs_base,
            runs=_split_csv(runs),
            output=output,
        )

    def load(self, *, commits: list[str] | None = None) -> Dataset:
        return load_dataset(
            self.nix,
            source_repo=self.nixpkgs,
            source_branch=self.nixpkgs_branch,
            source_base=self.nixpkgs_base,
            runs=self.runs,
            commits=commits,
        )


def format_record_issues(records: list[Record], *, limit: int = 20) -> list[str]:
    lines: list[str] = []
    for rec in records[:limit]:
        reason = rec.load_error or "missing data"
        lines.append(f"{rec.run_name} {rec.commit[:12]}: {reason}")
    if len(records) > limit:
        lines.append(f"... and {len(records) - limit} more incomplete rows")
    return lines


def format_dataset_issues(
    ds: Dataset,
    *,
    requested_runs: list[str] | None = None,
    include_records: bool = True,
    limit: int = 20,
) -> list[str]:
    lines: list[str] = []
    if requested_runs is not None:
        discovered = set(ds.run_names)
        for run_name in requested_runs:
            if run_name not in discovered:
                lines.append(f"{run_name}: requested run was not found")
    if include_records:
        lines.extend(format_record_issues(ds.incomplete_records, limit=limit))
    return lines


def report_dataset_issues(
    ds: Dataset,
    stream: TextIO,
    *,
    requested_runs: list[str] | None = None,
    include_records: bool = True,
    limit: int = 20,
) -> bool:
    """Print incomplete run rows. Return True if any issues were reported."""
    lines = format_dataset_issues(
        ds,
        requested_runs=requested_runs,
        include_records=include_records,
        limit=limit,
    )
    if not lines:
        return False
    print("Incomplete benchmark dataset:", file=stream)
    for line in lines:
        print(f"  {line}", file=stream)
    return True


def report_record_issues(records: list[Record], stream: TextIO, *, limit: int = 20) -> bool:
    """Print incomplete run rows. Return True if any issues were reported."""
    lines = format_record_issues(records, limit=limit)
    if not lines:
        return False
    print("Incomplete benchmark dataset:", file=stream)
    for line in lines:
        print(f"  {line}", file=stream)
    return True
