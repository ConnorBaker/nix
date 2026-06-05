# H3 scoping — "cut the 128K-dep walk" (2026-06-01)

Scoping for the #1 hot lever from EXPERIMENT 1 (`eval-trace-perf-cold-and-hot.md`). Not an
implementation — a grounded map of sub-levers, payoff, soundness, effort, and the analysis still
owed before committing. Repo-durable.

## What the walk actually costs (measured, not assumed)
Warm `-f closures.gnome`: wall 0.94s = user 0.45 + sys 0.40. The verify walk is most of it.
- `verify.depsChecked = 128,028` deps iterated; but the L1 (`currentDepHashes_`) dedups the
  607× flattened duplicates → only **~23K DISTINCT computes** (`cacheMisses=23,192`;
  `cacheHits=42,423` are repeat keys; ~63K deps classify Done/are StorePath-batched).
- `sys` 0.40s = **177,290 `newfstatat`** (+ **35,931 ENOENT**) — the single biggest cost, and it
  parallelizes only **~4x** (VFS/dentry plateau, spike 2). `resolve()` is a pure map lookup
  (semantic-registry.cc:8 — `entries_.find`), so the stats are DOWNSTREAM I/O on the ~23K
  computes: path canonicalization (lstat per path component), `readFile` (open/fstat), and
  `StorePathAvail` `queryValidPaths`. ⇒ **the iteration of 128K is NOT the bottleneck; the
  per-distinct-dep STAT cost is** (~7.7 stats/compute).
- Root cause of the 128K (architecture-trace-model-vs-CA.md): `replayMemoizedRange`
  (dep-recording-context.hh:307) copies each child's full dep range into EVERY ancestor scope;
  edges (`TraceValueContext`/`ParentSlot`) exist but are ~0% used. ~95% of stored deps are leaf
  observations. A trace stores its flattened transitive leaf closure, not edges.

## Three sub-levers

### H3-arch — compositional trace DAG (the ROOT fix)
Edge to child traces by hash instead of flattening (mirror the build layer: `Derivation::inputDrvs`
keyed by input drvPath + memoized `drvHashes` — shared stdenv hashed once, referenced N times).
- **Payoff: biggest.** Cuts the DISTINCT dep count (60K distinct vs 36.5M flattened) → fewer cold
  records, fewer hot computes/stats, smaller storage (343MB values_blob), AND unlocks
  "child subtree unchanged → skip it" (the short-circuit the flattened model can't express).
  Helps cold AND hot AND storage.
- **Effort: RFC-scale.** Design exists: `compositional-trace-dag-design.md`. It is the
  content-addressed-trace-node model the §3b RFC (DEFAULT-OFF) was a slice of.
- **Soundness prereq:** keyset-provenance — a node observes a child's SHAPE (`#keys` /
  negative-membership), not just its value; an edge that drops the keyset observation reproduces
  the ParentSlot stale-serve (OR-4). Floor already scaffolded: `store/keyset-escape.cc`.
- **Known blocker (producer-partition arc + Lever-2 spike):** `TracedExpr` identity is
  attr-path-tree-shaped; sub-results reached by function application/`import` have no
  edge-referenceable identity (`makeChild` can't construct one). So H3-arch needs a SECOND,
  content-addressed `TracedExpr` identity threaded through thunk creation — foundational, not a tweak.

### H3-cert — trace-level validity certificate (SKIP the walk)
One fingerprint that, unchanged, certifies the whole subtree without walking its deps.
- **Soundness: REFUTED in naive forms.** A fingerprint can't be re-derived without reading the
  inputs it summarizes — and reading them IS the walk. README lever-3: every sound form refuted on
  this workload (runs 909/980/987/1032/1042/1133/1107/1108). Only un-refuted shape: a cheap
  per-current-node eligibility BIT/index — an optimization of the walk, not a skip.
- **Payoff: highest ceiling (instant hot) IF a sound form existed.** Effort: design-blocked.

### H3-stat — cheaper per-dep I/O — ⚠️ REFUTED 2026-06-01 by the stat-source profile
**The premise was WRONG.** `perf record -e syscalls:sys_enter_newfstatat -g` (the analysis this
doc called for) shows the 177K stats are **89% libgit2 `git status`** (`getOrCreateTraceCache ->
observeGitIdentity -> computeGitIdentityHash -> GitRepoImpl::getWorkdirInfo`), and only **~5–8%**
the verify dep-walk. The verify path does NOT do 177K stats — it does ~9K. So "cache verify
canonicalization" optimizes a non-cost. **The real #1 hot-stat lever is G1 (the git-identity
worktree scan), NOT verify** — see `eval-trace-perf-cold-and-hot.md` → "STAT-SOURCE PROFILE
(2026-06-01)". Original (refuted) text retained below for the audit trail.

#### (refuted) H3-stat — cheaper per-dep I/O
Attack the 177K stats directly, no architectural change.
- **Hypothesis:** path canonicalization re-`lstat`s shared parent prefixes (`nixpkgs/pkgs/...`)
  across the ~23K distinct deps → massive redundant lstats. A per-process resolved-path /
  canonicalization cache (keyed on CanonPath) collapses them. The 35,931 ENOENT (failed stats) —
  pin the source (store-path-availability misses? symlink target probes?) and cache/avoid.
- **Payoff:** targets `sys` 0.40s directly. Bounded by what fraction of stats is redundant
  (unknown until the stat-source breakdown). Plausibly the cheapest real hot win.
- **Effort:** moderate. **Soundness:** a resolved-path cache keyed on immutable identity is sound;
  canonicalization is a pure function (same path → same canonical path) — no freshness concern.
- **Interaction with spike 2:** stats are the ~4x-parallel-capped bottleneck; CUTTING the stat
  count (H3-stat) beats PARALLELIZING it (~4x then plateau). So H3-stat > parallel-verify for the
  sys cost.

## Analysis owed BEFORE committing to H3
1. **Pin the 177K-stat source** — `perf record -e syscalls:sys_enter_newfstatat -g` (or
   `ltrace`/strace with `-k` stacks) to attribute stats to canonicalization vs readFile vs
   StorePathAvail vs the 35K ENOENT. This single measurement decides whether **H3-stat** is a real
   cheap win (lots of redundant canonicalization lstats) or whether stats are irreducible without
   **H3-arch** (genuinely 23K distinct files × unavoidable opens).
2. For H3-arch: the keyset-provenance RFC + the content-addressed-`TracedExpr`-identity design (the
   Lever-2 blocker). Prereq is large; scope separately if H3-stat doesn't suffice.
3. Confirm the cold side: the same 177K-stat / flattening cost is paid at RECORD time too (cold
   6.7s); H3-arch helps both, H3-stat helps both, so score against cold as well as hot.

## Recommendation
Pin the stat source (#1) FIRST — it's one measurement and it forks the decision:
- redundant-canonicalization-heavy ⇒ **H3-stat** is the cheap, sound, near-term hot+cold win.
- irreducible-distinct-file-I/O ⇒ only **H3-arch** (compositional DAG) moves it — RFC-scale, the
  fundamental fix, also the cold + storage win; **H3-cert** stays design-blocked.
Spike-1 (sync verify) and a future parallel-verify are orthogonal execution-model levers; H3
reduces the WORK, which is more fundamental than redistributing it.

## RETRACTION (2026-06-02) — H3 is a COLD lever, not a hot one
The hot cost of `-f /git` was the **git-identity worktree scan (G1)**, now eliminated by the #2 cache
(warm 0.94→0.35s; see `eval-trace-perf-cold-and-hot.md` ADVERSARIAL PASS). The dep walk this doc scopes
is only ~5% of the hot stats; its real cost is BLAKE3/decode, a small fraction of the residual 0.35s
hot. So **H3's HOT payoff is small and this doc's stat-based hot framing is retracted.** H3-arch (the
compositional DAG) stays valuable but its payoff is **COLD recording** (~6.7s — the flattening re-hashed
at record time) + STORAGE, not hot. Re-scope H3 as a cold lever.
