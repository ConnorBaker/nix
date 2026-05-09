"""`generate` subcommand — evaluate a commit sequence under one or more run modes.

Each run gets its own isolated `_state/` (cache, nix-store, eval-store,
state) directory.  Within a run the cache persists across commits (that
is the point of the cold mode), but different runs cannot see each
other's caches or stores.  Hot reuses the cold run's state copy by
default; `--hot-from=cold/0` points hot at a specific cold run.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Literal
from urllib.parse import quote

from cyclopts import App
from rich.console import Console
from rich.panel import Panel

from ..cliutil import DEFAULT_NIX_SRC, DEFAULT_NIXPKGS
from ..layout import CACHE_FIXED_FILES, RESULTS_DIR, RunId, RunPaths

console = Console()

DEFAULT_DISABLE_STRUCTURAL_VARIANT_RECOVERY_ARG = "--no-eval-trace-structural-recovery"
DEFAULT_ENABLE_STRUCTURAL_VARIANT_MISMATCH_TELEMETRY_ARG = (
    "--eval-trace-structural-recovery-mismatch-telemetry"
)
DEFAULT_EVAL_TRACE_HASH_ALGORITHM = ""
EVAL_TRACE_HASH_ALGORITHM_NIX_OPTION = "eval-trace-hash-algorithm"
SUPPORTED_EVAL_TRACE_HASH_ALGORITHMS = frozenset({"blake3", "sha256"})


# -- Data model --------------------------------------------------------------


@dataclass(frozen=True)
class EvalCase:
    case_id: str
    label: str
    argv: tuple[str, ...]
    suite: str
    workload: str
    checkout_cmd: tuple[str, ...] | None = None


@dataclass(frozen=True)
class Phase12Workload:
    name: str
    file: Path
    description: str


PHASE12_WORKLOADS: dict[str, Phase12Workload] = {
    "sibling-heavy": Phase12Workload(
        name="sibling-heavy",
        file=Path("benchmarks") / "eval-trace" / "sibling-heavy.nix",
        description="recursive sibling forcing and replay-heavy attrset access",
    ),
    "traced-data-heavy": Phase12Workload(
        name="traced-data-heavy",
        file=Path("benchmarks") / "eval-trace" / "traced-data-heavy.nix",
        description=("container derivation/materialization across attr and list transforms"),
    ),
    "alias-heavy": Phase12Workload(
        name="alias-heavy",
        file=Path("benchmarks") / "eval-trace" / "alias-heavy.nix",
        description="copied/aliased container equality and reuse pressure",
    ),
}


# -- Run-state helpers -------------------------------------------------------


def clear_caches(cache_dir: Path) -> None:
    """Delete every eval-trace-related cache entry in `cache_dir`.

    Removes fixed-name caches (stat-hash, attr-vocab) + their WAL/SHM
    journals, plus `eval-trace-*.sqlite*` regardless of version.
    """
    if not cache_dir.is_dir():
        return
    for name in CACHE_FIXED_FILES:
        for suffix in ("", "-wal", "-shm"):
            path = cache_dir / (name + suffix)
            if path.exists():
                console.print(f"removing {path}")
                path.unlink()
    for path in sorted(cache_dir.glob("eval-trace-*.sqlite*")):
        console.print(f"removing {path}")
        path.unlink()


def ensure_run_state(run_paths: RunPaths) -> None:
    """Create top-level per-run state directories if missing."""
    for d in (
        run_paths.cache_dir,
        run_paths.nix_state_dir,
        run_paths.nix_log_dir,
        run_paths.nix_store_dir,
        run_paths.eval_store_dir,
    ):
        d.mkdir(parents=True, exist_ok=True)


def _copy_state(src: RunPaths, dst: RunPaths) -> None:
    """Copy `src._state/` into an empty `dst._state/`."""
    for attr in ("cache_dir", "nix_store_dir", "eval_store_dir", "nix_state_dir", "nix_log_dir"):
        s: Path = getattr(src, attr)
        d: Path = getattr(dst, attr)
        if s.is_dir():
            shutil.copytree(s, d)


def reset_state_from(src: RunPaths, dst: RunPaths) -> None:
    """Replace `dst._state/` with an exact copy of `src._state/`."""
    if dst.state_dir.exists():
        shutil.rmtree(dst.state_dir)
    dst.state_dir.mkdir(parents=True)
    _copy_state(src, dst)
    ensure_run_state(dst)


def _store_uri(path: Path) -> str:
    def q(p: Path) -> str:
        return quote(str(p), safe="/")

    # `root` is a local-store root, so Nix stores objects under
    # root/nix/store and defaults state/logs under root/nix/var.  Thread
    # those defaults explicitly so this remains obvious and testable.
    return (
        f"local?root={q(path)}"
        f"&state={q(path / 'nix' / 'var' / 'nix')}"
        f"&log={q(path / 'nix' / 'var' / 'log' / 'nix')}"
    )


def _state_env(run_paths: RunPaths) -> dict[str, str]:
    return {
        "NIX_CACHE_HOME": str(run_paths.cache_dir),
        "NIX_STATE_HOME": str(run_paths.nix_state_dir),
        "NIX_STATE_DIR": str(run_paths.nix_state_dir),
        "NIX_LOG_DIR": str(run_paths.nix_log_dir),
    }


# -- git helper --------------------------------------------------------------


def git_output(repo: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(repo), *args],
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout.strip()


def _absolute_path(path: Path) -> Path:
    return path.expanduser().resolve()


# -- Eval invocation ---------------------------------------------------------


def stats_env(base_env: dict[str, str], stats_path: Path) -> dict[str, str]:
    return {
        **base_env,
        "NIX_SHOW_STATS": "1",
        "NIX_SHOW_STATS_PATH": str(stats_path),
    }


def invoke_eval(
    nix_bin: Path,
    argv: tuple[str, ...],
    *,
    stdout_path: Path,
    stderr_path: Path,
    extra_args: list[str],
    env: dict[str, str],
) -> None:
    cmd = [str(nix_bin), *argv, *extra_args]
    with open(stdout_path, "w") as out_f, open(stderr_path, "w") as err_f:
        subprocess.run(cmd, stdout=out_f, stderr=err_f, env=env, check=True)


def has_completed_outputs(stats: Path, result: Path, debug_log: Path, timing: Path) -> bool:
    if not (stats.exists() and result.exists() and debug_log.exists() and timing.exists()):
        return False
    if stats.stat().st_size == 0 or result.stat().st_size == 0:
        return False
    try:
        json.loads(stats.read_text())
        json.loads(result.read_text())
        json.loads(timing.read_text())
    except json.JSONDecodeError:
        return False
    return True


def prime_eval(nix_bin: Path, case: EvalCase, run_paths: RunPaths, extra_args: list[str]) -> None:
    """Warm-prime a fresh cache for warm-mode runs."""
    clear_caches(run_paths.cache_dir)
    base_env = {**os.environ, **_state_env(run_paths)}
    with tempfile.TemporaryDirectory(prefix="eval-trace-prime-") as tmp:
        tmp_path = Path(tmp)
        invoke_eval(
            nix_bin,
            case.argv,
            stdout_path=tmp_path / "prime-eval.json",
            stderr_path=tmp_path / "prime-debug.log",
            extra_args=extra_args,
            env=stats_env(base_env, tmp_path / "prime-stats.json"),
        )


def eval_case(
    nix_bin: Path,
    case: EvalCase,
    output_dir: Path,
    run_paths: RunPaths,
    mode: str,
    run_extra_args: tuple[str, ...] = (),
) -> float | None:
    stats = output_dir / "stats.json"
    result = output_dir / "eval.json"
    debug_log = output_dir / "debug.log"
    timing = output_dir / "timing.json"
    if has_completed_outputs(stats, result, debug_log, timing):
        return None

    output_dir.mkdir(parents=True, exist_ok=True)

    if case.checkout_cmd is not None:
        subprocess.run(case.checkout_cmd, capture_output=True, check=True)

    extra_args: list[str] = [
        "--store",
        _store_uri(run_paths.nix_store_dir),
        "--eval-store",
        _store_uri(run_paths.eval_store_dir),
    ]
    if mode == "reference":
        extra_args.append("--no-eval-trace")
    elif mode in ("cold", "hot", "warm"):
        extra_args.extend(run_extra_args)
    else:
        raise ValueError(f"Unsupported run mode: {mode}")

    if mode == "warm":
        prime_eval(nix_bin, case, run_paths, extra_args)

    base_env = {**os.environ, **_state_env(run_paths)}
    start = time.monotonic()
    invoke_eval(
        nix_bin,
        case.argv,
        stdout_path=result,
        stderr_path=debug_log,
        extra_args=extra_args,
        env=stats_env(base_env, stats),
    )
    elapsed = time.monotonic() - start
    timing.write_text(json.dumps({"wallTime": elapsed}))
    return elapsed


# -- Case enumeration --------------------------------------------------------


def _git_commit_window(repo: Path, num_commits: int, ref: str) -> list[str]:
    return git_output(repo, "log", f"-{num_commits}", "--format=%H", ref).splitlines()


def nixpkgs_release_cases(
    nixpkgs_path: Path,
    num_commits: int,
    branch: str,
    base: str | None,
) -> list[EvalCase]:
    commits = _git_commit_window(nixpkgs_path, num_commits, base or branch)
    return [
        EvalCase(
            case_id=commit,
            label=commit[:12],
            argv=(
                "eval",
                "-f",
                str(nixpkgs_path / "nixos" / "release.nix"),
                "closures.gnome",
                "--json",
                "--no-pretty",
                "--debug",
            ),
            suite="nixpkgs-release",
            workload="nixpkgs-release",
            checkout_cmd=("git", "-C", str(nixpkgs_path), "checkout", commit),
        )
        for commit in commits
    ]


def flake_attr_cases(
    flake_path: Path,
    attr: str,
    num_commits: int,
    branch: str,
) -> list[EvalCase]:
    commits = _git_commit_window(flake_path, num_commits, branch)
    return [
        EvalCase(
            case_id=commit,
            label=commit[:12],
            argv=(
                "eval",
                "--json",
                "--no-pretty",
                "--debug",
                f"{flake_path}?rev={commit}#{attr}",
            ),
            suite="flake-attr",
            workload="flake-attr",
        )
        for commit in commits
    ]


def phase12_cases(nix_root: Path, workloads: list[Phase12Workload]) -> list[EvalCase]:
    head = git_output(nix_root, "rev-parse", "HEAD")
    return [
        EvalCase(
            case_id=head,
            label=workload.name,
            argv=(
                "eval",
                "-f",
                str(nix_root / workload.file),
                "--json",
                "--no-pretty",
                "--debug",
            ),
            suite="phase12",
            workload=workload.name,
        )
        for workload in workloads
    ]


def parse_workloads(raw: str) -> list[Phase12Workload]:
    requested = [name.strip() for name in raw.split(",") if name.strip()]
    if not requested or requested == ["all"]:
        return list(PHASE12_WORKLOADS.values())
    missing = [name for name in requested if name not in PHASE12_WORKLOADS]
    if missing:
        available = ", ".join(sorted(PHASE12_WORKLOADS))
        raise SystemExit(f"unknown workload(s): {missing}. available: {available}")
    return [PHASE12_WORKLOADS[name] for name in requested]


# -- Run orchestration -------------------------------------------------------


def _run_id(suite: str, workload: str, mode: str, run_number: int) -> RunId:
    name = f"{workload}-{mode}" if suite == "phase12" else mode
    number = None if mode == "reference" else run_number
    return RunId(name=name, number=number)


def _mode_for_run_id(run_id: RunId) -> str:
    return run_id.name.rsplit("-", 1)[-1] if "-" in run_id.name else run_id.name


def _parse_hot_from(spec: str | None, default_cold: RunId) -> RunId:
    if spec is None:
        return default_cold
    if "/" in spec:
        name, num = spec.split("/", 1)
        return RunId(name=name, number=int(num))
    return RunId(name=spec, number=None)


def _parse_selector_csv(raw: str) -> set[str]:
    return {part.strip() for part in raw.split(",") if part.strip()}


def _run_selector_matches(run_id: RunId, mode: str, selectors: set[str] | frozenset[str]) -> bool:
    if not selectors:
        return False
    if "all" in selectors:
        return True

    aliases = {mode, run_id.name, run_id.display()}
    if run_id.number is not None:
        aliases.add(f"{mode}/{run_id.number}")
    return bool(aliases & selectors)


def _extra_eval_args_for_run(
    run_id: RunId,
    *,
    eval_trace_hash_algorithm: str = DEFAULT_EVAL_TRACE_HASH_ALGORITHM,
    disable_structural_variant_recovery_for: set[str] | frozenset[str],
    structural_variant_recovery_disable_arg: str,
    enable_structural_variant_mismatch_telemetry_for: set[str] | frozenset[str] = frozenset(),
    structural_variant_mismatch_telemetry_enable_arg: str = (
        DEFAULT_ENABLE_STRUCTURAL_VARIANT_MISMATCH_TELEMETRY_ARG
    ),
) -> tuple[str, ...]:
    mode = _mode_for_run_id(run_id)
    if mode == "reference":
        return ()

    args = list(_eval_trace_hash_algorithm_args(eval_trace_hash_algorithm))
    if _run_selector_matches(run_id, mode, disable_structural_variant_recovery_for):
        args.append(structural_variant_recovery_disable_arg)
    if _run_selector_matches(run_id, mode, enable_structural_variant_mismatch_telemetry_for):
        args.append(structural_variant_mismatch_telemetry_enable_arg)
    return tuple(args)


def _eval_trace_hash_algorithm_args(eval_trace_hash_algorithm: str) -> tuple[str, ...]:
    algorithm = eval_trace_hash_algorithm.strip().casefold()
    if not algorithm:
        return ()
    return ("--option", EVAL_TRACE_HASH_ALGORITHM_NIX_OPTION, algorithm)


def write_run_manifest(
    run_paths: RunPaths,
    run_id: RunId,
    cases: list[EvalCase],
    *,
    source_repo: Path,
    source_ref: str,
    extra_eval_args: tuple[str, ...] = (),
) -> None:
    payload = {
        "schemaVersion": 1,
        "run": run_id.display(),
        "suite": cases[0].suite if cases else None,
        "workloads": sorted({case.workload for case in cases}),
        "sourceRepo": str(source_repo),
        "sourceRef": source_ref,
        "commitOrder": [case.case_id for case in cases],
    }
    if extra_eval_args:
        payload["extraEvalArgs"] = list(extra_eval_args)
    tmp = run_paths.manifest_file.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(payload, indent=2) + "\n")
    tmp.replace(run_paths.manifest_file)


def execute_run(
    nix_bin: Path,
    nix_root: Path,
    run_id: RunId,
    cases: list[EvalCase],
    *,
    hot_source: RunId | None,
    source_repo: Path,
    source_ref: str,
    extra_eval_args: tuple[str, ...] = (),
) -> None:
    """Execute a single run across every case."""
    console.rule(f"run {run_id.display()}")
    run_paths = RunPaths(run_dir=run_id.resolve(nix_root))
    run_paths.run_dir.mkdir(parents=True, exist_ok=True)
    write_run_manifest(
        run_paths,
        run_id,
        cases,
        source_repo=source_repo,
        source_ref=source_ref,
        extra_eval_args=extra_eval_args,
    )
    mode = _mode_for_run_id(run_id)

    if mode in ("reference", "cold"):
        if run_paths.state_dir.exists():
            shutil.rmtree(run_paths.state_dir)
        ensure_run_state(run_paths)
    elif mode == "hot":
        if hot_source is None:
            raise SystemExit("hot run requires hot_source")
        source_paths = RunPaths(run_dir=hot_source.resolve(nix_root))
        if not source_paths.state_dir.is_dir():
            raise SystemExit(
                f"hot run cannot start: {source_paths.state_dir} does not exist "
                f"(expected the state of a prior {hot_source.display()})",
            )
        reset_state_from(source_paths, run_paths)
    else:
        ensure_run_state(run_paths)

    total = len(cases)
    samples: list[float] = []
    for index, case in enumerate(cases, 1):
        output_dir = run_paths.commit_dir(case.case_id)
        elapsed = eval_case(nix_bin, case, output_dir, run_paths, mode, extra_eval_args)
        prefix = f"[bold]\\[{index}/{total}][/]"
        if elapsed is None:
            console.print(f"{prefix} {case.label} [dim]skipped[/]")
            continue
        samples.append(elapsed)
        avg = sum(samples) / len(samples)
        remaining = total - index
        eta = f", ~{avg * remaining:.0f}s left" if remaining > 0 else ""
        console.print(
            f"{prefix} {case.label} [green]{elapsed:.1f}s[/] "
            f"({len(samples)} done, {remaining} remaining{eta})"
        )


def print_compare_hints(suite: str, run_ids: list[RunId]) -> None:
    if suite != "phase12":
        return
    console.print(Panel("compare each workload separately", expand=False))
    by_workload: dict[str, list[RunId]] = {}
    for rid in run_ids:
        workload = rid.name.rsplit("-", 1)[0]
        by_workload.setdefault(workload, []).append(rid)
    for workload, ids in by_workload.items():
        runs_csv = ",".join(rid.display() for rid in ids)
        ref = next((rid for rid in ids if rid.name.endswith("reference")), None)
        ref_arg = f" --reference {ref.display()}" if ref else ""
        console.print(
            f"eval-trace-bench runs --nix . --runs {runs_csv}{ref_arg}  # {workload}",
            soft_wrap=True,
        )


Suite = Literal["nixpkgs-release", "phase12", "flake-attr"]


def register(app: App) -> None:
    @app.command
    def generate(
        suite: Suite = "nixpkgs-release",
        nix: Path = DEFAULT_NIX_SRC,
        nixpkgs: Path = DEFAULT_NIXPKGS,
        nixpkgs_branch: str = "master",
        nixpkgs_base: str | None = None,
        flake_path: Path | None = None,
        flake_attr: str | None = None,
        flake_branch: str = "main",
        run_number: int = 0,
        num_commits: int = 10,
        runs: str = "reference,cold,hot",
        hot_from: str | None = None,
        workloads: str = "all",
        list_workloads: bool = False,
        eval_trace_hash_algorithm: str = DEFAULT_EVAL_TRACE_HASH_ALGORITHM,
        disable_structural_variant_recovery_for: str = "",
        structural_variant_recovery_disable_arg: str = DEFAULT_DISABLE_STRUCTURAL_VARIANT_RECOVERY_ARG,
        enable_structural_variant_mismatch_telemetry_for: str = "",
        structural_variant_mismatch_telemetry_enable_arg: str = (
            DEFAULT_ENABLE_STRUCTURAL_VARIANT_MISMATCH_TELEMETRY_ARG
        ),
    ) -> int:
        """Generate eval-trace benchmark runs.

        Evaluate a sequence of cases under one or more run modes, each
        with an isolated `_state/` (cache + nix-store + eval-store).

        Parameters
        ----------
        suite: Benchmark suite.
        nix: Nix source checkout (must contain ./result/bin/nix).
        nixpkgs: Nixpkgs checkout for commit enumeration.
        nixpkgs_branch: Nixpkgs branch to walk.
        nixpkgs_base: Optional nixpkgs commit/ref to use as the newest commit.
        flake_path: Flake directory (flake-attr suite only).
        flake_attr: Flake attribute (flake-attr suite only).
        flake_branch: Flake branch (flake-attr suite only).
        run_number: Run number for cold/hot.
        num_commits: How many commits to evaluate.
        runs: Comma-separated run modes.
        hot_from: Run spec (e.g. cold/0) whose _state hot should start
            from.  Defaults to cold/<run-number>.
        workloads: Comma-separated phase12 workload names, or 'all'.
        list_workloads: List phase12 workloads and exit.
        eval_trace_hash_algorithm: Eval-trace hash backend to pass to Nix
            (`blake3` or `sha256`). Empty uses the Nix default.
        disable_structural_variant_recovery_for: Comma-separated run
            selectors that should pass the structural-variant recovery
            disable arg. Selectors match mode (`cold`), run name
            (`sibling-heavy-cold`), displayed run (`cold/1`), or `all`.
        structural_variant_recovery_disable_arg: Nix CLI arg used when
            disabling structural-variant recovery.
        enable_structural_variant_mismatch_telemetry_for: Comma-separated
            run selectors that should pass the SV mismatch telemetry enable
            arg. Use only for diagnostic runs; it forces Nix to load
            candidate dependency values during structural-variant recovery.
        structural_variant_mismatch_telemetry_enable_arg: Nix CLI arg used
            when enabling structural-variant mismatch telemetry.
        """
        nix = _absolute_path(nix)
        nixpkgs = _absolute_path(nixpkgs)
        flake_path = _absolute_path(flake_path) if flake_path is not None else None

        if list_workloads:
            for workload in PHASE12_WORKLOADS.values():
                console.print(f"{workload.name}: {workload.description}")
            return 0

        nix_bin: Path = nix / "result" / "bin" / "nix"
        if not nix_bin.exists():
            console.print(f"[bold red]error:[/] {nix_bin} does not exist")
            return 1

        run_names: list[str] = [n.strip() for n in runs.split(",") if n.strip()]
        if "reference" not in run_names:
            console.print("[bold red]error:[/] runs must include 'reference'")
            return 1
        hash_algorithm = eval_trace_hash_algorithm.strip().casefold()
        if hash_algorithm and hash_algorithm not in SUPPORTED_EVAL_TRACE_HASH_ALGORITHMS:
            supported = ", ".join(sorted(SUPPORTED_EVAL_TRACE_HASH_ALGORITHMS))
            console.print(
                "[bold red]error:[/] "
                f"unsupported eval-trace hash algorithm {eval_trace_hash_algorithm!r}; "
                f"expected one of: {supported}"
            )
            return 1
        sv_disabled_selectors = _parse_selector_csv(disable_structural_variant_recovery_for)
        if sv_disabled_selectors and not structural_variant_recovery_disable_arg.strip():
            console.print("[bold red]error:[/] structural variant recovery disable arg is empty")
            return 1
        sv_mismatch_telemetry_selectors = _parse_selector_csv(
            enable_structural_variant_mismatch_telemetry_for
        )
        if (
            sv_mismatch_telemetry_selectors
            and not structural_variant_mismatch_telemetry_enable_arg.strip()
        ):
            console.print(
                "[bold red]error:[/] structural variant mismatch telemetry enable arg is empty"
            )
            return 1

        parsed_workloads = parse_workloads(workloads)
        if suite == "nixpkgs-release":
            cases = nixpkgs_release_cases(nixpkgs, num_commits, nixpkgs_branch, nixpkgs_base)
            source_repo = nixpkgs
            source_ref = nixpkgs_base or nixpkgs_branch
        elif suite == "flake-attr":
            if not flake_path or not flake_attr:
                console.print(
                    "[bold red]error:[/] --flake-path and --flake-attr required for flake-attr suite"
                )
                return 1
            cases = flake_attr_cases(flake_path, flake_attr, num_commits, flake_branch)
            source_repo = flake_path
            source_ref = flake_branch
        else:
            cases = phase12_cases(nix, parsed_workloads)
            source_repo = nix
            source_ref = git_output(nix, "rev-parse", "HEAD")

        if not cases:
            console.print("[bold red]error:[/] no cases found")
            return 1

        if suite == "phase12":
            all_run_ids: list[RunId] = []
            for workload in parsed_workloads:
                workload_cases = [c for c in cases if c.workload == workload.name]
                default_cold = RunId(name=f"{workload.name}-cold", number=run_number)
                hot_src = _parse_hot_from(hot_from, default_cold)
                for mode in run_names:
                    rid = _run_id("phase12", workload.name, mode, run_number)
                    extra_eval_args = _extra_eval_args_for_run(
                        rid,
                        eval_trace_hash_algorithm=hash_algorithm,
                        disable_structural_variant_recovery_for=sv_disabled_selectors,
                        structural_variant_recovery_disable_arg=structural_variant_recovery_disable_arg,
                        enable_structural_variant_mismatch_telemetry_for=sv_mismatch_telemetry_selectors,
                        structural_variant_mismatch_telemetry_enable_arg=(
                            structural_variant_mismatch_telemetry_enable_arg
                        ),
                    )
                    execute_run(
                        nix_bin,
                        nix,
                        rid,
                        workload_cases,
                        hot_source=hot_src,
                        source_repo=source_repo,
                        source_ref=source_ref,
                        extra_eval_args=extra_eval_args,
                    )
                    all_run_ids.append(rid)
            print_compare_hints("phase12", all_run_ids)
        else:
            default_cold = RunId(name="cold", number=run_number)
            hot_src = _parse_hot_from(hot_from, default_cold)
            for mode in run_names:
                rid = _run_id(suite, suite, mode, run_number)
                extra_eval_args = _extra_eval_args_for_run(
                    rid,
                    eval_trace_hash_algorithm=hash_algorithm,
                    disable_structural_variant_recovery_for=sv_disabled_selectors,
                    structural_variant_recovery_disable_arg=structural_variant_recovery_disable_arg,
                    enable_structural_variant_mismatch_telemetry_for=sv_mismatch_telemetry_selectors,
                    structural_variant_mismatch_telemetry_enable_arg=(
                        structural_variant_mismatch_telemetry_enable_arg
                    ),
                )
                execute_run(
                    nix_bin,
                    nix,
                    rid,
                    cases,
                    hot_source=hot_src,
                    source_repo=source_repo,
                    source_ref=source_ref,
                    extra_eval_args=extra_eval_args,
                )

        if suite == "nixpkgs-release":
            subprocess.run(
                ["git", "-C", str(nixpkgs), "checkout", nixpkgs_branch],
                capture_output=True,
                check=True,
            )

        console.print(
            Panel(
                "[bold]per-run stores[/bold]\n"
                f"each run's store root is {{nix}}/{RESULTS_DIR}/<run>/_state/nix-store.\n"
                "objects live under that root's nix/store; store state lives under nix/var/nix.\n"
                "inspect: nix path-info --store local?root=<path> /nix/store/...\n"
                "compare: eval-trace-bench db-inspect --db <path>/_state/cache",
                expand=False,
            )
        )
        if suite != "phase12":
            print(file=sys.stderr)
        return 0
