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
  ab-matrix --axis <ax>   Pairwise A/B across one axis of the
                          matrix.
"""

from __future__ import annotations

import argparse
import json
import re
import shlex
import shutil
import statistics
import subprocess
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path

# --- axis taxonomy ---------------------------------------------------------
#
# Must track the `bench.*` enums in `bench-options.nix`. Add a new enum value
# here when you add one there (and to NAME_RE / DISPATCH_SUFFIX_RE for
# tail-axis additions that appear in the derivation name).

ALL_BENCHES = (
    "optimise",
    "gc_barabasi",
    "gc_clusters",
)
ALL_FS = ("ext4", "xfs", "btrfs", "zfs", "tmpfs")
ALL_THROTTLES = ("gp3", "io2", "nvme", "none")
# Axes that don't exist at this commit — we keep them as constants
# so Cell names parse under the full-rig NAME_RE. The matrix
# driver pins each to its only baseline-valid value; GC benches
# additionally get `dispatch = "syscall"` inside _matrix_cells.
ALL_LAYOUTS = ("flat",)
ALL_REPLICAS = ("multi",)
ALL_DISPATCHES = ("syscall",)  # no io_uring at baseline
ALL_NPATHS = (2000, 10000, 50000)
ALL_THREADS = (4,)  # single-arg at baseline; current-branch uses more

# Axis order = derivation-name order. Used for filter construction
# and for parsing result-* names back into Cell tuples.
AXES = ("bench", "dispatch", "fs", "throttle", "layout", "replica", "npaths", "threads")

# Axes that can't sensibly A/B: different `bench`es measure different
# things; different `npaths`/`threads` are different work sizes.
AB_AXES = tuple(a for a in AXES if a not in ("bench", "npaths", "threads"))

GC_BENCHES = frozenset({"gc_barabasi", "gc_clusters"})
NON_VARIANT_BENCHES = frozenset()  # no non-variant benches at baseline

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
    r"(?:-multi)?"
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
    """

    path: Path
    by_name: dict[str, list[int]] = field(default_factory=dict)
    throws: dict[str, int] = field(default_factory=lambda: {"gc_threw": 0, "opt_threw": 0})

    @classmethod
    def load(cls, path: Path) -> BenchData:
        data = cls(path=path)
        for row in json.loads(path.read_text()).get("benchmarks", []):
            if row.get("run_type") != "iteration":
                continue
            if row.get("error_occurred"):
                print(
                    f"warning: dropping errored iteration in {path}: "
                    f"{row.get('name', '?')}: {row.get('error_message', '?')}",
                    file=sys.stderr,
                )
                continue
            factor = UNIT_FACTOR.get(row.get("time_unit", "ns"))
            if factor is None:
                raise ValueError(f"unknown time_unit {row.get('time_unit')!r} in {path} (known: {sorted(UNIT_FACTOR)})")
            data.by_name.setdefault(row.get("name", "?"), []).append(int(row["real_time"] * factor))
            for k in data.throws:
                if k in row:
                    data.throws[k] += int(row[k])
        return data

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
    return Cell(
        bench=head.replace("-", "_"),
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
    symlink: Path

    @property
    def bpf_path(self) -> Path | None:
        """GC-only: the `<dispatch>.bpf.txt` next to the JSON. None
        for non-GC cells or when bpftrace never produced output."""
        if self.cell.dispatch == "none":
            return None
        p = self.symlink.resolve() / f"{self.cell.dispatch}.bpf.txt"
        return p if p.is_file() else None


def _cell_json(symlink: Path) -> Path | None:
    """The `<dispatch>.json` or `single.json` inside the symlink's
    target directory."""
    if not symlink.is_symlink():
        return None
    target = symlink.resolve()
    for stem in ("syscall", "iouring", "single"):
        p = target / f"{stem}.json"
        if p.is_file():
            return p
    return None


def discover(results_dir: Path) -> list[Result]:
    """Every parsable `result-*` symlink in `results_dir`, paired
    with the JSON inside its target. Warns and skips entries that
    fail either step."""
    out: list[Result] = []
    for entry in sorted(results_dir.iterdir()):
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
        out.append(Result(cell, jp, entry))
    return out


def _filter_cells(args: argparse.Namespace, results: list[Result]) -> list[Result]:
    """Apply `--<axis>` filters from `args` to a result list. An axis
    with no filter = match all; non-empty = membership."""
    filters = [(axis, getattr(args, axis)) for axis in AXES if getattr(args, axis)]
    return [r for r in results if all(str(getattr(r.cell, axis)) in vs for axis, vs in filters)]


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


def do_ab(
    a_json: Path,
    b_json: Path,
    a_name: str,
    b_name: str,
    a_bpf_path: Path | None,
    b_bpf_path: Path | None,
    thr: Thresholds,
) -> int:
    """A/B engine. Returns 0 = PASS, 1 = FAIL, 2 = missing data."""
    a = BenchData.load(a_json)
    b = BenchData.load(b_json)

    if not a.all_samples or not b.all_samples:
        print(
            f"missing iteration rows ({a_name}={len(a.all_samples)} samples, {b_name}={len(b.all_samples)} samples)",
            file=sys.stderr,
        )
        return 2

    throws_seen = False
    for label, data in ((a_name, a), (b_name, b)):
        if data.has_throws:
            print(data.throws_message(label), file=sys.stderr)
            throws_seen = True
    if throws_seen:
        return 1

    a_bpf = parse_bpf(a_bpf_path)
    b_bpf = parse_bpf(b_bpf_path)
    a_s, b_s = Stats.of(a.all_samples), Stats.of(b.all_samples)

    print(f"=== A/B: {a_name} vs {b_name} ===")
    for label, s, bpf in ((a_name, a_s, a_bpf), (b_name, b_s, b_bpf)):
        extra = "(no bpftrace)" if bpf is None else f"vfs={bpf[0]:>7d}  syscalls={bpf[1]:>8d}"
        print(f"{label}: wall={fmt_ns(s.mean):>9s}  p99={fmt_ns(s.p99):>9s}  stddev={fmt_ns(s.stddev):>9s}  {extra}")

    failures: list[str] = []

    # VFS/syscall checks only apply when both bpf dumps were supplied.
    if a_bpf is not None and b_bpf is not None:
        a_vfs, a_sys = a_bpf
        b_vfs, b_sys = b_bpf

        if a_vfs == 0:
            failures.append(f"{a_name} VFS count is zero — bpftrace probably failed to attach")
        elif (delta := abs(a_vfs - b_vfs) / a_vfs) > thr.vfs_tolerance:
            failures.append(
                f"VFS op count diverged: {a_vfs} -> {b_vfs} (delta {delta:.1%}, threshold ≤ {thr.vfs_tolerance:.1%})"
            )

        if a_sys == 0:
            failures.append(f"{a_name} syscall count is zero — bpftrace probably failed")
        elif (ratio := b_sys / a_sys) > thr.syscall_ratio:
            failures.append(
                f"syscall regression: {a_sys} -> {b_sys} (ratio {ratio:.1%}, threshold ≤ {thr.syscall_ratio:.1%})"
            )

    if a_s.mean == 0:
        failures.append(f"{a_name} wall is zero — bench probably failed")
    elif (improvement := (a_s.mean - b_s.mean) / a_s.mean) < thr.wall_improvement:
        failures.append(f"wall regression: {improvement:+.1%} (threshold ≥ {thr.wall_improvement:+.1%})")

    if failures:
        print("\nVERDICT: FAIL")
        for f in failures:
            print(f"  - {f}")
        return 1

    print("\nVERDICT: PASS")
    return 0


def cmd_ab(args: argparse.Namespace) -> int:
    return do_ab(
        args.a_json,
        args.b_json,
        args.a_name,
        args.b_name,
        args.a_bpf,
        args.b_bpf,
        Thresholds.from_args(args),
    )


# --- `summary-matrix` (one row per cell) ----------------------------------

_TABLE_COLS = (
    # (header, cell→str extractor)
    ("bench", lambda r, s: r.cell.bench),
    ("dispatch", lambda r, s: r.cell.dispatch),
    ("fs", lambda r, s: r.cell.fs),
    ("throttle", lambda r, s: r.cell.throttle),
    ("layout", lambda r, s: r.cell.layout),
    ("replica", lambda r, s: r.cell.replica),
    ("n", lambda r, s: str(r.cell.npaths)),
    ("t", lambda r, s: str(r.cell.threads)),
    ("iters", lambda r, s: str(s.n)),
    ("mean", lambda r, s: fmt_ns(s.mean)),
    ("p99", lambda r, s: fmt_ns(s.p99)),
    ("stddev", lambda r, s: fmt_ns(s.stddev)),
)


def cmd_summary_matrix(args: argparse.Namespace) -> int:
    results = _filter_cells(args, discover(args.results_dir))
    if not results:
        print(f"no cells matched in {args.results_dir}", file=sys.stderr)
        return 1

    rows = [(r, Stats.of(BenchData.load(r.json_path).all_samples)) for r in results]

    if args.json:
        for r, s in rows:
            row = asdict(r.cell) | {
                "iters": s.n,
                "mean_ns": s.mean,
                "p99_ns": s.p99,
                "stddev_ns": s.stddev,
                "json_path": str(r.json_path),
            }
            print(json.dumps(row))
        return 0

    headers = tuple(h for h, _ in _TABLE_COLS)
    rendered = [tuple(fn(r, s) for _, fn in _TABLE_COLS) for r, s in rows]
    widths = [max(len(h), *(len(c[i]) for c in rendered)) for i, h in enumerate(headers)]
    fmt = "  ".join(f"{{:<{w}}}" for w in widths)
    print(fmt.format(*headers))
    print(fmt.format(*("-" * w for w in widths)))
    for cells in rendered:
        print(fmt.format(*cells))
    return 0


# --- `ab-matrix` (partition by fixed axes, A/B each pair) -----------------


def cmd_ab_matrix(args: argparse.Namespace) -> int:
    axis = args.axis
    thr = Thresholds.from_args(args)
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

    rc_total = 0
    pairs.sort(key=lambda g: tuple(getattr(g[0].cell, a) for a in fixed_axes))
    for group in pairs:
        group.sort(key=lambda r: getattr(r.cell, axis))
        a, b = group
        fixed = ", ".join(f"{ax}={getattr(a.cell, ax)}" for ax in fixed_axes)
        print(f"\n=== {axis}: {getattr(a.cell, axis)} vs {getattr(b.cell, axis)}  [{fixed}] ===")
        rc = do_ab(
            a.json_path,
            b.json_path,
            str(getattr(a.cell, axis)),
            str(getattr(b.cell, axis)),
            a.bpf_path,
            b.bpf_path,
            thr,
        )
        if rc != 0:
            rc_total = rc
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
#
# Baseline `bench-options.nix` has no layout/replica/dispatch
# options — the full-rig axes that don't exist at this commit. Cell
# still carries them so JSONs round-trip under the current-branch
# NAME_RE; the schema just ignores them on the build side.
_ARGSTR_AXES = {
    "benchName": "bench",
    "fs": "fs",
    "throttle": "throttle",
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
        type=Path,
        default=Path("./results"),
        help="where `bench.py run` dropped `result-*` symlinks",
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
    sm_p.add_argument("--json", action="store_true", help="JSONL output instead of a table")
    sm_p.set_defaults(func=cmd_summary_matrix)

    am_p = sp.add_parser("ab-matrix", help="Pairwise A/B across one axis.")
    # `choices=` gives argparse a standard error on unknown axes, so
    # we don't need to re-validate in cmd_ab_matrix.
    am_p.add_argument("--axis", required=True, choices=AB_AXES, help="axis to vary")
    _add_axis_filters(am_p)
    _add_results_dir(am_p)
    _add_thresholds(am_p)
    am_p.set_defaults(func=cmd_ab_matrix)

    return parser


def main(argv: list[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
