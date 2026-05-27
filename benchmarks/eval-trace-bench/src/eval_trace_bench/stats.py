"""Per-commit stats/timing/eval loader.

The heavy lifting is in `models.py` (Pydantic).  This module layers a
thin on-disk loader on top that knows about the
`{run}/{commit}/stats.json` / `timing.json` / `eval.json` file layout
`generate --with-stats` and `generate --with-debug` emit. Plain wall-time runs
intentionally omit `stats.json`; those load with default-zero stats so soundness
and wall-time comparison still work.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any, cast

from pydantic import ValidationError

from .layout import MANIFEST_FILE
from .models import RunStats, Timing, load_stats


@dataclass(frozen=True)
class RunData:
    """Per-commit data pulled from a single `{run}/{commit}/` directory."""

    stats: RunStats
    stats_present: bool
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

    Array-valued children (`primops`, `functions`) are not flattened —
    they're sequences of records, not sub-trees, and `logs` surfaces
    them separately.
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


def _manifest_expects_stats(run_dir: Path) -> bool | None:
    manifest = run_dir / MANIFEST_FILE
    if not manifest.is_file():
        return None
    try:
        payload: Any = json.loads(manifest.read_text())
    except json.JSONDecodeError:
        return None
    if not isinstance(payload, dict):
        return None
    typed_payload = cast(dict[str, Any], payload)
    raw = typed_payload.get("withStats")
    return raw if isinstance(raw, bool) else None


def _payload_bool(payload: dict[str, Any], key: str) -> bool | None:
    raw = payload.get(key)
    return raw if isinstance(raw, bool) else None


def load_run_data_result(run_dir: Path, commit: str) -> LoadResult:
    """Load `{run_dir}/{commit}/` eval output, timing, and optional stats."""
    commit_dir = run_dir / commit
    stats_file = commit_dir / "stats.json"
    expects_stats = _manifest_expects_stats(run_dir)
    stats_present = False
    if expects_stats is False:
        stats = load_stats({})
    elif stats_file.exists():
        stats_present = True
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
    else:
        if expects_stats is True:
            return LoadResult(None, "missing stats.json")
        stats = load_stats({})

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
    typed_timing_payload = cast(dict[str, Any], timing_payload)
    timing_expects_stats = _payload_bool(typed_timing_payload, "withStats")
    if expects_stats is not None and timing_expects_stats is not None:
        if timing_expects_stats != expects_stats:
            return LoadResult(None, "timing.json: withStats disagrees with manifest")
    elif expects_stats is None:
        if timing_expects_stats is False and stats_present:
            stats = load_stats({})
            stats_present = False
        elif timing_expects_stats is True and not stats_present:
            return LoadResult(None, "missing stats.json")
    try:
        wall_time = Timing.model_validate(typed_timing_payload).wall_time
    except ValidationError as err:
        return LoadResult(None, _validation_error(timing_file, err))

    return LoadResult(
        RunData(
            stats=stats,
            stats_present=stats_present,
            eval_raw=eval_raw,
            wall_time=wall_time,
        )
    )


def load_run_data(run_dir: Path, commit: str) -> RunData | None:
    """Compatibility wrapper returning only successfully loaded data."""
    return load_run_data_result(run_dir, commit).data
