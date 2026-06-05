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
for the whole arc (has a 2026-06-02 status banner at the top correcting the
stale §3b framing).

**Live perf work:** `plans/eval-trace-perf-cold-and-hot.md` — the current
cold-recording + hot-verification levers. This is where perf effort goes;
the producer-partition arc is concluded/dead (see Current state below).

**Production code:** `src/libexpr/eval-trace/CLAUDE.md` — architecture
overview, type-safety patterns, current status of every BUG-N / OR-N /
RFC §3b item.

**Tests:** `src/libexpr-tests/eval-trace/CLAUDE.md` — fixture hierarchy,
test index by directory, naming conventions, deferred-work index.

## Current state (2026-06-02)

- Branch: many commits ahead of `origin/vibe-coding/file-based-eval-cache`.
- Build: `nix build -L .` produces `result/bin/nix`. Sandboxed unit tests
  pass. All functional tests pass.
- **RFC §3b / producer-partition / CA-edge / re-keying — MEASURED 2026-06-02:
  the whole arc below was closures.gnome-ONLY; at scale (python3Packages, ~66K
  traces) §3b WARM-THRASHES (never converges, 6–21× slower, sound, mechanism
  unrooted). See `doc/eval-trace/measurements/3b-at-scale-2026-06-02.md` +
  the §3b banner in `src/libexpr/eval-trace/CLAUDE.md`. The closures-derived
  verdict (and my pass-3 "no-sharing confounded" / pass-6 "recording 0.2s"
  calibrations) do NOT generalize.** Closures-only chain (still valid for
  closures): the producer-edge direction is UNPROMISING + a real COLD blocker;
  don't implement it; the "structurally dead" claim is CALIBRATED-DOWN. Chain:
  `plans/derivation-producer-partition-rfc.md` §16–§18 +
  `plans/compositional-trace-dag-implementation-sketch.md` §10/§10.7. Short
  version: the often-quoted "3.6× cold / 14× hot" is **pre-Fix-1a**; Fix 1a
  (commit `868038369`) made conservative §3b hot-neutral and proved the 14×
  was 93% a cache-defeat bug. Post-Fix-1a, the as-built (mis-keyed) edge is
  hot-negative (1.81s vs 1.09s) because it loads ~12K separate producer-trace
  blobs on warm verify that inline conservative avoids (those loads are capped
  per distinct producer, so re-keying wouldn't reduce them — projected, not
  benched). These measured non-wins (conservative no win; mis-keyed aggressive
  negative) + the cold cost make it **unpromising** — but OSPI's own re-keyed
  shape was never benched, so this is "don't invest", NOT "proven to lose."
  **§17b/§18's "97% singly-consumed → no sharing →
  structural catch-22" is CONFOUNDED** — it reads the mis-keyed gate's 306 fires
  as fan-out, contradicting §11's 200–2000 estimate; the correctly-keyed shape was
  never benched (§10.7). (§3b also adds a cold cost, but the "+6.7s recording"
  figure is STALE — the perf doc re-measured store-write at 0.2s; cold is the
  always-on tax + dep-capture, not recording. Don't cite +6.7s.) Don't implement
  OSPI; don't chase a
  high-fan-out workload on the strength of the confounded reason. §3b stays
  DEFAULT-OFF as gated scaffolding; keep Fix 1a.
- **The LIVE perf direction is `plans/eval-trace-perf-cold-and-hot.md`**
  (orthogonal to producer-partition). HOT levers landed (persisted
  content-hash cache H1; HOT-1 posix tier now **DEFAULT-OFF** as of
  2026-06-04 — a real coarse-mtime stat-race soundness bug, see
  `doc/eval-trace/measurements/property-test-overinvalidation-2026-06-04.md`
  + `hot1_stat_race` memory; the H1 store-path tier is unaffected/sound;
  productionize-(b), #2 git-clean marker (default-on but its mtime token is
  precision-only, sound floor = content FileBytes), Spike-1 sync-verify).
  COLD: the split is **MEASURED** — ~57% BLAKE3
  hashing over the 607×-flattened dep vectors, only ~4% I/O. So "async
  recording" is LOW-leverage (it moves the 4%); off-thread hashing (C2) is
  blocked by pool/vocab thread-safety. H-cold-1 (single-pass key resolution)
  landed (−10%, byte-identical). The doc's named next cold lever is **C3 —
  reduce the flattened dep COUNT via record-time sub-vector dedup**
  ("unexplored; needs design"); note it is the 607× root again (the same
  thing the dead producer-edge arc attacked) and to cut *hashing* it needs
  fragment-hash composition, blocked today by the global-sort + positional
  `dep.ordinal` (the L-A blocker). Read the doc's COLD section before acting.
- Working tree is clean.

For chronological detail, see `git log --oneline master..HEAD | head -25`.

**Indexing discipline (lesson from the 2026-06-02 duplicated effort):** before
extending any arc, read its CONCLUSION, not just its setup — and check whether
that conclusion is measured or inferred (§16–§18's "structurally dead" was partly
inferred/confounded; a 3rd pass calibrated it). The arc was concluded in
`derivation-producer-partition-rfc.md` §16–§18, but the entry points (this file,
the README, the eval-trace CLAUDE.md) still pointed *toward* it — so a pass
re-derived a lever the arc had already concluded against. New findings/results
must land in the in-tree docs AND be cross-linked from the entry points (not only
in personal memory), with measured-vs-inferred marked per claim.

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
