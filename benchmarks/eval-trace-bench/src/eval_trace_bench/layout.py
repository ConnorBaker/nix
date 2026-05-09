"""Directory layout shared by all subcommands.

One `nix_root/eval-trace-bench-results/` directory hosts every benchmark
run.  A single "run" is one named execution of the evaluator across a
sequence of commits.  Each run owns a parallel copy of the cache /
nix-store / eval-store directories so independent runs cannot
contaminate one another — this is what lets us A/B-compare two code
versions without one pollution-run affecting the other, and what lets
the analysis subcommands compare eval outputs, cache behavior, logs, and DB
state without cross-run contamination.

Layout:

    {nix_root}/
        eval-trace-bench-results/
            {run_name}/{run_number}/      # run directory
                _state/                   # per-run, shared across commits
                    cache/                # XDG_CACHE_HOME for this run
                    nix-store/            # --store local?root=... for this run
                        nix/store/        # physical store objects
                        nix/var/nix/      # store db, gc roots, temp roots
                    eval-store/           # --eval-store local?root=...
                        nix/store/
                        nix/var/nix/
                    state/                # NIX_STATE_HOME / NIX_STATE_DIR
                    log/                  # NIX_LOG_DIR
                {commit_sha}/
                    stats.json
                    timing.json
                    eval.json
                    debug.log

The run-number sub-directory is omitted for `reference` runs, matching
the legacy layout that the analysis subcommands still understand.  The `_state/`
directory lives AT the run-number level (not at the commit level) so
cold→hot within a run can reuse the cache the cold run populated across
commits — that's what makes "cold" a meaningful "walking-the-commit-
graph with one persistent cache" measurement.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

# Fixed-name cache files in a NIX_CACHE_HOME directory. attr-vocab.sqlite is
# current; stat-hash-cache.sqlite is legacy but still cleared if present.
CACHE_FIXED_FILES: tuple[str, ...] = (
    "stat-hash-cache.sqlite",
    "attr-vocab.sqlite",
)

MANIFEST_FILE = "manifest.json"
RESULTS_DIR = "eval-trace-bench-results"


def results_root(nix_root: Path) -> Path:
    """Return the generated-results directory for a Nix checkout."""
    if nix_root.name == RESULTS_DIR:
        return nix_root
    return nix_root / RESULTS_DIR


def existing_results_root(nix_root: Path) -> Path:
    """Return the generated-results directory, falling back to the legacy root."""
    root = results_root(nix_root)
    return root if root.is_dir() else nix_root


@dataclass(frozen=True)
class RunId:
    """Identifier for a single run.

    `number` is None for `reference` runs (which use a flat directory).
    """

    name: str
    number: int | None

    @property
    def rel_path(self) -> Path:
        if self.number is None:
            return Path(self.name)
        return Path(self.name) / str(self.number)

    def resolve(self, nix_root: Path) -> Path:
        return results_root(nix_root) / self.rel_path

    def display(self) -> str:
        if self.number is None:
            return self.name
        return f"{self.name}/{self.number}"


@dataclass(frozen=True)
class RunPaths:
    """Paths inside one run directory."""

    run_dir: Path

    @property
    def state_dir(self) -> Path:
        return self.run_dir / "_state"

    @property
    def cache_dir(self) -> Path:
        return self.state_dir / "cache"

    @property
    def nix_state_dir(self) -> Path:
        return self.state_dir / "state"

    @property
    def nix_log_dir(self) -> Path:
        return self.state_dir / "log"

    @property
    def nix_store_dir(self) -> Path:
        return self.state_dir / "nix-store"

    @property
    def eval_store_dir(self) -> Path:
        return self.state_dir / "eval-store"

    def commit_dir(self, commit: str) -> Path:
        return self.run_dir / commit

    @property
    def manifest_file(self) -> Path:
        return self.run_dir / MANIFEST_FILE
