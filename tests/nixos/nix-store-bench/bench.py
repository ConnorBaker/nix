#!/usr/bin/env python3
"""bench: drive and interpret nix-store-bench VM runs.

Replaces the three former scripts (`run-matrix.sh`,
`compare-matrix.py`, `decide.py`) with a single CLI that shares the
iteration-parsing, stats, and matrix primitives.

Run `bench.py <subcommand> --help` for flag details. The five
subcommands are:

  run                     Build VM cells across the matrix via
                          `adhoc.nix`.
  summary <json>          Per-row stats for one bench JSON.
  ab <a> <b> [a_bpf b_bpf] --a-name N --b-name N
                          A/B two JSONs; bpftrace dumps optional.
  summary-matrix          One row per cell in `--results-dir`.
                          Add `--results-dir-b` to get a paired
                          per-cell table with Δmean + verdict.
  ab-matrix --axis <ax>   Pairwise A/B across one axis of the
                          matrix in a single dir.
  ab-matrix --results-dir-b <dir>
                          Pairwise A/B across two dirs by full
                          Cell identity. `--only pass/fail/missing/
                          nonpass` filters per-pair output; the
                          aggregate footer spans every pair.
"""

from __future__ import annotations

import argparse
import functools
import json
import os
import re
import shlex
import shutil
import statistics
import subprocess
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path

# UTF-8 by default; opt out via BENCH_ASCII=1 for cron/log environments
# whose locale can't roundtrip Δ, em-dash, ∈, ≥, ≤. Affects display
# only — failure messages embed numbers we don't translate.
_ASCII = bool(os.environ.get("BENCH_ASCII"))
DELTA = "d" if _ASCII else "Δ"
EM_DASH = "--" if _ASCII else "—"
GE = ">=" if _ASCII else "≥"
LE = "<=" if _ASCII else "≤"
IN_OP = "in" if _ASCII else "∈"

# Cells with `stddev/mean` above this fraction are flagged as noisy
# (a trailing `*` in tables, "[noisy]" in top-N entries, a count in
# the aggregate). Tuned so n=3 baseline runs that drift heavily stand
# out without spamming the whole table. Declared near the other
# module constants so AbResult.noisy isn't a forward reference.
NOISY_CV = 0.20

# --- axis taxonomy ---------------------------------------------------------
#
# Must track the `bench.*` enums in `bench-options.nix`. Add a new enum value
# here when you add one there (and to NAME_RE / DISPATCH_SUFFIX_RE for
# tail-axis additions that appear in the derivation name).

ALL_BENCHES = (
    "optimise",
    "optimise_migrate",
    "optimise_with_concurrent_gc",
    "gc_barabasi",
    "gc_clusters",
    "invalidate_paths",
)
ALL_FS = ("ext4", "xfs", "btrfs", "zfs", "tmpfs")
ALL_THROTTLES = ("gp3", "io2", "nvme", "none")
ALL_LAYOUTS = ("flat", "sharded")
ALL_REPLICAS = ("single", "multi")
ALL_DISPATCHES = ("syscall", "iouring")  # GC benches only
ALL_NPATHS = (2000, 10000, 50000)
ALL_THREADS = (4,)  # pass --threads 16 for thread-scaling cells

# Axis order = derivation-name order. Used for filter construction
# and for parsing result-* names back into Cell tuples.
AXES = ("bench", "dispatch", "fs", "throttle", "layout", "replica", "npaths", "threads")

# Axes that can't sensibly A/B: different `bench`es measure different
# things; different `npaths`/`threads` are different work sizes.
AB_AXES = tuple(a for a in AXES if a not in ("bench", "npaths", "threads"))

GC_BENCHES = frozenset({"gc_barabasi", "gc_clusters"})
NON_VARIANT_BENCHES = frozenset({"optimise_migrate", "invalidate_paths"})

# Default thresholds for the A/B "no regression" gate. io_uring's
# per-worker-rings rewrite lands at parity with the syscall path on
# the canonical gc_barabasi/ext4/flat/single cell — not a 10% win —
# so defaults tolerate up to 5% slower wall / 5% more syscalls
# without failing. Pass stricter values to demand a win.
VFS_PARITY_TOLERANCE_DEFAULT = 0.05
SYSCALL_REDUCTION_RATIO_DEFAULT = 1.05
WALL_IMPROVEMENT_MIN_DEFAULT = -0.05


@dataclass(frozen=True)
class Thresholds:
    """A/B gate thresholds. Built from parsed args by `Thresholds.from_args`."""

    vfs_tolerance: float = VFS_PARITY_TOLERANCE_DEFAULT
    syscall_ratio: float = SYSCALL_REDUCTION_RATIO_DEFAULT
    wall_improvement: float = WALL_IMPROVEMENT_MIN_DEFAULT

    @classmethod
    def from_args(cls, args: argparse.Namespace) -> Thresholds:
        return cls(args.vfs_tolerance, args.syscall_ratio, args.wall_improvement)


UNIT_FACTOR = {"ns": 1, "us": 1_000, "ms": 1_000_000, "s": 1_000_000_000}

# Parse the auto-derived test-derivation name back into axes. Shape
# (from `bench-options.nix`'s `name` default):
#   result-<bench>[-<dispatch>]-<fs>-<throttle>-<layout>-<replica>-n<N>-t<T>[-multi]
# Pin the deterministic tail as literal enums; split the head
# (`<bench>[-<dispatch>]`) with a second regex.
NAME_RE = re.compile(
    r"^result-(?P<head>.+?)"
    r"-(?P<fs>ext4|xfs|btrfs|zfs|tmpfs)"
    r"-(?P<throttle>gp3|io2|nvme|none)"
    r"-(?P<layout>flat|sharded)"
    r"-(?P<replica>single|multi)"
    r"-n(?P<npaths>\d+)"
    r"-t(?P<threads>\d+)"
    r"$"
)
# If you add a new dispatch to `bench-options.nix` (e.g. "aio-poll"),
# add it here too.
DISPATCH_SUFFIX_RE = re.compile(r"-(syscall|iouring)$")

# --- helpers --------------------------------------------------------------


def fmt_ns(ns: float) -> str:
    """Render `ns` with the largest unit that keeps the integer part
    non-trivial (ns → µs → ms → s). Per-call magnitude pick so 1.5 ms
    shows as `1.500ms`, not `0.002s`."""
    if ns == 0:
        return "0"
    a = abs(ns)
    if a >= 1e9:
        return f"{ns / 1e9:.3f}s"
    if a >= 1e6:
        return f"{ns / 1e6:.3f}ms"
    if a >= 1e3:
        return f"{ns / 1e3:.3f}µs"
    return f"{ns:.0f}ns"


def _p99(samples: list[int]) -> int:
    """99th percentile via type-7 linear interpolation. Matches
    `numpy.percentile(..., 99)` and `statistics.quantiles(...,
    method='inclusive')`."""
    if not samples:
        return 0
    if len(samples) == 1:
        return samples[0]
    return int(statistics.quantiles(samples, n=100, method="inclusive")[98])


@dataclass(frozen=True)
class Stats:
    """Summary of a sample list — populated lazily from `BenchData`."""

    n: int
    mean: int
    median: int
    p99: int
    stddev: int
    min: int
    max: int

    @classmethod
    def of(cls, samples: list[int]) -> Stats:
        if not samples:
            return cls(0, 0, 0, 0, 0, 0, 0)
        return cls(
            n=len(samples),
            mean=int(statistics.fmean(samples)),
            median=int(statistics.median(samples)),
            p99=_p99(samples),
            stddev=int(statistics.stdev(samples)) if len(samples) > 1 else 0,
            min=min(samples),
            max=max(samples),
        )


@dataclass
class BenchData:
    """One google-benchmark JSON, parsed once.

    `by_name` groups iteration wall-times (ns) by bench-cell name
    (there's usually one row for non-matrix use, or many under
    `--benchmark_filter=...` patterns). `throws` totals `gc_threw`
    and `opt_threw` counters — emitted by `optimise_with_concurrent_gc`
    to expose exceptions the inner loop caught that would otherwise
    masquerade as speed-ups.

    Instances returned from `load()` are cached by resolved path;
    treat them as read-only — mutating `by_name` or `throws` will
    corrupt every later call that returns the same instance.
    """

    path: Path
    by_name: dict[str, list[int]] = field(default_factory=dict)
    throws: dict[str, int] = field(default_factory=lambda: {"gc_threw": 0, "opt_threw": 0})

    @classmethod
    def load(cls, path: Path) -> BenchData:
        return _load_bench_cached(path.resolve())

    @property
    def all_samples(self) -> list[int]:
        return [ns for samples in self.by_name.values() for ns in samples]

    @property
    def has_throws(self) -> bool:
        return any(v > 0 for v in self.throws.values())

    def throws_message(self, label: str) -> str:
        return (
            f"FAIL: {label} reported uncaught throws: {self.throws} "
            f"(measurement is suspect; the bench's inner loop caught an exception while running)"
        )


@functools.lru_cache(maxsize=None)
def _load_bench_cached(resolved_path: Path) -> BenchData:
    """Actual JSON parse, keyed on the resolved path so different
    spellings of the same file hit one cache entry. Treat the result
    as immutable — see `BenchData` docstring."""
    data = BenchData(path=resolved_path)
    for row in json.loads(resolved_path.read_text()).get("benchmarks", []):
        if row.get("run_type") != "iteration":
            continue
        if row.get("error_occurred"):
            print(
                f"warning: dropping errored iteration in {resolved_path}: "
                f"{row.get('name', '?')}: {row.get('error_message', '?')}",
                file=sys.stderr,
            )
            continue
        factor = UNIT_FACTOR.get(row.get("time_unit", "ns"))
        if factor is None:
            raise ValueError(
                f"unknown time_unit {row.get('time_unit')!r} in {resolved_path} (known: {sorted(UNIT_FACTOR)})"
            )
        data.by_name.setdefault(row.get("name", "?"), []).append(int(row["real_time"] * factor))
        for k in data.throws:
            if k in row:
                data.throws[k] += int(row[k])
    return data


def parse_bpf(path: Path | None) -> tuple[int, int] | None:
    """(vfs_count, syscall_count) from a bpftrace map dump. None when
    no path was passed at all (non-GC cells never run bpftrace).
    (0, 0) when the path was passed but the file is missing or empty
    — the caller raises a "bpftrace failed to attach" diagnostic."""
    if path is None:
        return None
    if not path.exists():
        return 0, 0
    text = path.read_text()
    vfs = sum(int(m) for m in re.findall(r"@vfs\[[^\]]+\]:\s*(\d+)", text))
    sc = sum(int(m) for m in re.findall(r"@sc\[\d+\]:\s*(\d+)", text))
    return vfs, sc


# --- cell discovery --------------------------------------------------------


@dataclass(frozen=True, order=True)
class Cell:
    bench: str
    dispatch: str  # "syscall" / "iouring" / "none" (non-GC)
    fs: str
    throttle: str
    layout: str
    replica: str
    npaths: int
    threads: int

    @property
    def name(self) -> str:
        """The `result-*` symlink name produced by `adhoc.nix` for
        this cell. Must match `bench-options.nix`'s auto-derived
        `name` shape exactly — see NAME_RE on the parse side."""
        dispatch_tag = f"-{self.dispatch}" if self.dispatch != "none" else ""
        return (
            f"result-{self.bench.replace('_', '-')}{dispatch_tag}"
            f"-{self.fs}-{self.throttle}-{self.layout}-{self.replica}"
            f"-n{self.npaths}-t{self.threads}"
        )


def parse_name(name: str) -> Cell | None:
    m = NAME_RE.match(name)
    if not m:
        return None
    head = m["head"]
    dispatch = "none"
    if dm := DISPATCH_SUFFIX_RE.search(head):
        dispatch = dm.group(1)
        head = head[: dm.start()]
    # Schema hyphenates underscores for derivation names; undo so
    # the parsed bench matches the original `benchName` enum.
    bench = head.replace("-", "_")
    # Defend against DISPATCH_SUFFIX_RE mis-stripping a future bench
    # whose canonical name ends in `_syscall`/`_iouring`, and against
    # benches added to `bench-options.nix` without `ALL_BENCHES`.
    # Both cases produce a `bench` value that won't appear in the
    # registry, so refuse to parse and let `_discover_cached` emit
    # the standard "unparsable result name" warning.
    if bench not in ALL_BENCHES:
        return None
    return Cell(
        bench=bench,
        dispatch=dispatch,
        fs=m["fs"],
        throttle=m["throttle"],
        layout=m["layout"],
        replica=m["replica"],
        npaths=int(m["npaths"]),
        threads=int(m["threads"]),
    )


@dataclass(frozen=True)
class Result:
    cell: Cell
    json_path: Path
    entry: Path  # the `result-*` symlink or materialized directory

    @property
    def bpf_path(self) -> Path | None:
        """GC-only: the `<dispatch>.bpf.txt` next to the JSON. None
        for non-GC cells or when bpftrace never produced output."""
        if self.cell.dispatch == "none":
            return None
        p = self.entry.resolve() / f"{self.cell.dispatch}.bpf.txt"
        return p if p.is_file() else None


def _cell_json(entry: Path) -> Path | None:
    """The `<dispatch>.json` or `single.json` inside the entry's
    target. Accepts both the `nix build -o` symlink shape and a
    materialized directory (e.g. after `cp -L` of the original tree)."""
    if not entry.is_dir():  # follows symlinks
        return None
    target = entry.resolve()
    for stem in ("syscall", "iouring", "single"):
        p = target / f"{stem}.json"
        if p.is_file():
            return p
    return None


@functools.lru_cache(maxsize=None)
def _discover_cached(resolved_dir: Path) -> tuple[Result, ...]:
    """Internal cache keyed on the *resolved* path so different
    spellings (`./results`, `results`, `/abs/results`) collapse onto
    the same cache entry and the warning side-effects fire once.

    Sorted by `Cell` (axis-tuple order), not by filename. Lexicographic
    sort on `result-*` interleaves benches that share a prefix
    (`optimise` / `optimise_migrate` / `optimise_with_concurrent_gc`)
    and orders npaths as `n10000` < `n2000` < `n50000`. `Cell` is a
    frozen ordered dataclass, so sorting by it groups benches first
    and treats `npaths` as the integer it is."""
    parsed: list[Result] = []
    for entry in sorted(resolved_dir.iterdir()):
        if not entry.name.startswith("result-"):
            continue
        cell = parse_name(entry.name)
        if cell is None:
            print(f"warning: unparsable result name {entry.name!r}", file=sys.stderr)
            continue
        jp = _cell_json(entry)
        if jp is None:
            print(f"warning: no JSON inside {entry.name}", file=sys.stderr)
            continue
        parsed.append(Result(cell=cell, json_path=jp, entry=entry))
    parsed.sort(key=lambda r: r.cell)
    return tuple(parsed)


def discover(results_dir: Path) -> tuple[Result, ...]:
    """Every parsable `result-*` symlink in `results_dir`, paired
    with the JSON inside its target. Warns and skips entries that
    fail either step. Walks each real directory once even if called
    multiple times under different path spellings."""
    return _discover_cached(results_dir.resolve())


def _filter_cells(args: argparse.Namespace, results: tuple[Result, ...] | list[Result]) -> list[Result]:
    """Apply `--<axis>` filters from `args` to a result sequence. An
    axis with no filter = match all; non-empty = membership."""
    filters = [(axis, getattr(args, axis)) for axis in AXES if getattr(args, axis)]
    return [r for r in results if all(str(getattr(r.cell, axis)) in vs for axis, vs in filters)]


@dataclass(frozen=True)
class PairedDirs:
    pairs: list[tuple[Result, Result]]
    only_a: list[Result]
    only_b: list[Result]


def _pair_dirs(dir_a: Path, dir_b: Path, args: argparse.Namespace) -> PairedDirs:
    """Pair Results from two dirs by full Cell identity (post-filter
    intersection). Also surfaces the unpaired Results on each side
    so callers can summarise *what* is missing, not just how many."""
    by_a = {r.cell: r for r in _filter_cells(args, discover(dir_a))}
    by_b = {r.cell: r for r in _filter_cells(args, discover(dir_b))}
    pairs = [(by_a[k], by_b[k]) for k in sorted(by_a.keys() & by_b.keys())]
    only_a = [by_a[k] for k in sorted(by_a.keys() - by_b.keys())]
    only_b = [by_b[k] for k in sorted(by_b.keys() - by_a.keys())]
    return PairedDirs(pairs=pairs, only_a=only_a, only_b=only_b)


def _summarise_unpaired(results: list[Result], side_label: str, show_names: bool) -> list[str]:
    """Per-axis value-distribution of unpaired cells. Lets you tell
    "the 1069 unpaired cells are exactly the ones with dispatch=iouring"
    at a glance, vs "they span every axis". With `show_names` also
    includes the directory names. Returns the `#`-prefixed lines so
    callers can route them — stderr for text-mode tables, fenced code
    block on stdout for markdown."""
    if not results:
        return []
    lines = [f"# {len(results)} cell(s) only in {side_label}:"]
    for axis in AXES:
        counts: dict[str, int] = {}
        for r in results:
            v = str(getattr(r.cell, axis))
            counts[v] = counts.get(v, 0) + 1
        if len(counts) == 1:
            (only,) = counts
            lines.append(f"#   {axis} = {only}")
        else:
            joined = ", ".join(f"{v}({n})" for v, n in sorted(counts.items()))
            lines.append(f"#   {axis} {IN_OP} {{{joined}}}")
    if show_names:
        lines.extend(f"#     {r.entry.name}" for r in results)
    return lines


def _require_dirs(*dirs: Path) -> int:
    """Returns rc=2 with a clean message if any path isn't a directory,
    so callers don't propagate a raw `FileNotFoundError` from `iterdir`."""
    for d in dirs:
        if not d.is_dir():
            print(f"error: not a directory: {d}", file=sys.stderr)
            return 2
    return 0


def _validate_pair_flags(args: argparse.Namespace) -> int:
    """Catch flag combinations that used to silently no-op. Returns
    0 when OK, otherwise an exit code for the caller to propagate."""
    if args.results_dir_b is None:
        bad = []
        if args.a_name or args.b_name:
            bad.append("--a-name / --b-name")
        if getattr(args, "show_missing", False):
            bad.append("--show-missing")
        if bad:
            verb = "applies" if len(bad) == 1 else "apply"
            print(
                f"error: {' and '.join(bad)} only {verb} with --results-dir-b",
                file=sys.stderr,
            )
            return 2
        return 0
    # Same-dir A=B produces an A/B against itself: every Δ is 0%,
    # every verdict is PASS, with no signal that something's wrong.
    a_resolved = args.results_dir.resolve()
    b_resolved = args.results_dir_b.resolve()
    if a_resolved == b_resolved:
        print(
            f"error: --results-dir-a and --results-dir-b both resolve to {a_resolved}; "
            f"a directory cannot be A/B'd against itself",
            file=sys.stderr,
        )
        return 2
    return 0


def _throws_marker(throws: dict[str, int]) -> str:
    """`'!'` when the bench reported caught throws (silently bad
    data); empty string otherwise. Surfaces what `cmd_summary` /
    `cmd_ab` already gate on, in the matrix-table context where rows
    were being shown without complaint."""
    return "!" if any(v > 0 for v in throws.values()) else ""


# --- `summary` (one JSON, possibly multiple named rows) -------------------


def cmd_summary(args: argparse.Namespace) -> int:
    data = BenchData.load(args.json)
    # `test -s` only checks file non-empty; a `{"benchmarks": []}` payload
    # passes that but means the filter matched nothing.
    if not data.by_name:
        print(
            f"FAIL: no iteration rows in {args.json} (bench filter matched nothing, or every iteration errored)",
            file=sys.stderr,
        )
        return 1
    if data.has_throws:
        print(data.throws_message(str(args.json)), file=sys.stderr)
        return 1
    print(f"=== summary of {args.json} ===")
    for name, samples in data.by_name.items():
        s = Stats.of(samples)
        print(
            f"{name}  n={s.n:>2d}  "
            f"mean={fmt_ns(s.mean):>9s}  median={fmt_ns(s.median):>9s}  "
            f"p99={fmt_ns(s.p99):>9s}  stddev={fmt_ns(s.stddev):>9s}  "
            f"min={fmt_ns(s.min):>9s}  max={fmt_ns(s.max):>9s}"
        )
    return 0


# --- `ab` (single pair, with or without bpftrace dumps) --------------------


# Failure categories. Stable strings so aggregate output can be
# scripted against. Add new categories here when you add new gates.
FAIL_WALL = "wall"
FAIL_WALL_ZERO = "wall_zero"
FAIL_SYSCALL = "syscall"
FAIL_VFS = "vfs"
FAIL_BPF_ZERO = "bpf_zero"
FAIL_BPF_ASYMMETRIC = "bpf_asymmetric"
FAIL_THROWS = "throws"


@dataclass(frozen=True)
class Failure:
    """A single gate failure or throws report on an A/B pair. The
    category drives aggregate counts; the message is the human-
    readable explanation already shaped for `print_ab`."""

    category: str
    message: str


@dataclass(frozen=True)
class AbResult:
    """One A/B comparison, structured so callers can aggregate. `rc`
    follows the same convention as the old `do_ab` return: 0 PASS,
    1 FAIL (gate or throws), 2 missing data."""

    a_name: str
    b_name: str
    a_stats: Stats
    b_stats: Stats
    a_bpf: tuple[int, int] | None
    b_bpf: tuple[int, int] | None
    a_throws: dict[str, int]
    b_throws: dict[str, int]
    failures: tuple[Failure, ...]
    missing: bool

    @property
    def rc(self) -> int:
        if self.missing:
            return 2
        if self.failures:
            return 1
        return 0

    @property
    def has_throws(self) -> bool:
        return any(f.category == FAIL_THROWS for f in self.failures)

    @property
    def wall_delta(self) -> float | None:
        """(b - a) / a as a fraction (multiply by 100 for percent).
        None when the A side wall is zero."""
        if self.a_stats.mean == 0:
            return None
        return (self.b_stats.mean - self.a_stats.mean) / self.a_stats.mean

    @property
    def syscall_delta(self) -> float | None:
        """(b - a) / a syscall count as a fraction. None when either
        side's bpf is missing OR reads zero (bpftrace failed to
        attach). The zero-check on the B side matters: without it,
        a B failure would emit a misleading -100% "improvement" into
        ranked output."""
        if self.a_bpf is None or self.b_bpf is None:
            return None
        if self.a_bpf[1] == 0 or self.b_bpf[1] == 0:
            return None
        return (self.b_bpf[1] - self.a_bpf[1]) / self.a_bpf[1]

    @property
    def noisy(self) -> bool:
        """Either side's CV exceeds the noise threshold — Δmean is
        dominated by sample variance and the verdict is less trustworthy."""
        for s in (self.a_stats, self.b_stats):
            if s.mean and s.stddev / s.mean > NOISY_CV:
                return True
        return False


def compute_ab(
    a_json: Path,
    b_json: Path,
    a_name: str,
    b_name: str,
    a_bpf_path: Path | None,
    b_bpf_path: Path | None,
    thr: Thresholds,
) -> AbResult:
    """Pure computation: parse both sides, run the gates, package the
    structured result. Doesn't print — see `print_ab`. Throws short-
    circuit the metric gates: when either side reported caught throws,
    the data is suspect and we don't bother running wall/syscall/vfs
    comparisons that would be reading lies."""
    a = BenchData.load(a_json)
    b = BenchData.load(b_json)
    missing = not a.all_samples or not b.all_samples

    failures: list[Failure] = []
    for label, data in ((a_name, a), (b_name, b)):
        if data.has_throws:
            failures.append(Failure(FAIL_THROWS, data.throws_message(label)))

    a_bpf = parse_bpf(a_bpf_path) if not missing else None
    b_bpf = parse_bpf(b_bpf_path) if not missing else None
    a_s = Stats.of(a.all_samples)
    b_s = Stats.of(b.all_samples)

    has_throws = any(f.category == FAIL_THROWS for f in failures)
    if not missing and not has_throws:
        # bpf gates require both sides; if exactly one is missing,
        # the gate would silently no-op — surface that as a failure
        # so a partial test run can't pass by accident.
        if (a_bpf is None) != (b_bpf is None):
            has, missing_side = (a_name, b_name) if a_bpf is not None else (b_name, a_name)
            failures.append(
                Failure(
                    FAIL_BPF_ASYMMETRIC,
                    f"bpf data is one-sided ({has} has it, {missing_side} doesn't) {EM_DASH} VFS/syscall gates skipped",
                )
            )
        elif a_bpf is not None and b_bpf is not None:
            a_vfs, a_sys = a_bpf
            b_vfs, b_sys = b_bpf
            # Both sides must have non-zero counts; a zero on either
            # side is bpftrace failing to attach, not a real metric.
            # Checking B-side explicitly closes a silent-pass hole:
            # `ratio = 0 / a_sys = 0` would otherwise be ≤ threshold
            # and the cell would pass with no bpf data.
            if a_vfs == 0:
                failures.append(Failure(FAIL_BPF_ZERO, f"{a_name} VFS count is zero {EM_DASH} bpftrace probably failed to attach"))
            elif b_vfs == 0:
                failures.append(Failure(FAIL_BPF_ZERO, f"{b_name} VFS count is zero {EM_DASH} bpftrace probably failed to attach"))
            elif (delta := abs(a_vfs - b_vfs) / a_vfs) > thr.vfs_tolerance:
                failures.append(
                    Failure(
                        FAIL_VFS,
                        f"VFS op count diverged: {a_vfs} -> {b_vfs} (delta {delta:.1%}, threshold {LE} {thr.vfs_tolerance:.1%})",
                    )
                )
            if a_sys == 0:
                failures.append(Failure(FAIL_BPF_ZERO, f"{a_name} syscall count is zero {EM_DASH} bpftrace probably failed"))
            elif b_sys == 0:
                failures.append(Failure(FAIL_BPF_ZERO, f"{b_name} syscall count is zero {EM_DASH} bpftrace probably failed"))
            elif (ratio := b_sys / a_sys) > thr.syscall_ratio:
                failures.append(
                    Failure(
                        FAIL_SYSCALL,
                        f"syscall regression: {a_sys} -> {b_sys} (ratio {ratio:.1%}, threshold {LE} {thr.syscall_ratio:.1%})",
                    )
                )
        if a_s.mean == 0:
            failures.append(Failure(FAIL_WALL_ZERO, f"{a_name} wall is zero {EM_DASH} bench probably failed"))
        elif (improvement := (a_s.mean - b_s.mean) / a_s.mean) < thr.wall_improvement:
            failures.append(
                Failure(FAIL_WALL, f"wall regression: {improvement:+.1%} (threshold {GE} {thr.wall_improvement:+.1%})")
            )

    return AbResult(
        a_name=a_name,
        b_name=b_name,
        a_stats=a_s,
        b_stats=b_s,
        a_bpf=a_bpf,
        b_bpf=b_bpf,
        a_throws=dict(a.throws),
        b_throws=dict(b.throws),
        failures=tuple(failures),
        missing=missing,
    )


def print_ab(res: AbResult) -> None:
    """Render one AbResult to stdout/stderr in the historical shape.
    Throws messages route to stderr and short-circuit the metrics
    block — matches the prior behaviour where a throws-tainted pair
    never showed misleading numbers."""
    if res.missing:
        print(
            f"missing iteration rows ({res.a_name}={res.a_stats.n} samples, "
            f"{res.b_name}={res.b_stats.n} samples)",
            file=sys.stderr,
        )
        return
    throws_fs = [f for f in res.failures if f.category == FAIL_THROWS]
    for f in throws_fs:
        print(f.message, file=sys.stderr)
    if throws_fs:
        return

    print(f"=== A/B: {res.a_name} vs {res.b_name} ===")
    for label, s, bpf in ((res.a_name, res.a_stats, res.a_bpf), (res.b_name, res.b_stats, res.b_bpf)):
        extra = "(no bpftrace)" if bpf is None else f"vfs={bpf[0]:>7d}  syscalls={bpf[1]:>8d}"
        print(f"{label}: wall={fmt_ns(s.mean):>9s}  p99={fmt_ns(s.p99):>9s}  stddev={fmt_ns(s.stddev):>9s}  {extra}")
    if res.failures:
        print("\nVERDICT: FAIL")
        for f in res.failures:
            print(f"  - {f.message}")
    else:
        print("\nVERDICT: PASS")


def cmd_ab(args: argparse.Namespace) -> int:
    res = compute_ab(
        args.a_json,
        args.b_json,
        args.a_name,
        args.b_name,
        args.a_bpf,
        args.b_bpf,
        Thresholds.from_args(args),
    )
    print_ab(res)
    return res.rc


# --- `summary-matrix` (one row per cell) ----------------------------------


def _fmt_cv(stats: Stats) -> str:
    """Coefficient of variation as a percent. Flagged with `*` when
    above NOISY_CV — a high-CV row's mean is dominated by noise, and
    any A/B verdict over it should be taken with a grain of salt."""
    if stats.mean == 0:
        return EM_DASH
    cv = stats.stddev / stats.mean
    suffix = "*" if cv > NOISY_CV else ""
    return f"{cv * 100:.0f}%{suffix}"


def _fmt_delta_pct(a_mean: int, b_mean: int) -> str:
    """B vs A as a signed percent, with em-dash when the baseline is zero."""
    if a_mean == 0:
        return EM_DASH
    pct = (b_mean - a_mean) / a_mean * 100.0
    return f"{pct:+.1f}%"


# Single-dir summary-matrix columns. Adds `iters`, `mean`, `p99`,
# `stddev`, `cv`. `cv` is the headline noise indicator; high CV
# means the mean isn't trustworthy.
_TABLE_COLS = (
    ("bench", lambda r, s, d: r.cell.bench),
    ("dispatch", lambda r, s, d: r.cell.dispatch),
    ("fs", lambda r, s, d: r.cell.fs),
    ("throttle", lambda r, s, d: r.cell.throttle),
    ("layout", lambda r, s, d: r.cell.layout),
    ("replica", lambda r, s, d: r.cell.replica),
    ("n", lambda r, s, d: str(r.cell.npaths)),
    ("t", lambda r, s, d: str(r.cell.threads)),
    ("iters", lambda r, s, d: str(s.n) + _throws_marker(d.throws)),
    ("mean", lambda r, s, d: fmt_ns(s.mean)),
    ("p99", lambda r, s, d: fmt_ns(s.p99)),
    ("stddev", lambda r, s, d: fmt_ns(s.stddev)),
    ("cv", lambda r, s, d: _fmt_cv(s)),
)

_VERDICT_WORD = {"pass": "pass", "fail": "fail", "missing": "miss"}


# Two-dir summary-matrix columns. One row per *paired* cell. Δmean
# is the headline number; `verdict` runs the same gates as ab-matrix
# so the table can stand alone for review (no need to also run
# ab-matrix to know what passed); `cv_a`/`cv_b` flag noise.
_TABLE_COLS_AB = (
    ("bench", lambda c, r: c.bench),
    ("dispatch", lambda c, r: c.dispatch),
    ("fs", lambda c, r: c.fs),
    ("throttle", lambda c, r: c.throttle),
    ("layout", lambda c, r: c.layout),
    ("replica", lambda c, r: c.replica),
    ("n", lambda c, r: str(c.npaths)),
    ("t", lambda c, r: str(c.threads)),
    ("a_iters", lambda c, r: str(r.a_stats.n) + _throws_marker(r.a_throws)),
    ("b_iters", lambda c, r: str(r.b_stats.n) + _throws_marker(r.b_throws)),
    ("a_mean", lambda c, r: fmt_ns(r.a_stats.mean)),
    ("b_mean", lambda c, r: fmt_ns(r.b_stats.mean)),
    (f"{DELTA}mean", lambda c, r: _fmt_delta_pct(r.a_stats.mean, r.b_stats.mean)),
    ("a_p99", lambda c, r: fmt_ns(r.a_stats.p99)),
    ("b_p99", lambda c, r: fmt_ns(r.b_stats.p99)),
    ("a_cv", lambda c, r: _fmt_cv(r.a_stats)),
    ("b_cv", lambda c, r: _fmt_cv(r.b_stats)),
    ("verdict", lambda c, r: _VERDICT_WORD[_verdict(r)]),
)


def _render_table(headers: tuple[str, ...], rendered: list[tuple[str, ...]], fmt_kind: str = "text") -> None:
    """Print a table to stdout in `text` (left-aligned columns) or `md`
    (GitHub-flavored Markdown). Both shapes accept the same headers
    and pre-stringified rows."""
    if not rendered:
        return
    if fmt_kind == "md":
        print("| " + " | ".join(headers) + " |")
        print("| " + " | ".join("---" for _ in headers) + " |")
        for cells in rendered:
            print("| " + " | ".join(cells) + " |")
        return
    widths = [max(len(h), *(len(c[i]) for c in rendered)) for i, h in enumerate(headers)]
    fmt = "  ".join(f"{{:<{w}}}" for w in widths)
    print(fmt.format(*headers))
    print(fmt.format(*("-" * w for w in widths)))
    for cells in rendered:
        print(fmt.format(*cells))


def _emit_comments(lines: list[str], fmt_kind: str) -> None:
    """Route `#`-prefixed annotation lines. Text mode keeps them on
    stderr (historical behavior — keeps the table itself paste-able as
    fixed-width data). Markdown mode wraps them in a fenced code block
    on stdout so they survive a single `>` pipe and don't render as
    headings (`#` would otherwise become an `<h1>`)."""
    if not lines:
        return
    if fmt_kind == "md":
        print("```")
        for line in lines:
            print(line)
        print("```")
        print()
    else:
        for line in lines:
            print(line, file=sys.stderr)


def cmd_summary_matrix(args: argparse.Namespace) -> int:
    if (rc := _validate_pair_flags(args)) != 0:
        return rc

    if args.results_dir_b is not None:
        if (rc := _require_dirs(args.results_dir, args.results_dir_b)) != 0:
            return rc
        return _cmd_summary_matrix_two_dir(args)

    if (rc := _require_dirs(args.results_dir)) != 0:
        return rc
    results = _filter_cells(args, discover(args.results_dir))
    if not results:
        print(f"no cells matched in {args.results_dir}", file=sys.stderr)
        return 1

    rows: list[tuple[Result, Stats, BenchData]] = []
    for r in results:
        d = BenchData.load(r.json_path)
        rows.append((r, Stats.of(d.all_samples), d))

    if args.json:
        for r, s, d in rows:
            row = asdict(r.cell) | {
                "iters": s.n,
                "mean_ns": s.mean,
                "p99_ns": s.p99,
                "stddev_ns": s.stddev,
                "cv": (s.stddev / s.mean) if s.mean else None,
                "throws": d.throws,
                "json_path": str(r.json_path),
            }
            print(json.dumps(row))
        return 0

    headers = tuple(h for h, _ in _TABLE_COLS)
    rendered = [tuple(fn(r, s, d) for _, fn in _TABLE_COLS) for r, s, d in rows]
    _render_table(headers, rendered, args.format)
    # Footer goes to stdout for the same reason as the two-dir branch
    # (predictable interleaving under `2>&1 |` and `less`). Markdown
    # mode wraps it in a fenced block so the `#` doesn't render as a
    # heading.
    if any(d.has_throws for _, _, d in rows):
        footer = ["# `!` next to iters = cell reported caught throws; row is suspect"]
        if args.format == "md":
            print()
            _emit_comments(footer, args.format)
        else:
            print()
            print(footer[0])
    return 0


def _cmd_summary_matrix_two_dir(args: argparse.Namespace) -> int:
    """Two-dir branch: one row per paired cell, with delta + CV cols."""
    a_label, b_label = _ab_labels(args)
    paired = _pair_dirs(args.results_dir, args.results_dir_b, args)
    if not paired.pairs:
        print(
            f"no cells in common between {args.results_dir} and {args.results_dir_b} "
            f"(only_in_a={len(paired.only_a)}, only_in_b={len(paired.only_b)})",
            file=sys.stderr,
        )
        # Error-path summaries always go to stderr; markdown's fenced-
        # block routing only kicks in when we actually emit a table.
        for line in _summarise_unpaired(paired.only_a, a_label, args.show_missing):
            print(line, file=sys.stderr)
        for line in _summarise_unpaired(paired.only_b, b_label, args.show_missing):
            print(line, file=sys.stderr)
        return 1

    pre_comments: list[str] = []
    if paired.only_a or paired.only_b:
        pre_comments.append(
            f"# {len(paired.pairs)} pair(s); skipping {len(paired.only_a)} cell(s) only in {a_label}, "
            f"{len(paired.only_b)} cell(s) only in {b_label} "
            f"(--show-missing to list)"
        )
        pre_comments.extend(_summarise_unpaired(paired.only_a, a_label, args.show_missing))
        pre_comments.extend(_summarise_unpaired(paired.only_b, b_label, args.show_missing))
    _emit_comments(pre_comments, args.format)

    # Use compute_ab so the table shares one source of truth with
    # ab-matrix for "what counts as a regression". The verdict column
    # then reflects the *same* gate decision a separate ab-matrix
    # would produce.
    thr = Thresholds.from_args(args)
    rows: list[tuple[Cell, AbResult]] = []
    for a_r, b_r in paired.pairs:
        res = compute_ab(a_r.json_path, b_r.json_path, a_label, b_label, a_r.bpf_path, b_r.bpf_path, thr)
        rows.append((a_r.cell, res))

    if args.json:
        for cell, r in rows:
            row = asdict(cell) | {
                "a_iters": r.a_stats.n,
                "b_iters": r.b_stats.n,
                "a_mean_ns": r.a_stats.mean,
                "b_mean_ns": r.b_stats.mean,
                "wall_delta": r.wall_delta,
                "syscall_delta": r.syscall_delta,
                "a_p99_ns": r.a_stats.p99,
                "b_p99_ns": r.b_stats.p99,
                "a_cv": (r.a_stats.stddev / r.a_stats.mean) if r.a_stats.mean else None,
                "b_cv": (r.b_stats.stddev / r.b_stats.mean) if r.b_stats.mean else None,
                "noisy": r.noisy,
                "a_throws": r.a_throws,
                "b_throws": r.b_throws,
                "verdict": _verdict(r),
                "fail_categories": sorted({f.category for f in r.failures}),
                "a_name": a_label,
                "b_name": b_label,
            }
            print(json.dumps(row))
        return 0

    def _label_header(h: str) -> str:
        # Carry the side labels into the table headers. Prefix-only so
        # an axis name containing `a_` (none today, but future-proof) or
        # a user-supplied label that happens to start with `a_`/`b_`
        # doesn't recurse through str.replace.
        if h.startswith("a_"):
            return f"{a_label}_{h[2:]}"
        if h.startswith("b_"):
            return f"{b_label}_{h[2:]}"
        return h

    headers = tuple(_label_header(h) for h, _ in _TABLE_COLS_AB)
    rendered = [tuple(fn(c, r) for _, fn in _TABLE_COLS_AB) for c, r in rows]
    _render_table(headers, rendered, args.format)
    # Footer notes go to stdout (not stderr) so the table and its
    # legend stay in order when piped through `head`/`less`/etc.
    # Compact aggregate footer — full breakdown lives in `ab-matrix`,
    # this is just the one-liner so the table is self-contained.
    totals = {"pass": 0, "fail": 0, "missing": 0}
    for _, r in rows:
        totals[_verdict(r)] += 1
    footer: list[str] = []
    if any(r.has_throws for _, r in rows):
        footer.append("# `!` next to iters = cell reported caught throws; row is suspect")
    if any(_fmt_cv(s).endswith("*") for _, r in rows for s in (r.a_stats, r.b_stats)):
        footer.append(f"# `*` after a CV value = noise > {NOISY_CV:.0%}; {DELTA}mean is mostly statistical")
    footer.append(
        f"# {len(rows)} pair(s)  pass: {totals['pass']}  fail: {totals['fail']}  missing: {totals['missing']}"
    )
    if args.format == "md":
        print()
        _emit_comments(footer, args.format)
    else:
        print()
        for line in footer:
            print(line)
    return 0


def _ab_labels(args: argparse.Namespace) -> tuple[str, str]:
    """Resolve --a-name / --b-name, defaulting to each dir's basename.
    Warns to stderr when labels collide and need to be auto-suffixed."""
    a = args.a_name or args.results_dir.name
    b = args.b_name or (args.results_dir_b.name if args.results_dir_b is not None else "b")
    if a == b:
        new_a, new_b = f"{a}-a", f"{b}-b"
        print(
            f"warning: --a-name and --b-name both resolve to {a!r}; "
            f"using {new_a!r} / {new_b!r} so the output is unambiguous",
            file=sys.stderr,
        )
        return new_a, new_b
    return a, b


# --- `ab-matrix` (partition by fixed axes, A/B each pair) -----------------


def _verdict(res: AbResult) -> str:
    if res.missing:
        return "missing"
    if res.failures:
        return "fail"
    return "pass"


def _cell_desc(cell: Cell) -> str:
    """Compact slash-joined cell description for ranked lists."""
    return (
        f"{cell.bench}/{cell.dispatch}/{cell.fs}/{cell.throttle}"
        f"/{cell.layout}/{cell.replica}/n{cell.npaths}/t{cell.threads}"
    )


def _top_n_flags(r: AbResult) -> str:
    """Trailing markers for top-N entries: `[noisy]` if either side
    is over the CV threshold, `[FAIL]` if the gate tripped. Returns
    empty string when neither applies (the common case)."""
    flags = []
    if r.noisy:
        flags.append("noisy")
    if _verdict(r) == "fail":
        flags.append("FAIL")
    return f"  [{','.join(flags)}]" if flags else ""


def _print_per_axis(
    items: list[tuple[Cell, AbResult]],
    title: str,
    delta_getter,
    show_passes: bool,
) -> None:
    """Render one per-axis breakdown table. `show_passes=True` adds
    the pass-rate ratio next to the avg Δ; `False` shows the avg
    alone (used by the syscall block, where pass-rate is identical
    to the wall block and would duplicate)."""
    rendered: list[tuple[str, list[str]]] = []
    for axis in AXES:
        vals = sorted({str(getattr(c, axis)) for c, _ in items})
        if len(vals) < 2:
            continue
        parts: list[str] = []
        for v in vals:
            sub = [(c, r) for c, r in items if str(getattr(c, axis)) == v]
            deltas = [d for _, r in sub if (d := delta_getter(r)) is not None]
            avg = statistics.fmean(deltas) if deltas else None
            avg_s = f"{avg * 100:+.1f}%" if avg is not None else EM_DASH
            if show_passes:
                passed = sum(1 for _, r in sub if _verdict(r) == "pass")
                parts.append(f"{v}({passed}/{len(sub)} {avg_s})")
            else:
                parts.append(f"{v}({avg_s})")
        rendered.append((axis, parts))
    print(f"\n{title}:")
    if not rendered:
        print("  (no axis varies in this batch)")
        return
    for axis, parts in rendered:
        print(f"  {axis}: " + "  ".join(parts))


def _print_top_n(
    ranked: list[tuple[Cell, AbResult, float]],
    metric: str,
    a_label: str,
    b_label: str,
    top_n: int,
) -> None:
    """Print top-N regressions and improvements for one delta metric.
    `ranked` must be pre-sorted ascending by the delta value (negative
    = improvement, positive = regression)."""
    wins = [(c, r, d) for c, r, d in ranked if d < 0.0][:top_n]
    regs = [(c, r, d) for c, r, d in reversed(ranked) if d > 0.0][:top_n]
    if regs:
        print(f"\ntop {len(regs)} {metric} regression(s) ({b_label} worse than {a_label}):")
        for c, r, d in regs:
            print(f"  {d * 100:+6.1f}%  {_cell_desc(c)}{_top_n_flags(r)}")
    if wins:
        print(f"\ntop {len(wins)} {metric} improvement(s) ({b_label} better than {a_label}):")
        for c, r, d in wins:
            print(f"  {d * 100:+6.1f}%  {_cell_desc(c)}{_top_n_flags(r)}")


def _aggregate_ab(
    items: list[tuple[Cell, AbResult]], a_label: str, b_label: str, top_n: int, thr: Thresholds
) -> None:
    """Post-loop summary: totals, threshold echo, failure breakdown
    by category, top-N wall + syscall deltas (with noisy/FAIL flags),
    per-axis pass-rate + avg Δmean, noisy-pair count with overlap on
    the fail set."""
    totals = {"pass": 0, "fail": 0, "missing": 0}
    for _, r in items:
        totals[_verdict(r)] += 1

    print(f"\n=== aggregate: {a_label} vs {b_label} ===")
    print(
        f"{len(items)} pair(s)  pass: {totals['pass']}  "
        f"fail: {totals['fail']}  missing: {totals['missing']}"
    )

    # Threshold echo so logs are self-describing — readers can tell
    # what gate produced the verdict without re-checking the command.
    print(
        f"thresholds: wall {GE} {thr.wall_improvement:+.1%}  "
        f"syscall {LE} {thr.syscall_ratio:.1%}  vfs {LE} {thr.vfs_tolerance:.1%}"
    )

    # Failure breakdown: count *pairs* that hit each category, not
    # raw Failure objects (one pair can throw on both sides and would
    # otherwise double-count). Sum across rows can exceed the number
    # of failed pairs because a pair can hit multiple categories.
    if totals["fail"]:
        cat_pair_counts: dict[str, int] = {}
        for _, r in items:
            for cat in {f.category for f in r.failures}:
                cat_pair_counts[cat] = cat_pair_counts.get(cat, 0) + 1
        ordered = [FAIL_WALL, FAIL_SYSCALL, FAIL_VFS, FAIL_WALL_ZERO, FAIL_BPF_ZERO, FAIL_BPF_ASYMMETRIC, FAIL_THROWS]
        joined = ", ".join(f"{c}={cat_pair_counts[c]}" for c in ordered if cat_pair_counts.get(c))
        if joined:
            print(f"failure reasons across {totals['fail']} failed pair(s): {joined}")
        noisy_fails = sum(1 for _, r in items if _verdict(r) == "fail" and r.noisy)
        if noisy_fails:
            print(f"# {noisy_fails} of {totals['fail']} fail(s) are noisy (cv > {NOISY_CV:.0%}); treat as unconfirmed")

    # Wall ranking. Bind the non-None delta into the tuple so the
    # sort key can't see None and we don't need a type: ignore.
    wall_ranked: list[tuple[Cell, AbResult, float]] = [
        (c, r, r.wall_delta) for c, r in items if r.wall_delta is not None and not r.missing
    ]
    wall_ranked.sort(key=lambda x: x[2])
    _print_top_n(wall_ranked, "wall", a_label, b_label, top_n)

    # Syscall ranking — only meaningful when bpf data exists on both
    # sides for at least one pair. The list is empty for pure non-GC
    # batches and we skip the section entirely.
    syscall_ranked: list[tuple[Cell, AbResult, float]] = [
        (c, r, r.syscall_delta) for c, r in items if r.syscall_delta is not None
    ]
    syscall_ranked.sort(key=lambda x: x[2])
    _print_top_n(syscall_ranked, "syscall", a_label, b_label, top_n)

    # Per-axis breakdown: avg Δ along each varying axis. Wall block
    # also carries pass-rate (which depends on threshold tuning); the
    # avg Δ doesn't, which makes it the more honest "is this axis
    # better or worse on B" signal. Syscall block (when bpf exists)
    # drops pass-rate — it would duplicate the wall block's count.
    _print_per_axis(items, f"per-axis pass-rate and avg wall {DELTA}mean", lambda r: r.wall_delta, show_passes=True)
    if any(r.syscall_delta is not None for _, r in items):
        _print_per_axis(items, f"per-axis avg syscall {DELTA}", lambda r: r.syscall_delta, show_passes=False)

    noisy_total = sum(1 for _, r in items if not r.missing and r.noisy)
    if noisy_total:
        print(f"\nnoisy pairs (either side cv > {NOISY_CV:.0%}): {noisy_total}/{len(items)}")
        print("# headline regressions inside noisy pairs are dominated by sample variance, not real change")


def _should_print_pair(res: AbResult, only: str | None) -> bool:
    """`--only` filter for per-pair printing. `nonpass` covers both
    `fail` and `missing` so CI can ask "show me everything that
    isn't passing" without losing missing-data cells."""
    if only is None:
        return True
    v = _verdict(res)
    if only == "nonpass":
        return v != "pass"
    return v == only


def cmd_ab_matrix(args: argparse.Namespace) -> int:
    if (rc := _validate_pair_flags(args)) != 0:
        return rc
    thr = Thresholds.from_args(args)

    # Two-dir mode: pair by full Cell identity across --results-dir
    # and --results-dir-b. `--axis` is meaningless here so we hard-
    # reject the combo to surface user error.
    if args.results_dir_b is not None:
        if args.axis is not None:
            print("error: --axis is incompatible with --results-dir-b (vary one axis OR vary the dir)", file=sys.stderr)
            return 2
        if (rc := _require_dirs(args.results_dir, args.results_dir_b)) != 0:
            return rc
        return _cmd_ab_matrix_two_dir(args, thr)

    if args.axis is None:
        print("error: --axis is required unless --results-dir-b is given", file=sys.stderr)
        return 2
    if (rc := _require_dirs(args.results_dir)) != 0:
        return rc
    axis = args.axis
    fixed_axes = [a for a in AXES if a != axis]
    results = _filter_cells(args, discover(args.results_dir))

    # Group by every axis except `axis`; each group becomes one A/B.
    groups: dict[tuple, list[Result]] = {}
    for r in results:
        key = tuple(str(getattr(r.cell, a)) for a in fixed_axes)
        groups.setdefault(key, []).append(r)

    pairs = [g for g in groups.values() if len(g) == 2]
    singletons = sum(1 for g in groups.values() if len(g) == 1)
    overfull = sum(1 for g in groups.values() if len(g) > 2)
    if not pairs:
        print(
            f"no pairs found varying only on `{axis}` (singletons={singletons}, overfull={overfull})",
            file=sys.stderr,
        )
        return 1
    if singletons or overfull:
        print(
            f"# note: {singletons} cell(s) had no pair, {overfull} group(s) "
            f"had >2 cells (only 2-cell groups are compared)",
            file=sys.stderr,
        )

    pairs.sort(key=lambda g: tuple(getattr(g[0].cell, a) for a in fixed_axes))

    # Collect results before printing so the aggregate block at the
    # end has every pair available; also lets `--only` filter the
    # per-pair output without losing aggregate fidelity.
    collected: list[tuple[Cell, AbResult]] = []
    rc_total = 0
    a_label_for_agg = b_label_for_agg = ""
    for group in pairs:
        group.sort(key=lambda r: getattr(r.cell, axis))
        a, b = group
        a_name = str(getattr(a.cell, axis))
        b_name = str(getattr(b.cell, axis))
        # Aggregate header label: the axis being varied.
        a_label_for_agg, b_label_for_agg = a_name, b_name
        res = compute_ab(a.json_path, b.json_path, a_name, b_name, a.bpf_path, b.bpf_path, thr)
        collected.append((a.cell, res))
        rc_total = max(rc_total, res.rc)

        if _should_print_pair(res, args.only):
            fixed = ", ".join(f"{ax}={getattr(a.cell, ax)}" for ax in fixed_axes)
            print(f"\n=== {axis}: {a_name} vs {b_name}  [{fixed}] ===")
            print_ab(res)

    _aggregate_ab(collected, f"{axis}={a_label_for_agg}", f"{axis}={b_label_for_agg}", args.top_n, thr)
    return rc_total


def _cmd_ab_matrix_two_dir(args: argparse.Namespace, thr: Thresholds) -> int:
    """A/B every paired cell across --results-dir vs --results-dir-b."""
    a_label, b_label = _ab_labels(args)
    paired = _pair_dirs(args.results_dir, args.results_dir_b, args)
    if not paired.pairs:
        print(
            f"no cells in common between {args.results_dir} and {args.results_dir_b} "
            f"(only_in_a={len(paired.only_a)}, only_in_b={len(paired.only_b)})",
            file=sys.stderr,
        )
        _summarise_unpaired(paired.only_a, a_label, args.show_missing)
        _summarise_unpaired(paired.only_b, b_label, args.show_missing)
        return 1
    if paired.only_a or paired.only_b:
        print(
            f"# {len(paired.pairs)} pair(s); skipping {len(paired.only_a)} only in {a_label}, "
            f"{len(paired.only_b)} only in {b_label} (--show-missing to list)",
            file=sys.stderr,
        )
        _summarise_unpaired(paired.only_a, a_label, args.show_missing)
        _summarise_unpaired(paired.only_b, b_label, args.show_missing)

    collected: list[tuple[Cell, AbResult]] = []
    rc_total = 0
    for a, b in paired.pairs:
        res = compute_ab(a.json_path, b.json_path, a_label, b_label, a.bpf_path, b.bpf_path, thr)
        collected.append((a.cell, res))
        rc_total = max(rc_total, res.rc)

        if _should_print_pair(res, args.only):
            cell_desc = ", ".join(f"{ax}={getattr(a.cell, ax)}" for ax in AXES)
            print(f"\n=== {a_label} vs {b_label}  [{cell_desc}] ===")
            print_ab(res)

    _aggregate_ab(collected, a_label, b_label, args.top_n, thr)
    return rc_total


# --- `run` matrix driver ---------------------------------------------------
#
# Matrix dedups two no-ops:
#   * fs=tmpfs has no block device → pin throttle=none.
#   * optimise_migrate / invalidate_paths don't compose
#     layout/replica into the BENCHMARK_CAPTURE tag → pin
#     layout=flat, replica=single.
#
# optimise_with_concurrent_gc's second thread axis (threads2) is
# pinned to match threads — registered BENCHMARK_CAPTURE cells
# are symmetric at (4,4) and (16,16).
#
# For cross-host-comparable wall times, point the Nix daemon's
# build directory at a tmpfs (pass `--build-dir /dev/shm` to the
# `run` subcommand). The qcow2 empty disk
# `virtualisation.emptyDiskImages` creates lives under that
# directory; if it's a real disk the host's disk/page-cache
# characteristics dominate the measurement.


def _matrix_cells(args: argparse.Namespace) -> list[Cell]:
    """Enumerate the filtered cartesian product as `Cell`s."""

    def pick(name: str, full: tuple) -> list[str]:
        return [str(v) for v in (getattr(args, name) or full)]

    cells: list[Cell] = []
    for bench in pick("bench", ALL_BENCHES):
        # non-GC benches pass "" (= no --argstr dispatch).
        dispatches = pick("dispatch", ALL_DISPATCHES) if bench in GC_BENCHES else [""]

        if bench in NON_VARIANT_BENCHES:
            layouts, replicas = ["flat"], ["single"]
        else:
            layouts = pick("layout", ALL_LAYOUTS)
            replicas = pick("replica", ALL_REPLICAS)

        for dispatch in dispatches:
            for layout in layouts:
                for replica in replicas:
                    for fs in pick("fs", ALL_FS):
                        # tmpfs has no block device → throttle is a no-op.
                        throttles = ["none"] if fs == "tmpfs" else pick("throttle", ALL_THROTTLES)
                        for throttle in throttles:
                            for npaths in pick("npaths", ALL_NPATHS):
                                for threads in pick("threads", ALL_THREADS):
                                    cells.append(
                                        Cell(
                                            bench=bench,
                                            dispatch=dispatch or "none",
                                            fs=fs,
                                            throttle=throttle,
                                            layout=layout,
                                            replica=replica,
                                            npaths=int(npaths),
                                            threads=int(threads),
                                        )
                                    )
    return cells


# Maps bench.<option> name → cell attribute name. Each entry becomes
# a `--argstr <option> <value>` or `--arg <option> <value>` pair on
# the nix build command line.
_ARGSTR_AXES = {
    "benchName": "bench",
    "fs": "fs",
    "throttle": "throttle",
    "layout": "layout",
    "replica": "replica",
}
_ARG_AXES = {
    "nPaths": "npaths",
    "threads": "threads",
}


def _cell_cmd(cell: Cell, adhoc_nix: Path, results_dir: Path, build_dir: Path | None) -> list[str]:
    """Argv for `nix build` of one cell."""
    cmd = ["nix", "build", "--builders", "", "-L", "-f", str(adhoc_nix)]
    if build_dir is not None:
        cmd += ["--option", "build-dir", str(build_dir)]
    for opt, attr in _ARGSTR_AXES.items():
        cmd += ["--argstr", opt, getattr(cell, attr)]
    for opt, attr in _ARG_AXES.items():
        cmd += ["--arg", opt, str(getattr(cell, attr))]
    if cell.dispatch != "none":
        cmd += ["--argstr", "dispatch", cell.dispatch]
    # optimise_with_concurrent_gc needs a second thread axis.
    if cell.bench == "optimise_with_concurrent_gc":
        cmd += ["--arg", "threads2", str(cell.threads)]
    # Cores must cover threads + 2; default cores=8 handles up to
    # threads=6, raise for threads=16.
    if cell.threads >= 16:
        cmd += ["--arg", "cores", "24"]
    cmd += ["-o", str(results_dir / cell.name)]
    return cmd


def _shell_quote(argv: list[str]) -> str:
    """Copy-pasteable shell representation of `argv`. Empty arguments
    render as `''` so `--builders ''` round-trips correctly."""
    return " ".join(shlex.quote(a) if a else "''" for a in argv)


def cmd_run(args: argparse.Namespace) -> int:
    adhoc_nix = Path(__file__).resolve().with_name("adhoc.nix")
    if not adhoc_nix.is_file():
        print(f"error: {adhoc_nix} not found", file=sys.stderr)
        return 1

    # Resolve results-dir so the `-o` flag we emit doesn't depend
    # on invocation cwd (makes `--dry-run` output pipeable too).
    results_dir = args.results_dir.resolve()
    build_dir = args.build_dir.resolve() if args.build_dir is not None else None
    cells = _matrix_cells(args)

    if args.dry_run:
        for cell in cells:
            print(_shell_quote(_cell_cmd(cell, adhoc_nix, results_dir, build_dir)))
        print(f"total cells: {len(cells)}", file=sys.stderr)
        return 0

    results_dir.mkdir(parents=True, exist_ok=True)
    (results_dir / "logs").mkdir(exist_ok=True)

    # stdbuf -oL -eL keeps the pipe line-buffered so a killed run
    # leaves a readable partial log.
    stdbuf = shutil.which("stdbuf")

    failures: list[str] = []
    for i, cell in enumerate(cells, start=1):
        cmd = _cell_cmd(cell, adhoc_nix, results_dir, build_dir)
        print(f"[{i}] {cell.name}", file=sys.stderr)

        log_path = results_dir / "logs" / f"{cell.name}.log"
        # First line of the log is the reproducing command.
        log_path.write_text(f"# {_shell_quote(cmd)}\n")

        wrapped = [stdbuf, "-oL", "-eL", *cmd] if stdbuf else cmd
        with log_path.open("a") as log_fh:
            proc = subprocess.Popen(
                wrapped,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
            )
            assert proc.stdout is not None
            for line in proc.stdout:
                log_fh.write(line)
                log_fh.flush()
                sys.stderr.write(line)
            rc = proc.wait()
        if rc != 0:
            failures.append(cell.name)

    print(f"\ntotal cells: {len(cells)}   failed: {len(failures)}", file=sys.stderr)
    if failures:
        print("failures:", file=sys.stderr)
        for name in failures:
            print(f"  {name}", file=sys.stderr)
        return 1
    return 0


# --- argparse --------------------------------------------------------------


def _existing_file(s: str) -> Path:
    p = Path(s)
    if not p.is_file():
        raise argparse.ArgumentTypeError(f"file not found: {s}")
    return p


def _add_axis_filters(sub: argparse.ArgumentParser) -> None:
    """`--<axis>` flags, repeatable, one per AXES entry."""
    for axis in AXES:
        sub.add_argument(f"--{axis}", action="append", help=f"filter by {axis} (repeatable)")


def _add_results_dir(sub: argparse.ArgumentParser) -> None:
    sub.add_argument(
        "--results-dir",
        "--results-dir-a",
        dest="results_dir",
        type=Path,
        default=Path("./results"),
        help="where `bench.py run` dropped `result-*` symlinks (the A side when --results-dir-b is given)",
    )


def _add_dir_pair(sub: argparse.ArgumentParser) -> None:
    """For summary-matrix and ab-matrix: optional second dir + labels.
    Pairs cells across --results-dir and --results-dir-b by full Cell
    identity, no file moves required."""
    sub.add_argument(
        "--results-dir-b",
        dest="results_dir_b",
        type=Path,
        default=None,
        help="optional second results dir; enables two-dir compare mode",
    )
    sub.add_argument("--a-name", default=None, help="label for --results-dir (defaults to its basename)")
    sub.add_argument("--b-name", default=None, help="label for --results-dir-b (defaults to its basename)")
    sub.add_argument(
        "--show-missing",
        action="store_true",
        help="when running with --results-dir-b, also dump the names of unpaired cells (not just an axis summary)",
    )


def _add_thresholds(sub: argparse.ArgumentParser) -> None:
    sub.add_argument(
        "--vfs-tolerance",
        type=float,
        default=VFS_PARITY_TOLERANCE_DEFAULT,
        help=f"max |VFS_a - VFS_b| / VFS_a as a fraction (default {VFS_PARITY_TOLERANCE_DEFAULT})",
    )
    sub.add_argument(
        "--syscall-ratio",
        type=float,
        default=SYSCALL_REDUCTION_RATIO_DEFAULT,
        help=(
            f"max syscall_b / syscall_a ratio. Default {SYSCALL_REDUCTION_RATIO_DEFAULT} "
            "= up to 5%% more syscalls allowed (no regression). Pass < 1.0 to demand a reduction."
        ),
    )
    sub.add_argument(
        "--wall-improvement",
        type=float,
        default=WALL_IMPROVEMENT_MIN_DEFAULT,
        help=(
            f"min (wall_a - wall_b) / wall_a. Default {WALL_IMPROVEMENT_MIN_DEFAULT} "
            "= up to 5%% slower wall allowed (no regression). Pass > 0 to demand a win."
        ),
    )


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="bench",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sp = parser.add_subparsers(dest="cmd", required=True)

    run_p = sp.add_parser("run", help="Build VM cells across the matrix.")
    _add_axis_filters(run_p)
    _add_results_dir(run_p)
    run_p.add_argument(
        "--build-dir",
        type=Path,
        default=None,
        help=(
            "Pass `--option build-dir <path>` to each `nix build`. Point at a tmpfs "
            "(e.g. /dev/shm) so the VM qcow2 empty disks live in RAM — otherwise "
            "host disk/page-cache characteristics dominate the measurement."
        ),
    )
    run_p.add_argument("--dry-run", action="store_true", help="Print nix build commands without executing.")
    run_p.set_defaults(func=cmd_run)

    sum_p = sp.add_parser("summary", help="Print stats for one bench JSON.")
    sum_p.add_argument("json", type=_existing_file)
    sum_p.set_defaults(func=cmd_summary)

    ab_p = sp.add_parser("ab", help="A/B-compare two bench JSONs.")
    ab_p.add_argument("a_json", type=_existing_file, help="Baseline JSON (the 'before')")
    ab_p.add_argument("b_json", type=_existing_file, help="Comparison JSON (the 'after')")
    ab_p.add_argument(
        "a_bpf",
        type=Path,
        nargs="?",
        default=None,
        help="Baseline bpftrace dump (optional; omit for non-GC benches)",
    )
    ab_p.add_argument(
        "b_bpf",
        type=Path,
        nargs="?",
        default=None,
        help="Comparison bpftrace dump (optional; omit for non-GC benches)",
    )
    # Labels are required: defaulting to `syscall`/`iouring` would
    # mislead whenever the A/B isn't the GC dispatch comparison.
    ab_p.add_argument("--a-name", required=True, help="Label for the baseline run (required)")
    ab_p.add_argument("--b-name", required=True, help="Label for the comparison run (required)")
    _add_thresholds(ab_p)
    ab_p.set_defaults(func=cmd_ab)

    sm_p = sp.add_parser("summary-matrix", help="One row per cell in a results dir.")
    _add_axis_filters(sm_p)
    _add_results_dir(sm_p)
    _add_dir_pair(sm_p)
    # Thresholds are only consulted in two-dir mode (the verdict
    # column runs the same gates as ab-matrix); harmless in single-dir.
    _add_thresholds(sm_p)
    sm_p.add_argument("--json", action="store_true", help="JSONL output instead of a table")
    sm_p.add_argument(
        "--format",
        choices=("text", "md"),
        default="text",
        help="table shape: `text` = whitespace-aligned columns (default), `md` = GitHub Markdown table",
    )
    sm_p.set_defaults(func=cmd_summary_matrix)

    am_p = sp.add_parser("ab-matrix", help="Pairwise A/B across one axis, or across two dirs.")
    # `choices=` gives argparse a standard error on unknown axes, so
    # we don't need to re-validate in cmd_ab_matrix. Not required so
    # the --results-dir-b mode can drop it.
    am_p.add_argument("--axis", choices=AB_AXES, help="axis to vary (required unless --results-dir-b is given)")
    _add_axis_filters(am_p)
    _add_results_dir(am_p)
    _add_dir_pair(am_p)
    _add_thresholds(am_p)
    am_p.add_argument(
        "--only",
        choices=("pass", "fail", "missing", "nonpass"),
        default=None,
        help=(
            "restrict the per-pair verdict listing (aggregate still spans all pairs). "
            "`nonpass` covers fail + missing"
        ),
    )
    am_p.add_argument(
        "--top-n",
        type=int,
        default=10,
        help="how many entries to show in the top wall-regression and wall-improvement lists (default 10)",
    )
    am_p.set_defaults(func=cmd_ab_matrix)

    return parser


def main(argv: list[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
