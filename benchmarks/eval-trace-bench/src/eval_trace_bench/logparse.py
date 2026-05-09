"""Parsing and aggregation for `nix --debug` output.

The log is a line-oriented stream of messages emitted by `debug()`
calls in libexpr and related subsystems.  eval-trace specifically uses
a structured `eval-trace/<sub>: ...` prefix (see
`src/libexpr/eval-trace/CLAUDE.md` → "Debug-log Conventions") — those
lines carry key-value pairs that the `logs` subcommand slices.
"""

from __future__ import annotations

import json
import re
from collections import Counter
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

DAEMON_OP_NAMES: dict[int, str] = {
    1: "IsValidPath",
    2: "HasSubstitutes",
    3: "QueryPathHash",
    4: "QueryReferences",
    5: "QueryReferrers",
    6: "AddToStore",
    7: "AddTextToStore (legacy)",
    8: "BuildPaths",
    9: "EnsurePath",
    10: "AddTempRoot",
    11: "AddIndirectRoot",
    12: "SyncWithGC",
    13: "FindRoots",
    14: "ExportPath (removed)",
    15: "QueryDeriver",
    16: "SetOptions",
    17: "CollectGarbage",
    18: "QuerySubstitutablePathInfo",
    19: "QueryDerivationOutputs (removed)",
    20: "QueryAllValidPaths",
    21: "QueryFailedPaths (removed)",
    22: "ClearFailedPaths (removed)",
    23: "QueryPathInfo",
    24: "ImportPaths (removed)",
    25: "QueryDerivationOutputNames (removed)",
    26: "QueryPathFromHashPart",
    27: "QuerySubstitutablePathInfos",
    28: "QueryValidPaths",
    29: "QuerySubstitutablePaths",
    30: "QueryValidDerivers",
    31: "OptimiseStore",
    32: "VerifyStore",
    33: "BuildDerivation",
    34: "AddSignatures",
    35: "NarFromPath",
    36: "AddToStoreNar",
    37: "QueryMissing",
    38: "QueryDerivationOutputMap",
    39: "RegisterDrvOutput",
    40: "QueryRealisation",
    41: "AddMultipleToStore",
    42: "AddBuildLog",
    43: "BuildPathsWithResults",
    44: "AddPermRoot",
}


_KV_RE = re.compile(r"(\w+)=(?:'([^']*)'|\"([^\"]*)\"|(\S+))")
_EVAL_TRACE_PREFIX = re.compile(r"eval-trace/(\w+):\s*(.*)$")

# Secondary parsers applied to the "rest" string of `eval-trace/recovery`
# events.  Each one peels off the specific shape described in its docstring.
_SV_ABORT_RE = re.compile(
    r"SV candidate depKeySet=\d+ traceId=\d+ abort reason=(\w+) kind=(\w+) key='(.*?)'"
)
_SV_HASH_MISMATCH_RE = re.compile(
    r"SV candidate depKeySet=(\d+) traceId=(\d+) hash-mismatch: candidate=\S+ \((\d+) deps resolved\)"
)
_RECOMPUTE_RE = re.compile(r"recomputed (\d+)/(\d+) dep hashes for '(.+?)'")
_SUCCESS_GIT = "GitIdentity-indexed recovery succeeded"
_SUCCESS_DIRECT = "direct hash recovery succeeded"
_SUCCESS_SV = "structural variant recovery succeeded"


# (pattern, kind, transform) — the transform receives the regex `Match`
# and returns the LogEvent.data dict.  None means the event has no data.
_PatternTransform = Callable[[re.Match[str]], dict[str, Any]]
_PATTERNS: list[tuple[re.Pattern[str], str, _PatternTransform]] = [
    (
        _EVAL_TRACE_PREFIX,
        "eval_trace",
        lambda m: {"sub": m.group(1), "rest": m.group(2), "kv": _extract_kv(m.group(2))},
    ),
    (
        re.compile(r"copying '(.+?)' to the store\.\.\."),
        "copy_start",
        lambda m: {"path": m.group(1)},
    ),
    (
        re.compile(r"copied '(.+?)' to '(.+?)' \(hash '(.+?)'\)"),
        "copy_done",
        lambda m: {"src": m.group(1), "dst": m.group(2), "hash": m.group(3)},
    ),
    (
        re.compile(r"source path '(.+?)' is uncacheable"),
        "uncacheable",
        lambda m: {"path": m.group(1)},
    ),
    (
        re.compile(r"performing daemon worker op: (\d+)"),
        "daemon_op",
        lambda m: {"op": int(m.group(1))},
    ),
    (
        re.compile(r"evaluating file '(.+?)'"),
        "eval_file",
        lambda m: {"path": m.group(1)},
    ),
    (
        re.compile(r"instantiated '(.+?)' -> '(.+?)'"),
        "instantiated",
        lambda m: {"name": m.group(1), "drv": m.group(2)},
    ),
    (
        re.compile(r"recording (\w+) \((\w+)\) dep: input='(.*?)' key='(.*?)'"),
        "recording_dep",
        lambda m: {
            "type": m.group(1),
            "variant": m.group(2),
            "input": m.group(3),
            "key": m.group(4),
        },
    ),
    (
        re.compile(r"trace verify hit for '(.+?)'"),
        "trace_hit",
        lambda m: {"attr": m.group(1)},
    ),
    (
        re.compile(r"stat hash store: loading (\d+) entries"),
        "stat_hash_store",
        lambda m: {"count": int(m.group(1))},
    ),
]

# Bare-prefix matches — no regex needed.
_PREFIX_EVENTS: list[tuple[str, str, bool]] = [
    ("getting root value", "root_value", False),  # no data
    ('{"', "output_json", True),  # store the line
    ("warning:", "warning", True),
]


@dataclass
class LogEvent:
    lineno: int
    kind: str
    data: dict[str, Any] = field(default_factory=dict[str, Any])


@dataclass
class PassData:
    index: int
    start_line: int
    end_line: int
    instantiated: list[tuple[str, str]]
    drv_paths: set[str]
    eval_files: list[str]
    copies: list[str]
    daemon_ops: Counter[int]
    recordings: list[tuple[str, str, str, str]]
    trace_hits: list[tuple[int, str]]
    uncacheable: list[str]
    event_count: int


@dataclass(frozen=True)
class SvAbort:
    """An `eval-trace/recovery: SV candidate ... abort reason=X kind=Y key='Z'` line."""

    reason: str  # resolveFailed | volatile | traceContextMiss
    kind: str
    key: str


@dataclass(frozen=True)
class SvHashMismatch:
    """An `eval-trace/recovery: SV candidate ... hash-mismatch: candidate=H (N deps resolved)` line."""

    dep_key_set_id: int
    trace_id: int
    deps_resolved: int


@dataclass(frozen=True)
class RecomputeSample:
    """An `eval-trace/recovery: recomputed N/M dep hashes for '<attr>'` line.

    `n` = deps recomputed, `m` = deps recorded; `n < m` means some deps
    were unresolvable at recovery time (DirectHash would be partial).
    """

    n: int
    m: int
    attr: str

    @property
    def is_full(self) -> bool:
        return self.n == self.m


@dataclass
class RecoverySuccess:
    """Count of recovery-strategy success lines by strategy name."""

    git_identity: int = 0
    direct_hash: int = 0
    struct_variant: int = 0


@dataclass
class LogSummary:
    copying_to_store: list[str]
    copying_nixpkgs: int
    daemon_ops: Counter[int]
    evaluating_files: list[str]
    instantiated: list[tuple[str, str]]
    recording_deps: Counter[str]
    recording_deps_detail: list[tuple[str, str, str, str]]
    stats_json: dict[str, Any] | None
    total_lines: int
    events: list[LogEvent]
    copy_done: list[tuple[str, str, str]]
    uncacheable: list[str]
    trace_hits: list[tuple[int, str]]
    passes: list[PassData]
    eval_trace_events: dict[str, list[dict[str, str]]] = field(
        default_factory=dict[str, list[dict[str, str]]]
    )
    # Structured recovery-line extractions (plan E-series).
    sv_aborts: list[SvAbort] = field(default_factory=list[SvAbort])
    sv_hash_mismatches: list[SvHashMismatch] = field(default_factory=list[SvHashMismatch])
    recompute_samples: list[RecomputeSample] = field(default_factory=list[RecomputeSample])
    recovery_successes: RecoverySuccess = field(default_factory=RecoverySuccess)


def _extract_kv(rest: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for m in _KV_RE.finditer(rest):
        key = m.group(1)
        value = (
            m.group(2)
            if m.group(2) is not None
            else (m.group(3) if m.group(3) is not None else m.group(4))
        )
        out[key] = value
    return out


def _match_line(line: str) -> tuple[str, dict[str, Any]] | None:
    """Return `(kind, data)` for `line` or `None` if it doesn't match."""
    for pattern, kind, transform in _PATTERNS:
        m = pattern.match(line)
        if m:
            return kind, transform(m)
    for prefix, kind, keep_line in _PREFIX_EVENTS:
        if line.startswith(prefix):
            data: dict[str, Any] = {}
            if keep_line:
                data = {"line" if prefix == '{"' else "msg": line}
            return kind, data
    return None


def parse_log_events(path: str | Path) -> list[LogEvent]:
    events: list[LogEvent] = []
    json_lines: list[str] = []
    in_json = False
    json_depth = 0

    with open(path) as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.rstrip("\n")

            if in_json:
                json_lines.append(line)
                json_depth += line.count("{") - line.count("}")
                if json_depth == 0:
                    try:
                        obj: Any = json.loads("\n".join(json_lines))
                        if isinstance(obj, dict):
                            events.append(LogEvent(lineno, "stats_json", {"json": obj}))
                    except json.JSONDecodeError:
                        pass
                    in_json = False
                continue
            if line == "{":
                in_json = True
                json_depth = 1
                json_lines = [line]
                continue

            match = _match_line(line)
            if match is not None:
                kind, data = match
                events.append(LogEvent(lineno, kind, data))

    return events


def detect_passes(events: list[LogEvent], nixpkgs_dir: str | None) -> list[PassData]:
    boundaries = [0]
    if nixpkgs_dir:
        for i, e in enumerate(events):
            if e.kind == "copy_start" and e.data.get("path") == nixpkgs_dir:
                boundaries.append(i)
    boundaries.append(len(events))

    passes: list[PassData] = []
    for j in range(len(boundaries) - 1):
        start = boundaries[j]
        end = boundaries[j + 1]
        span = events[start:end]

        drvs: list[tuple[str, str]] = [
            (str(e.data["name"]), str(e.data["drv"])) for e in span if e.kind == "instantiated"
        ]
        evals: list[str] = [str(e.data["path"]) for e in span if e.kind == "eval_file"]
        copies: list[str] = [str(e.data["path"]) for e in span if e.kind == "copy_start"]
        daemon: Counter[int] = Counter(int(e.data["op"]) for e in span if e.kind == "daemon_op")
        recs: list[tuple[str, str, str, str]] = [
            (
                str(e.data["type"]),
                str(e.data["variant"]),
                str(e.data["input"]),
                str(e.data["key"]),
            )
            for e in span
            if e.kind == "recording_dep"
        ]
        hits: list[tuple[int, str]] = [
            (e.lineno, str(e.data["attr"])) for e in span if e.kind == "trace_hit"
        ]
        uncache: list[str] = [str(e.data["path"]) for e in span if e.kind == "uncacheable"]

        passes.append(
            PassData(
                index=j,
                start_line=span[0].lineno if span else 0,
                end_line=span[-1].lineno if span else 0,
                instantiated=drvs,
                drv_paths={d[1] for d in drvs},
                eval_files=evals,
                copies=copies,
                daemon_ops=daemon,
                recordings=recs,
                trace_hits=hits,
                uncacheable=uncache,
                event_count=len(span),
            )
        )
    return passes


def _parse_recovery_rest(
    rest: str,
    sv_aborts: list[SvAbort],
    sv_hash_mismatches: list[SvHashMismatch],
    recompute_samples: list[RecomputeSample],
    recovery_successes: RecoverySuccess,
) -> None:
    """Slice the `rest` string of an `eval-trace/recovery:` event into
    structured records (E3/E4/E5/E6 in the plan doc).
    """
    if (m := _SV_ABORT_RE.search(rest)) is not None:
        sv_aborts.append(SvAbort(reason=m.group(1), kind=m.group(2), key=m.group(3)))
        return
    if (m := _SV_HASH_MISMATCH_RE.search(rest)) is not None:
        sv_hash_mismatches.append(
            SvHashMismatch(
                dep_key_set_id=int(m.group(1)),
                trace_id=int(m.group(2)),
                deps_resolved=int(m.group(3)),
            )
        )
        return
    if (m := _RECOMPUTE_RE.search(rest)) is not None:
        recompute_samples.append(
            RecomputeSample(n=int(m.group(1)), m=int(m.group(2)), attr=m.group(3))
        )
        return
    if _SUCCESS_GIT in rest:
        recovery_successes.git_identity += 1
    elif _SUCCESS_DIRECT in rest:
        recovery_successes.direct_hash += 1
    elif _SUCCESS_SV in rest:
        recovery_successes.struct_variant += 1


def aggregate_events(events: list[LogEvent], nixpkgs_dir: str | None) -> LogSummary:
    copying_to_store: list[str] = []
    copying_nixpkgs = 0
    daemon_ops: Counter[int] = Counter()
    evaluating_files: list[str] = []
    instantiated: list[tuple[str, str]] = []
    recording_deps: Counter[str] = Counter()
    recording_deps_detail: list[tuple[str, str, str, str]] = []
    stats_json: dict[str, Any] | None = None
    copy_done: list[tuple[str, str, str]] = []
    uncacheable: list[str] = []
    trace_hits: list[tuple[int, str]] = []
    eval_trace_events: dict[str, list[dict[str, str]]] = {}
    sv_aborts: list[SvAbort] = []
    sv_hash_mismatches: list[SvHashMismatch] = []
    recompute_samples: list[RecomputeSample] = []
    recovery_successes = RecoverySuccess()

    for e in events:
        if e.kind == "copy_start":
            p = str(e.data["path"])
            copying_to_store.append(p)
            if nixpkgs_dir and p == nixpkgs_dir:
                copying_nixpkgs += 1
        elif e.kind == "copy_done":
            copy_done.append((str(e.data["src"]), str(e.data["dst"]), str(e.data["hash"])))
        elif e.kind == "uncacheable":
            uncacheable.append(str(e.data["path"]))
        elif e.kind == "daemon_op":
            daemon_ops[int(e.data["op"])] += 1
        elif e.kind == "eval_file":
            evaluating_files.append(str(e.data["path"]))
        elif e.kind == "instantiated":
            instantiated.append((str(e.data["name"]), str(e.data["drv"])))
        elif e.kind == "recording_dep":
            label = f"{e.data['type']} ({e.data['variant']})"
            recording_deps[label] += 1
            recording_deps_detail.append(
                (
                    str(e.data["type"]),
                    str(e.data["variant"]),
                    str(e.data["input"]),
                    str(e.data["key"]),
                )
            )
        elif e.kind == "stats_json":
            obj: Any = e.data["json"]
            if isinstance(obj, dict):
                stats_json = obj  # pyright: ignore[reportUnknownVariableType]
        elif e.kind == "trace_hit":
            trace_hits.append((e.lineno, str(e.data["attr"])))
        elif e.kind == "eval_trace":
            sub = str(e.data["sub"])
            rest = str(e.data.get("rest", ""))
            kv: Any = e.data["kv"]
            if isinstance(kv, dict):
                # kv is extracted by `_extract_kv`, which always returns dict[str, str].
                kv_typed: dict[str, str] = kv  # pyright: ignore[reportAssignmentType, reportUnknownVariableType]
                eval_trace_events.setdefault(sub, []).append(kv_typed)
            if sub == "recovery" and rest:
                _parse_recovery_rest(
                    rest, sv_aborts, sv_hash_mismatches, recompute_samples, recovery_successes
                )

    return LogSummary(
        copying_to_store=copying_to_store,
        copying_nixpkgs=copying_nixpkgs,
        daemon_ops=daemon_ops,
        evaluating_files=evaluating_files,
        instantiated=instantiated,
        recording_deps=recording_deps,
        recording_deps_detail=recording_deps_detail,
        stats_json=stats_json,
        total_lines=events[-1].lineno if events else 0,
        events=events,
        copy_done=copy_done,
        uncacheable=uncacheable,
        trace_hits=trace_hits,
        passes=detect_passes(events, nixpkgs_dir),
        eval_trace_events=eval_trace_events,
        sv_aborts=sv_aborts,
        sv_hash_mismatches=sv_hash_mismatches,
        recompute_samples=recompute_samples,
        recovery_successes=recovery_successes,
    )


def analyze_recording_redundancy(
    details: list[tuple[str, str, str, str]],
) -> tuple[int, int, list[tuple[tuple[str, str, str, str], int]]]:
    key_counter: Counter[tuple[str, str, str, str]] = Counter(details)
    return len(details), len(key_counter), key_counter.most_common(30)
