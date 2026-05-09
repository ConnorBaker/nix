"""Metric catalog for the `runs` / `logs` / `series` subcommands.

Each metric carries a typed `accessor` over the Pydantic model rather
than a dotted-path string, so pyright strict catches typos against the
live schema.  The `flat` label is still used for column headers.
"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass

from .models import RunStats

Accessor = Callable[[RunStats], float]


@dataclass(frozen=True)
class Metric:
    label: str
    accessor: Accessor
    fmt: str = ".0f"
    scale: float = 1.0

    def get(self, stats: RunStats | None) -> float | None:
        if stats is None:
            return None
        try:
            return float(self.accessor(stats)) * self.scale
        except (AttributeError, TypeError):
            return None


def _et(accessor: Callable[[RunStats], float]) -> Accessor:
    return accessor


EVAL_TRACE_METRICS: list[Metric] = [
    Metric("hits", _et(lambda s: s.eval_trace.hits)),
    Metric("misses", _et(lambda s: s.eval_trace.misses)),
    Metric("verify.count", _et(lambda s: s.eval_trace.verify.count)),
    Metric("verify.passed", _et(lambda s: s.eval_trace.verify.passed)),
    Metric("verify.failed", _et(lambda s: s.eval_trace.verify.failed)),
    Metric("recovery.attempts", _et(lambda s: s.eval_trace.recovery.attempts)),
    Metric(
        "recovery.gitIdentity.attempts",
        _et(lambda s: s.eval_trace.recovery.git_identity.attempts),
    ),
    Metric(
        "recovery.gitIdentity.candidates",
        _et(lambda s: s.eval_trace.recovery.git_identity.candidates),
    ),
    Metric(
        "recovery.gitIdentity.rejected",
        _et(lambda s: s.eval_trace.recovery.git_identity.rejected),
    ),
    Metric("recovery.directHash.hits", _et(lambda s: s.eval_trace.recovery.direct_hash.hits)),
    Metric(
        "recovery.structVariant.hits",
        _et(lambda s: s.eval_trace.recovery.struct_variant.hits),
    ),
    Metric("recovery.gitIdentityHits", _et(lambda s: s.eval_trace.recovery.git_identity_hits)),
    Metric(
        "recovery.historyBootstraps",
        _et(lambda s: s.eval_trace.recovery.history_bootstraps),
    ),
    Metric("recovery.failures", _et(lambda s: s.eval_trace.recovery.failures)),
    Metric(
        "recovery.implicitGuard.candidates",
        _et(lambda s: s.eval_trace.recovery.acceptance.implicit_guard_candidates),
    ),
    Metric(
        "recovery.implicitGuard.checks",
        _et(lambda s: s.eval_trace.recovery.acceptance.implicit_guard_checks),
    ),
    Metric(
        "recovery.implicitGuard.failures",
        _et(lambda s: s.eval_trace.recovery.acceptance.implicit_guard_failures),
    ),
    Metric("record.count", _et(lambda s: s.eval_trace.record.count)),
    Metric("loadTrace.count", _et(lambda s: s.eval_trace.load_trace.count)),
    Metric("loadKeySet.count", _et(lambda s: s.eval_trace.load_key_set.count)),
    Metric("loadKeySet.cacheHits", _et(lambda s: s.eval_trace.load_key_set.cache_hits)),
    Metric("loadKeySet.cacheMisses", _et(lambda s: s.eval_trace.load_key_set.cache_misses)),
    Metric("depTracker.scopes", _et(lambda s: s.eval_trace.dep_tracker.scopes)),
    Metric("depTracker.ownDepsTotal", _et(lambda s: s.eval_trace.dep_tracker.own_deps_total)),
]


PERF_METRICS: list[Metric] = [
    Metric("cpuTime (s)", _et(lambda s: s.cpu_time), fmt=".2f"),
    Metric("nrThunks", _et(lambda s: s.nr_thunks)),
    Metric("nrFunctionCalls", _et(lambda s: s.nr_function_calls)),
    Metric("nrPrimOpCalls", _et(lambda s: s.nr_prim_op_calls)),
    Metric("gc.heapSize (MB)", _et(lambda s: s.gc.heap_size), fmt=".2f", scale=1 / (1024 * 1024)),
    Metric(
        "gc.totalBytes (MB)",
        _et(lambda s: s.gc.total_bytes),
        fmt=".2f",
        scale=1 / (1024 * 1024),
    ),
    Metric("gc.cycles", _et(lambda s: s.gc.cycles)),
    Metric("time.gc (s)", _et(lambda s: s.time.gc), fmt=".2f"),
]


# Per-dep-hash timing break-down — H2 in the plan doc.
DEP_HASH_US_METRICS: list[Metric] = [
    Metric("depHash.contentUs", _et(lambda s: s.eval_trace.dep_hash.content_us)),
    Metric("depHash.directoryUs", _et(lambda s: s.eval_trace.dep_hash.directory_us)),
    Metric("depHash.existenceUs", _et(lambda s: s.eval_trace.dep_hash.existence_us)),
    Metric("depHash.storePathUs", _et(lambda s: s.eval_trace.dep_hash.store_path_us)),
    Metric(
        "depHash.structuredJsonUs",
        _et(lambda s: s.eval_trace.dep_hash.structured_json_us),
    ),
    Metric(
        "depHash.structuredTomlUs",
        _et(lambda s: s.eval_trace.dep_hash.structured_toml_us),
    ),
    Metric("depHash.structuredDirUs", _et(lambda s: s.eval_trace.dep_hash.structured_dir_us)),
    Metric("depHash.structuredNixUs", _et(lambda s: s.eval_trace.dep_hash.structured_nix_us)),
    Metric(
        "depHash.structuredOuterUs",
        _et(lambda s: s.eval_trace.dep_hash.structured_outer_us),
    ),
    Metric("depHash.gitIdentityUs", _et(lambda s: s.eval_trace.dep_hash.git_identity_us)),
    Metric("depHash.otherUs", _et(lambda s: s.eval_trace.dep_hash.other_us)),
    Metric("record.hashUs", _et(lambda s: s.eval_trace.record.hash_us)),
    Metric("record.serializeKeysUs", _et(lambda s: s.eval_trace.record.serialize_keys_us)),
    Metric("record.serializeValuesUs", _et(lambda s: s.eval_trace.record.serialize_values_us)),
    Metric("record.flushUs", _et(lambda s: s.eval_trace.record.flush_us)),
    Metric("loadKeySet.timeUs", _et(lambda s: s.eval_trace.load_key_set.time_us)),
    Metric(
        "recovery.lookup.latestHistoryUs",
        _et(lambda s: s.eval_trace.recovery.lookups.latest_history.time_us),
    ),
    Metric(
        "recovery.lookup.directHashUs",
        _et(lambda s: s.eval_trace.recovery.lookups.direct_hash.time_us),
    ),
    Metric(
        "recovery.lookup.gitIdentityUs",
        _et(lambda s: s.eval_trace.recovery.lookups.git_identity.time_us),
    ),
    Metric(
        "recovery.lookup.scanHistoryUs",
        _et(lambda s: s.eval_trace.recovery.lookups.scan_history.time_us),
    ),
    Metric(
        "recovery.gitIdentity.timeUs",
        _et(lambda s: s.eval_trace.recovery.git_identity.time_us),
    ),
    Metric(
        "recovery.implicitGuard.timeUs",
        _et(lambda s: s.eval_trace.recovery.acceptance.implicit_guard_time_us),
    ),
]


# SV phase-time decomposition — used by both `series` and `runs`.
SV_TIME_METRICS: list[Metric] = [
    Metric("sv.candidates", _et(lambda s: s.eval_trace.dep_hash.struct_variant_candidates)),
    Metric(
        "sv.depsResolved",
        _et(lambda s: s.eval_trace.dep_hash.struct_variant_deps_resolved),
    ),
    Metric(
        "sv.loadKeySetUs",
        _et(lambda s: s.eval_trace.dep_hash.struct_variant_load_key_set_us),
    ),
    Metric("sv.hashUs", _et(lambda s: s.eval_trace.dep_hash.struct_variant_hash_us)),
    Metric(
        "sv.depResolveUs",
        _et(lambda s: s.eval_trace.dep_hash.struct_variant_dep_resolve_us),
    ),
]
