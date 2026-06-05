# Upstream nix-core perf findings (2026-06-02)

Two performance findings surfaced during the eval-trace work that are NOT eval-trace-specific (they
reproduce in plain pre-eval-trace nix, the merge-base 616df9797). Drafted for upstream, but read the
status: one is ready-with-caveats, one is NOT ready (mechanism unresolved). A sub-agent drafted both;
the parent VERIFIED and corrected them — do not file without re-confirming the corrected numbers.

## Finding A — Boehm conservative-GC black-listing during large nixpkgs evals (READY, magnitude caveated)

**Verified mechanism.** Nix inits Boehm with `GC_set_all_interior_pointers(0)` (eval-gc.cc:53) +
`GC_set_no_dls(1)` (:57). Evaluating a large config (`closures.gnome`) builds a multi-GiB heap of
millions of 16-byte `Value`/`Bindings` objects whose payloads contain integers, tags, and string
lengths that numerically alias heap addresses. The conservative mark phase follows these, finds
out-of-heap/unallocated targets, and records them via `GC_add_to_black_list_normal`. On this heap that
routine is the single largest GC leaf.

**Magnitude — CAVEAT (parent correction).** The original "~54% of CPU" was measured on a HYBRID
P+E-core CPU (i9-13900K) where `perf report` emits two PMU blocks (cpu_core, cpu_atom) and the GC mark
thread is pinned to an E-core: `GC_add_to_black_list_normal` is ~48-54% of the cpu_atom (E-core) block
but only ~6% of cpu_core (P-core). **The GC is PARALLEL and largely off the wall critical path** —
cpuTime exceeds wall by ~1s on a ~4.5s eval, so the GC's WALL impact is ≈1s/4.5s ≈ ~20%, not 50%. Honest
framing for upstream: "Boehm conservative-GC mark (dominated by black-listing) is the largest CPU
consumer during large-nixpkgs eval and adds ~20% wall on closures.gnome; on a homogeneous CPU it
approaches ~half of total CPU." Re-measure on a NON-hybrid CPU before quoting a single percentage.

**Reproduce:** `perf record -g --call-graph dwarf -F 2000 -- nix eval -f nixos/release.nix
closures.gnome --json`; `perf report -g none --no-children --sort=symbol`. On a hybrid CPU, inspect the
per-PMU blocks separately (`--pmu`), don't sum them.

**Known/novel.** Boehm overhead + replacing it is tracked: NixOS/nix#14088 (Managed Heap — open),
#8621 (eval memory — open), #10879 (GC unmap memory — merged). The SPECIFIC quantification of
`GC_add_to_black_list_normal` dominance is not in any of them. Mitigation knobs: `GC_INITIAL_HEAP_SIZE`
(documented), `GC_FREE_SPACE_DIVISOR` (Boehm).

## Finding B — flake `git+file` eval does ~1.58M `newfstatat` per eval = ~10 redundant `git status` worktree scans (PINNED + PROTOTYPE-MEASURED: 9.95× fewer stats, ~9× WALL)

**Symptom (verified, reproducible).** `nix eval 'git+file:///path/nixpkgs?rev=X#lib.version'
--extra-experimental-features 'nix-command flakes'` does ~1.58M `newfstatat` + 716K `getdents64` + 359K
`openat` PER EVAL — attr-independent (lib.version vs hello identical), and IDENTICAL with/without
`--no-eval-trace` (so nix-core, not eval-trace). The `-f /abs/nixpkgs` path does ~few-K. Re-measured
1.58M on cold-isolated, warm-isolated (2nd eval same XDG), AND warm-SHARED `~/.cache` (fetcher-cache-v4
present) — it does NOT amortize.

**Sub-agent's "cold NAR walk" mechanism was REFUTED** (`sourcePathToHash` cache HITS per `--debug`,
yet the 1.58M happens anyway). The PINNED mechanism (stat-source profile,
`perf record -e syscalls:sys_enter_newfstatat -g`, the technique that cracked the git-identity scan):

**~99.99% of the 1.58M `newfstatat` are `nix::GitRepoImpl::getWorkdirInfo()` → libgit2
`git_status_foreach_ext` — the SAME worktree scan as the git-identity scan, but called ~10× per flake
eval from the FETCHER.** The chain: `GitInputScheme::getSourcePath` (git.cc:548) → `getRepoInfo`
(git.cc:696, NOT memoized) → `getWorkdirInfo()` (git.cc:795, the UNCACHED variant). `getSourcePath`/
`getRepoInfo` are invoked many times per flake lock+eval — the profile shows distinct chains through
`flake::lockFlake` (30.9%, incl. via `getSourcePath` 23.2% + `flake::shouldPreserveLiveEvaluationRoot`
7.7%), two more `getSourcePath`→`getRepoInfo` chains (20.4% + 15.5%), and others — summing to the full
1.58M ≈ ~10 × ~159K (one `git status` of the 50,931-file `~/nixpkgs` worktree). nix-CORE (fetcher);
`--no-eval-trace` is identical (1,585,237).

**Why it's uncached is INTENTIONAL (don't propose the naive fix).** git.cc:790-793 comment: "Do not use
path-only cached workdir info here: fetcher resolution needs current tracked dirty state, and stale
workdir metadata silently stabilizes fetchGit/fetchTree across staged or removed tracked files." So
`getCachedWorkdirInfo` (the existing per-process path-only cache, git-utils.cc:1534) is correctly
avoided — it can't see dirty-state changes.

**Sound fix:** memoize `getRepoInfo` per `Input` WITHIN a single fetch/lock/eval (the worktree is
stable there — collapses the ~10 calls to 1), OR token-cache `getWorkdirInfo` keyed on
`HEAD oid + .git/index mtime+size` so staged/removed-file changes correctly invalidate — **this is
exactly the freshness-token cache eval-trace's #2 git-identity cache uses** (dep-hash-fns.cc
`gitCleanToken`). The redundant-call angle (the ~10 calls) is the simplest: getRepoInfo's result is
deterministic for a given input within one eval; memoizing it (with the existing dirty-state caveat
preserved) eliminates 9 of 10 scans → ~1.58M → ~159K.

**PROTOTYPE MEASURED (2026-06-02).** A token-cache of `getWorkdirInfo` (env `NIX_GIT_WORKDIR_CACHE=1`,
default-off, `git.cc` getRepoInfo) keyed on `HEAD oid + .git/index mtime+size`, A/B on a warm
`git+file://~/nixpkgs?rev=HEAD#lib.version`:

| | newfstatat | wall (warm) |
|---|---:|---:|
| baseline (gate off) | 1,585,265 | ~6.5s (5.8–6.8) |
| cached (gate on) | 159,327 | **0.71s** |
| ratio | **9.95×** | **~9×** |

Output byte-identical. The WALL win exceeds the stat ratio because each `git status` is ~0.6s of real work
(stat + getdents + openat + libgit2 diff), so ~9 redundant scans ≈ 5.8s of the 6.5s warm wall — **a trivial
flake eval spends ~90% of its wall re-scanning the worktree**. Soundness: within one eval the ~10 calls share
a token (worktree stable mid-eval) → trivially sound; cross-eval reuse invalidates on HEAD/staged/removed
changes (the documented concern), residual = unstaged-content-edit-without-index-change (same as eval-trace #2).
The fully-sound production form is a within-eval memoization (no token, no cross-eval residual). This measured
9× wall is the headline number for the upstream report.

**SOUND FIX IMPLEMENTED (2026-06-02) — even simpler than memoization: no cache at all.** `getSourcePath` does
`getRepoInfo(input).getPath()`, and `getPath()` (git.cc:651) reads ONLY `repoInfo.location` — it NEVER touches
`workdirInfo` (the `isDirty` check is in a separate `warnDirty()` that `getSourcePath` never calls). So
`getSourcePath` does not need the O(worktree) `git_status` scan at all. Added a `needWorkdirInfo` param to
`getRepoInfo` (default true); `getSourcePath` passes false → skips the scan. Since `getSourcePath` drives ~all
the ~10 redundant scans, this DEFAULT-ON, cache-free, provably-sound change gives the full win: **1,585,265 →
158,911 newfstatat (9.97×), wall ~6.5s → 0.716s, byte-identical.** This is the recommended upstream PR (the
`getPath()`/`workdirInfo` independence makes it obviously correct, no caching or freshness-token review needed).

**Known/novel.** The store-copy lazy-trees issues (#3121, #13225 — real/open) do NOT cover this (the
store copy here is CACHED; the cost is the redundant `git status`). #9684 (libgit accessor 60%
regression) is adjacent (git-accessor cost) but not this specific redundancy. The ~10× redundant
`getRepoInfo`/`getWorkdirInfo` per flake eval appears NOVEL. (Caveat: re-confirm the call count + the
memoization fix's soundness against the git.cc:790-793 dirty-state concern before filing.)
