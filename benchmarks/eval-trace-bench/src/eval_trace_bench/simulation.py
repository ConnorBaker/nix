"""Simulation harness for structural-variant recovery policies.

The `byDepKeySet` telemetry recorded by each commit gives us a replay
log: for every (commit, depKeySetId) we know how many candidates were
tried, how many succeeded, how many aborted early, and the mean time
per candidate.  A *policy* takes this log and decides which candidates
the SV loop would have tried under an alternative strategy.  The
simulator then tallies `(saved_tries, saved_us, missed_wins, lost_us)`
against the recorded truth.

Policies can compose: every policy's `decide(state, event)` returns
`SKIP` / `TRY`.  A commit visits its buckets in the order they appear
in the replay log (which is `tried`-desc, SV's current scheduling —
see `src/libexpr/eval.cc` around the `byDepKeySet` emit).

The replay semantics are intentionally conservative:

- A SKIP saves `event.tried × event.avg_us` time and sacrifices
  `event.succeeded × event.avg_us` potential wins.
- A TRY preserves recorded behaviour — it counts neither savings nor
  losses against the policy.

This is a *pessimistic lower bound* on policy value: a policy that
never skips anything has zero savings and zero losses.  A policy that
skips a winning bucket has nonzero `missed_wins`, which the report
colours red.

The historical reference policy is `NeverSkip` (the SV loop as
implemented today).  OPP-1, newest-first prefix, top-K, success-
weighted, and time-budget are all proposals.
"""

from __future__ import annotations

import statistics
from collections.abc import Iterable
from dataclasses import dataclass, field
from typing import Protocol, runtime_checkable

from .dataframe import Record


@dataclass(frozen=True)
class BucketEvent:
    """One (commit, bucket) observation extracted from the replay log."""

    proc_idx: int
    commit: str
    dep_key_set_id: int
    tried: int
    succeeded: int
    aborted_early: int
    hash_mismatch: int
    avg_us: float
    avg_deps: float
    # Position of this event within its commit (0-indexed; 0 is the
    # first bucket tried by the SV loop on this commit).
    position_in_commit: int


@dataclass
class SimResult:
    """Cumulative savings + losses across the whole replay."""

    saved_tries: int = 0
    saved_us: float = 0.0
    missed_wins: int = 0
    lost_us: float = 0.0
    skipped_bucket_events: int = 0
    tried_bucket_events: int = 0


@dataclass
class PolicyState:
    """Per-policy mutable state threaded through the replay."""

    failing_streak: dict[int, int] = field(default_factory=dict[int, int])
    """Per-depKeySetId consecutive-fail streak (OPP-1)."""
    permanent_skip: set[int] = field(default_factory=set[int])
    """Buckets this policy has decided to skip henceforth (OPP-1)."""
    bucket_history: dict[int, tuple[int, int]] = field(default_factory=dict[int, tuple[int, int]])
    """Cumulative (success, tries) per bucket (success-weighted)."""
    commit_time_consumed: float = 0.0
    """Time budget consumed within the current commit (time-budget)."""
    commit_tries_used: int = 0
    """Number of tries within the current commit (top-K / prefix)."""
    current_commit: str = ""
    """Current commit id — policies reset per-commit state on boundary."""


class Policy(Protocol):
    name: str

    def decide(self, state: PolicyState, event: BucketEvent) -> bool:
        """Return True to TRY the bucket, False to SKIP it."""
        ...

    def reset_commit(self, state: PolicyState, commit: str) -> None:
        """Called when the replay crosses a commit boundary."""
        ...


@runtime_checkable
class ObservingPolicy(Protocol):
    def observe(self, state: PolicyState, event: BucketEvent, tried: bool) -> None:
        """Update historical state after the policy decision."""
        ...


# -- Built-in policies -------------------------------------------------------


@dataclass
class NeverSkip:
    """Historical reference: SV loop as implemented today.  Always tries."""

    name: str = "never-skip"

    def decide(self, state: PolicyState, event: BucketEvent) -> bool:
        return True

    def reset_commit(self, state: PolicyState, commit: str) -> None:
        return


@dataclass
class OPP1:
    """Skip a bucket once it has failed `threshold` times in a row.

    Per the plan doc (§D4): "replay the bucket-try sequence in
    processing order with a 'skip after N consecutive fails' heuristic".
    """

    threshold: int
    name: str = field(init=False)

    def __post_init__(self) -> None:
        self.name = f"opp1-T{self.threshold}"

    def decide(self, state: PolicyState, event: BucketEvent) -> bool:
        return event.dep_key_set_id not in state.permanent_skip

    def reset_commit(self, state: PolicyState, commit: str) -> None:
        return

    def observe(self, state: PolicyState, event: BucketEvent, tried: bool) -> None:
        """Called by `run_simulation` AFTER `decide`, so the policy can
        update its internal streak counters against the recorded truth.
        """
        if not tried:
            return
        if event.succeeded > 0:
            state.failing_streak[event.dep_key_set_id] = 0
        else:
            state.failing_streak[event.dep_key_set_id] = (
                state.failing_streak.get(event.dep_key_set_id, 0) + event.tried
            )
            if state.failing_streak[event.dep_key_set_id] >= self.threshold:
                state.permanent_skip.add(event.dep_key_set_id)


@dataclass
class TopKPrefix:
    """Try only the first K buckets each commit.

    Proxy for the plan's D5 "order-preserving vector" ceiling: if the
    SV loop were to visit buckets in DESC trace_id order (which the
    emit order of `byDepKeySet` already approximates), stopping after
    K candidates bounds the worst-case commit cost without a learned
    policy.
    """

    k: int
    name: str = field(init=False)

    def __post_init__(self) -> None:
        self.name = f"top-{self.k}"

    def decide(self, state: PolicyState, event: BucketEvent) -> bool:
        return event.position_in_commit < self.k

    def reset_commit(self, state: PolicyState, commit: str) -> None:
        return


@dataclass
class TimeBudget:
    """Skip every remaining bucket once commit-local `sum(tried × avgUs)`
    exceeds `budget_ms`.

    Models the "per-commit SV budget" the plan doc discusses in §A5.
    """

    budget_ms: float
    name: str = field(init=False)

    def __post_init__(self) -> None:
        self.name = f"budget-{self.budget_ms:.0f}ms"

    def decide(self, state: PolicyState, event: BucketEvent) -> bool:
        return state.commit_time_consumed < self.budget_ms * 1000.0

    def reset_commit(self, state: PolicyState, commit: str) -> None:
        state.commit_time_consumed = 0.0


@dataclass
class SuccessWeighted:
    """Skip buckets whose cumulative success rate is below `rate`.

    Until a bucket has been tried at least `warmup` times, the policy
    always tries it.  After warmup, `(succeeded + 1) / (tried + 2)`
    (Laplace-smoothed) must clear `rate`.
    """

    rate: float
    warmup: int = 5
    name: str = field(init=False)

    def __post_init__(self) -> None:
        self.name = f"success>={self.rate:.2f}"

    def decide(self, state: PolicyState, event: BucketEvent) -> bool:
        succ, tried = state.bucket_history.get(event.dep_key_set_id, (0, 0))
        if tried < self.warmup:
            return True
        return (succ + 1) / (tried + 2) >= self.rate

    def reset_commit(self, state: PolicyState, commit: str) -> None:
        return


@dataclass
class Combined:
    """Logical AND of child policies — a bucket is tried only if
    every child votes TRY.
    """

    children: list[Policy]
    label: str
    name: str = field(init=False)

    def __post_init__(self) -> None:
        self.name = self.label

    def decide(self, state: PolicyState, event: BucketEvent) -> bool:
        return all(child.decide(state, event) for child in self.children)

    def reset_commit(self, state: PolicyState, commit: str) -> None:
        for child in self.children:
            child.reset_commit(state, commit)


# -- Replay extraction + driver ----------------------------------------------


def _observe_policy(policy: Policy, state: PolicyState, event: BucketEvent, tried: bool) -> None:
    if isinstance(policy, Combined):
        for child in policy.children:
            _observe_policy(child, state, event, tried)
    elif isinstance(policy, ObservingPolicy):
        policy.observe(state, event, tried)


def extract_events(records: Iterable[Record]) -> list[BucketEvent]:
    """Turn the (commit-ordered) records into a flat replay log."""
    out: list[BucketEvent] = []
    for rec in sorted(records, key=lambda r: r.proc_idx):
        if rec.et is None:
            continue
        entries = rec.et.struct_variant.by_dep_key_set
        for position, entry in enumerate(entries):
            out.append(
                BucketEvent(
                    proc_idx=rec.proc_idx,
                    commit=rec.commit,
                    dep_key_set_id=entry.dep_key_set_id,
                    tried=entry.tried,
                    succeeded=entry.succeeded,
                    aborted_early=entry.aborted_early,
                    hash_mismatch=entry.hash_mismatch,
                    avg_us=entry.avg_us,
                    avg_deps=entry.avg_deps,
                    position_in_commit=position,
                )
            )
    return out


def run_simulation(policy: Policy, events: list[BucketEvent]) -> SimResult:
    """Replay `events` under `policy` and return cumulative savings."""
    state = PolicyState()
    result = SimResult()
    last_commit = ""
    for event in events:
        if event.commit != last_commit:
            state.current_commit = event.commit
            state.commit_time_consumed = 0.0
            state.commit_tries_used = 0
            policy.reset_commit(state, event.commit)
            last_commit = event.commit

        try_it = policy.decide(state, event)
        if try_it:
            # Preserves recorded behaviour — no savings or losses.
            result.tried_bucket_events += 1
            state.commit_time_consumed += event.avg_us * event.tried
            state.commit_tries_used += event.tried
        else:
            result.skipped_bucket_events += 1
            result.saved_tries += event.tried
            result.saved_us += event.avg_us * event.tried
            if event.succeeded > 0:
                result.missed_wins += event.succeeded
                result.lost_us += event.avg_us * event.succeeded

        # Let policies update any historical counters.
        _observe_policy(policy, state, event, try_it)
        succ_prev, tried_prev = state.bucket_history.get(event.dep_key_set_id, (0, 0))
        state.bucket_history[event.dep_key_set_id] = (
            succ_prev + (event.succeeded if try_it else 0),
            tried_prev + (event.tried if try_it else 0),
        )
    return result


# -- Retrospective (not a policy — a measurement) ---------------------------


@dataclass(frozen=True)
class WinnerRankSummary:
    """D5: where does the winning bucket land within each commit?

    The SV loop emits `byDepKeySet` sorted by `tried` descending; we
    surface the rank of the succeeded bucket within that ordering to
    quantify how close SV already is to an "order-preserving" ideal.
    """

    total_commits_with_wins: int
    mean_rank: float
    median_rank: float
    top_3_share: float
    top_10_share: float
    ranks: list[int]


def winner_ranks(records: Iterable[Record]) -> WinnerRankSummary:
    ranks: list[int] = []
    for rec in records:
        if rec.et is None:
            continue
        entries = rec.et.struct_variant.by_dep_key_set
        for position, entry in enumerate(entries):
            if entry.succeeded > 0:
                ranks.append(position)
                break  # The first successful bucket terminates SV.
    n = len(ranks)
    if n == 0:
        return WinnerRankSummary(0, 0.0, 0.0, 0.0, 0.0, [])
    sorted_ranks = sorted(ranks)
    mean = statistics.mean(ranks)
    median = statistics.median(sorted_ranks)
    top3 = sum(1 for r in ranks if r < 3) / n
    top10 = sum(1 for r in ranks if r < 10) / n
    return WinnerRankSummary(n, mean, float(median), top3, top10, ranks)
