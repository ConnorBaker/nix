"""Run / commit discovery shared by eval-trace-bench analysis subcommands."""

from __future__ import annotations

import json
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any, cast

from .layout import MANIFEST_FILE


def is_commit_hash(name: str) -> bool:
    return len(name) == 40 and all(c in "0123456789abcdef" for c in name)


def has_commit_dirs(path: Path) -> bool:
    return path.is_dir() and any(is_commit_hash(p.name) for p in path.iterdir() if p.is_dir())


@dataclass(frozen=True)
class Run:
    """A discovered run directory."""

    name: str
    path: Path

    def commits(self) -> set[str]:
        return {p.name for p in self.path.iterdir() if p.is_dir() and is_commit_hash(p.name)}


_RESERVED_LEAF = {"_state"}


def discover_runs(nix_root: Path) -> list[Run]:
    """Walk a results root and return every directory that contains commit dirs.

    Accepts both flat (`reference/{sha}/`) and nested
    (`cold/0/{sha}/`) run layouts inside the grouped results directory.
    """
    runs: list[Run] = []
    if not nix_root.is_dir():
        return runs
    for entry in sorted(nix_root.iterdir()):
        if not entry.is_dir() or entry.is_symlink() or entry.name.startswith("."):
            continue
        if entry.name in _RESERVED_LEAF:
            continue
        if has_commit_dirs(entry):
            runs.append(Run(entry.name, entry))
            continue
        for sub in sorted(entry.iterdir()):
            if not sub.is_dir() or sub.name in _RESERVED_LEAF:
                continue
            if has_commit_dirs(sub):
                runs.append(Run(f"{entry.name}/{sub.name}", sub))
    return runs


def select_runs(runs: list[Run], wanted: list[str] | None) -> list[Run]:
    if wanted is None:
        return runs
    wanted_set = set(wanted)
    return [r for r in runs if r.name in wanted_set]


def discover_commits(runs: list[Run]) -> set[str]:
    commits: set[str] = set()
    for run in runs:
        commits |= run.commits()
    return commits


def order_commits(
    source_repo: Path,
    candidates: set[str],
    branch: str = "master",
    base: str | None = None,
) -> list[str]:
    """Order `candidates` by the `branch`'s git history (newest first).

    Any commit not found in `git log` is appended alphabetically at the
    end — useful for locally-edited histories or rebases.
    """
    remaining = set(candidates)
    ordered: list[str] = []
    ref = base or branch
    if source_repo.is_dir() and (source_repo / ".git").exists():
        result = subprocess.run(
            ["git", "-C", str(source_repo), "log", "--format=%H", ref],
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode == 0:
            for line in result.stdout.splitlines():
                if line in remaining:
                    ordered.append(line)
                    remaining.discard(line)
                    if not remaining:
                        break
    ordered.extend(sorted(remaining))
    return ordered


def _manifest_commit_order(run: Run) -> list[str]:
    path = run.path / MANIFEST_FILE
    if not path.is_file():
        return []
    try:
        payload_raw: Any = json.loads(path.read_text())
    except json.JSONDecodeError:
        return []
    if not isinstance(payload_raw, dict):
        return []
    payload: dict[str, Any] = payload_raw  # pyright: ignore[reportAssignmentType, reportUnknownVariableType]
    raw_order: Any = payload.get("commitOrder")
    if not isinstance(raw_order, list):
        return []
    order_items = cast(list[object], raw_order)
    return [c for c in order_items if isinstance(c, str) and is_commit_hash(c)]


def manifest_commit_order(runs: list[Run], candidates: set[str]) -> list[str] | None:
    """Return commit order from generated run manifests when available.

    `generate` writes the exact processing order into each run directory.
    Prefer that over git history so non-nixpkgs suites and `--nixpkgs-base`
    reports preserve the order that was actually benchmarked.
    """
    manifest_orders: list[list[str]] = []
    for run in runs:
        seen: set[str] = set()
        ordered: list[str] = []
        for commit in _manifest_commit_order(run):
            if commit in candidates and commit not in seen:
                ordered.append(commit)
                seen.add(commit)
        if ordered:
            manifest_orders.append(ordered)
    complete_orders = [order for order in manifest_orders if set(order) >= candidates]
    if not complete_orders:
        return None
    first = complete_orders[0]
    if any(order != first for order in complete_orders[1:]):
        return None
    return first


def classify_run_mode(run_name: str) -> str | None:
    """Return the underlying mode (reference/cold/hot/warm) or None.

    Accepts both "cold/0" and "sibling-heavy-cold/0" style names.
    """
    stem = run_name.split("/", 1)[0]
    for mode in ("reference", "cold", "hot", "warm"):
        if stem == mode or stem.endswith(f"-{mode}"):
            return mode
    return None


def expects_cache_misses(run_name: str) -> bool:
    return classify_run_mode(run_name) == "cold"
