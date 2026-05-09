"""Pydantic models for `stats.json` + `timing.json`.

The underlying JSON is emitted by `src/libexpr/eval.cc` around the
`topObj["evalTrace"]` assignment.  We model ONLY the parts this tool
consumes; `model_config = ConfigDict(extra="allow")` means older /
newer Nix binaries can emit counters we don't know about without
crashing the loader.

Design notes:
- Every field has a default of 0 so a commit from a stats-schema
  version missing a counter doesn't explode analyses that sum /
  mean that counter — they just see zeros.
- Counter names on the Python side are snake_case; the JSON keys
  (camelCase) are aliased via `Field(validation_alias=...)`.
- The raw `dict` is still accessible as `RunStats.raw` so the
  flat-key dump in `logs` (plan H1) continues to surface every key
  the JSON carries, not just the ones we know about.
- Submodel fields use `Field(default_factory=SubModel)` — safe
  because all models are frozen.
"""

from __future__ import annotations

from typing import Any

from pydantic import BaseModel, ConfigDict, Field

_MODEL_CONFIG = ConfigDict(
    extra="allow",
    populate_by_name=True,
    frozen=True,
)


class DbCounters(BaseModel):
    model_config = _MODEL_CONFIG
    init_time_us: int = Field(default=0, validation_alias="initTimeUs")
    close_time_us: int = Field(default=0, validation_alias="closeTimeUs")


class TimedCounter(BaseModel):
    """Counter with a companion `timeUs`."""

    model_config = _MODEL_CONFIG
    count: int = 0
    time_us: int = Field(default=0, validation_alias="timeUs")


class LoadKeySetCounters(BaseModel):
    model_config = _MODEL_CONFIG
    count: int = 0
    cache_hits: int = Field(default=0, validation_alias="cacheHits")
    cache_misses: int = Field(default=0, validation_alias="cacheMisses")
    time_us: int = Field(default=0, validation_alias="timeUs")


class RecordCounters(TimedCounter):
    model_config = _MODEL_CONFIG
    hash_us: int = Field(default=0, validation_alias="hashUs")
    serialize_keys_us: int = Field(default=0, validation_alias="serializeKeysUs")
    serialize_values_us: int = Field(default=0, validation_alias="serializeValuesUs")
    flush_us: int = Field(default=0, validation_alias="flushUs")


class VerifyCounters(BaseModel):
    model_config = _MODEL_CONFIG
    count: int = 0
    passed: int = 0
    failed: int = 0
    deps_checked: int = Field(default=0, validation_alias="depsChecked")
    time_us: int = Field(default=0, validation_alias="timeUs")


class VerifyTraceCounters(BaseModel):
    model_config = _MODEL_CONFIG
    time_us: int = Field(default=0, validation_alias="timeUs")


class RecoveryStageHits(BaseModel):
    model_config = _MODEL_CONFIG
    hits: int = 0
    time_us: int = Field(default=0, validation_alias="timeUs")


class RecoveryLookupStage(BaseModel):
    model_config = _MODEL_CONFIG
    count: int = 0
    rows: int = 0
    time_us: int = Field(default=0, validation_alias="timeUs")


class RecoveryLookups(BaseModel):
    model_config = _MODEL_CONFIG
    latest_history: RecoveryLookupStage = Field(
        default_factory=RecoveryLookupStage, validation_alias="latestHistory"
    )
    direct_hash: RecoveryLookupStage = Field(
        default_factory=RecoveryLookupStage, validation_alias="directHash"
    )
    git_identity: RecoveryLookupStage = Field(
        default_factory=RecoveryLookupStage, validation_alias="gitIdentity"
    )
    scan_history: RecoveryLookupStage = Field(
        default_factory=RecoveryLookupStage, validation_alias="scanHistory"
    )


class RecoveryGitIdentityCounters(BaseModel):
    model_config = _MODEL_CONFIG
    attempts: int = 0
    candidates: int = 0
    accepted: int = 0
    rejected: int = 0
    time_us: int = Field(default=0, validation_alias="timeUs")


class RecoveryAcceptanceCounters(BaseModel):
    model_config = _MODEL_CONFIG
    implicit_guard_candidates: int = Field(default=0, validation_alias="implicitGuardCandidates")
    implicit_guard_full_trace_loads: int = Field(
        default=0, validation_alias="implicitGuardFullTraceLoads"
    )
    implicit_guard_checks: int = Field(default=0, validation_alias="implicitGuardChecks")
    implicit_guard_failures: int = Field(default=0, validation_alias="implicitGuardFailures")
    implicit_guard_time_us: int = Field(default=0, validation_alias="implicitGuardTimeUs")


class RecoveryCounters(BaseModel):
    model_config = _MODEL_CONFIG
    attempts: int = 0
    failures: int = 0
    time_us: int = Field(default=0, validation_alias="timeUs")
    acceptance: RecoveryAcceptanceCounters = Field(default_factory=RecoveryAcceptanceCounters)
    direct_hash: RecoveryStageHits = Field(
        default_factory=RecoveryStageHits, validation_alias="directHash"
    )
    struct_variant: RecoveryStageHits = Field(
        default_factory=RecoveryStageHits, validation_alias="structVariant"
    )
    git_identity: RecoveryGitIdentityCounters = Field(
        default_factory=RecoveryGitIdentityCounters, validation_alias="gitIdentity"
    )
    git_identity_hits: int = Field(default=0, validation_alias="gitIdentityHits")
    history_bootstraps: int = Field(default=0, validation_alias="historyBootstraps")
    lookups: RecoveryLookups = Field(default_factory=RecoveryLookups)


class DepTrackerCounters(BaseModel):
    model_config = _MODEL_CONFIG
    scopes: int = 0
    own_deps_total: int = Field(default=0, validation_alias="ownDepsTotal")
    own_deps_max: int = Field(default=0, validation_alias="ownDepsMax")


class ReplayCounters(BaseModel):
    model_config = _MODEL_CONFIG
    total_calls: int = Field(default=0, validation_alias="totalCalls")
    bloom_hits: int = Field(default=0, validation_alias="bloomHits")
    epoch_hits: int = Field(default=0, validation_alias="epochHits")
    added: int = 0


class DepHashCounters(BaseModel):
    """`evalTrace.depHash.*` — hot-path timing + SV phase decomposition."""

    model_config = _MODEL_CONFIG
    cache_hits: int = Field(default=0, validation_alias="cacheHits")
    cache_misses: int = Field(default=0, validation_alias="cacheMisses")
    structured_misses: int = Field(default=0, validation_alias="structuredMisses")
    content_subsumption_skips: int = Field(default=0, validation_alias="contentSubsumptionSkips")

    content_us: int = Field(default=0, validation_alias="contentUs")
    directory_us: int = Field(default=0, validation_alias="directoryUs")
    existence_us: int = Field(default=0, validation_alias="existenceUs")
    store_path_us: int = Field(default=0, validation_alias="storePathUs")
    structured_json_us: int = Field(default=0, validation_alias="structuredJsonUs")
    structured_toml_us: int = Field(default=0, validation_alias="structuredTomlUs")
    structured_dir_us: int = Field(default=0, validation_alias="structuredDirUs")
    structured_nix_us: int = Field(default=0, validation_alias="structuredNixUs")
    structured_outer_us: int = Field(default=0, validation_alias="structuredOuterUs")
    git_identity_us: int = Field(default=0, validation_alias="gitIdentityUs")
    git_identity_misses: int = Field(default=0, validation_alias="gitIdentityMisses")
    other_us: int = Field(default=0, validation_alias="otherUs")

    sc_dir_set_misses: int = Field(default=0, validation_alias="scDirSetMisses")
    sc_json_parse_us: int = Field(default=0, validation_alias="scJsonParseUs")

    recovery_recompute_us: int = Field(default=0, validation_alias="recoveryRecomputeUs")
    recovery_recompute_count: int = Field(default=0, validation_alias="recoveryRecomputeCount")

    struct_variant_candidates: int = Field(default=0, validation_alias="structVariantCandidates")
    struct_variant_deps_resolved: int = Field(
        default=0, validation_alias="structVariantDepsResolved"
    )
    struct_variant_load_key_set_us: int = Field(
        default=0, validation_alias="structVariantLoadKeySetUs"
    )
    struct_variant_hash_us: int = Field(default=0, validation_alias="structVariantHashUs")
    struct_variant_dep_resolve_us: int = Field(
        default=0, validation_alias="structVariantDepResolveUs"
    )

    backend_setup_failed: int = Field(default=0, validation_alias="backendSetupFailed")
    resolve_via_registry: int = Field(default=0, validation_alias="resolveViaRegistry")
    resolve_via_path_object: int = Field(default=0, validation_alias="resolveViaPathObject")
    resolve_via_absolute: int = Field(default=0, validation_alias="resolveViaAbsolute")
    dep_record_no_active_context: int = Field(
        default=0, validation_alias="depRecordNoActiveContext"
    )


class ThunksCounters(BaseModel):
    model_config = _MODEL_CONFIG
    created: int = 0
    from_materialize: int = Field(default=0, validation_alias="fromMaterialize")
    from_data_file: int = Field(default=0, validation_alias="fromDataFile")
    forced: int = 0
    lazy_state_allocated: int = Field(default=0, validation_alias="lazyStateAllocated")


class DataFileCounters(BaseModel):
    model_config = _MODEL_CONFIG
    scalar_children: int = Field(default=0, validation_alias="scalarChildren")
    container_children: int = Field(default=0, validation_alias="containerChildren")


class ByDepKeySetEntry(BaseModel):
    """One row of `evalTrace.structVariant.byDepKeySet` (plan D series)."""

    model_config = _MODEL_CONFIG
    dep_key_set_id: int = Field(validation_alias="depKeySetId")
    tried: int = 0
    succeeded: int = 0
    aborted_early: int = Field(default=0, validation_alias="abortedEarly")
    hash_mismatch: int = Field(default=0, validation_alias="hashMismatch")
    avg_deps: float = Field(default=0.0, validation_alias="avgDeps")
    avg_us: float = Field(default=0.0, validation_alias="avgUs")
    both_set_count: int = Field(default=0, validation_alias="bothSetCount")
    earlier_hash_mismatch_count: int = Field(default=0, validation_alias="earlierHashMismatchCount")
    earlier_hash_mismatch_saved_deps: int = Field(
        default=0, validation_alias="earlierHashMismatchSavedDeps"
    )
    hash_mismatch_only_count: int = Field(default=0, validation_alias="hashMismatchOnlyCount")
    hash_mismatch_only_saved_deps: int = Field(
        default=0, validation_alias="hashMismatchOnlySavedDeps"
    )


class StructVariantSummary(BaseModel):
    model_config = _MODEL_CONFIG
    by_dep_key_set: list[ByDepKeySetEntry] = Field(
        default_factory=list[ByDepKeySetEntry], validation_alias="byDepKeySet"
    )


class EvalTraceStats(BaseModel):
    """`stats.json.evalTrace` subtree."""

    model_config = _MODEL_CONFIG

    hits: int = 0
    misses: int = 0
    db: DbCounters = Field(default_factory=DbCounters)
    load_trace: TimedCounter = Field(default_factory=TimedCounter, validation_alias="loadTrace")
    load_key_set: LoadKeySetCounters = Field(
        default_factory=LoadKeySetCounters, validation_alias="loadKeySet"
    )
    record: RecordCounters = Field(default_factory=RecordCounters)
    recovery: RecoveryCounters = Field(default_factory=RecoveryCounters)
    verify: VerifyCounters = Field(default_factory=VerifyCounters)
    verify_trace: VerifyTraceCounters = Field(
        default_factory=VerifyTraceCounters, validation_alias="verifyTrace"
    )
    thunks: ThunksCounters = Field(default_factory=ThunksCounters)
    data_file: DataFileCounters = Field(
        default_factory=DataFileCounters, validation_alias="dataFile"
    )
    dep_tracker: DepTrackerCounters = Field(
        default_factory=DepTrackerCounters, validation_alias="depTracker"
    )
    replay: ReplayCounters = Field(default_factory=ReplayCounters)
    dep_hash: DepHashCounters = Field(default_factory=DepHashCounters, validation_alias="depHash")
    struct_variant: StructVariantSummary = Field(
        default_factory=StructVariantSummary, validation_alias="structVariant"
    )


class GcStats(BaseModel):
    model_config = _MODEL_CONFIG
    heap_size: int = Field(default=0, validation_alias="heapSize")
    total_bytes: int = Field(default=0, validation_alias="totalBytes")
    cycles: int = 0


class TimeStats(BaseModel):
    model_config = _MODEL_CONFIG
    gc: float = 0.0


class RunStats(BaseModel):
    """Top-level `stats.json` document.  `raw` preserves the unknown keys."""

    model_config = _MODEL_CONFIG

    cpu_time: float = Field(default=0.0, validation_alias="cpuTime")
    nr_thunks: int = Field(default=0, validation_alias="nrThunks")
    nr_function_calls: int = Field(default=0, validation_alias="nrFunctionCalls")
    nr_prim_op_calls: int = Field(default=0, validation_alias="nrPrimOpCalls")
    gc: GcStats = Field(default_factory=GcStats)
    time: TimeStats = Field(default_factory=TimeStats)
    eval_trace: EvalTraceStats = Field(default_factory=EvalTraceStats, validation_alias="evalTrace")

    # The original JSON object — kept so the full-key dump in `logs`
    # (plan H1) can surface counters we don't model, and so we can
    # round-trip to JSON for CSV/JSON export.
    raw: dict[str, Any] = Field(default_factory=dict[str, Any])


class Timing(BaseModel):
    model_config = _MODEL_CONFIG
    wall_time: float | None = Field(default=None, validation_alias="wallTime")


def load_stats(payload: dict[str, Any]) -> RunStats:
    """Parse a `stats.json` payload and stash the original dict in `.raw`."""
    model = RunStats.model_validate(payload)
    return model.model_copy(update={"raw": payload})
