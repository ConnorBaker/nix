"""Recovery-outcome classification for eval-trace-bench run analysis.

Splitting commits by their hit / miss / full-miss outcome is the
single most useful partition the plan doc identifies: mean cold wall
looks flat whenever the hit/miss mix drifts across buckets but the
worst-miss commits clearly degrade.
"""

from __future__ import annotations

from enum import StrEnum

from .models import EvalTraceStats


class OutcomeClass(StrEnum):
    HIT_ONLY = "hit-only"
    PARTIAL_MISS = "partial-miss"
    FULL_MISS = "full-miss"
    NO_RECOVERY = "no-recovery"  # no recovery attempts — reference or empty cache
    MISSING = "missing"  # commit absent from this run


def classify(attempts: int, failures: int) -> OutcomeClass:
    if attempts == 0:
        return OutcomeClass.NO_RECOVERY
    if failures == 0:
        return OutcomeClass.HIT_ONLY
    if failures == attempts:
        return OutcomeClass.FULL_MISS
    return OutcomeClass.PARTIAL_MISS


def classify_eval_trace(et: EvalTraceStats) -> OutcomeClass:
    return classify(et.recovery.attempts, et.recovery.failures)
