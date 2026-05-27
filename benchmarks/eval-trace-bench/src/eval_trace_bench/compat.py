"""Run-manifest compatibility checks for analysis commands."""

from __future__ import annotations

from typing import Any, cast

from .discovery import Run, classify_run_mode, run_manifest_commit_order, run_manifest_payload

STATEFUL_MODES = {"cold", "hot", "warm"}

WORKLOAD_MANIFEST_KEYS: tuple[str, ...] = (
    "schemaVersion",
    "suite",
    "workloads",
    "sourceRepo",
    "withDebug",
    "withStats",
    "measurementMode",
    "extraEvalArgs",
)

REQUIRED_WORKLOAD_MANIFEST_KEYS: tuple[str, ...] = (
    "schemaVersion",
    "run",
    "suite",
    "workloads",
    "sourceRepo",
    "commitOrder",
    "withDebug",
    "withStats",
    "measurementMode",
)

BENCHMARK_PROVENANCE_MANIFEST_KEYS: tuple[str, ...] = (
    "benchmarkSubject",
    "benchmarkEnvironment",
)

SOURCE_PROVENANCE_MANIFEST_KEYS: tuple[str, ...] = ("sourceRepoProvenance",)


def _run_named(runs: list[Run], name: str) -> Run | None:
    return next((run for run in runs if run.name == name), None)


def run_manifest_readability_issue(runs: list[Run], run_names: list[str]) -> str | None:
    selected = [run for name in run_names if (run := _run_named(runs, name)) is not None]
    for run in selected:
        if run_manifest_payload(run) is None:
            return (
                f"{run.name} has no readable manifest; refuse to compare runs "
                "whose workload/provenance cannot be checked."
            )
    return None


def _manifest_missing_keys_issue(
    run: Run,
    payload: dict[str, Any],
    keys: tuple[str, ...],
    *,
    scope: str,
) -> str | None:
    missing = [key for key in keys if key not in payload]
    if not missing:
        return None
    return (
        f"{run.name} manifest is missing required {scope} fields "
        f"{', '.join(repr(key) for key in missing)}; rerun generate with the current harness."
    )


def _manifest_run_identity_issue(run: Run, payload: dict[str, Any]) -> str | None:
    if payload.get("run") == run.name:
        return None
    return (
        f"{run.name} manifest run field is {payload.get('run')!r}; "
        "refuse to compare a run directory with a mismatched manifest."
    )


def _benchmark_subject_identity(subject: Any) -> Any:
    if not isinstance(subject, dict):
        return subject
    subject_dict = cast(dict[str, Any], subject)
    if subject_dict.get("nixRootKind") != "result-wrapper":
        return subject_dict
    return {
        key: subject_dict.get(key)
        for key in ("nixBinSha256", "nixStorePath")
        if key in subject_dict
    }


def _source_provenance_identity(provenance: Any) -> Any:
    if not isinstance(provenance, dict):
        return provenance
    provenance_dict = cast(dict[str, Any], provenance)
    dirty = provenance_dict.get("dirty")
    if dirty is False:
        clean_identity: dict[str, Any] = {
            "isGitRepository": provenance_dict.get("isGitRepository"),
            "dirty": False,
        }
        for key in ("diffSha256", "dirtyTreeSha256"):
            if key in provenance_dict:
                clean_identity[key] = provenance_dict[key]
        return clean_identity
    return provenance_dict


def _benchmark_provenance_value(payload: dict[str, Any], key: str) -> Any:
    value = payload.get(key)
    if key == "benchmarkSubject":
        return _benchmark_subject_identity(value)
    return value


def run_manifest_shape_issue(runs: list[Run], run_names: list[str]) -> str | None:
    """Return why selected runs do not describe the same workload shape.

    This is the part of compatibility that must hold even for an intentional
    baseline/prototype comparison. The measured source window and mode must
    match, or timing/soundness rows are not measuring the same workload.
    """
    selected = [run for name in run_names if (run := _run_named(runs, name)) is not None]
    payloads: list[tuple[Run, dict[str, Any]]] = []
    for run in selected:
        payload = run_manifest_payload(run)
        if payload is None:
            return run_manifest_readability_issue(runs, run_names)
        if issue := _manifest_missing_keys_issue(
            run,
            payload,
            REQUIRED_WORKLOAD_MANIFEST_KEYS,
            scope="workload",
        ):
            return issue
        if issue := _manifest_run_identity_issue(run, payload):
            return issue
        payloads.append((run, payload))
    if len(payloads) <= 1:
        return None

    baseline_run, baseline_payload = payloads[0]
    for run, payload in payloads[1:]:
        for key in WORKLOAD_MANIFEST_KEYS:
            if payload.get(key) != baseline_payload.get(key):
                return (
                    f"{run.name} manifest field {key!r} differs from "
                    f"{baseline_run.name}; refuse to compare incompatible runs."
                )
    return None


def run_manifest_provenance_issue(
    runs: list[Run],
    run_names: list[str],
    *,
    allow_provenance_mismatch: bool,
) -> str | None:
    selected = [run for name in run_names if (run := _run_named(runs, name)) is not None]
    payloads: list[tuple[Run, dict[str, Any]]] = []
    for run in selected:
        payload = run_manifest_payload(run)
        if payload is None:
            return run_manifest_readability_issue(runs, run_names)
        if issue := _manifest_missing_keys_issue(
            run,
            payload,
            BENCHMARK_PROVENANCE_MANIFEST_KEYS,
            scope="benchmark provenance",
        ):
            return issue
        if issue := _manifest_missing_keys_issue(
            run,
            payload,
            SOURCE_PROVENANCE_MANIFEST_KEYS,
            scope="source provenance",
        ):
            return issue
        if issue := _manifest_run_identity_issue(run, payload):
            return issue
        payloads.append((run, payload))
    if len(payloads) <= 1:
        return None

    baseline_run, baseline_payload = payloads[0]
    for run, payload in payloads[1:]:
        for key in SOURCE_PROVENANCE_MANIFEST_KEYS:
            if _source_provenance_identity(payload.get(key)) != _source_provenance_identity(
                baseline_payload.get(key)
            ):
                return (
                    f"{run.name} manifest field {key!r} differs from "
                    f"{baseline_run.name}; source provenance must match for "
                    "benchmark comparisons."
                )

    if allow_provenance_mismatch:
        return None

    for run, payload in payloads[1:]:
        for key in BENCHMARK_PROVENANCE_MANIFEST_KEYS:
            if _benchmark_provenance_value(payload, key) != _benchmark_provenance_value(
                baseline_payload,
                key,
            ):
                return (
                    f"{run.name} manifest field {key!r} differs from "
                    f"{baseline_run.name}; pass --allow-provenance-mismatch only "
                    "for an intentional comparison across benchmark binaries or "
                    "benchmark environment flags."
                )
    return None


def stateful_manifest_order_issue(
    runs: list[Run],
    run_names: list[str],
    *,
    allow_intersection: bool,
    comparison_commits: list[str] | None = None,
) -> str | None:
    if allow_intersection:
        return None

    selected = [run for name in run_names if (run := _run_named(runs, name)) is not None]
    stateful = [run for run in selected if classify_run_mode(run.name) in STATEFUL_MODES]
    if not stateful:
        return None

    baseline = stateful[0]
    baseline_order = run_manifest_commit_order(baseline)
    if not baseline_order:
        return (
            "stateful comparisons require manifest commitOrder on every stateful "
            "run; rerun generate with the current harness or pass "
            "--allow-intersection for a legacy best-effort comparison."
        )

    if comparison_commits is not None:
        for run in stateful:
            order = run_manifest_commit_order(run)
            if not order:
                return (
                    "stateful comparisons require manifest commitOrder on every "
                    "stateful run; rerun generate with the current harness or pass "
                    "--allow-intersection for a legacy best-effort comparison."
                )
            if order[: len(comparison_commits)] != comparison_commits:
                return (
                    "stateful comparisons require each run's manifest commitOrder "
                    "to start with the selected comparison window; different cache "
                    "prefix state means different stateful workloads. Pass "
                    "--allow-intersection only for a legacy best-effort comparison."
                )
        return None

    for run in stateful[1:]:
        order = run_manifest_commit_order(run)
        if not order:
            return (
                "stateful comparisons require manifest commitOrder on every "
                "stateful run; rerun generate with the current harness or pass "
                "--allow-intersection for a legacy best-effort comparison."
            )
        if order != baseline_order:
            return (
                "stateful comparisons require identical manifest commitOrder; "
                "different processing order means different cache prefix state. "
                "Pass --allow-intersection only for a legacy best-effort comparison."
            )
    return None


def run_analysis_compatibility_issue(
    runs: list[Run],
    run_names: list[str],
    *,
    allow_intersection: bool = False,
    allow_provenance_mismatch: bool = False,
    comparison_commits: list[str] | None = None,
) -> str | None:
    return (
        run_manifest_readability_issue(runs, run_names)
        or run_manifest_shape_issue(runs, run_names)
        or run_manifest_provenance_issue(
            runs,
            run_names,
            allow_provenance_mismatch=allow_provenance_mismatch,
        )
        or stateful_manifest_order_issue(
            runs,
            run_names,
            allow_intersection=allow_intersection,
            comparison_commits=comparison_commits,
        )
    )
