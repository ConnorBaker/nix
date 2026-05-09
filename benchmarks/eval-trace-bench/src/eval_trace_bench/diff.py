"""JSON diff helpers used by the soundness check."""

from __future__ import annotations

import json
from typing import Any


def json_diff(a: Any, b: Any, path: str = "") -> list[str]:
    """Recursively compare two JSON values and describe their differences."""
    if a == b:
        return []
    if type(a) is not type(b):
        return [f"{path or '<root>'}: type {type(a).__name__} vs {type(b).__name__}"]
    if isinstance(a, dict) and isinstance(b, dict):
        a_dict: dict[str, Any] = a  # pyright: ignore[reportAssignmentType, reportUnknownVariableType]
        b_dict: dict[str, Any] = b  # pyright: ignore[reportAssignmentType, reportUnknownVariableType]
        diffs: list[str] = []
        for k in sorted(set(a_dict) | set(b_dict)):
            cp = f"{path}.{k}" if path else k
            if k not in a_dict:
                diffs.append(f"{cp}: added")
            elif k not in b_dict:
                diffs.append(f"{cp}: removed")
            else:
                diffs.extend(json_diff(a_dict[k], b_dict[k], cp))
        return diffs
    if isinstance(a, list) and isinstance(b, list):
        a_list: list[Any] = a  # pyright: ignore[reportAssignmentType, reportUnknownVariableType]
        b_list: list[Any] = b  # pyright: ignore[reportAssignmentType, reportUnknownVariableType]
        if len(a_list) != len(b_list):
            return [f"{path or '<root>'}: length {len(a_list)} vs {len(b_list)}"]
        diffs = []
        for i, (x, y) in enumerate(zip(a_list, b_list, strict=False)):
            diffs.extend(json_diff(x, y, f"{path}[{i}]"))
        return diffs
    return [f"{path or '<root>'}: {a!r} vs {b!r}"]


def eval_diff_detail(ref_raw: str | None, other_raw: str | None, max_diffs: int = 5) -> str:
    if ref_raw is None or other_raw is None:
        return " (one side missing)"
    try:
        ref_obj: Any = json.loads(ref_raw)
        other_obj: Any = json.loads(other_raw)
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
