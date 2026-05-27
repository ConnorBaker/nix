from __future__ import annotations

import json
import subprocess
from pathlib import Path
from urllib.parse import parse_qs

from rich.console import Console

from eval_trace_bench.cliutil import format_dataset_issues
from eval_trace_bench.compat import run_analysis_compatibility_issue, run_manifest_shape_issue
from eval_trace_bench.dataframe import Record, load_dataset
from eval_trace_bench.discovery import (
    Run,
    classify_run_mode,
    expects_cache_misses,
    manifest_commit_order,
)
from eval_trace_bench.layout import RESULTS_DIR, RunId, RunPaths
from eval_trace_bench.logparse import analyze_recording_redundancy
from eval_trace_bench.models import load_stats
from eval_trace_bench.stats import RunData, load_run_data_result
from eval_trace_bench.subcommands import classify_cmd, generate_cmd
from eval_trace_bench.subcommands.generate_cmd import (
    DEFAULT_DISABLE_STRUCTURAL_VARIANT_RECOVERY_ARG,
    DEFAULT_ENABLE_STRUCTURAL_VARIANT_MISMATCH_TELEMETRY_ARG,
    EVAL_TRACE_HASH_ALGORITHM_NIX_OPTION,
    _absolute_path,
    _benchmark_environment_provenance,
    _commits_from_file,
    _eval_trace_hash_algorithm_args,
    _extra_eval_args_for_run,
    _manifest_resume_compatible,
    _mode_for_run_id,
    _next_unused_run_number,
    _parse_selector_csv,
    _resolve_benchmark_nix_bin,
    _run_id,
    _state_env,
    _store_uri,
    ensure_run_state,
    has_completed_outputs,
    measurement_env,
    nixpkgs_release_cases,
    reset_state_from,
)
from eval_trace_bench.subcommands.logs_cmd import _distribution
from eval_trace_bench.subcommands.pairwise_cmd import (
    _pairwise_candidate_commits,
    _pairwise_comparison_commits,
    _pairwise_no_pairs_issue,
    _stateful_pairwise_order_issue,
)
from eval_trace_bench.subcommands.runs_cmd import (
    _baseline_counterpart,
    _commits_from_reference_manifest,
    _reference_issue,
    _runs_including_baseline_run_if_available,
    _runs_including_reference_if_available,
)

COMMIT_A = "a" * 40
COMMIT_B = "b" * 40
COMMIT_C = "c" * 40


def _bench_manifest(run: str, commit_order: list[str]) -> dict[str, object]:
    return {
        "schemaVersion": 1,
        "run": run,
        "suite": "suite",
        "workloads": ["workload"],
        "sourceRepo": "/repo",
        "sourceRef": "master",
        "commitOrder": commit_order,
        "withDebug": False,
        "withStats": False,
        "measurementMode": "wall-only",
        "benchmarkSubject": {"nixStorePath": "/nix/store/current"},
        "benchmarkEnvironment": {},
        "sourceRepoProvenance": {"head": COMMIT_A, "dirty": False},
    }


def test_run_id_resolves_under_results_directory(tmp_path):
    assert RunId("cold", 0).resolve(tmp_path) == tmp_path / RESULTS_DIR / "cold" / "0"
    assert RunId("reference", None).resolve(tmp_path) == tmp_path / RESULTS_DIR / "reference"


def test_debug_run_ids_use_separate_names_but_keep_modes():
    cold = _run_id(
        "nixpkgs-release",
        "nixpkgs-release",
        "cold",
        7,
        with_debug=True,
        with_stats=True,
    )
    reference = _run_id(
        "nixpkgs-release",
        "nixpkgs-release",
        "reference",
        7,
        with_debug=True,
        with_stats=True,
    )

    assert cold.display() == "cold-debug/7"
    assert reference.display() == "reference-debug"
    assert _mode_for_run_id(cold) == "cold"
    assert _mode_for_run_id(reference) == "reference"


def test_stats_run_ids_use_separate_names_but_keep_modes():
    cold = _run_id(
        "nixpkgs-release",
        "nixpkgs-release",
        "cold",
        7,
        with_debug=False,
        with_stats=True,
    )
    reference = _run_id(
        "nixpkgs-release",
        "nixpkgs-release",
        "reference",
        7,
        with_debug=False,
        with_stats=True,
    )

    assert cold.display() == "cold-stats/7"
    assert reference.display() == "reference-stats"
    assert _mode_for_run_id(cold) == "cold"
    assert _mode_for_run_id(reference) == "reference"


def test_stats_run_names_classify_like_underlying_modes():
    assert classify_run_mode("cold-stats/7") == "cold"
    assert classify_run_mode("hot-stats/7") == "hot"
    assert classify_run_mode("reference-stats") == "reference"
    assert expects_cache_misses("cold-stats/7")
    assert not expects_cache_misses("hot-stats/7")


def test_next_unused_run_number_scans_planned_run_names(tmp_path):
    (tmp_path / RESULTS_DIR / "cold" / "0").mkdir(parents=True)
    (tmp_path / RESULTS_DIR / "hot" / "2").mkdir(parents=True)
    (tmp_path / RESULTS_DIR / "cold" / "scratch").mkdir()
    (tmp_path / RESULTS_DIR / "cold-debug" / "9").mkdir(parents=True)
    (tmp_path / RESULTS_DIR / "cold" / "0" / "manifest.json").write_text("{}")
    (tmp_path / RESULTS_DIR / "hot" / "2" / COMMIT_A).mkdir()
    (tmp_path / RESULTS_DIR / "cold-debug" / "9" / "manifest.json").write_text("{}")

    assert _next_unused_run_number(tmp_path, [RunId("cold", 0), RunId("hot", 0)]) == 3
    assert _next_unused_run_number(tmp_path, [RunId("cold-debug", 0)]) == 10
    assert _next_unused_run_number(tmp_path, [RunId("missing", 0)]) == 0


def test_next_unused_run_number_ignores_empty_manifestless_dirs(tmp_path):
    (tmp_path / RESULTS_DIR / "cold" / "0").mkdir(parents=True)

    assert _next_unused_run_number(tmp_path, [RunId("cold", 0)]) == 0


def test_resolve_benchmark_nix_bin_locks_result_symlink(tmp_path):
    store_like = tmp_path / "nix-store-result"
    bin_dir = store_like / "bin"
    bin_dir.mkdir(parents=True)
    nix_bin = bin_dir / "nix"
    nix_bin.write_text("#!/bin/sh\n")
    result = tmp_path / "result"
    result.symlink_to(store_like, target_is_directory=True)

    assert _resolve_benchmark_nix_bin(tmp_path) == nix_bin.resolve(strict=True)


def test_plain_completed_outputs_do_not_require_stats(tmp_path):
    stats = tmp_path / "stats.json"
    result = tmp_path / "eval.json"
    stderr_log = tmp_path / "stderr.log"
    timing = tmp_path / "timing.json"
    result.write_text('{"ok": true}')
    stderr_log.write_text("")
    timing.write_text('{"wallTime": 1.25}')

    assert has_completed_outputs(stats, result, stderr_log, timing, with_stats=False)
    assert not has_completed_outputs(stats, result, stderr_log, timing, with_stats=True)


def test_completed_outputs_require_wall_time(tmp_path):
    stats = tmp_path / "stats.json"
    result = tmp_path / "eval.json"
    stderr_log = tmp_path / "stderr.log"
    timing = tmp_path / "timing.json"
    result.write_text('{"ok": true}')
    stderr_log.write_text("")
    timing.write_text('{"withStats": false}')

    assert not has_completed_outputs(stats, result, stderr_log, timing, with_stats=False)


def test_completed_outputs_require_positive_finite_numeric_wall_time(tmp_path):
    stats = tmp_path / "stats.json"
    result = tmp_path / "eval.json"
    stderr_log = tmp_path / "stderr.log"
    timing = tmp_path / "timing.json"
    result.write_text('{"ok": true}')
    stderr_log.write_text("")

    for payload in (
        '{"wallTime": "1.25"}',
        '{"wallTime": 0}',
        '{"wallTime": -1}',
        '{"wallTime": NaN}',
        '{"wallTime": Infinity}',
    ):
        timing.write_text(payload)
        assert not has_completed_outputs(stats, result, stderr_log, timing, with_stats=False)


def test_plain_run_data_loads_eval_and_timing_without_stats(tmp_path):
    commit_dir = tmp_path / COMMIT_A
    commit_dir.mkdir()
    (commit_dir / "eval.json").write_text('{"ok": true}')
    (commit_dir / "timing.json").write_text('{"wallTime": 1.25}')

    result = load_run_data_result(tmp_path, COMMIT_A)

    assert result.error is None
    assert result.data is not None
    assert result.data.eval_raw == '{"ok": true}'
    assert result.data.wall_time == 1.25
    assert not result.data.stats_present
    assert result.data.stats.raw == {}


def test_wall_only_manifest_ignores_stale_stats(tmp_path):
    (tmp_path / "manifest.json").write_text(json.dumps({"withStats": False}))
    commit_dir = tmp_path / COMMIT_A
    commit_dir.mkdir()
    (commit_dir / "eval.json").write_text('{"ok": true}')
    (commit_dir / "timing.json").write_text('{"wallTime": 1.25, "withStats": false}')
    (commit_dir / "stats.json").write_text('{"evalTrace": {"hits": 99}}')

    result = load_run_data_result(tmp_path, COMMIT_A)

    assert result.error is None
    assert result.data is not None
    assert not result.data.stats_present
    assert result.data.stats.raw == {}


def test_wall_only_timing_ignores_stale_stats_without_manifest(tmp_path):
    commit_dir = tmp_path / COMMIT_A
    commit_dir.mkdir()
    (commit_dir / "eval.json").write_text('{"ok": true}')
    (commit_dir / "timing.json").write_text('{"wallTime": 1.25, "withStats": false}')
    (commit_dir / "stats.json").write_text('{"evalTrace": {"hits": 99}}')

    result = load_run_data_result(tmp_path, COMMIT_A)

    assert result.error is None
    assert result.data is not None
    assert not result.data.stats_present
    assert result.data.stats.raw == {}


def test_stats_manifest_requires_stats_file(tmp_path):
    (tmp_path / "manifest.json").write_text(json.dumps({"withStats": True}))
    commit_dir = tmp_path / COMMIT_A
    commit_dir.mkdir()
    (commit_dir / "eval.json").write_text('{"ok": true}')
    (commit_dir / "timing.json").write_text('{"wallTime": 1.25, "withStats": true}')

    result = load_run_data_result(tmp_path, COMMIT_A)

    assert result.data is None
    assert result.error == "missing stats.json"


def test_timing_stats_mode_must_match_manifest(tmp_path):
    (tmp_path / "manifest.json").write_text(json.dumps({"withStats": False}))
    commit_dir = tmp_path / COMMIT_A
    commit_dir.mkdir()
    (commit_dir / "eval.json").write_text('{"ok": true}')
    (commit_dir / "timing.json").write_text('{"wallTime": 1.25, "withStats": true}')

    result = load_run_data_result(tmp_path, COMMIT_A)

    assert result.data is None
    assert result.error == "timing.json: withStats disagrees with manifest"


def test_reset_state_from_replaces_stale_hot_state(tmp_path):
    src = RunPaths(tmp_path / "cold" / "0")
    dst = RunPaths(tmp_path / "hot" / "0")
    ensure_run_state(src)
    ensure_run_state(dst)
    (src.cache_dir / "fresh.sqlite").write_text("fresh")
    (dst.cache_dir / "stale.sqlite").write_text("stale")

    reset_state_from(src, dst)

    assert (dst.cache_dir / "fresh.sqlite").read_text() == "fresh"
    assert not (dst.cache_dir / "stale.sqlite").exists()


def test_warm_run_resets_stale_state_without_outputs(tmp_path, monkeypatch):
    planned = {
        "schemaVersion": 1,
        "run": "warm/0",
        "suite": None,
        "workloads": [],
        "sourceRepo": "/repo",
        "sourceRef": "master",
        "commitOrder": [],
        "withDebug": False,
        "withStats": False,
        "measurementMode": "wall-only",
        "benchmarkSubject": {"nixStorePath": "/nix/store/current"},
        "benchmarkEnvironment": {},
        "sourceRepoProvenance": {"head": COMMIT_A},
    }
    run_id = RunId("warm", 0)
    run_paths = RunPaths(run_dir=run_id.resolve(tmp_path))
    ensure_run_state(run_paths)
    stale = run_paths.cache_dir / "stale.sqlite"
    stale_store = run_paths.nix_store_dir / "stale-store"
    stale.write_text("stale")
    stale_store.write_text("stale")
    monkeypatch.setattr(
        generate_cmd,
        "build_run_manifest_payload",
        lambda *args, **kwargs: planned,
    )

    generate_cmd.execute_run(
        Path("/does-not-matter"),
        tmp_path,
        run_id,
        [],
        hot_source=None,
        source_repo=tmp_path,
        source_ref="master",
    )

    assert run_paths.cache_dir.is_dir()
    assert not stale.exists()
    assert not stale_store.exists()


def test_store_uri_threads_root_state_and_log_dirs(tmp_path):
    root = tmp_path / "store root"

    uri = _store_uri(root)

    assert uri.startswith("local?")
    params = {key: values[0] for key, values in parse_qs(uri.split("?", 1)[1]).items()}
    assert params == {
        "root": str(root),
        "state": str(root / "nix" / "var" / "nix"),
        "log": str(root / "nix" / "var" / "log" / "nix"),
    }


def test_state_env_threads_cache_and_global_state_dirs(tmp_path):
    run_paths = RunPaths(tmp_path / "cold" / "0")

    env = _state_env(run_paths)

    assert env == {
        "NIX_CACHE_HOME": str(run_paths.cache_dir),
        "NIX_STATE_HOME": str(run_paths.nix_state_dir),
        "NIX_STATE_DIR": str(run_paths.nix_state_dir),
        "NIX_LOG_DIR": str(run_paths.nix_log_dir),
    }


def test_plain_measurement_env_scrubs_stats_vars(tmp_path):
    env = measurement_env(
        {"NIX_SHOW_STATS": "1", "NIX_SHOW_STATS_PATH": "old", "KEEP": "yes"},
        tmp_path / "stats.json",
        with_stats=False,
    )

    assert env == {"KEEP": "yes"}


def test_stats_measurement_env_sets_stats_vars(tmp_path):
    env = measurement_env(
        {"NIX_SHOW_STATS": "0", "NIX_SHOW_STATS_PATH": "old", "KEEP": "yes"},
        tmp_path / "stats.json",
        with_stats=True,
    )

    assert env == {
        "KEEP": "yes",
        "NIX_SHOW_STATS": "1",
        "NIX_SHOW_STATS_PATH": str(tmp_path / "stats.json"),
    }


def test_manifest_resume_compatibility_rejects_changed_subject():
    existing = {
        "schemaVersion": 1,
        "run": "cold/0",
        "suite": "nixpkgs-release",
        "workloads": ["nixpkgs-release"],
        "sourceRepo": "/repo",
        "sourceRef": "master",
        "commitOrder": [COMMIT_A],
        "withDebug": False,
        "withStats": False,
        "measurementMode": "wall-only",
        "benchmarkSubject": {"nixStorePath": "/nix/store/old"},
        "benchmarkEnvironment": {},
        "sourceRepoProvenance": {"head": COMMIT_A},
    }
    planned = {
        **existing,
        "benchmarkSubject": {"nixStorePath": "/nix/store/new"},
    }

    assert _manifest_resume_compatible(existing, existing)
    assert not _manifest_resume_compatible(existing, planned)


def test_execute_run_rejects_incompatible_manifest_without_overwriting(tmp_path, monkeypatch):
    existing = {
        "schemaVersion": 1,
        "run": "cold/0",
        "suite": None,
        "workloads": [],
        "sourceRepo": "/repo",
        "sourceRef": "master",
        "commitOrder": [],
        "withDebug": False,
        "withStats": False,
        "measurementMode": "wall-only",
        "benchmarkSubject": {"nixStorePath": "/nix/store/old"},
        "benchmarkEnvironment": {},
        "sourceRepoProvenance": {"head": COMMIT_A},
    }
    planned = {
        **existing,
        "benchmarkSubject": {"nixStorePath": "/nix/store/new"},
    }
    run_id = RunId("cold", 0)
    run_paths = RunPaths(run_dir=run_id.resolve(tmp_path))
    run_paths.run_dir.mkdir(parents=True)
    run_paths.manifest_file.write_text(json.dumps(existing, indent=2) + "\n")
    monkeypatch.setattr(
        generate_cmd,
        "build_run_manifest_payload",
        lambda *args, **kwargs: planned,
    )
    monkeypatch.setattr(
        generate_cmd,
        "_git_repo_provenance",
        lambda *args, **kwargs: {"head": COMMIT_A},
    )

    try:
        generate_cmd.execute_run(
            Path("/does-not-matter"),
            tmp_path,
            run_id,
            [],
            hot_source=None,
            source_repo=tmp_path,
            source_ref="master",
        )
    except SystemExit as exc:
        assert "different measurement" in str(exc)
    else:
        raise AssertionError("expected incompatible manifest to abort")

    assert json.loads(run_paths.manifest_file.read_text()) == existing


def test_execute_run_rejects_unmanifested_outputs(tmp_path, monkeypatch):
    planned = {
        "schemaVersion": 1,
        "run": "cold/0",
        "suite": "suite",
        "workloads": ["workload"],
        "sourceRepo": "/repo",
        "sourceRef": "master",
        "commitOrder": [COMMIT_A],
        "withDebug": False,
        "withStats": False,
        "measurementMode": "wall-only",
        "benchmarkSubject": {"nixStorePath": "/nix/store/current"},
        "benchmarkEnvironment": {},
        "sourceRepoProvenance": {"head": COMMIT_A},
    }
    run_id = RunId("cold", 0)
    run_paths = RunPaths(run_dir=run_id.resolve(tmp_path))
    output_dir = run_paths.commit_dir(COMMIT_A)
    output_dir.mkdir(parents=True)
    (output_dir / "eval.json").write_text("{}")
    monkeypatch.setattr(
        generate_cmd,
        "build_run_manifest_payload",
        lambda *args, **kwargs: planned,
    )
    monkeypatch.setattr(
        generate_cmd,
        "_git_repo_provenance",
        lambda *args, **kwargs: {"head": COMMIT_A},
    )

    try:
        generate_cmd.execute_run(
            Path("/does-not-matter"),
            tmp_path,
            run_id,
            [generate_cmd.EvalCase(COMMIT_A, "case", (), "suite", "workload")],
            hot_source=None,
            source_repo=tmp_path,
            source_ref="master",
        )
    except SystemExit as exc:
        assert "no readable manifest" in str(exc)
    else:
        raise AssertionError("expected unmanifested outputs to abort")

    assert not run_paths.manifest_file.exists()


def test_execute_run_rejects_unmanifested_outputs_outside_planned_commits(tmp_path, monkeypatch):
    planned = {
        "schemaVersion": 1,
        "run": "cold/0",
        "suite": "suite",
        "workloads": ["workload"],
        "sourceRepo": "/repo",
        "sourceRef": "master",
        "commitOrder": [COMMIT_A],
        "withDebug": False,
        "withStats": False,
        "measurementMode": "wall-only",
        "benchmarkSubject": {"nixStorePath": "/nix/store/current"},
        "benchmarkEnvironment": {},
        "sourceRepoProvenance": {"head": COMMIT_A},
    }
    run_id = RunId("cold", 0)
    run_paths = RunPaths(run_dir=run_id.resolve(tmp_path))
    stale_output = run_paths.commit_dir(COMMIT_B)
    stale_output.mkdir(parents=True)
    (stale_output / "eval.json").write_text("{}")
    monkeypatch.setattr(
        generate_cmd,
        "build_run_manifest_payload",
        lambda *args, **kwargs: planned,
    )
    monkeypatch.setattr(
        generate_cmd,
        "_git_repo_provenance",
        lambda *args, **kwargs: {"head": COMMIT_A},
    )

    try:
        generate_cmd.execute_run(
            Path("/does-not-matter"),
            tmp_path,
            run_id,
            [generate_cmd.EvalCase(COMMIT_A, "case", (), "suite", "workload")],
            hot_source=None,
            source_repo=tmp_path,
            source_ref="master",
        )
    except SystemExit as exc:
        assert "no readable manifest" in str(exc)
    else:
        raise AssertionError("expected stale unmanifested outputs to abort")


def test_execute_run_rejects_stateful_output_resume(tmp_path, monkeypatch):
    planned = {
        "schemaVersion": 1,
        "run": "cold/0",
        "suite": "suite",
        "workloads": ["workload"],
        "sourceRepo": "/repo",
        "sourceRef": "master",
        "commitOrder": [COMMIT_A],
        "withDebug": False,
        "withStats": False,
        "measurementMode": "wall-only",
        "benchmarkSubject": {"nixStorePath": "/nix/store/current"},
        "benchmarkEnvironment": {},
        "sourceRepoProvenance": {"head": COMMIT_A},
    }
    run_id = RunId("cold", 0)
    run_paths = RunPaths(run_dir=run_id.resolve(tmp_path))
    run_paths.run_dir.mkdir(parents=True)
    run_paths.manifest_file.write_text(json.dumps(planned, indent=2) + "\n")
    output_dir = run_paths.commit_dir(COMMIT_A)
    output_dir.mkdir()
    (output_dir / "eval.json").write_text("{}")
    monkeypatch.setattr(
        generate_cmd,
        "build_run_manifest_payload",
        lambda *args, **kwargs: planned,
    )
    monkeypatch.setattr(
        generate_cmd,
        "_git_repo_provenance",
        lambda *args, **kwargs: {"head": COMMIT_A},
    )

    try:
        generate_cmd.execute_run(
            Path("/does-not-matter"),
            tmp_path,
            run_id,
            [generate_cmd.EvalCase(COMMIT_A, "case", (), "suite", "workload")],
            hot_source=None,
            source_repo=tmp_path,
            source_ref="master",
        )
    except SystemExit as exc:
        assert "stateful run" in str(exc)
    else:
        raise AssertionError("expected stateful output resume to abort")


def test_execute_run_rejects_stateful_output_resume_outside_planned_commits(tmp_path, monkeypatch):
    planned = {
        "schemaVersion": 1,
        "run": "cold/0",
        "suite": "suite",
        "workloads": ["workload"],
        "sourceRepo": "/repo",
        "sourceRef": "master",
        "commitOrder": [COMMIT_A],
        "withDebug": False,
        "withStats": False,
        "measurementMode": "wall-only",
        "benchmarkSubject": {"nixStorePath": "/nix/store/current"},
        "benchmarkEnvironment": {},
        "sourceRepoProvenance": {"head": COMMIT_A},
    }
    run_id = RunId("cold", 0)
    run_paths = RunPaths(run_dir=run_id.resolve(tmp_path))
    run_paths.run_dir.mkdir(parents=True)
    run_paths.manifest_file.write_text(json.dumps(planned, indent=2) + "\n")
    stale_output = run_paths.commit_dir(COMMIT_B)
    stale_output.mkdir()
    (stale_output / "eval.json").write_text("{}")
    monkeypatch.setattr(
        generate_cmd,
        "build_run_manifest_payload",
        lambda *args, **kwargs: planned,
    )

    try:
        generate_cmd.execute_run(
            Path("/does-not-matter"),
            tmp_path,
            run_id,
            [generate_cmd.EvalCase(COMMIT_A, "case", (), "suite", "workload")],
            hot_source=None,
            source_repo=tmp_path,
            source_ref="master",
        )
    except SystemExit as exc:
        assert "stateful run" in str(exc)
    else:
        raise AssertionError("expected stale stateful output resume to abort")


def test_execute_run_rejects_hot_source_that_is_not_cold(tmp_path, monkeypatch):
    planned = {
        "schemaVersion": 1,
        "run": "hot/1",
        "suite": "suite",
        "workloads": ["workload"],
        "sourceRepo": "/repo",
        "sourceRef": "master",
        "commitOrder": [],
        "withDebug": False,
        "withStats": False,
        "measurementMode": "wall-only",
        "benchmarkSubject": {"nixStorePath": "/nix/store/current"},
        "benchmarkEnvironment": {},
        "sourceRepoProvenance": {"head": COMMIT_A},
    }
    source = RunId("hot", 0)
    source_paths = RunPaths(run_dir=source.resolve(tmp_path))
    source_paths.state_dir.mkdir(parents=True)
    monkeypatch.setattr(
        generate_cmd,
        "build_run_manifest_payload",
        lambda *args, **kwargs: planned,
    )

    try:
        generate_cmd.execute_run(
            Path("/does-not-matter"),
            tmp_path,
            RunId("hot", 1),
            [],
            hot_source=source,
            source_repo=tmp_path,
            source_ref="master",
        )
    except SystemExit as exc:
        assert "--hot-from must reference a cold run" in str(exc)
    else:
        raise AssertionError("expected non-cold hot source to abort")


def test_execute_run_rejects_hot_source_manifest_run_mismatch(tmp_path, monkeypatch):
    source_manifest = {
        "schemaVersion": 1,
        "run": "cold/1",
        "suite": "suite",
        "workloads": ["workload"],
        "sourceRepo": "/repo",
        "sourceRef": "master",
        "commitOrder": [],
        "withDebug": False,
        "withStats": False,
        "measurementMode": "wall-only",
        "benchmarkSubject": {"nixStorePath": "/nix/store/current"},
        "benchmarkEnvironment": {},
        "sourceRepoProvenance": {"head": COMMIT_A},
    }
    source = RunId("cold", 0)
    source_paths = RunPaths(run_dir=source.resolve(tmp_path))
    source_paths.state_dir.mkdir(parents=True)
    source_paths.manifest_file.write_text(json.dumps(source_manifest, indent=2) + "\n")
    planned = {**source_manifest, "run": "hot/0"}
    monkeypatch.setattr(
        generate_cmd,
        "build_run_manifest_payload",
        lambda *args, **kwargs: planned,
    )

    try:
        generate_cmd.execute_run(
            Path("/does-not-matter"),
            tmp_path,
            RunId("hot", 0),
            [],
            hot_source=source,
            source_repo=tmp_path,
            source_ref="master",
        )
    except SystemExit as exc:
        assert "different run id" in str(exc)
    else:
        raise AssertionError("expected source manifest run mismatch to abort")


def test_execute_run_rejects_incompatible_hot_source_manifest(tmp_path, monkeypatch):
    source_manifest = {
        "schemaVersion": 1,
        "run": "cold/0",
        "suite": "suite",
        "workloads": ["workload"],
        "sourceRepo": "/repo",
        "sourceRef": "master",
        "commitOrder": [],
        "withDebug": False,
        "withStats": False,
        "measurementMode": "wall-only",
        "benchmarkSubject": {"nixStorePath": "/nix/store/old"},
        "benchmarkEnvironment": {},
        "sourceRepoProvenance": {"head": COMMIT_A},
    }
    planned = {
        **source_manifest,
        "run": "hot/0",
        "benchmarkSubject": {"nixStorePath": "/nix/store/new"},
    }
    source = RunId("cold", 0)
    source_paths = RunPaths(run_dir=source.resolve(tmp_path))
    source_paths.state_dir.mkdir(parents=True)
    source_paths.manifest_file.write_text(json.dumps(source_manifest, indent=2) + "\n")
    monkeypatch.setattr(
        generate_cmd,
        "build_run_manifest_payload",
        lambda *args, **kwargs: planned,
    )
    monkeypatch.setattr(
        generate_cmd,
        "_git_repo_provenance",
        lambda *args, **kwargs: {"head": COMMIT_A},
    )

    try:
        generate_cmd.execute_run(
            Path("/does-not-matter"),
            tmp_path,
            RunId("hot", 0),
            [],
            hot_source=source,
            source_repo=tmp_path,
            source_ref="master",
        )
    except SystemExit as exc:
        assert "manifest does not match" in str(exc)
    else:
        raise AssertionError("expected incompatible hot source to abort")

    assert not RunId("hot", 0).resolve(tmp_path).joinpath("manifest.json").exists()


def test_execute_run_hot_manifest_inherits_source_provenance(tmp_path, monkeypatch):
    source_manifest = {
        "schemaVersion": 1,
        "run": "cold/0",
        "suite": "suite",
        "workloads": ["workload"],
        "sourceRepo": "/repo",
        "sourceRef": "master",
        "commitOrder": [],
        "withDebug": False,
        "withStats": False,
        "measurementMode": "wall-only",
        "benchmarkSubject": {"nixStorePath": "/nix/store/current"},
        "benchmarkEnvironment": {},
        "sourceRepoProvenance": {"head": COMMIT_A},
    }
    planned = {
        **source_manifest,
        "run": "hot/0",
        "sourceRepoProvenance": {"head": COMMIT_B},
    }
    source = RunId("cold", 0)
    source_paths = RunPaths(run_dir=source.resolve(tmp_path))
    source_paths.state_dir.mkdir(parents=True)
    source_paths.manifest_file.write_text(json.dumps(source_manifest, indent=2) + "\n")
    monkeypatch.setattr(
        generate_cmd,
        "build_run_manifest_payload",
        lambda *args, **kwargs: planned,
    )
    monkeypatch.setattr(
        generate_cmd,
        "_git_repo_provenance",
        lambda *args, **kwargs: {"head": COMMIT_A},
    )

    generate_cmd.execute_run(
        Path("/does-not-matter"),
        tmp_path,
        RunId("hot", 0),
        [],
        hot_source=source,
        source_repo=tmp_path,
        source_ref="master",
    )

    hot_manifest = json.loads(
        RunId("hot", 0).resolve(tmp_path).joinpath("manifest.json").read_text()
    )
    assert hot_manifest["sourceRepoProvenance"] == source_manifest["sourceRepoProvenance"]


def test_execute_run_hot_rechecks_current_source_provenance(tmp_path, monkeypatch):
    source_manifest = {
        "schemaVersion": 1,
        "run": "cold/0",
        "suite": "suite",
        "workloads": ["workload"],
        "sourceRepo": "/repo",
        "sourceRef": "master",
        "commitOrder": [],
        "withDebug": False,
        "withStats": False,
        "measurementMode": "wall-only",
        "benchmarkSubject": {"nixStorePath": "/nix/store/current"},
        "benchmarkEnvironment": {},
        "sourceRepoProvenance": {"head": COMMIT_A},
    }
    planned = {
        **source_manifest,
        "run": "hot/0",
        "sourceRepoProvenance": {"head": COMMIT_A},
    }
    source = RunId("cold", 0)
    source_paths = RunPaths(run_dir=source.resolve(tmp_path))
    source_paths.state_dir.mkdir(parents=True)
    source_paths.manifest_file.write_text(json.dumps(source_manifest, indent=2) + "\n")
    monkeypatch.setattr(
        generate_cmd,
        "build_run_manifest_payload",
        lambda *args, **kwargs: planned,
    )
    monkeypatch.setattr(
        generate_cmd,
        "_git_repo_provenance",
        lambda *args, **kwargs: {"head": COMMIT_B},
    )

    try:
        generate_cmd.execute_run(
            Path("/does-not-matter"),
            tmp_path,
            RunId("hot", 0),
            [],
            hot_source=source,
            source_repo=tmp_path,
            source_ref="master",
        )
    except SystemExit as exc:
        assert "current source repository provenance differs" in str(exc)
    else:
        raise AssertionError("expected hot source provenance mismatch to abort")


def test_execute_run_rejects_incomplete_hot_source(tmp_path, monkeypatch):
    manifest = {
        "schemaVersion": 1,
        "run": "cold/0",
        "suite": "suite",
        "workloads": ["workload"],
        "sourceRepo": "/repo",
        "sourceRef": "master",
        "commitOrder": [COMMIT_A],
        "withDebug": False,
        "withStats": False,
        "measurementMode": "wall-only",
        "benchmarkSubject": {"nixStorePath": "/nix/store/current"},
        "benchmarkEnvironment": {},
        "sourceRepoProvenance": {"head": COMMIT_A},
    }
    source = RunId("cold", 0)
    source_paths = RunPaths(run_dir=source.resolve(tmp_path))
    source_paths.state_dir.mkdir(parents=True)
    source_paths.manifest_file.write_text(json.dumps(manifest, indent=2) + "\n")
    planned = {**manifest, "run": "hot/0"}
    monkeypatch.setattr(
        generate_cmd,
        "build_run_manifest_payload",
        lambda *args, **kwargs: planned,
    )
    monkeypatch.setattr(
        generate_cmd,
        "_git_repo_provenance",
        lambda *args, **kwargs: {"head": COMMIT_A},
    )

    try:
        generate_cmd.execute_run(
            Path("/does-not-matter"),
            tmp_path,
            RunId("hot", 0),
            [generate_cmd.EvalCase(COMMIT_A, "case", (), "suite", "workload")],
            hot_source=source,
            source_repo=tmp_path,
            source_ref="master",
        )
    except SystemExit as exc:
        assert "does not have complete outputs" in str(exc)
    else:
        raise AssertionError("expected incomplete hot source to abort")


def test_benchmark_environment_provenance_records_eval_trace_switches(monkeypatch):
    monkeypatch.setenv("NIX_EVAL_TRACE_FLAG", "1")
    monkeypatch.setenv("NIX_ALLOW_EVAL", "0")
    monkeypatch.setenv("NIX_SHOW_STATS", "1")
    monkeypatch.setenv("UNRELATED", "ignored")

    assert _benchmark_environment_provenance() == {
        "NIX_ALLOW_EVAL": "0",
        "NIX_EVAL_TRACE_FLAG": "1",
    }


def test_generate_cli_paths_are_absolutized_for_nix_state_dirs(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    (tmp_path / "checkout").mkdir()

    assert _absolute_path(Path("checkout")).is_absolute()
    assert _absolute_path(Path("checkout")) == (tmp_path / "checkout").resolve()


def test_commit_list_file_preserves_order_and_ignores_comments(tmp_path):
    path = tmp_path / "commits.txt"
    path.write_text(
        f"""
        # targeted diagnostic window
        {COMMIT_B}
        {COMMIT_A}  # known reusable cert

        """
    )

    assert _commits_from_file(path) == [COMMIT_B, COMMIT_A]


def test_commit_list_file_rejects_ambiguous_lines(tmp_path):
    path = tmp_path / "commits.txt"
    path.write_text(f"{COMMIT_A} {COMMIT_B}\n")

    try:
        _commits_from_file(path)
    except ValueError as exc:
        assert "expected one commit" in str(exc)
    else:
        raise AssertionError("expected invalid commit-list line to fail")


def test_nixpkgs_release_cases_use_explicit_commit_list_order(tmp_path):
    cases = nixpkgs_release_cases(
        tmp_path,
        num_commits=1,
        branch="ignored",
        base=None,
        commit_list=[COMMIT_B, COMMIT_A],
    )

    assert [case.case_id for case in cases] == [COMMIT_B, COMMIT_A]
    assert [case.label for case in cases] == [COMMIT_B[:12], COMMIT_A[:12]]


def test_structural_variant_disable_selector_matches_mode_and_display_name():
    selectors = _parse_selector_csv("cold,hot/1")

    assert DEFAULT_DISABLE_STRUCTURAL_VARIANT_RECOVERY_ARG == "--no-eval-trace-structural-recovery"
    assert _extra_eval_args_for_run(
        RunId("cold", 0),
        disable_structural_variant_recovery_for=selectors,
        structural_variant_recovery_disable_arg=DEFAULT_DISABLE_STRUCTURAL_VARIANT_RECOVERY_ARG,
    ) == (DEFAULT_DISABLE_STRUCTURAL_VARIANT_RECOVERY_ARG,)
    assert _extra_eval_args_for_run(
        RunId("hot", 1),
        disable_structural_variant_recovery_for=selectors,
        structural_variant_recovery_disable_arg="--no-sv",
    ) == ("--no-sv",)
    assert (
        _extra_eval_args_for_run(
            RunId("hot", 0),
            disable_structural_variant_recovery_for=selectors,
            structural_variant_recovery_disable_arg="--no-sv",
        )
        == ()
    )


def test_structural_variant_disable_selector_skips_reference_and_matches_phase12_mode():
    selectors = _parse_selector_csv("all,cold")

    assert (
        _extra_eval_args_for_run(
            RunId("reference", None),
            disable_structural_variant_recovery_for=selectors,
            structural_variant_recovery_disable_arg="--no-sv",
        )
        == ()
    )
    assert _extra_eval_args_for_run(
        RunId("sibling-heavy-cold", 2),
        disable_structural_variant_recovery_for=_parse_selector_csv("cold"),
        structural_variant_recovery_disable_arg="--no-sv",
    ) == ("--no-sv",)


def test_eval_trace_hash_algorithm_args_pass_nix_option():
    assert _eval_trace_hash_algorithm_args("") == ()
    assert _eval_trace_hash_algorithm_args(" SHA256 ") == (
        "--option",
        EVAL_TRACE_HASH_ALGORITHM_NIX_OPTION,
        "sha256",
    )


def test_eval_trace_hash_algorithm_applies_to_traced_runs_only():
    assert (
        _extra_eval_args_for_run(
            RunId("reference", None),
            eval_trace_hash_algorithm="sha256",
            disable_structural_variant_recovery_for=set(),
            structural_variant_recovery_disable_arg="--no-sv",
        )
        == ()
    )
    assert _extra_eval_args_for_run(
        RunId("cold", 0),
        eval_trace_hash_algorithm="sha256",
        disable_structural_variant_recovery_for=set(),
        structural_variant_recovery_disable_arg="--no-sv",
    ) == ("--option", EVAL_TRACE_HASH_ALGORITHM_NIX_OPTION, "sha256")


def test_eval_trace_hash_algorithm_combines_with_structural_disable_arg():
    assert _extra_eval_args_for_run(
        RunId("hot", 0),
        eval_trace_hash_algorithm="sha256",
        disable_structural_variant_recovery_for=_parse_selector_csv("hot"),
        structural_variant_recovery_disable_arg="--no-sv",
    ) == ("--option", EVAL_TRACE_HASH_ALGORITHM_NIX_OPTION, "sha256", "--no-sv")


def test_structural_variant_mismatch_telemetry_selector_passes_flag():
    assert (
        DEFAULT_ENABLE_STRUCTURAL_VARIANT_MISMATCH_TELEMETRY_ARG
        == "--eval-trace-structural-recovery-mismatch-telemetry"
    )
    assert _extra_eval_args_for_run(
        RunId("cold", 7),
        disable_structural_variant_recovery_for=set(),
        structural_variant_recovery_disable_arg="--no-sv",
        enable_structural_variant_mismatch_telemetry_for=_parse_selector_csv("cold/7"),
        structural_variant_mismatch_telemetry_enable_arg="--sv-mismatch-telemetry",
    ) == ("--sv-mismatch-telemetry",)


def test_structural_variant_mismatch_telemetry_skips_reference_and_combines():
    assert (
        _extra_eval_args_for_run(
            RunId("reference", None),
            eval_trace_hash_algorithm="sha256",
            disable_structural_variant_recovery_for=set(),
            structural_variant_recovery_disable_arg="--no-sv",
            enable_structural_variant_mismatch_telemetry_for=_parse_selector_csv("all"),
            structural_variant_mismatch_telemetry_enable_arg="--sv-mismatch-telemetry",
        )
        == ()
    )
    assert _extra_eval_args_for_run(
        RunId("hot", 0),
        eval_trace_hash_algorithm="sha256",
        disable_structural_variant_recovery_for=_parse_selector_csv("hot"),
        structural_variant_recovery_disable_arg="--no-sv",
        enable_structural_variant_mismatch_telemetry_for=_parse_selector_csv("hot"),
        structural_variant_mismatch_telemetry_enable_arg="--sv-mismatch-telemetry",
    ) == (
        "--option",
        EVAL_TRACE_HASH_ALGORITHM_NIX_OPTION,
        "sha256",
        "--no-sv",
        "--sv-mismatch-telemetry",
    )


def test_manifest_order_is_preferred_over_git_fallback(tmp_path):
    run_dir = tmp_path / RESULTS_DIR / "cold" / "0"
    (run_dir / COMMIT_A).mkdir(parents=True)
    (run_dir / COMMIT_B).mkdir()
    (run_dir / "manifest.json").write_text(json.dumps({"commitOrder": [COMMIT_B, COMMIT_A]}))

    ds = load_dataset(tmp_path, source_repo=tmp_path, runs=["cold/0"])

    assert ds.commits == [COMMIT_B, COMMIT_A]


def test_manifest_only_run_keeps_planned_missing_commits(tmp_path):
    run_dir = tmp_path / RESULTS_DIR / "cold" / "0"
    run_dir.mkdir(parents=True)
    (run_dir / "manifest.json").write_text(json.dumps({"commitOrder": [COMMIT_A]}))

    ds = load_dataset(tmp_path, source_repo=tmp_path, runs=["cold/0"])

    assert ds.run_names == ["cold/0"]
    assert ds.commits == [COMMIT_A]
    assert ds.records[0].load_error == "missing eval.json"


def test_runs_can_use_reference_manifest_as_short_comparison_window(tmp_path):
    cold_100 = tmp_path / RESULTS_DIR / "cold" / "906"
    cold_10 = tmp_path / RESULTS_DIR / "cold" / "932"
    cold_100.mkdir(parents=True)
    cold_10.mkdir(parents=True)
    (cold_100 / COMMIT_A).mkdir()
    (cold_100 / COMMIT_B).mkdir()
    (cold_100 / COMMIT_C).mkdir()
    (cold_10 / COMMIT_B).mkdir()
    (cold_10 / COMMIT_A).mkdir()
    (cold_100 / "manifest.json").write_text(
        json.dumps({"commitOrder": [COMMIT_A, COMMIT_B, COMMIT_C]})
    )
    (cold_10 / "manifest.json").write_text(json.dumps({"commitOrder": [COMMIT_B, COMMIT_A]}))

    assert _commits_from_reference_manifest(
        tmp_path,
        ["cold/906", "cold/932"],
        "cold/932",
    ) == [COMMIT_B, COMMIT_A]


def test_runs_selection_includes_available_reference(tmp_path):
    (tmp_path / RESULTS_DIR / "reference" / COMMIT_A).mkdir(parents=True)
    (tmp_path / RESULTS_DIR / "cold" / "0" / COMMIT_A).mkdir(parents=True)

    assert _runs_including_reference_if_available(tmp_path, ["cold/0"], "reference") == [
        "cold/0",
        "reference",
    ]


def test_runs_selection_includes_available_same_mode_baseline(tmp_path):
    (tmp_path / RESULTS_DIR / "cold" / "0" / COMMIT_A).mkdir(parents=True)
    (tmp_path / RESULTS_DIR / "hot" / "0" / COMMIT_A).mkdir(parents=True)
    (tmp_path / RESULTS_DIR / "cold" / "1" / COMMIT_A).mkdir(parents=True)
    (tmp_path / RESULTS_DIR / "hot" / "1" / COMMIT_A).mkdir(parents=True)

    assert _baseline_counterpart("cold/1", 0) == "cold/0"
    assert _baseline_counterpart("reference", 0) is None
    assert _runs_including_baseline_run_if_available(
        tmp_path,
        ["cold/1", "hot/1"],
        0,
    ) == ["cold/1", "hot/1", "cold/0", "hot/0"]


def test_runs_selection_includes_requested_missing_baseline(tmp_path):
    (tmp_path / RESULTS_DIR / "cold" / "1" / COMMIT_A).mkdir(parents=True)

    assert _runs_including_baseline_run_if_available(
        tmp_path,
        ["cold/1"],
        0,
    ) == ["cold/1", "cold/0"]


def test_manifest_commit_order_ignores_commits_outside_candidates(tmp_path):
    run_dir = tmp_path / "cold"
    run_dir.mkdir()
    (run_dir / "manifest.json").write_text(
        json.dumps({"commitOrder": [COMMIT_B, COMMIT_A, COMMIT_C]})
    )

    order = manifest_commit_order([Run("cold", run_dir)], {COMMIT_A, COMMIT_B})

    assert order == [COMMIT_B, COMMIT_A]


def test_manifest_order_falls_back_when_manifest_has_no_candidate_overlap(tmp_path):
    run_dir = tmp_path / "cold"
    run_dir.mkdir()
    (run_dir / "manifest.json").write_text(json.dumps({"commitOrder": [COMMIT_C]}))

    order = manifest_commit_order([Run("cold", run_dir)], {COMMIT_A, COMMIT_B})

    assert order is None


def test_manifest_order_falls_back_on_conflicting_complete_manifests(tmp_path):
    run_a = tmp_path / "cold"
    run_b = tmp_path / "hot"
    run_a.mkdir()
    run_b.mkdir()
    (run_a / "manifest.json").write_text(json.dumps({"commitOrder": [COMMIT_A, COMMIT_B]}))
    (run_b / "manifest.json").write_text(json.dumps({"commitOrder": [COMMIT_B, COMMIT_A]}))

    order = manifest_commit_order(
        [Run("cold", run_a), Run("hot", run_b)],
        {COMMIT_A, COMMIT_B},
    )

    assert order is None


def test_analysis_compatibility_rejects_provenance_mismatch_unless_allowed(tmp_path):
    run_a = tmp_path / "cold" / "0"
    run_b = tmp_path / "cold" / "1"
    run_a.mkdir(parents=True)
    run_b.mkdir(parents=True)
    base_manifest = {
        "schemaVersion": 1,
        "run": "cold/0",
        "suite": "suite",
        "workloads": ["workload"],
        "sourceRepo": "/repo",
        "sourceRef": "master",
        "commitOrder": [COMMIT_A],
        "withDebug": False,
        "withStats": False,
        "measurementMode": "wall-only",
        "benchmarkSubject": {"nixStorePath": "/nix/store/a"},
        "benchmarkEnvironment": {"NIX_EVAL_TRACE_FLAG": "0"},
        "sourceRepoProvenance": {"head": COMMIT_A, "dirty": False},
    }
    (run_a / "manifest.json").write_text(json.dumps(base_manifest))
    (run_b / "manifest.json").write_text(
        json.dumps(
            {
                **base_manifest,
                "run": "cold/1",
                "benchmarkSubject": {"nixStorePath": "/nix/store/b"},
                "benchmarkEnvironment": {"NIX_EVAL_TRACE_FLAG": "1"},
            }
        )
    )

    runs = [Run("cold/0", run_a), Run("cold/1", run_b)]
    assert run_manifest_shape_issue(runs, ["cold/0", "cold/1"]) is None
    issue = run_analysis_compatibility_issue(runs, ["cold/0", "cold/1"])
    assert issue is not None
    assert "benchmarkSubject" in issue
    assert (
        run_analysis_compatibility_issue(
            runs,
            ["cold/0", "cold/1"],
            allow_provenance_mismatch=True,
        )
        is None
    )

    (run_b / "manifest.json").write_text(
        json.dumps({**base_manifest, "run": "cold/1", "measurementMode": "stats"})
    )

    issue = run_manifest_shape_issue(runs, ["cold/0", "cold/1"])
    assert issue is not None
    assert "measurementMode" in issue


def test_analysis_compatibility_ignores_source_ref_when_commit_order_matches(tmp_path):
    run_a = tmp_path / "cold" / "0"
    run_b = tmp_path / "cold" / "1"
    run_a.mkdir(parents=True)
    run_b.mkdir(parents=True)
    base_manifest = {
        "schemaVersion": 1,
        "run": "cold/0",
        "suite": "suite",
        "workloads": ["workload"],
        "sourceRepo": "/repo",
        "sourceRef": "commit-list:/old/path.txt",
        "commitOrder": [COMMIT_A],
        "withDebug": False,
        "withStats": False,
        "measurementMode": "wall-only",
        "benchmarkSubject": {"nixStorePath": "/nix/store/a"},
        "benchmarkEnvironment": {},
        "sourceRepoProvenance": {"head": COMMIT_A, "dirty": False},
    }
    (run_a / "manifest.json").write_text(json.dumps(base_manifest))
    (run_b / "manifest.json").write_text(
        json.dumps({**base_manifest, "run": "cold/1", "sourceRef": "commit-list:/new/path.txt"})
    )

    assert (
        run_manifest_shape_issue([Run("cold/0", run_a), Run("cold/1", run_b)], ["cold/0", "cold/1"])
        is None
    )


def test_analysis_compatibility_compares_result_wrapper_identity_only(tmp_path):
    run_a = tmp_path / "cold" / "0"
    run_b = tmp_path / "cold" / "1"
    run_a.mkdir(parents=True)
    run_b.mkdir(parents=True)
    subject = {
        "nixRootKind": "result-wrapper",
        "nixBin": "/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-nix/bin/nix",
        "nixBinSha256": "sha",
        "nixRoot": "/checkout/a",
        "nixCheckout": {"head": COMMIT_A, "dirty": True},
        "nixStorePath": "/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-nix",
        "nixStoreDeriver": "/nix/store/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb-nix.drv",
    }
    base_manifest = {
        "schemaVersion": 1,
        "run": "cold/0",
        "suite": "suite",
        "workloads": ["workload"],
        "sourceRepo": "/repo",
        "sourceRef": "master",
        "commitOrder": [COMMIT_A],
        "withDebug": False,
        "withStats": False,
        "measurementMode": "wall-only",
        "benchmarkSubject": subject,
        "benchmarkEnvironment": {},
        "sourceRepoProvenance": {"head": COMMIT_A, "dirty": False},
    }
    (run_a / "manifest.json").write_text(json.dumps(base_manifest))
    (run_b / "manifest.json").write_text(
        json.dumps(
            {
                **base_manifest,
                "run": "cold/1",
                "benchmarkSubject": {
                    **subject,
                    "nixRoot": "/checkout/b",
                    "nixCheckout": {"head": COMMIT_B, "dirty": True},
                },
            }
        )
    )

    runs = [Run("cold/0", run_a), Run("cold/1", run_b)]
    assert run_analysis_compatibility_issue(runs, ["cold/0", "cold/1"]) is None

    (run_b / "manifest.json").write_text(
        json.dumps(
            {
                **base_manifest,
                "run": "cold/1",
                "benchmarkSubject": {
                    key: value for key, value in subject.items() if key != "nixStoreDeriver"
                },
            }
        )
    )
    assert run_analysis_compatibility_issue(runs, ["cold/0", "cold/1"]) is None

    (run_b / "manifest.json").write_text(
        json.dumps(
            {
                **base_manifest,
                "run": "cold/1",
                "benchmarkSubject": {**subject, "nixBinSha256": "different"},
            }
        )
    )
    issue = run_analysis_compatibility_issue(runs, ["cold/0", "cold/1"])
    assert issue is not None
    assert "benchmarkSubject" in issue


def test_analysis_compatibility_allows_clean_source_head_mismatch_when_commit_order_matches(
    tmp_path,
):
    run_a = tmp_path / "cold" / "0"
    run_b = tmp_path / "cold" / "1"
    run_a.mkdir(parents=True)
    run_b.mkdir(parents=True)
    base_manifest = _bench_manifest("cold/0", [COMMIT_A])
    (run_a / "manifest.json").write_text(json.dumps(base_manifest))
    (run_b / "manifest.json").write_text(
        json.dumps(
            {
                **base_manifest,
                "run": "cold/1",
                "sourceRepoProvenance": {"head": COMMIT_B, "dirty": False},
            }
        )
    )

    issue = run_analysis_compatibility_issue(
        [Run("cold/0", run_a), Run("cold/1", run_b)],
        ["cold/0", "cold/1"],
    )

    assert issue is None


def test_analysis_compatibility_rejects_dirty_source_provenance_even_when_benchmark_mismatch_allowed(
    tmp_path,
):
    run_a = tmp_path / "cold" / "0"
    run_b = tmp_path / "cold" / "1"
    run_a.mkdir(parents=True)
    run_b.mkdir(parents=True)
    base_manifest = {
        "schemaVersion": 1,
        "run": "cold/0",
        "suite": "suite",
        "workloads": ["workload"],
        "sourceRepo": "/repo",
        "sourceRef": "master",
        "commitOrder": [COMMIT_A],
        "withDebug": False,
        "withStats": False,
        "measurementMode": "wall-only",
        "benchmarkSubject": {"nixStorePath": "/nix/store/a"},
        "benchmarkEnvironment": {"NIX_EVAL_TRACE_FLAG": "0"},
        "sourceRepoProvenance": {"head": COMMIT_A, "dirty": True, "dirtyTreeSha256": "one"},
    }
    (run_a / "manifest.json").write_text(json.dumps(base_manifest))
    (run_b / "manifest.json").write_text(
        json.dumps(
            {
                **base_manifest,
                "run": "cold/1",
                "benchmarkSubject": {"nixStorePath": "/nix/store/b"},
                "benchmarkEnvironment": {"NIX_EVAL_TRACE_FLAG": "1"},
                "sourceRepoProvenance": {
                    "head": COMMIT_B,
                    "dirty": True,
                    "dirtyTreeSha256": "two",
                },
            }
        )
    )

    issue = run_analysis_compatibility_issue(
        [Run("cold/0", run_a), Run("cold/1", run_b)],
        ["cold/0", "cold/1"],
        allow_provenance_mismatch=True,
    )

    assert issue is not None
    assert "sourceRepoProvenance" in issue


def test_analysis_compatibility_requires_readable_manifests(tmp_path):
    run_a = tmp_path / "cold" / "0"
    run_b = tmp_path / "cold" / "1"
    run_a.mkdir(parents=True)
    run_b.mkdir(parents=True)
    (run_a / "manifest.json").write_text(json.dumps({"commitOrder": [COMMIT_A]}))

    issue = run_analysis_compatibility_issue(
        [Run("cold/0", run_a), Run("cold/1", run_b)],
        ["cold/0", "cold/1"],
    )

    assert issue is not None
    assert "no readable manifest" in issue

    (run_b / "manifest.json").write_text("{not json")
    issue = run_analysis_compatibility_issue(
        [Run("cold/0", run_a), Run("cold/1", run_b)],
        ["cold/0", "cold/1"],
    )

    assert issue is not None
    assert "no readable manifest" in issue


def test_analysis_compatibility_rejects_skeletal_manifest(tmp_path):
    run_a = tmp_path / "cold" / "0"
    run_b = tmp_path / "cold" / "1"
    run_a.mkdir(parents=True)
    run_b.mkdir(parents=True)
    (run_a / "manifest.json").write_text(json.dumps({"commitOrder": [COMMIT_A]}))
    (run_b / "manifest.json").write_text(json.dumps({"commitOrder": [COMMIT_A]}))

    issue = run_analysis_compatibility_issue(
        [Run("cold/0", run_a), Run("cold/1", run_b)],
        ["cold/0", "cold/1"],
    )

    assert issue is not None
    assert "missing required workload fields" in issue


def test_analysis_compatibility_rejects_mismatched_manifest_run(tmp_path):
    run_a = tmp_path / "cold" / "0"
    run_b = tmp_path / "cold" / "1"
    run_a.mkdir(parents=True)
    run_b.mkdir(parents=True)
    (run_a / "manifest.json").write_text(json.dumps(_bench_manifest("cold/0", [COMMIT_A])))
    (run_b / "manifest.json").write_text(json.dumps(_bench_manifest("hot/1", [COMMIT_A])))

    issue = run_analysis_compatibility_issue(
        [Run("cold/0", run_a), Run("cold/1", run_b)],
        ["cold/0", "cold/1"],
    )

    assert issue is not None
    assert "manifest run field" in issue


def test_stateful_run_order_must_match_reference_window(tmp_path):
    reference = tmp_path / "reference"
    cold = tmp_path / "cold" / "0"
    reference.mkdir()
    cold.mkdir(parents=True)
    (reference / "manifest.json").write_text(
        json.dumps(_bench_manifest("reference", [COMMIT_A, COMMIT_B]))
    )
    (cold / "manifest.json").write_text(json.dumps(_bench_manifest("cold/0", [COMMIT_B, COMMIT_A])))

    issue = run_analysis_compatibility_issue(
        [Run("reference", reference), Run("cold/0", cold)],
        ["reference", "cold/0"],
        comparison_commits=[COMMIT_A, COMMIT_B],
    )

    assert issue is not None
    assert "comparison window" in issue
    assert (
        run_analysis_compatibility_issue(
            [Run("reference", reference), Run("cold/0", cold)],
            ["reference", "cold/0"],
            comparison_commits=[COMMIT_A, COMMIT_B],
            allow_intersection=True,
        )
        is None
    )


def test_stateful_run_order_rejects_non_prefix_short_window(tmp_path):
    long = tmp_path / "cold" / "0"
    short = tmp_path / "cold" / "1"
    long.mkdir(parents=True)
    short.mkdir(parents=True)
    (long / "manifest.json").write_text(
        json.dumps(_bench_manifest("cold/0", [COMMIT_A, COMMIT_B, COMMIT_C]))
    )
    (short / "manifest.json").write_text(
        json.dumps(_bench_manifest("cold/1", [COMMIT_B, COMMIT_C]))
    )

    issue = run_analysis_compatibility_issue(
        [Run("cold/0", long), Run("cold/1", short)],
        ["cold/0", "cold/1"],
        comparison_commits=[COMMIT_B, COMMIT_C],
    )

    assert issue is not None
    assert "cache prefix state" in issue


def test_stateful_run_order_allows_matching_prefix_window(tmp_path):
    long = tmp_path / "cold" / "0"
    short = tmp_path / "cold" / "1"
    long.mkdir(parents=True)
    short.mkdir(parents=True)
    (long / "manifest.json").write_text(
        json.dumps(_bench_manifest("cold/0", [COMMIT_A, COMMIT_B, COMMIT_C]))
    )
    (short / "manifest.json").write_text(
        json.dumps(_bench_manifest("cold/1", [COMMIT_A, COMMIT_B]))
    )

    issue = run_analysis_compatibility_issue(
        [Run("cold/0", long), Run("cold/1", short)],
        ["cold/0", "cold/1"],
        comparison_commits=[COMMIT_A, COMMIT_B],
    )

    assert issue is None


def test_run_manifest_records_source_dirty_diff_hash(tmp_path):
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    nix_bin = bin_dir / "nix"
    nix_bin.write_text("#!/bin/sh\n")

    payload = generate_cmd.build_run_manifest_payload(
        RunId("cold", 0),
        [],
        nix_bin=nix_bin,
        nix_root=tmp_path,
        source_repo=tmp_path,
        source_ref="master",
    )

    source_provenance = payload["sourceRepoProvenance"]
    assert isinstance(source_provenance, dict)
    assert "diffSha256" in source_provenance
    assert "dirtyTreeSha256" in source_provenance


def test_git_dirty_tree_hash_includes_untracked_files(tmp_path, monkeypatch):
    untracked = tmp_path / "untracked.txt"
    untracked.write_text("one\n")

    def fake_git_run(args, *, capture_output, check=False, text=False):
        assert args[:3] == ["git", "-C", str(tmp_path)]
        git_args = tuple(args[3:])
        pathspec = generate_cmd.GIT_PROVENANCE_PATHSPEC
        if git_args == ("rev-parse", "HEAD"):
            stdout = COMMIT_A + "\n" if text else (COMMIT_A + "\n").encode()
        elif git_args == ("status", "--porcelain=v1", *pathspec):
            stdout = "?? untracked.txt\n" if text else b"?? untracked.txt\n"
        elif git_args == ("status", "--porcelain=v1", "-z", *pathspec):
            stdout = "?? untracked.txt\0" if text else b"?? untracked.txt\0"
        elif git_args == ("diff", "--no-ext-diff", "--binary", "HEAD", *pathspec):
            stdout = "" if text else b""
        elif git_args == ("ls-files", "--others", "--exclude-standard", "-z", *pathspec):
            stdout = "untracked.txt\0" if text else b"untracked.txt\0"
        else:
            raise AssertionError(f"unexpected git args: {git_args}")
        return subprocess.CompletedProcess(args, 0, stdout=stdout)

    monkeypatch.setattr(generate_cmd.subprocess, "run", fake_git_run)

    first = generate_cmd._git_repo_provenance(tmp_path, include_diff_hash=True)
    untracked.write_text("two\n")
    second = generate_cmd._git_repo_provenance(tmp_path, include_diff_hash=True)

    assert first["dirty"] is True
    assert first["diffSha256"] == second["diffSha256"]
    assert first["dirtyTreeSha256"] != second["dirtyTreeSha256"]


def test_git_dirty_tree_hash_includes_untracked_symlink_targets(tmp_path, monkeypatch):
    link = tmp_path / "link"
    link.symlink_to("target-one")

    def fake_git_run(args, *, capture_output, check=False, text=False):
        assert args[:3] == ["git", "-C", str(tmp_path)]
        git_args = tuple(args[3:])
        pathspec = generate_cmd.GIT_PROVENANCE_PATHSPEC
        if git_args == ("status", "--porcelain=v1", "-z", *pathspec):
            stdout = "?? link\0" if text else b"?? link\0"
        elif git_args == ("diff", "--no-ext-diff", "--binary", "HEAD", *pathspec):
            stdout = "" if text else b""
        elif git_args == ("ls-files", "--others", "--exclude-standard", "-z", *pathspec):
            stdout = "link\0" if text else b"link\0"
        else:
            raise AssertionError(f"unexpected git args: {git_args}")
        return subprocess.CompletedProcess(args, 0, stdout=stdout)

    monkeypatch.setattr(generate_cmd.subprocess, "run", fake_git_run)

    first = generate_cmd._git_dirty_tree_sha256(tmp_path)
    link.unlink()
    link.symlink_to("target-two")
    second = generate_cmd._git_dirty_tree_sha256(tmp_path)

    assert first is not None
    assert second is not None
    assert first != second


def test_git_dirty_tree_hash_includes_untracked_mode(tmp_path, monkeypatch):
    untracked = tmp_path / "script.sh"
    untracked.write_text("#!/bin/sh\n")
    untracked.chmod(0o644)

    def fake_git_run(args, *, capture_output, check=False, text=False):
        assert args[:3] == ["git", "-C", str(tmp_path)]
        git_args = tuple(args[3:])
        pathspec = generate_cmd.GIT_PROVENANCE_PATHSPEC
        if git_args == ("status", "--porcelain=v1", "-z", *pathspec):
            stdout = "?? script.sh\0" if text else b"?? script.sh\0"
        elif git_args == ("diff", "--no-ext-diff", "--binary", "HEAD", *pathspec):
            stdout = "" if text else b""
        elif git_args == ("ls-files", "--others", "--exclude-standard", "-z", *pathspec):
            stdout = "script.sh\0" if text else b"script.sh\0"
        else:
            raise AssertionError(f"unexpected git args: {git_args}")
        return subprocess.CompletedProcess(args, 0, stdout=stdout)

    monkeypatch.setattr(generate_cmd.subprocess, "run", fake_git_run)

    first = generate_cmd._git_dirty_tree_sha256(tmp_path)
    untracked.chmod(0o755)
    second = generate_cmd._git_dirty_tree_sha256(tmp_path)

    assert first is not None
    assert second is not None
    assert first != second


def test_benchmark_subject_marks_result_wrapper_roots(tmp_path, monkeypatch):
    wrapper = tmp_path / "wrapper"
    nix_bin_dir = wrapper / "result" / "bin"
    nix_bin_dir.mkdir(parents=True)
    nix_bin = nix_bin_dir / "nix"
    nix_bin.write_text("#!/bin/sh\n")
    monkeypatch.setattr(
        generate_cmd,
        "_nix_store_path_for",
        lambda path: "/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-nix-cli",
    )

    provenance = generate_cmd._benchmark_subject_provenance(nix_bin, wrapper)

    assert provenance["nixRootKind"] == "result-wrapper"
    checkout = provenance["nixCheckout"]
    assert isinstance(checkout, dict)
    assert checkout["isGitRepository"] is False


def test_benchmark_subject_records_store_deriver_when_available(tmp_path, monkeypatch):
    wrapper = tmp_path / "wrapper"
    nix_bin_dir = wrapper / "result" / "bin"
    nix_bin_dir.mkdir(parents=True)
    nix_bin = nix_bin_dir / "nix"
    nix_bin.write_text("#!/bin/sh\n")

    monkeypatch.setattr(
        generate_cmd,
        "_nix_store_path_for",
        lambda path: "/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-nix-cli",
    )
    monkeypatch.setattr(
        generate_cmd,
        "_nix_deriver_for_store_path",
        lambda store_path: "/nix/store/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb-nix-cli.drv",
    )

    provenance = generate_cmd._benchmark_subject_provenance(nix_bin, wrapper)

    assert provenance["nixStorePath"] == "/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-nix-cli"
    assert (
        provenance["nixStoreDeriver"] == "/nix/store/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb-nix-cli.drv"
    )


def test_pairwise_rejects_stateful_runs_with_different_manifest_order(tmp_path):
    baseline = tmp_path / "cold" / "0"
    target = tmp_path / "cold" / "1"
    baseline.mkdir(parents=True)
    target.mkdir(parents=True)
    (baseline / "manifest.json").write_text(json.dumps({"commitOrder": [COMMIT_A, COMMIT_B]}))
    (target / "manifest.json").write_text(json.dumps({"commitOrder": [COMMIT_B, COMMIT_A]}))

    issue = _stateful_pairwise_order_issue(
        [Run("cold/0", baseline), Run("cold/1", target)],
        "cold/0",
        "cold/1",
        allow_intersection=False,
    )

    assert issue is not None
    assert "identical manifest commitOrder" in issue
    assert (
        _stateful_pairwise_order_issue(
            [Run("cold/0", baseline), Run("cold/1", target)],
            "cold/0",
            "cold/1",
            allow_intersection=True,
        )
        is None
    )


def test_pairwise_rejects_stateful_runs_without_manifest_order(tmp_path):
    baseline = tmp_path / "cold" / "0"
    target = tmp_path / "hot" / "0"
    baseline.mkdir(parents=True)
    target.mkdir(parents=True)

    issue = _stateful_pairwise_order_issue(
        [Run("cold/0", baseline), Run("hot/0", target)],
        "cold/0",
        "hot/0",
        allow_intersection=False,
    )

    assert issue is not None
    assert "require manifest commitOrder" in issue


def test_pairwise_comparison_commits_come_from_actual_pairs():
    base = Record(
        run_name="cold/0",
        commit=COMMIT_A,
        proc_idx=0,
        mode="cold",
        data=None,
    )
    target = Record(
        run_name="cold/1",
        commit=COMMIT_A,
        proc_idx=0,
        mode="cold",
        data=None,
    )

    assert _pairwise_comparison_commits([(COMMIT_A, base, target)]) == [COMMIT_A]


def test_pairwise_candidate_commits_use_run_intersection(tmp_path):
    results = tmp_path / RESULTS_DIR
    baseline = results / "cold" / "0"
    target = results / "cold" / "1"
    baseline.mkdir(parents=True)
    target.mkdir(parents=True)
    (baseline / "manifest.json").write_text(
        json.dumps({"commitOrder": [COMMIT_A, COMMIT_B, COMMIT_C]})
    )
    (target / "manifest.json").write_text(json.dumps({"commitOrder": [COMMIT_A, COMMIT_B]}))

    ds = load_dataset(tmp_path, runs=["cold/0", "cold/1"])

    assert _pairwise_candidate_commits(ds, "cold/0", "cold/1") == [COMMIT_A, COMMIT_B]


def test_pairwise_candidate_commits_use_stateful_common_prefix(tmp_path, monkeypatch):
    results = tmp_path / RESULTS_DIR
    baseline = results / "cold" / "0"
    target = results / "cold" / "1"
    baseline.mkdir(parents=True)
    target.mkdir(parents=True)
    (baseline / "manifest.json").write_text(
        json.dumps({"commitOrder": [COMMIT_A, COMMIT_B, COMMIT_C]})
    )
    (target / "manifest.json").write_text(
        json.dumps({"commitOrder": [COMMIT_A, COMMIT_B, "d" * 40]})
    )

    ds = load_dataset(tmp_path, runs=["cold/0", "cold/1"])
    monkeypatch.setattr(ds, "commits", [COMMIT_B, COMMIT_A, COMMIT_C])

    assert _pairwise_candidate_commits(ds, "cold/0", "cold/1") == [COMMIT_A, COMMIT_B]


def test_pairwise_rejects_empty_same_commit_pairs():
    issue = _pairwise_no_pairs_issue([], "cold/0", "hot/0")

    assert issue is not None
    assert "no complete same-commit pairs" in issue
    assert _pairwise_no_pairs_issue([(COMMIT_A, object(), object())], "cold/0", "hot/0") is None


def test_explicit_commits_are_not_silently_dropped(tmp_path):
    run_dir = tmp_path / "cold"
    (run_dir / COMMIT_A).mkdir(parents=True)

    ds = load_dataset(tmp_path, runs=["cold"], commits=[COMMIT_A])

    assert ds.commits == [COMMIT_A]
    assert ds.records[0].load_error == "missing eval.json"


def test_requested_missing_run_is_reported(tmp_path):
    ds = load_dataset(tmp_path, runs=["cold/0"])

    issues = format_dataset_issues(ds, requested_runs=["cold/0"])

    assert issues == ["cold/0: requested run was not found"]


def test_runs_missing_reference_is_not_silently_replaced():
    assert _reference_issue(["cold/0"], "reference") == (
        "reference run 'reference' was not found; refuse to compare against 'cold/0'."
    )
    assert _reference_issue(["reference", "cold/0"], "reference") is None


def test_dataset_issue_reporting_can_skip_record_completeness(tmp_path):
    run_dir = tmp_path / "cold"
    (run_dir / COMMIT_A).mkdir(parents=True)
    ds = load_dataset(tmp_path, runs=["cold"], commits=[COMMIT_A])

    issues = format_dataset_issues(ds, requested_runs=["cold"], include_records=False)

    assert issues == []


def test_corrupt_or_incomplete_run_data_reports_error(tmp_path):
    commit_dir = tmp_path / COMMIT_A
    commit_dir.mkdir()
    (commit_dir / "stats.json").write_text("{}")

    result = load_run_data_result(tmp_path, COMMIT_A)

    assert result.data is None
    assert result.error == "missing eval.json"


def test_recording_redundancy_uses_full_dependency_identity():
    details = [
        ("content", "direct", "input-a", "same-key"),
        ("content", "direct", "input-b", "same-key"),
    ]

    total, unique, _ = analyze_recording_redundancy(details)

    assert total == 2
    assert unique == 2


def test_distribution_uses_interpolated_quantiles():
    dist = _distribution([0.0, 1.0])

    assert dist["p25"] == 0.25
    assert dist["p50"] == 0.5
    assert dist["p75"] == 0.75


def test_classify_reports_recovery_counts_and_rates():
    records = [
        _record_with_trace(
            commit=COMMIT_A,
            hits=8,
            misses=2,
            attempts=4,
            failures=0,
        ),
        _record_with_trace(
            commit=COMMIT_B,
            hits=6,
            misses=4,
            attempts=5,
            failures=2,
        ),
    ]
    console = Console(record=True, width=120)

    classify_cmd._render_recovery_summary(console, records, "cold/0")

    text = console.export_text()
    assert "miss/recovery coverage" in text
    assert "hit-only" in text
    assert "partial-miss" in text
    assert "TOTAL" in text
    assert "14" in text
    assert "6" in text
    assert "70.0%" in text
    assert "40.0%" in text


def test_stats_model_loads_new_recovery_and_keyset_counters():
    stats = load_stats(
        {
            "evalTrace": {
                "loadKeySet": {
                    "count": 7,
                    "cacheHits": 5,
                    "cacheMisses": 2,
                    "timeUs": 300,
                },
                "record": {
                    "count": 1,
                    "hashUs": 10,
                    "serializeKeysUs": 20,
                    "serializeValuesUs": 30,
                    "flushUs": 40,
                    "timeUs": 100,
                },
                "recovery": {
                    "gitIdentity": {
                        "attempts": 3,
                        "candidates": 4,
                        "accepted": 1,
                        "rejected": 3,
                        "timeUs": 50,
                    },
                    "lookups": {
                        "directHash": {"count": 2, "rows": 1, "timeUs": 11},
                        "scanHistory": {"count": 1, "rows": 9, "timeUs": 22},
                    },
                    "acceptance": {
                        "implicitGuardCandidates": 2,
                        "implicitGuardChecks": 5,
                        "implicitGuardFailures": 1,
                        "implicitGuardTimeUs": 33,
                    },
                },
            }
        }
    )

    et = stats.eval_trace
    assert et.load_key_set.cache_hits == 5
    assert et.record.serialize_values_us == 30
    assert et.recovery.git_identity.rejected == 3
    assert et.recovery.lookups.scan_history.rows == 9
    assert et.recovery.acceptance.implicit_guard_failures == 1


def test_sv_aggregate_loads_flat_candidate_counters():
    # Production now emits `structVariant.candidates` as a single object
    # (the per-`depKeySetId` bucketing was removed once the v26 schema
    # dropped the DepKeySets table — the bucket key was permanently zero
    # and meaningless).  The model loader must surface every documented
    # field on the aggregate so downstream consumers don't silently see
    # zeros after a schema drift.
    stats = load_stats(
        {
            "evalTrace": {
                "structVariant": {
                    "candidates": {
                        "tried": 4,
                        "succeeded": 1,
                        "abortedEarly": 1,
                        "hashMismatch": 2,
                        "avgDeps": 7.5,
                        "avgUs": 42.0,
                        "bothSetCount": 1,
                        "earlierHashMismatchCount": 1,
                        "earlierHashMismatchSavedDeps": 3,
                        "hashMismatchOnlyCount": 2,
                        "hashMismatchOnlySavedDeps": 5,
                    }
                }
            }
        }
    )

    c = stats.eval_trace.struct_variant.candidates
    assert c.tried == 4
    assert c.succeeded == 1
    assert c.aborted_early == 1
    assert c.hash_mismatch == 2
    assert c.avg_deps == 7.5
    assert c.avg_us == 42.0
    assert c.both_set_count == 1
    assert c.earlier_hash_mismatch_count == 1
    assert c.earlier_hash_mismatch_saved_deps == 3
    assert c.hash_mismatch_only_count == 2
    assert c.hash_mismatch_only_saved_deps == 5


def _record_with_trace(
    *,
    commit: str,
    hits: int,
    misses: int,
    attempts: int,
    failures: int,
) -> Record:
    stats = load_stats(
        {
            "evalTrace": {
                "hits": hits,
                "misses": misses,
                "recovery": {
                    "attempts": attempts,
                    "failures": failures,
                },
            }
        }
    )
    return Record(
        run_name="cold/0",
        commit=commit,
        proc_idx=0,
        mode="cold",
        data=RunData(stats=stats, stats_present=True, eval_raw="{}", wall_time=1.0),
    )
