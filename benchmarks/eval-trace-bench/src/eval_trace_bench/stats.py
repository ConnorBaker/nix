"""Per-commit stats/timing/eval loader.

The heavy lifting is in `models.py` (Pydantic).  This module layers a
thin on-disk loader on top that knows about the
`{run}/{commit}/stats.json` / `timing.json` / `eval.json` file layout
`generate` emits.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from pydantic import ValidationError

from .models import RunStats, Timing, load_stats


@dataclass(frozen=True)
class RunData:
    """Per-commit data pulled from a single `{run}/{commit}/` directory."""

    stats: RunStats
    eval_raw: str | None
    wall_time: float | None

    @property
    def hits(self) -> int:
        return self.stats.eval_trace.hits

    @property
    def misses(self) -> int:
        return self.stats.eval_trace.misses

    @property
    def hit_rate(self) -> float:
        total = self.hits + self.misses
        return (self.hits / total) if total else 0.0


@dataclass(frozen=True)
class LoadResult:
    """Loaded run data plus a user-facing error for incomplete/corrupt rows."""

    data: RunData | None
    error: str | None = None


def flatten(d: dict[str, Any], prefix: str = "") -> dict[str, int | float]:
    """Flatten a nested JSON object into dotted keys.

    Array-valued children (`byDepKeySet`, `primops`, `functions`) are
    not flattened — they're sequences of records, not sub-trees, and
    `logs` surfaces them separately.
    """
    out: dict[str, int | float] = {}
    for k, v in d.items():
        key = f"{prefix}.{k}" if prefix else k
        if isinstance(v, dict):
            out.update(flatten(v, key))  # type: ignore[arg-type]
        elif isinstance(v, int | float):
            out[key] = v
    return out


def _json_error(path: Path, err: json.JSONDecodeError) -> str:
    return f"{path.name}: invalid JSON at line {err.lineno}, column {err.colno}"


def _validation_error(path: Path, err: ValidationError) -> str:
    first_line = str(err).splitlines()[0]
    return f"{path.name}: schema validation failed: {first_line}"


def load_run_data_result(run_dir: Path, commit: str) -> LoadResult:
    """Load `{run_dir}/{commit}/stats.json` (+ eval.json / timing.json)."""
    commit_dir = run_dir / commit
    stats_file = commit_dir / "stats.json"
    if not stats_file.exists():
        return LoadResult(None, "missing stats.json")
    try:
        payload: Any = json.loads(stats_file.read_text())
    except json.JSONDecodeError as err:
        return LoadResult(None, _json_error(stats_file, err))
    if not isinstance(payload, dict):
        return LoadResult(None, "stats.json: expected JSON object")
    typed_payload: dict[str, Any] = payload  # pyright: ignore[reportAssignmentType, reportUnknownVariableType]
    try:
        stats = load_stats(typed_payload)
    except ValidationError as err:
        return LoadResult(None, _validation_error(stats_file, err))

    eval_file = commit_dir / "eval.json"
    if not eval_file.exists():
        return LoadResult(None, "missing eval.json")
    eval_raw = eval_file.read_text()
    try:
        json.loads(eval_raw)
    except json.JSONDecodeError as err:
        return LoadResult(None, _json_error(eval_file, err))

    timing_file = commit_dir / "timing.json"
    if not timing_file.exists():
        return LoadResult(None, "missing timing.json")
    try:
        timing_payload: Any = json.loads(timing_file.read_text())
    except json.JSONDecodeError as err:
        return LoadResult(None, _json_error(timing_file, err))
    if not isinstance(timing_payload, dict):
        return LoadResult(None, "timing.json: expected JSON object")
    try:
        wall_time = Timing.model_validate(timing_payload).wall_time
    except ValidationError as err:
        return LoadResult(None, _validation_error(timing_file, err))

    return LoadResult(RunData(stats=stats, eval_raw=eval_raw, wall_time=wall_time))


def load_run_data(run_dir: Path, commit: str) -> RunData | None:
    """Compatibility wrapper returning only successfully loaded data."""
    return load_run_data_result(run_dir, commit).data
