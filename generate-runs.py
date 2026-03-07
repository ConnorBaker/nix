"""Generate eval trace benchmark runs.

Runs nix eval on recent nixpkgs commits in three modes:
  reference  -- no eval trace (--no-eval-trace), single shared directory
  cold       -- fresh eval trace cache, indexed by run number
  hot        -- populated eval trace cache, indexed by run number
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

from rich.console import Console

CACHE_FILES = [
    "stat-hash-cache.sqlite",
    "attr-vocab.sqlite",
    "eval-trace-v4.sqlite",
]

console = Console()


def clear_caches(cache_dir: Path) -> None:
    for name in CACHE_FILES:
        path = cache_dir / name
        if path.exists():
            console.print(f"Removing {path}")
            path.unlink()


def eval_commit(
    nix_bin: Path,
    nixpkgs_path: Path,
    commit: str,
    output_dir: Path,
    extra_args: list[str],
) -> float | None:
    """Evaluate a single commit, writing stats.json, eval.json, and debug.log.

    Returns elapsed seconds if evaluation ran, None if skipped.
    """
    stats = output_dir / "stats.json"
    result = output_dir / "eval.json"
    debug_log = output_dir / "debug.log"

    if stats.exists() and result.exists() and debug_log.exists():
        return None

    output_dir.mkdir(parents=True, exist_ok=True)

    subprocess.run(
        ["git", "-C", str(nixpkgs_path), "checkout", commit],
        capture_output=True,
        check=True,
    )

    start = time.monotonic()
    with open(result, "w") as out_f, open(debug_log, "w") as err_f:
        subprocess.run(
            [
                str(nix_bin),
                "eval",
                "-f",
                str(nixpkgs_path / "nixos" / "release.nix"),
                "closures.gnome",
                "--json",
                "--no-pretty",
                "--debug",
                *extra_args,
            ],
            stdout=out_f,
            stderr=err_f,
            env={
                **dict(__import__("os").environ),
                "NIX_SHOW_STATS": "1",
                "NIX_SHOW_STATS_PATH": str(stats),
            },
            check=True,
        )

    return time.monotonic() - start


def main() -> None:
    parser = argparse.ArgumentParser(
        prog="generate-runs",
        description="Generate eval trace benchmark runs",
    )
    parser.add_argument(
        "--nixpkgs",
        type=Path,
        default=Path.home() / "nixpkgs",
        help="Path to nixpkgs checkout (default: ~/nixpkgs)",
    )
    parser.add_argument(
        "--nix",
        type=Path,
        default=Path.home() / "nix",
        help="Path to nix source dir with ./result symlink (default: ~/nix)",
    )
    parser.add_argument(
        "--run-number",
        type=int,
        default=0,
        help="Run number for indexing non-reference runs (default: 0)",
    )
    parser.add_argument(
        "--num-commits",
        type=int,
        default=10,
        help="Number of recent commits to evaluate (default: 10)",
    )
    parser.add_argument(
        "--runs",
        type=str,
        default="reference,cold,hot",
        help="Comma-separated run names (default: reference,cold,hot)",
    )
    args = parser.parse_args()

    nix_bin = args.nix / "result" / "bin" / "nix"
    if not nix_bin.exists():
        console.print(f"[bold red]Error:[/] {nix_bin} does not exist")
        sys.exit(1)

    run_names = [r.strip() for r in args.runs.split(",")]

    result = subprocess.run(
        [
            "git",
            "-C",
            str(args.nixpkgs),
            "log",
            f"-{args.num_commits}",
            "--format=%H",
            "master",
        ],
        capture_output=True,
        text=True,
        check=True,
    )
    commits = result.stdout.strip().splitlines()

    console.print("Clearing Nix caches")
    clear_caches(Path.home() / ".cache" / "nix")

    for run_name in run_names:
        extra_args: list[str] = []
        run_dir = args.nix / run_name

        if run_name == "reference":
            extra_args.append("--no-eval-trace")
        else:
            run_dir = run_dir / str(args.run_number)

        console.rule(f"Run '{run_name}' (number {args.run_number})")

        total = len(commits)
        done = 0
        remaining_secs: list[float] = []

        for i, commit in enumerate(commits, 1):
            short = commit[:12]
            prefix = f"[bold]\\[{i}/{total}][/]"
            output_dir = run_dir / commit
            elapsed = eval_commit(nix_bin, args.nixpkgs, commit, output_dir, extra_args)
            if elapsed is not None:
                done += 1
                remaining_secs.append(elapsed)
                avg = sum(remaining_secs) / len(remaining_secs)
                left = total - i
                eta = f", ~{avg * left:.0f}s left" if left > 0 else ""
                console.print(
                    f"{prefix} {short} [green]{elapsed:.1f}s[/] ({done} done, {left} remaining{eta})"
                )
            else:
                console.print(f"{prefix} {short} [dim]skipped[/]")

    subprocess.run(
        ["git", "-C", str(args.nixpkgs), "checkout", "master"],
        capture_output=True,
        check=True,
    )


if __name__ == "__main__":
    main()
