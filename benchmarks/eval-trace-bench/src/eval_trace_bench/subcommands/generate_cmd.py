"""`generate` subcommand — evaluate a commit sequence under one or more run modes.

Each run gets its own isolated `_state/` (cache, nix-store, eval-store,
state) directory.  Within a run the cache persists across commits (that
is the point of the cold mode), but different runs cannot see each
other's caches or stores.  Hot reuses the cold run's state copy by
default; `--hot-from=cold/0` points hot at a specific cold run.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import time
from contextlib import suppress
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Literal, cast
from urllib.parse import quote

from cyclopts import App
from pydantic import ValidationError
from rich.console import Console
from rich.panel import Panel

from ..cliutil import DEFAULT_NIX_SRC, DEFAULT_NIXPKGS
from ..layout import CACHE_FIXED_FILES, RESULTS_DIR, RunId, RunPaths
from ..models import Timing

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


def remove_run_state(path: Path) -> None:
    """Remove a benchmark `_state/` tree, including read-only local-store paths."""
    if not path.exists():
        return
    for root, dirs, files in os.walk(path):
        root_path = Path(root)
        with suppress(OSError):
            root_path.chmod(root_path.stat().st_mode | stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR)
        for name in (*dirs, *files):
            child = root_path / name
            with suppress(OSError):
                child.chmod(child.stat().st_mode | stat.S_IWUSR)
    shutil.rmtree(path)


def _copy_state(src: RunPaths, dst: RunPaths) -> None:
    """Copy `src._state/` into an empty `dst._state/`."""
    for attr in ("cache_dir", "nix_store_dir", "eval_store_dir", "nix_state_dir", "nix_log_dir"):
        s: Path = getattr(src, attr)
        d: Path = getattr(dst, attr)
        if s.is_dir():
            shutil.copytree(s, d)


def reset_state_from(src: RunPaths, dst: RunPaths) -> None:
    """Replace `dst._state/` with an exact copy of `src._state/`."""
    remove_run_state(dst.state_dir)
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


GIT_PROVENANCE_PATHSPEC: tuple[str, ...] = (
    "--",
    ".",
    ":(exclude)eval-trace-bench-results",
    ":(exclude)result",
)


def git_output(repo: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(repo), *args],
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout.strip()


def git_output_optional(repo: Path, *args: str) -> str | None:
    try:
        result = subprocess.run(
            ["git", "-C", str(repo), *args],
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError:
        return None
    if result.returncode != 0:
        return None
    return result.stdout.strip()


def _file_sha256(path: Path) -> str | None:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError:
        return None
    return digest.hexdigest()


def _git_diff_sha256(repo: Path) -> str | None:
    try:
        result = subprocess.run(
            [
                "git",
                "-C",
                str(repo),
                "diff",
                "--no-ext-diff",
                "--binary",
                "HEAD",
                *GIT_PROVENANCE_PATHSPEC,
            ],
            capture_output=True,
            check=False,
        )
    except OSError:
        return None
    if result.returncode != 0:
        return None
    return hashlib.sha256(result.stdout).hexdigest()


def _git_dirty_tree_sha256(repo: Path) -> str | None:
    def update_untracked_entry(digest: Any, path: Path, display_name: bytes) -> None:
        digest.update(display_name)
        digest.update(b"\0")
        try:
            st = path.lstat()
        except OSError as exc:
            digest.update(f"lstat-error:{exc.errno}\0".encode())
            return
        digest.update(f"mode:{stat.S_IMODE(st.st_mode):o}\0".encode())
        digest.update(f"type:{stat.S_IFMT(st.st_mode):o}\0".encode())
        if stat.S_ISLNK(st.st_mode):
            try:
                digest.update(os.fsencode(os.readlink(path)))
            except OSError as exc:
                digest.update(f"readlink-error:{exc.errno}".encode())
        elif stat.S_ISREG(st.st_mode):
            try:
                digest.update(path.read_bytes())
            except OSError as exc:
                digest.update(f"read-error:{exc.errno}".encode())
        elif stat.S_ISDIR(st.st_mode):
            try:
                children = sorted(path.iterdir(), key=lambda child: os.fsencode(child.name))
            except OSError as exc:
                digest.update(f"iterdir-error:{exc.errno}".encode())
            else:
                digest.update(b"dir-children\0")
                for child in children:
                    child_name = display_name + b"/" + os.fsencode(child.name)
                    update_untracked_entry(digest, child, child_name)
        else:
            digest.update(b"unsupported-entry-type")
        digest.update(b"\0")

    try:
        status = subprocess.run(
            [
                "git",
                "-C",
                str(repo),
                "status",
                "--porcelain=v1",
                "-z",
                *GIT_PROVENANCE_PATHSPEC,
            ],
            capture_output=True,
            check=False,
        )
        diff = subprocess.run(
            [
                "git",
                "-C",
                str(repo),
                "diff",
                "--no-ext-diff",
                "--binary",
                "HEAD",
                *GIT_PROVENANCE_PATHSPEC,
            ],
            capture_output=True,
            check=False,
        )
        untracked = subprocess.run(
            [
                "git",
                "-C",
                str(repo),
                "ls-files",
                "--others",
                "--exclude-standard",
                "-z",
                *GIT_PROVENANCE_PATHSPEC,
            ],
            capture_output=True,
            check=False,
        )
    except OSError:
        return None
    if status.returncode != 0 or diff.returncode != 0 or untracked.returncode != 0:
        return None

    digest = hashlib.sha256()
    digest.update(b"status\0")
    digest.update(status.stdout)
    digest.update(b"diff\0")
    digest.update(diff.stdout)
    digest.update(b"untracked\0")
    for raw_name in sorted(name for name in untracked.stdout.split(b"\0") if name):
        update_untracked_entry(digest, repo / os.fsdecode(raw_name), raw_name)
    return digest.hexdigest()


def _git_repo_provenance(repo: Path, *, include_diff_hash: bool) -> dict[str, object]:
    status = git_output_optional(repo, "status", "--porcelain=v1", *GIT_PROVENANCE_PATHSPEC)
    head = git_output_optional(repo, "rev-parse", "HEAD")
    provenance: dict[str, object] = {
        "path": str(repo),
        "isGitRepository": head is not None,
        "head": head,
        "dirty": None if status is None else bool(status),
    }
    if include_diff_hash:
        provenance["diffSha256"] = _git_diff_sha256(repo)
        provenance["dirtyTreeSha256"] = _git_dirty_tree_sha256(repo)
    return provenance


def _nix_store_path_for(path: Path) -> str | None:
    parts = path.resolve().parts
    if len(parts) >= 4 and parts[1:3] == ("nix", "store"):
        return str(Path("/").joinpath(*parts[1:4]))
    return None


def _nix_deriver_for_store_path(store_path: str) -> str | None:
    nix_store = shutil.which("nix-store")
    if nix_store is None:
        return None
    try:
        result = subprocess.run(
            [nix_store, "--query", "--deriver", store_path],
            capture_output=True,
            text=True,
            check=False,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if result.returncode != 0:
        return None
    deriver = result.stdout.strip()
    if not deriver or deriver == "unknown-deriver":
        return None
    return deriver


def _benchmark_subject_provenance(nix_bin: Path, nix_root: Path) -> dict[str, object]:
    resolved_nix_bin = nix_bin.expanduser().resolve()
    nix_root_provenance = _git_repo_provenance(nix_root, include_diff_hash=True)
    store_path = _nix_store_path_for(resolved_nix_bin)
    result_nix = nix_root / "result" / "bin" / "nix"
    nix_root_kind = "git-checkout" if nix_root_provenance["isGitRepository"] else "directory"
    if store_path and result_nix.exists() and result_nix.resolve() == resolved_nix_bin:
        nix_root_kind = "result-wrapper"
    provenance: dict[str, object] = {
        "nixBin": str(resolved_nix_bin),
        "nixBinSha256": _file_sha256(resolved_nix_bin),
        "nixRoot": str(nix_root),
        "nixRootKind": nix_root_kind,
        "nixCheckout": nix_root_provenance,
    }
    if store_path := _nix_store_path_for(resolved_nix_bin):
        provenance["nixStorePath"] = store_path
        if deriver := _nix_deriver_for_store_path(store_path):
            provenance["nixStoreDeriver"] = deriver
    return provenance


def _benchmark_environment_provenance() -> dict[str, str]:
    prefixes = ("NIX_EVAL_TRACE_",)
    names = {
        "NIX_ALLOW_EVAL",
        "NIX_DEBUG_SQLITE_TRACES",
    }
    return {
        name: os.environ[name]
        for name in sorted(os.environ)
        if name in names or any(name.startswith(prefix) for prefix in prefixes)
    }


def _absolute_path(path: Path) -> Path:
    return path.expanduser().resolve()


def _resolve_benchmark_nix_bin(nix: Path) -> Path | None:
    nix_bin = nix / "result" / "bin" / "nix"
    if not nix_bin.exists():
        return None
    return nix_bin.resolve(strict=True)


# -- Eval invocation ---------------------------------------------------------


def stats_env(base_env: dict[str, str], stats_path: Path) -> dict[str, str]:
    return {
        **base_env,
        "NIX_SHOW_STATS": "1",
        "NIX_SHOW_STATS_PATH": str(stats_path),
    }


def measurement_env(
    base_env: dict[str, str], stats_path: Path, *, with_stats: bool
) -> dict[str, str]:
    if with_stats:
        return stats_env(base_env, stats_path)
    env = dict(base_env)
    env.pop("NIX_SHOW_STATS", None)
    env.pop("NIX_SHOW_STATS_PATH", None)
    return env


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


def has_completed_outputs(
    stats: Path,
    result: Path,
    stderr_log: Path,
    timing: Path,
    *,
    with_stats: bool,
) -> bool:
    if not (result.exists() and stderr_log.exists() and timing.exists()):
        return False
    if result.stat().st_size == 0:
        return False
    if with_stats and (not stats.exists() or stats.stat().st_size == 0):
        return False
    try:
        json.loads(result.read_text())
        timing_payload = json.loads(timing.read_text())
        if not isinstance(timing_payload, dict):
            return False
        Timing.model_validate(timing_payload)
        if with_stats:
            json.loads(stats.read_text())
    except (json.JSONDecodeError, ValidationError):
        return False
    return True


def prime_eval(
    nix_bin: Path,
    case: EvalCase,
    run_paths: RunPaths,
    extra_args: list[str],
    *,
    with_debug: bool,
    with_stats: bool,
) -> None:
    """Warm-prime a fresh cache for warm-mode runs."""
    clear_caches(run_paths.cache_dir)
    base_env = {**os.environ, **_state_env(run_paths)}
    with tempfile.TemporaryDirectory(prefix="eval-trace-prime-") as tmp:
        tmp_path = Path(tmp)
        prime_args = [*extra_args]
        if with_debug:
            prime_args.append("--debug")
        invoke_eval(
            nix_bin,
            case.argv,
            stdout_path=tmp_path / "prime-eval.json",
            stderr_path=tmp_path / ("prime-debug.log" if with_debug else "prime-stderr.log"),
            extra_args=prime_args,
            env=measurement_env(base_env, tmp_path / "prime-stats.json", with_stats=with_stats),
        )


def eval_case(
    nix_bin: Path,
    case: EvalCase,
    output_dir: Path,
    run_paths: RunPaths,
    mode: str,
    run_extra_args: tuple[str, ...] = (),
    with_debug: bool = False,
    with_stats: bool = False,
) -> float | None:
    stats = output_dir / "stats.json"
    result = output_dir / "eval.json"
    stderr_log = output_dir / ("debug.log" if with_debug else "stderr.log")
    timing = output_dir / "timing.json"
    if has_completed_outputs(stats, result, stderr_log, timing, with_stats=with_stats):
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
        prime_eval(
            nix_bin,
            case,
            run_paths,
            extra_args,
            with_debug=with_debug,
            with_stats=with_stats,
        )

    base_env = {**os.environ, **_state_env(run_paths)}
    eval_args = [*extra_args]
    if with_debug:
        eval_args.append("--debug")
    start = time.monotonic()
    invoke_eval(
        nix_bin,
        case.argv,
        stdout_path=result,
        stderr_path=stderr_log,
        extra_args=eval_args,
        env=measurement_env(base_env, stats, with_stats=with_stats),
    )
    elapsed = time.monotonic() - start
    timing.write_text(
        json.dumps({"wallTime": elapsed, "withDebug": with_debug, "withStats": with_stats})
    )
    return elapsed


# -- Case enumeration --------------------------------------------------------


def _git_commit_window(repo: Path, num_commits: int, ref: str) -> list[str]:
    return git_output(repo, "log", f"-{num_commits}", "--format=%H", ref).splitlines()


def _commits_from_file(path: Path) -> list[str]:
    commits: list[str] = []
    for lineno, raw_line in enumerate(path.read_text().splitlines(), 1):
        line = raw_line.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) != 1:
            raise ValueError(f"{path}:{lineno}: expected one commit, got {raw_line!r}")
        commits.append(parts[0])
    if not commits:
        raise ValueError(f"{path}: commit list is empty")
    return commits


def _validate_git_commits(repo: Path, commits: list[str]) -> None:
    for commit in commits:
        subprocess.run(
            ["git", "-C", str(repo), "cat-file", "-e", f"{commit}^{{commit}}"],
            capture_output=True,
            text=True,
            check=True,
        )


def nixpkgs_release_cases(
    nixpkgs_path: Path,
    num_commits: int,
    branch: str,
    base: str | None,
    commit_list: list[str] | None = None,
) -> list[EvalCase]:
    commits = (
        commit_list
        if commit_list is not None
        else _git_commit_window(nixpkgs_path, num_commits, base or branch)
    )
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
    commit_list: list[str] | None = None,
) -> list[EvalCase]:
    commits = (
        commit_list
        if commit_list is not None
        else _git_commit_window(flake_path, num_commits, branch)
    )
    return [
        EvalCase(
            case_id=commit,
            label=commit[:12],
            argv=(
                "eval",
                "--json",
                "--no-pretty",
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


def _run_id(
    suite: str,
    workload: str,
    mode: str,
    run_number: int,
    *,
    with_debug: bool,
    with_stats: bool,
) -> RunId:
    name = f"{workload}-{mode}" if suite == "phase12" else mode
    if with_debug:
        name = f"{name}-debug"
    elif with_stats:
        name = f"{name}-stats"
    number = None if mode == "reference" else run_number
    return RunId(name=name, number=number)


def _mode_for_run_id(run_id: RunId) -> str:
    name = run_id.name
    if name.endswith("-debug"):
        name = name[: -len("-debug")]
    elif name.endswith("-stats"):
        name = name[: -len("-stats")]
    return name.rsplit("-", 1)[-1] if "-" in name else name


def _parse_hot_from(spec: str | None, default_cold: RunId) -> RunId:
    if spec is None:
        return default_cold
    if "/" in spec:
        name, num = spec.split("/", 1)
        return RunId(name=name, number=int(num))
    return RunId(name=spec, number=None)


def _parse_selector_csv(raw: str) -> set[str]:
    return {part.strip() for part in raw.split(",") if part.strip()}


def _next_unused_run_number(nix_root: Path, run_ids: list[RunId]) -> int:
    """Return the next append-only run number for the planned non-reference runs."""
    max_seen = -1
    for run_id in run_ids:
        if run_id.number is None:
            continue
        run_root = run_id.resolve(nix_root).parent
        if not run_root.is_dir():
            continue
        for child in run_root.iterdir():
            if not child.is_dir():
                continue
            if not (
                child.joinpath("manifest.json").exists()
                or _run_has_any_commit_outputs(RunPaths(run_dir=child))
            ):
                continue
            try:
                max_seen = max(max_seen, int(child.name))
            except ValueError:
                continue
    return max_seen + 1


def _planned_non_reference_run_ids(
    suite: str,
    workloads: list[Phase12Workload],
    run_names: list[str],
    run_number: int,
    *,
    with_debug: bool,
    with_stats: bool,
) -> list[RunId]:
    if suite == "phase12":
        return [
            _run_id(
                "phase12",
                workload.name,
                mode,
                run_number,
                with_debug=with_debug,
                with_stats=with_stats,
            )
            for workload in workloads
            for mode in run_names
            if mode != "reference"
        ]
    return [
        _run_id(suite, suite, mode, run_number, with_debug=with_debug, with_stats=with_stats)
        for mode in run_names
        if mode != "reference"
    ]


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


def build_run_manifest_payload(
    run_id: RunId,
    cases: list[EvalCase],
    *,
    nix_bin: Path,
    nix_root: Path,
    source_repo: Path,
    source_ref: str,
    extra_eval_args: tuple[str, ...] = (),
    with_debug: bool = False,
    with_stats: bool = False,
    source_repo_provenance: dict[str, object] | None = None,
) -> dict[str, object]:
    measurement_mode = "debug-stats" if with_debug else "stats" if with_stats else "wall-only"
    payload: dict[str, object] = {
        "schemaVersion": 1,
        "run": run_id.display(),
        "suite": cases[0].suite if cases else None,
        "workloads": sorted({case.workload for case in cases}),
        "sourceRepo": str(source_repo),
        "sourceRef": source_ref,
        "commitOrder": [case.case_id for case in cases],
        "withDebug": with_debug,
        "withStats": with_stats,
        "measurementMode": measurement_mode,
        "benchmarkSubject": _benchmark_subject_provenance(nix_bin, nix_root),
        "benchmarkEnvironment": _benchmark_environment_provenance(),
        "sourceRepoProvenance": source_repo_provenance
        if source_repo_provenance is not None
        else _git_repo_provenance(source_repo, include_diff_hash=True),
    }
    if extra_eval_args:
        payload["extraEvalArgs"] = list(extra_eval_args)
    return payload


def write_run_manifest(
    run_paths: RunPaths,
    payload: dict[str, object],
) -> None:
    tmp = run_paths.manifest_file.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(payload, indent=2) + "\n")
    tmp.replace(run_paths.manifest_file)


def _read_json_file(path: Path) -> object | None:
    try:
        return json.loads(path.read_text())
    except (OSError, json.JSONDecodeError):
        return None


def _manifest_resume_compatible(
    existing: object | None,
    planned: object | None,
) -> bool:
    if not isinstance(existing, dict) or not isinstance(planned, dict):
        return False
    existing_manifest = cast(dict[str, Any], existing)
    planned_manifest = cast(dict[str, Any], planned)
    checked_keys: tuple[str, ...] = (
        "schemaVersion",
        "run",
        "suite",
        "workloads",
        "sourceRepo",
        "sourceRef",
        "commitOrder",
        "withDebug",
        "withStats",
        "measurementMode",
        "extraEvalArgs",
        "benchmarkSubject",
        "benchmarkEnvironment",
        "sourceRepoProvenance",
    )
    return all(existing_manifest.get(key) == planned_manifest.get(key) for key in checked_keys)


def _benchmark_subject_state_identity(subject: object) -> object:
    if not isinstance(subject, dict):
        return subject
    subject_dict = cast(dict[str, Any], subject)
    if subject_dict.get("nixRootKind") != "result-wrapper":
        return subject_dict
    return {
        key: subject_dict.get(key)
        for key in ("nixBinSha256", "nixStorePath")
        if key in subject_dict
    }


def _manifest_state_source_value(manifest: dict[str, Any], key: str) -> object:
    value = manifest.get(key)
    if key == "benchmarkSubject":
        return _benchmark_subject_state_identity(value)
    return value


def _manifest_state_source_compatible(
    source: object | None,
    planned: object | None,
) -> bool:
    if not isinstance(source, dict) or not isinstance(planned, dict):
        return False
    source_manifest = cast(dict[str, Any], source)
    planned_manifest = cast(dict[str, Any], planned)
    checked_keys: tuple[str, ...] = (
        "schemaVersion",
        "suite",
        "workloads",
        "sourceRepo",
        "sourceRef",
        "commitOrder",
        "withDebug",
        "withStats",
        "measurementMode",
        "extraEvalArgs",
        "benchmarkSubject",
        "benchmarkEnvironment",
        "sourceRepoProvenance",
    )
    return all(
        _manifest_state_source_value(source_manifest, key)
        == _manifest_state_source_value(planned_manifest, key)
        for key in checked_keys
    )


def _planned_hot_manifest_from_source(
    source: dict[str, Any],
    planned: dict[str, object],
) -> dict[str, object]:
    adjusted = dict(planned)
    if "sourceRepoProvenance" in source:
        adjusted["sourceRepoProvenance"] = source["sourceRepoProvenance"]
    return adjusted


def _source_repo_provenance_matches_hot_source(
    source: dict[str, Any],
    current: dict[str, object],
) -> bool:
    expected = source.get("sourceRepoProvenance")
    if not isinstance(expected, dict):
        return False
    expected_provenance = cast(dict[str, object], expected)
    dirty_keys = ("isGitRepository", "dirty", "diffSha256", "dirtyTreeSha256")
    if all(key in expected_provenance for key in dirty_keys):
        return all(expected_provenance.get(key) == current.get(key) for key in dirty_keys)
    return expected_provenance == current


def _run_has_any_commit_outputs(run_paths: RunPaths) -> bool:
    if not run_paths.run_dir.is_dir():
        return False
    return any(
        child.is_dir()
        and len(child.name) == 40
        and all(c in "0123456789abcdef" for c in child.name)
        for child in run_paths.run_dir.iterdir()
    )


def _run_has_completed_outputs(
    run_paths: RunPaths,
    cases: list[EvalCase],
    *,
    with_debug: bool,
    with_stats: bool,
) -> bool:
    for case in cases:
        output_dir = run_paths.commit_dir(case.case_id)
        if not has_completed_outputs(
            output_dir / "stats.json",
            output_dir / "eval.json",
            output_dir / ("debug.log" if with_debug else "stderr.log"),
            output_dir / "timing.json",
            with_stats=with_stats,
        ):
            return False
    return True


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
    with_debug: bool = False,
    with_stats: bool = False,
    source_repo_provenance: dict[str, object] | None = None,
) -> None:
    """Execute a single run across every case."""
    console.rule(f"run {run_id.display()}")
    run_paths = RunPaths(run_dir=run_id.resolve(nix_root))
    run_paths.run_dir.mkdir(parents=True, exist_ok=True)
    existing_manifest = (
        _read_json_file(run_paths.manifest_file) if run_paths.manifest_file.exists() else None
    )
    planned_manifest = build_run_manifest_payload(
        run_id,
        cases,
        nix_bin=nix_bin,
        nix_root=nix_root,
        source_repo=source_repo,
        source_ref=source_ref,
        extra_eval_args=extra_eval_args,
        with_debug=with_debug,
        with_stats=with_stats,
        source_repo_provenance=source_repo_provenance,
    )
    mode = _mode_for_run_id(run_id)
    source_paths: RunPaths | None = None
    typed_source_manifest: dict[str, Any] | None = None
    if mode == "hot":
        if hot_source is None:
            raise SystemExit("hot run requires hot_source")
        if _mode_for_run_id(hot_source) != "cold":
            raise SystemExit(
                f"hot run cannot start: --hot-from must reference a cold run, got {hot_source.display()}."
            )
        source_paths = RunPaths(run_dir=hot_source.resolve(nix_root))
        if not source_paths.state_dir.is_dir():
            raise SystemExit(
                f"hot run cannot start: {source_paths.state_dir} does not exist "
                f"(expected the state of a prior {hot_source.display()})",
            )
        source_manifest = (
            _read_json_file(source_paths.manifest_file)
            if source_paths.manifest_file.exists()
            else None
        )
        if not isinstance(source_manifest, dict):
            raise SystemExit(
                f"hot run cannot start: {hot_source.display()} manifest is missing or has "
                "a different run id."
            )
        typed_source_manifest = cast(dict[str, Any], source_manifest)
        if typed_source_manifest.get("run") != hot_source.display():
            raise SystemExit(
                f"hot run cannot start: {hot_source.display()} manifest is missing or has "
                "a different run id."
            )
        planned_manifest = _planned_hot_manifest_from_source(
            typed_source_manifest,
            planned_manifest,
        )
        current_source_provenance = _git_repo_provenance(source_repo, include_diff_hash=True)
        if not _source_repo_provenance_matches_hot_source(
            typed_source_manifest,
            current_source_provenance,
        ):
            raise SystemExit(
                f"hot run cannot start: current source repository provenance differs from "
                f"{hot_source.display()}; restore the source checkout before reusing hot state."
            )
    if existing_manifest is None and _run_has_any_commit_outputs(run_paths):
        raise SystemExit(
            f"{run_id.display()} already contains commit outputs but has no readable manifest. "
            "Remove the run directory or choose a new --run-number."
        )
    if existing_manifest is not None and not _manifest_resume_compatible(
        existing_manifest,
        planned_manifest,
    ):
        raise SystemExit(
            f"{run_id.display()} already has a manifest for a different measurement. "
            "Remove the run directory or choose a new --run-number."
        )
    if mode in ("cold", "hot", "warm") and _run_has_any_commit_outputs(run_paths):
        raise SystemExit(
            f"{run_id.display()} already contains commit outputs for a stateful run. "
            "Remove the run directory or choose a new --run-number."
        )

    if mode in ("reference", "cold", "warm"):
        write_run_manifest(run_paths, planned_manifest)
        remove_run_state(run_paths.state_dir)
        ensure_run_state(run_paths)
    elif mode == "hot":
        assert hot_source is not None
        assert source_paths is not None
        assert typed_source_manifest is not None
        if not _manifest_state_source_compatible(typed_source_manifest, planned_manifest):
            raise SystemExit(
                f"hot run cannot start: {hot_source.display()} manifest does not match "
                f"{run_id.display()} except for the run name."
            )
        if not _run_has_completed_outputs(
            source_paths,
            cases,
            with_debug=with_debug,
            with_stats=with_stats,
        ):
            raise SystemExit(
                f"hot run cannot start: {hot_source.display()} does not have complete "
                "outputs for the planned commit list."
            )
        write_run_manifest(run_paths, planned_manifest)
        reset_state_from(source_paths, run_paths)
    else:
        write_run_manifest(run_paths, planned_manifest)
        ensure_run_state(run_paths)

    total = len(cases)
    samples: list[float] = []
    for index, case in enumerate(cases, 1):
        output_dir = run_paths.commit_dir(case.case_id)
        elapsed = eval_case(
            nix_bin,
            case,
            output_dir,
            run_paths,
            mode,
            extra_eval_args,
            with_debug=with_debug,
            with_stats=with_stats,
        )
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
        name = rid.name
        if name.endswith("-debug"):
            name = name[: -len("-debug")]
        elif name.endswith("-stats"):
            name = name[: -len("-stats")]
        workload = name.rsplit("-", 1)[0]
        by_workload.setdefault(workload, []).append(rid)
    for workload, ids in by_workload.items():
        runs_csv = ",".join(rid.display() for rid in ids)
        ref = next((rid for rid in ids if _mode_for_run_id(rid) == "reference"), None)
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
        commit_list: Path | None = None,
        flake_path: Path | None = None,
        flake_attr: str | None = None,
        flake_branch: str = "main",
        run_number: int | None = None,
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
        with_debug: bool = False,
        with_stats: bool = False,
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
        commit_list: Optional text file of commits to evaluate in exact order.
            Blank lines and `#` comments are ignored. Supported by the
            nixpkgs-release and flake-attr suites.
        flake_path: Flake directory (flake-attr suite only).
        flake_attr: Flake attribute (flake-attr suite only).
        flake_branch: Flake branch (flake-attr suite only).
        run_number: Run number for non-reference runs. If omitted, generate
            selects the next unused number across the planned run directories.
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
        with_debug: Run the legacy diagnostic mode: pass --debug to Nix,
            set NIX_SHOW_STATS=1/NIX_SHOW_STATS_PATH, capture debug.log,
            and write results under *-debug run directories. The default
            mode omits --debug and NIX_SHOW_STATS and records process wall
            time only.
        with_stats: Set NIX_SHOW_STATS=1/NIX_SHOW_STATS_PATH and require
            stats.json without passing --debug. Results are written under
            *-stats run directories. Ignored when with_debug is set because
            debug mode already records stats.
        """
        with_stats = with_debug or with_stats
        nix = _absolute_path(nix)
        nixpkgs = _absolute_path(nixpkgs)
        commit_list = _absolute_path(commit_list) if commit_list is not None else None
        flake_path = _absolute_path(flake_path) if flake_path is not None else None

        if list_workloads:
            for workload in PHASE12_WORKLOADS.values():
                console.print(f"{workload.name}: {workload.description}")
            return 0

        nix_bin = _resolve_benchmark_nix_bin(nix)
        if nix_bin is None:
            console.print(f"[bold red]error:[/] {nix / 'result' / 'bin' / 'nix'} does not exist")
            return 1

        run_names: list[str] = [n.strip() for n in runs.split(",") if n.strip()]

        if not run_names:
            console.print("[bold red]error:[/] runs must name at least one mode")
            return 1
        invalid_run_names = [
            run_name
            for run_name in run_names
            if run_name not in {"reference", "cold", "warm", "hot"}
        ]
        if invalid_run_names:
            console.print(
                "[bold red]error:[/] unsupported run mode(s): "
                + ", ".join(sorted(invalid_run_names))
            )
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
        explicit_commits: list[str] | None = None
        if commit_list is not None:
            if suite == "phase12":
                console.print("[bold red]error:[/] --commit-list is not supported for phase12")
                return 1
            try:
                explicit_commits = _commits_from_file(commit_list)
            except OSError as exc:
                console.print(f"[bold red]error:[/] cannot read --commit-list {commit_list}: {exc}")
                return 1
            except ValueError as exc:
                console.print(f"[bold red]error:[/] {exc}")
                return 1
        if run_number is None:
            planned_ids = _planned_non_reference_run_ids(
                suite,
                parsed_workloads,
                run_names,
                0,
                with_debug=with_debug,
                with_stats=with_stats,
            )
            if planned_ids:
                run_number = _next_unused_run_number(nix, planned_ids)
                console.print(f"[dim]auto-selected run number {run_number}[/]")
            else:
                run_number = 0
        elif run_number < 0:
            console.print("[bold red]error:[/] run-number must be non-negative")
            return 1

        if suite == "nixpkgs-release":
            if explicit_commits is not None:
                try:
                    _validate_git_commits(nixpkgs, explicit_commits)
                except subprocess.CalledProcessError as exc:
                    console.print(
                        "[bold red]error:[/] --commit-list contains a commit "
                        f"that is not present in {nixpkgs}: {exc.cmd[-1]}"
                    )
                    return 1
            cases = nixpkgs_release_cases(
                nixpkgs,
                num_commits,
                nixpkgs_branch,
                nixpkgs_base,
                explicit_commits,
            )
            source_repo = nixpkgs
            source_ref = (
                f"commit-list:{commit_list}"
                if commit_list is not None
                else nixpkgs_base or nixpkgs_branch
            )
        elif suite == "flake-attr":
            if not flake_path or not flake_attr:
                console.print(
                    "[bold red]error:[/] --flake-path and --flake-attr required for flake-attr suite"
                )
                return 1
            if explicit_commits is not None:
                try:
                    _validate_git_commits(flake_path, explicit_commits)
                except subprocess.CalledProcessError as exc:
                    console.print(
                        "[bold red]error:[/] --commit-list contains a commit "
                        f"that is not present in {flake_path}: {exc.cmd[-1]}"
                    )
                    return 1
            cases = flake_attr_cases(
                flake_path,
                flake_attr,
                num_commits,
                flake_branch,
                explicit_commits,
            )
            source_repo = flake_path
            source_ref = f"commit-list:{commit_list}" if commit_list is not None else flake_branch
        else:
            cases = phase12_cases(nix, parsed_workloads)
            source_repo = nix
            source_ref = git_output(nix, "rev-parse", "HEAD")

        if not cases:
            console.print("[bold red]error:[/] no cases found")
            return 1

        source_repo_provenance = _git_repo_provenance(source_repo, include_diff_hash=True)
        restore_failed = False
        try:
            if suite == "phase12":
                all_run_ids: list[RunId] = []
                for workload in parsed_workloads:
                    workload_cases = [c for c in cases if c.workload == workload.name]
                    default_cold = RunId(name=f"{workload.name}-cold", number=run_number)
                    if with_debug:
                        default_cold = RunId(
                            name=f"{workload.name}-cold-debug",
                            number=run_number,
                        )
                    elif with_stats:
                        default_cold = RunId(
                            name=f"{workload.name}-cold-stats",
                            number=run_number,
                        )
                    hot_src = _parse_hot_from(hot_from, default_cold)
                    for mode in run_names:
                        rid = _run_id(
                            "phase12",
                            workload.name,
                            mode,
                            run_number,
                            with_debug=with_debug,
                            with_stats=with_stats,
                        )
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
                            with_debug=with_debug,
                            with_stats=with_stats,
                            source_repo_provenance=source_repo_provenance,
                        )
                        all_run_ids.append(rid)
                print_compare_hints("phase12", all_run_ids)
            else:
                default_cold = RunId(name="cold", number=run_number)
                if with_debug:
                    default_cold = RunId(name="cold-debug", number=run_number)
                elif with_stats:
                    default_cold = RunId(name="cold-stats", number=run_number)
                hot_src = _parse_hot_from(hot_from, default_cold)
                for mode in run_names:
                    rid = _run_id(
                        suite,
                        suite,
                        mode,
                        run_number,
                        with_debug=with_debug,
                        with_stats=with_stats,
                    )
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
                        with_debug=with_debug,
                        with_stats=with_stats,
                        source_repo_provenance=source_repo_provenance,
                    )
        finally:
            if suite == "nixpkgs-release":
                restore = subprocess.run(
                    ["git", "-C", str(nixpkgs), "checkout", nixpkgs_branch],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                if restore.returncode != 0:
                    restore_failed = True
                    console.print(
                        "[bold red]warning:[/] failed to restore "
                        f"{nixpkgs} to {nixpkgs_branch}: {restore.stderr.strip()}"
                    )
        if restore_failed:
            return 1

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
