from __future__ import annotations

import json
from pathlib import Path
from urllib.parse import parse_qs

from rich.console import Console

from eval_trace_bench.cliutil import format_dataset_issues
from eval_trace_bench.dataframe import Record, load_dataset
from eval_trace_bench.discovery import Run, manifest_commit_order
from eval_trace_bench.layout import RESULTS_DIR, RunId, RunPaths
from eval_trace_bench.logparse import analyze_recording_redundancy
from eval_trace_bench.models import load_stats
from eval_trace_bench.simulation import (
    OPP1,
    BucketEvent,
    Combined,
    NeverSkip,
    run_simulation,
    winner_ranks,
)
from eval_trace_bench.stats import RunData, load_run_data_result
from eval_trace_bench.subcommands import classify_cmd, sv_cmd
from eval_trace_bench.subcommands.generate_cmd import (
    DEFAULT_DISABLE_STRUCTURAL_VARIANT_RECOVERY_ARG,
    DEFAULT_ENABLE_STRUCTURAL_VARIANT_MISMATCH_TELEMETRY_ARG,
    EVAL_TRACE_HASH_ALGORITHM_NIX_OPTION,
    _absolute_path,
    _eval_trace_hash_algorithm_args,
    _extra_eval_args_for_run,
    _parse_selector_csv,
    _state_env,
    _store_uri,
    ensure_run_state,
    reset_state_from,
)
from eval_trace_bench.subcommands.logs_cmd import _distribution

COMMIT_A = "a" * 40
COMMIT_B = "b" * 40
COMMIT_C = "c" * 40


def test_run_id_resolves_under_results_directory(tmp_path):
    assert RunId("cold", 0).resolve(tmp_path) == tmp_path / RESULTS_DIR / "cold" / "0"
    assert RunId("reference", None).resolve(tmp_path) == tmp_path / RESULTS_DIR / "reference"


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


def test_generate_cli_paths_are_absolutized_for_nix_state_dirs(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    (tmp_path / "checkout").mkdir()

    assert _absolute_path(Path("checkout")).is_absolute()
    assert _absolute_path(Path("checkout")) == (tmp_path / "checkout").resolve()


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


def test_explicit_commits_are_not_silently_dropped(tmp_path):
    run_dir = tmp_path / "cold"
    (run_dir / COMMIT_A).mkdir(parents=True)

    ds = load_dataset(tmp_path, runs=["cold"], commits=[COMMIT_A])

    assert ds.commits == [COMMIT_A]
    assert ds.records[0].load_error == "missing stats.json"


def test_requested_missing_run_is_reported(tmp_path):
    ds = load_dataset(tmp_path, runs=["cold/0"])

    issues = format_dataset_issues(ds, requested_runs=["cold/0"])

    assert issues == ["cold/0: requested run was not found"]


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


def test_combined_policy_observes_opp1_child_and_counts_tried_failures():
    events = [
        BucketEvent(0, COMMIT_A, 1, 2, 0, 0, 2, 100.0, 1.0, 0),
        BucketEvent(1, COMMIT_B, 1, 1, 0, 0, 1, 100.0, 1.0, 0),
        BucketEvent(2, COMMIT_C, 1, 5, 0, 0, 5, 100.0, 1.0, 0),
    ]

    result = run_simulation(Combined([OPP1(3), NeverSkip()], "combo"), events)

    assert result.skipped_bucket_events == 1
    assert result.saved_tries == 5


def test_winner_rank_median_uses_standard_even_sample_median():
    records = [
        _record_with_sv(COMMIT_A, [{"depKeySetId": 1, "tried": 1, "succeeded": 1}]),
        _record_with_sv(
            COMMIT_B,
            [
                {"depKeySetId": 1, "tried": 1, "succeeded": 0},
                {"depKeySetId": 2, "tried": 1, "succeeded": 1},
            ],
        ),
    ]

    summary = winner_ranks(records)

    assert summary.ranks == [0, 1]
    assert summary.median_rank == 0.5


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


def test_sv_aggregate_estimates_early_exit_saved_time():
    records = [
        _record_with_sv(
            COMMIT_A,
            [
                {
                    "depKeySetId": 1,
                    "tried": 1,
                    "avgDeps": 10.0,
                    "avgUs": 100.0,
                    "earlierHashMismatchSavedDeps": 3,
                    "hashMismatchOnlySavedDeps": 2,
                }
            ],
        )
    ]

    agg = sv_cmd.aggregate(records)

    assert agg[1].earlier_hash_mismatch_saved_deps == 3
    assert agg[1].hash_mismatch_only_saved_deps == 2
    assert agg[1].estimated_early_exit_saved_us == 50.0


def _record_with_sv(commit: str, entries: list[dict[str, object]]) -> Record:
    stats = load_stats({"evalTrace": {"structVariant": {"byDepKeySet": entries}}})
    return Record(
        run_name="cold/0",
        commit=commit,
        proc_idx=0,
        mode="cold",
        data=RunData(stats=stats, eval_raw="{}", wall_time=1.0),
    )


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
        data=RunData(stats=stats, eval_raw="{}", wall_time=1.0),
    )
