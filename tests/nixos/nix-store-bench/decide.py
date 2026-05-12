"""bench-decide — interpret nix-store-bench JSON output.

Two subcommands:

  summary <json>
      Print mean / median / p99 / stddev / min / max wall times for
      every iteration row in `<json>`. Exits 0 unless the file has
      no rows or the bench reported uncaught throws.

  ab <a-json> <b-json> <a-bpf> <b-bpf> [--a-name N --b-name N] \\
     [--vfs-tolerance F --syscall-ratio F --wall-improvement F]
      Three-criterion A/B comparison between two bench JSONs + their
      bpftrace dumps:

        1. VFS op count within --vfs-tolerance (same kernel work)
        2. b/a syscall ratio ≤ --syscall-ratio (no syscall regression)
        3. (a - b) / a ≥ --wall-improvement (no wall regression)

      Defaults are a "no regression" gate, not a "win required" gate:
      `--wall-improvement -0.05` tolerates up to 5% slower wall on `b`,
      `--syscall-ratio 1.05` tolerates up to 5% more syscalls on `b`.
      A pass means b is within noise of a, not that b is dramatically
      faster. Pass `--wall-improvement 0.10` etc. on the CLI to demand
      an actual win.
"""

from __future__ import annotations

import argparse
import json
import re
import statistics
import sys
from dataclasses import dataclass, field
from pathlib import Path


VFS_PARITY_TOLERANCE_DEFAULT = 0.05
# Noise-tolerant "no regression" gates. The per-worker-rings io_uring
# rewrite lands at parity with the syscall path on the canonical
# `gc_barabasi/ext4/flat/single` scenario, not a 10% improvement, so
# the defaults are calibrated for that. CLI flags can demand a stricter
# win when investigating perf changes.
SYSCALL_REDUCTION_RATIO_DEFAULT = 1.05  # b/a ≤ 1.05 → ≤ 5 % more syscalls
WALL_IMPROVEMENT_MIN_DEFAULT = -0.05    # (a-b)/a ≥ -0.05 → ≤ 5 % slower wall

UNIT_FACTOR = {"ns": 1, "us": 1_000, "ms": 1_000_000, "s": 1_000_000_000}


@dataclass
class BenchData:
    """One bench JSON, parsed once.

    `by_name` groups iteration wall-times (ns) by bench cell name.
    `throws` totals `gc_threw` / `opt_threw` counters across rows.
    """

    path: Path
    by_name: dict[str, list[int]] = field(default_factory=dict)
    throws: dict[str, int] = field(
        default_factory=lambda: {"gc_threw": 0, "opt_threw": 0}
    )

    @classmethod
    def load(cls, path: Path) -> BenchData:
        data = cls(path=path)
        doc = json.loads(path.read_text())
        for row in doc.get("benchmarks", []):
            if row.get("run_type") != "iteration":
                continue
            if row.get("error_occurred"):
                print(
                    f"warning: dropping errored iteration in {path}: "
                    f"{row.get('name', '?')}: "
                    f"{row.get('error_message', '?')}",
                    file=sys.stderr,
                )
                continue
            unit = row.get("time_unit", "ns")
            factor = UNIT_FACTOR.get(unit)
            if factor is None:
                raise ValueError(
                    f"unknown time_unit {unit!r} in {path} "
                    f"(known: {sorted(UNIT_FACTOR)})"
                )
            ns = int(row["real_time"] * factor)
            data.by_name.setdefault(row.get("name", "?"), []).append(ns)
            for k in data.throws:
                if k in row:
                    data.throws[k] += int(row[k])
        return data

    @property
    def all_samples(self) -> list[int]:
        return [ns for samples in self.by_name.values() for ns in samples]

    @property
    def mean_wall_ns(self) -> int:
        s = self.all_samples
        return int(sum(s) / len(s)) if s else 0

    def has_throws(self) -> bool:
        return any(v > 0 for v in self.throws.values())


def parse_bpf(path: Path) -> tuple[int, int]:
    """(vfs_count, syscall_count) from a bpftrace map dump."""
    if not path.exists():
        return 0, 0
    text = path.read_text()
    vfs = sum(int(m) for m in re.findall(r"@vfs\[[^\]]+\]:\s*(\d+)", text))
    sc = sum(int(m) for m in re.findall(r"@sc\[\d+\]:\s*(\d+)", text))
    return vfs, sc


def p99(samples: list[int]) -> int:
    """99th percentile via type-7 linear interpolation. Matches
    numpy's default and `statistics.quantiles` with method='inclusive'."""
    if not samples:
        return 0
    if len(samples) == 1:
        return samples[0]
    return int(statistics.quantiles(samples, n=100, method="inclusive")[98])


def fmt_ns(ns: int) -> str:
    """Format `ns` with the largest unit that keeps the integer part
    non-trivial. Picks ns / µs / ms / s based on magnitude rather
    than always rendering in seconds (so 1.5 ms shows as `1.500ms`,
    not `0.002s`)."""
    if ns == 0:
        return "0"
    abs_ns = abs(ns)
    if abs_ns >= 1_000_000_000:
        return f"{ns / 1e9:.3f}s"
    if abs_ns >= 1_000_000:
        return f"{ns / 1e6:.3f}ms"
    if abs_ns >= 1_000:
        return f"{ns / 1e3:.3f}µs"
    return f"{ns}ns"


@dataclass
class SummaryStats:
    n: int
    mean: int
    median: int
    p99: int
    stddev: int
    min: int
    max: int

    @classmethod
    def of(cls, samples: list[int]) -> SummaryStats:
        n = len(samples)
        if n == 0:
            return cls(n=0, mean=0, median=0, p99=0, stddev=0, min=0, max=0)
        return cls(
            n=n,
            mean=int(statistics.fmean(samples)),
            median=int(statistics.median(samples)),
            p99=p99(samples),
            stddev=int(statistics.stdev(samples)) if n > 1 else 0,
            min=min(samples),
            max=max(samples),
        )


def _print_throws(label: str, data: BenchData) -> None:
    print(
        f"FAIL: {label} reported uncaught throws: {data.throws} "
        f"(measurement is suspect; the bench's inner loop caught "
        f"an exception while running)",
        file=sys.stderr,
    )


def run_summary(args: argparse.Namespace) -> int:
    data = BenchData.load(args.json)
    if not data.by_name:
        # `test -s` only checks file non-empty; a `{"benchmarks": []}`
        # payload passes that check but means the filter matched
        # nothing or every iteration errored. Fail loudly.
        print(
            f"FAIL: no iteration rows in {args.json} (bench filter "
            f"matched nothing, or every iteration errored)",
            file=sys.stderr,
        )
        return 1
    if data.has_throws():
        _print_throws(str(args.json), data)
        return 1
    print(f"=== summary of {args.json} ===")
    for name, samples in data.by_name.items():
        s = SummaryStats.of(samples)
        print(
            f"{name}  n={s.n:>2d}  "
            f"mean={fmt_ns(s.mean):>9s}  "
            f"median={fmt_ns(s.median):>9s}  "
            f"p99={fmt_ns(s.p99):>9s}  "
            f"stddev={fmt_ns(s.stddev):>9s}  "
            f"min={fmt_ns(s.min):>9s}  "
            f"max={fmt_ns(s.max):>9s}"
        )
    return 0


def run_ab(args: argparse.Namespace) -> int:
    a = BenchData.load(args.a_json)
    b = BenchData.load(args.b_json)

    if not a.all_samples or not b.all_samples:
        print(
            f"missing iteration rows ({args.a_name}="
            f"{len(a.all_samples)} samples, {args.b_name}="
            f"{len(b.all_samples)} samples)",
            file=sys.stderr,
        )
        return 2
    fail = False
    if a.has_throws():
        _print_throws(args.a_name, a)
        fail = True
    if b.has_throws():
        _print_throws(args.b_name, b)
        fail = True
    if fail:
        return 1

    a_vfs, a_sys = parse_bpf(args.a_bpf)
    b_vfs, b_sys = parse_bpf(args.b_bpf)
    a_s = SummaryStats.of(a.all_samples)
    b_s = SummaryStats.of(b.all_samples)

    print(f"=== A/B: {args.a_name} vs {args.b_name} ===")
    for label, s, vfs, sc in (
        (args.a_name, a_s, a_vfs, a_sys),
        (args.b_name, b_s, b_vfs, b_sys),
    ):
        print(
            f"{label}: wall={fmt_ns(s.mean):>9s}  "
            f"p99={fmt_ns(s.p99):>9s}  "
            f"stddev={fmt_ns(s.stddev):>9s}  "
            f"vfs={vfs:>7d}  syscalls={sc:>8d}"
        )

    failures: list[str] = []

    if a_vfs == 0:
        failures.append(
            f"{args.a_name} VFS count is zero — bpftrace probably "
            f"failed to attach"
        )
    else:
        delta = abs(a_vfs - b_vfs) / a_vfs
        if delta > args.vfs_tolerance:
            failures.append(
                f"VFS op count diverged: {a_vfs} -> {b_vfs} "
                f"(delta {delta:.1%}, threshold ≤ {args.vfs_tolerance:.1%})"
            )

    if a_sys == 0:
        failures.append(
            f"{args.a_name} syscall count is zero — bpftrace probably "
            f"failed"
        )
    else:
        ratio = b_sys / a_sys
        if ratio > args.syscall_ratio:
            failures.append(
                f"syscall regression: {a_sys} -> {b_sys} "
                f"(ratio {ratio:.1%}, threshold ≤ {args.syscall_ratio:.1%})"
            )

    if a_s.mean == 0:
        failures.append(
            f"{args.a_name} wall is zero — bench probably failed"
        )
    else:
        improvement = (a_s.mean - b_s.mean) / a_s.mean
        if improvement < args.wall_improvement:
            failures.append(
                f"wall regression: {improvement:+.1%} "
                f"(threshold ≥ {args.wall_improvement:+.1%})"
            )

    if failures:
        print("\nVERDICT: FAIL")
        for f in failures:
            print(f"  - {f}")
        return 1

    print("\nVERDICT: PASS")
    return 0


def _existing_file(s: str) -> Path:
    p = Path(s)
    if not p.is_file():
        raise argparse.ArgumentTypeError(f"file not found: {s}")
    return p


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="bench-decide",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    subparsers = parser.add_subparsers(dest="cmd", required=True)

    summary_p = subparsers.add_parser(
        "summary",
        help="Print stats for one bench JSON.",
        description=(
            "Print mean / median / p99 / stddev / min / max wall "
            "times for every iteration row in <json>."
        ),
    )
    summary_p.add_argument("json", type=_existing_file)
    summary_p.set_defaults(func=run_summary)

    ab_p = subparsers.add_parser(
        "ab",
        help="Compare two bench runs.",
        description=(
            "Three-criterion A/B comparison: VFS parity, syscall "
            "reduction, wall improvement."
        ),
    )
    ab_p.add_argument("a_json", type=_existing_file,
                      help="Baseline JSON (the 'before')")
    ab_p.add_argument("b_json", type=_existing_file,
                      help="Comparison JSON (the 'after')")
    # bpf dumps are NOT validated — `parse_bpf` returns (0, 0) on a
    # missing file, which then triggers the "bpftrace failed to
    # attach" failure message. That's the intended behaviour.
    ab_p.add_argument("a_bpf", type=Path,
                      help="Baseline bpftrace dump")
    ab_p.add_argument("b_bpf", type=Path,
                      help="Comparison bpftrace dump")
    ab_p.add_argument("--a-name", default="syscall",
                      help="Label for baseline (default: syscall)")
    ab_p.add_argument("--b-name", default="iouring",
                      help="Label for comparison (default: iouring)")
    ab_p.add_argument(
        "--vfs-tolerance", type=float,
        default=VFS_PARITY_TOLERANCE_DEFAULT,
        help=("Max |VFS_a - VFS_b| / VFS_a as a fraction "
              f"(default {VFS_PARITY_TOLERANCE_DEFAULT})"),
    )
    ab_p.add_argument(
        "--syscall-ratio", type=float,
        default=SYSCALL_REDUCTION_RATIO_DEFAULT,
        help=("Max syscall_b / syscall_a ratio. Default "
              f"{SYSCALL_REDUCTION_RATIO_DEFAULT} = up to 5 % more "
              "syscalls allowed (no regression). Pass < 1.0 to demand "
              "an actual reduction."),
    )
    ab_p.add_argument(
        "--wall-improvement", type=float,
        default=WALL_IMPROVEMENT_MIN_DEFAULT,
        help=("Min (wall_a - wall_b) / wall_a. Default "
              f"{WALL_IMPROVEMENT_MIN_DEFAULT} = up to 5 % slower wall "
              "allowed (no regression). Pass > 0 to demand a win."),
    )
    ab_p.set_defaults(func=run_ab)

    return parser


def main(argv: list[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
