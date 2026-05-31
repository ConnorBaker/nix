# Branch-specific notes — `vibe-coding/file-based-eval-cache`

This is the eval-trace cache work-in-progress branch. Master has none of this.

## What this branch is

A research + development branch building a **constructive trace cache** for
the Nix evaluator (BSàlC + Adapton). Eval results are recorded with their
dependencies; subsequent evals verify deps and skip re-evaluation when
nothing changed. The implementation lives under `src/libexpr/eval-trace/`,
its tests under `src/libexpr-tests/eval-trace/`, and its design + perf
research under `doc/` and `plans/`.

## Where to start

**Entry point:** `doc/eval-trace-cache-README.md` — the consolidated index
for the whole arc. Has a "Picking this up — start here" section at the top
covering the current §3b state, bench reproduction, and a decision tree
for what to do next.

**Production code:** `src/libexpr/eval-trace/CLAUDE.md` — architecture
overview, type-safety patterns, current status of every BUG-N / OR-N /
RFC §3b item.

**Tests:** `src/libexpr-tests/eval-trace/CLAUDE.md` — fixture hierarchy,
test index by directory, naming conventions, deferred-work index.

## Current state (2026-05-31)

- Branch: ~17 commits ahead of `origin/vibe-coding/file-based-eval-cache`.
- Build: `nix build -L .` produces `result/bin/nix`. Sandboxed unit tests
  pass (1844/1847; 3 documented skips). All functional tests pass.
- RFC §3b (CA producer-trace boundary) is **implemented + benchmarked +
  DEFAULT-OFF** behind `NIX_ENABLE_CA_PRODUCER=1`. Benchmark on the
  Ledger-D anchor (`closures.gnome`) measured a 3.6× cold / 14× hot
  regression — soundness PASS but cost ≫ benefit on this workload.
  Default-off was the right call. Infrastructure stays in tree as
  scaffolding.
- Working tree is clean.

For chronological detail, see `git log --oneline master..HEAD | head -25`.

## Conventions

- **Build the release binary:** `nix build -L .`. The harness measures
  `./result/bin/nix` via `resolve(strict=True)`; without `result/`,
  `eval-trace-bench generate` hard-fails.
- **Incremental dev builds:** use `nix develop .#native-clangStdenv`
  (clang stdenv, sanitizer-friendly) per `src/libexpr/eval-trace/CLAUDE.md`
  "Incremental Build" section. The default `nix develop` shell hits a
  PCH/`_MSVC_LANG` toolchain mismatch.
- **Bench:** `NIX_CONFIG="builders ="` suppresses remote-builder
  SSH-timeout noise. Use `--num-commits 100 --run-number N` for a
  Ledger-D-comparable run; clear `eval-trace-bench-results/{cold,hot,
  reference}/N` first if reusing a number.
- **Functional tests:** `tests/functional/CLAUDE.md` documents the
  shell-based suite. The `eval-trace-*` tests are the §3b-relevant ones.
- **Personal memory:** if you're using Claude Code with auto-memory, the
  session-spanning notes live in `~/.claude/projects/.../memory/`.
  Without that, the in-tree docs above are self-contained.
