# Nix Eval Trace: Upstreaming Plan

**Target audience:** Nix core team reviewing the PR.

This document covers how to prepare the `vibe-coding/file-based-cas-trace-cache`
branch for upstreaming: commit cleanup, code organization review, documentation
checklist, and verification.

For terminology and prior art references, see [design.md](design.md) Section 2.

---

## 1. Branch Overview

**7 commits, 82 files changed, +14,055/-998 lines.**

The 25 original development commits have been squashed and reorganized into
7 clean, incremental commits. Each commit is independently buildable.

### Commits (oldest to newest)

```
5453c6274 BLAKE3 HashSink support and SQLite BLOB binding
2d1673b1c DependencyTracker, dep types, and StatHashCache
5365b9e0f Add getStableIdentity() to all fetcher input schemes
2d198c5cf TracedExpr thunks, CAS blob traces, and three-phase recovery
fe2b6b8e4 Eval trace functional tests
de8e0dfb7 Disable eval trace in GC and CA tests
bb1642e07 Eval trace design, implementation, and upstreaming documentation
```

---

## 2. Commit Cleanup Strategy

The original 25 development commits have been squashed and reorganized into
7 clean, incremental commits. Each commit is independently buildable and
testable.

### Commit 1: BLAKE3 + SQLite BLOB Prerequisites

**Small prerequisite changes needed by later commits.**

**Modified files:**
- `src/libutil/hash.cc` -- BLAKE3 HashSink support (+9)
- `src/libutil/include/nix/util/hash.hh` -- BLAKE3 hash type (+8)
- `src/libstore/sqlite.cc` -- BLOB binding extension (+9)
- `src/libstore/include/nix/store/sqlite.hh` -- BLOB API (+2)
- `package.nix` -- Build dependency additions (+2)

**~30 lines.**

### Commit 2: DependencyTracker + Dep Types + StatHashCache

**Pure recording infrastructure. No eval trace behavior changes.**

This commit introduces the dependency tracking layer (analogous to Adapton's DDG
construction) without any eval trace changes. It can be reviewed and tested
independently.

**New files** *(reorganized in Phase 13 — see current layout below)*:
- `src/libexpr/include/nix/expr/eval-trace-deps.hh` (~250 lines)
  - `DepType` enum (11 types), `Dep` struct, `Blake3Hash` struct
  - `DepHashValue` variant, `DepKey` dedup key
- `src/libexpr/eval-trace-deps.cc` (~25 lines)
  - `depTypeName()` implementation
- `src/libexpr/include/nix/expr/dependency-tracker.hh` (~170 lines)
  - `DependencyTracker` RAII tracker (Adapton DDG builder), `SuspendDepTracking`
  - `depHash*` functions, `recordDep`, `resolveToInput`
- `src/libexpr/dependency-tracker.cc` (~450 lines)
  - Dep recording, BLAKE3 hashing, input resolution
  - StatHashCache (anonymous namespace): L1 concurrent_flat_map, L2 bulk-loaded from TraceStore, dirty tracking for flush

**Modified files:**
- `src/libexpr/eval.cc` -- `evalFile` Content dep recording (+88 lines)
- `src/libexpr/primops.cc` -- `recordDep` calls for all builtins (+184 lines)
- `src/libexpr/primops/fetchTree.cc` -- UnhashedFetch dep recording (+12)
- `src/libexpr/primops/fetchMercurial.cc` -- UnhashedFetch dep recording (+7)
- `src/libexpr/include/nix/expr/eval.hh` -- `EvalTraceContext` (via `traceCtx`) (+41)
- `src/libexpr/include/nix/expr/eval-trace-context.hh` -- trace context struct
- `src/libexpr/eval-trace-context.cc` -- trace context implementation
- Meson build files for new sources

**~1,700 lines.**

### Commit 3: Fetcher Stable Identities

**`getStableIdentity()` methods for all fetcher schemes.**

Provides the `contextHash` partition key used by the trace store (BSàlC trace
store). Small and self-contained -- can be reviewed independently.

**Modified files:**
- `src/libfetchers/fetchers.cc` -- `getStableIdentity()` (+5)
- `src/libfetchers/git.cc` -- `git:<url>` identity (+5)
- `src/libfetchers/github.cc` -- `<scheme>:<host>/<owner>/<repo>` identity (+24)
- `src/libfetchers/mercurial.cc` -- `hg:<url>` identity (+5)
- `src/libfetchers/path.cc` -- `path:<path>` identity (+5)
- `src/libfetchers/tarball.cc` -- `tarball:<url>` identity (+10)
- `src/libfetchers/include/nix/fetchers/fetchers.hh` -- API (+18)

**~72 lines.**

### Commit 4: TracedExpr + CAS Store + Recovery + Unit Tests

**Core eval trace: TracedExpr thunks (Adapton articulation points), CAS blob
traces (BSàlC constructive traces), three-phase constructive recovery.**

This is the largest commit. It replaces the existing `AttrCursor` tree navigation
with `TracedExpr` GC-allocated thunks, adds the CAS store backend with CBOR
serialization, and includes all C++ unit tests.

**New/rewritten files:**
- `src/libexpr/include/nix/expr/trace-cache.hh` (124 lines, rewrite)
  - `TraceCache` class, `ResultKind`/`CachedResult` types, performance counters
- `src/libexpr/trace-cache.cc` (888 lines, rewrite)
  - `TracedExpr` (Adapton articulation point), `ExprOrigChild`, `SharedParentResult`
  - `eval()` dispatch: verify / fresh evaluation / constructive recovery paths
  - `navigateToReal()`, `evaluateFresh()` (Adapton demand-driven recomputation),
    `materializeResult()`
  - `recordSiblingTrace()`, `replayTrace()` (Adapton change propagation)
- `src/libexpr/include/nix/expr/trace-cache-store.hh` (262 lines)
  - `TraceCacheStore`: record/verify/recover (BSàlC CT operations), trace I/O,
    deferred writes
  - Two-object trace model documentation
- `src/libexpr/trace-cache-store.cc` (603 lines)
  - `record()` (BSàlC trace recording), `verify()` (BSàlC VT check),
    `recovery()` (BSàlC constructive recovery) with direct hash + structural variant strategies
  - `storeTrace()`, `loadTrace()`, `loadDepsForTrace()`
- `src/libexpr/include/nix/expr/eval-index-db.hh` (142 lines)
  - `EvalIndexDb`: SQLite index with 3 tables
- `src/libexpr/eval-index-db.cc` (283 lines)
  - Schema creation, prepared statements, batch writes
- `src/libexpr/include/nix/expr/trace-hash.hh` (150 lines)
  - `EvalTrace` struct (BSàlC trace = key + dep hashes + result), CBOR
    serialize/deserialize, hash functions
  - `serializeTrace`/`deserializeTrace` (zstd-compressed CBOR)
- `src/libexpr/trace-hash.cc` (385 lines)
  - CBOR encoding via nlohmann::json, dep hash computation

**Modified files:**
- `src/libcmd/installable-attr-path.cc` -- TracedExpr root creation (+233)
- `src/libcmd/installable-flake.cc` -- Flake eval trace lifecycle (+145)
- `src/libcmd/installables.cc` -- `getRootValue()` integration (+66)
- `src/libcmd/common-eval-args.cc` -- Trace option plumbing (+27)
- `src/libflake/flake.cc` -- Input accessor mappings (+85)
- `src/nix/eval.cc` -- Value-based API (+28)
- `src/nix/search.cc` -- Value-based API (+55)
- `src/nix/flake.cc` -- Value-based API (+121)
- `src/nix/app.cc` -- Value-based API (+101)
- `src/nix/main.cc` -- Stats output (+10)
- `src/libexpr/include/nix/expr/eval-inline.hh` -- `forceValue` hook (+14)
- `src/libexpr/include/nix/expr/eval-settings.hh` -- Settings (+10)
- `src/libexpr/print.cc`, `src/libexpr/include/nix/expr/print.hh` -- Print support (+2)
- Various `.hh` files in `src/libcmd/include/` -- Interface changes

**Deleted:**
- `tests/functional/flakes/trace-cache.sh` (replaced by new test suites)

**Unit test files (9 files):**
- `src/libexpr-tests/eval-trace/helpers.hh` (174 lines)
- `src/libexpr-tests/eval-trace/helpers.cc` (64 lines)
- `src/libexpr-tests/eval-trace/trace-cache.cc` (459 lines)
- `src/libexpr-tests/eval-trace/trace-store.cc` (1,478 lines)
- `src/libexpr-tests/eval-trace/trace-hash.cc` (170 lines)
- `src/libexpr-tests/eval-trace/dep-tracker.cc` (456 lines)
- `src/libexpr-tests/eval-trace/stat-hash-cache.cc` (186 lines)
- `src/libexpr-tests/eval-trace/dep-tracking.cc` (790 lines)
- `src/libexpr-tests/eval-trace/integration.cc` (371 lines)

**~4,150 lines.**

### Commit 5: Functional Tests

**10 functional test suites covering all eval trace behavior.**

**New files:**
- `tests/functional/flakes/eval-trace-core.sh` (427 lines)
- `tests/functional/flakes/eval-trace-deps.sh` (286 lines)
- `tests/functional/flakes/eval-trace-output.sh` (192 lines)
- `tests/functional/flakes/eval-trace-recovery.sh` (157 lines)
- `tests/functional/flakes/eval-trace-volatile.sh` (129 lines)
- `tests/functional/eval-trace-impure-core.sh` (200 lines)
- `tests/functional/eval-trace-impure-deps.sh` (422 lines)
- `tests/functional/eval-trace-impure-advanced.sh` (343 lines)
- `tests/functional/eval-trace-impure-output.sh` (351 lines)
- `tests/functional/eval-trace-impure-regression.sh` (273 lines)

**Modified files:**
- `tests/functional/flakes/meson.build` -- Register new test suites (+6)
- `tests/functional/meson.build` -- Register new test suites (+5)
- `tests/functional/common/functions.sh` -- Shared helpers (+5)

**~2,800 lines.**

### Commit 6: Test Fixes + GC Workaround

**Disable eval trace in GC tests and fix CA test interaction.**

**Modified files:**
- `tests/functional/gc-auto.sh` -- Disable eval trace in GC tests (+11)
- `tests/functional/ca/issue-13247.sh` -- Test fix (+26)

**~37 lines.**

### Commit 7: Documentation

**Design, implementation, and upstreaming documentation.**

**New files:**
- `doc/eval-trace/design.md` -- Architecture, prior art, and design decisions
- `doc/eval-trace/implementation.md` -- Implementation phases and details
- `doc/eval-trace/upstreaming.md` -- This document

### Key Dependency Constraints

- **Commit 1 before 2:** DependencyTracker uses BLAKE3 HashSink
- **Commit 2 before 4:** TracedExpr depends on DependencyTracker (Adapton DDG)
- **Commit 3 before 4:** TraceCache uses `getStableIdentity()` for contextHash
- **Commit 4 before 5:** Functional tests exercise the eval trace
- **All unit tests in Commit 4:** `helpers.hh` includes the rewritten `trace-cache.hh`

---

## 3. Code Organization Review

Checklist of items to consider before upstreaming:

### File Size

- [x] **trace-cache.cc (888 lines)**: The `TracedExpr` struct definition (~130
  lines) + support types (`ExprOrigChild`, `SharedParentResult`, ~70 lines) + the
  `eval()` method (~200 lines, implementing BSàlC verify/record/recover dispatch)
  are all in one file. Acceptable -- layout keeps everything close to usage, which
  aids understanding.

- [x] **eval-trace-deps.hh + dependency-tracker.hh** (formerly dep-tracker.hh):
  Split into vocabulary types (`DepType`, `Blake3Hash`, `DepHashValue`, `Dep`,
  `DepKey` in eval-trace-deps.hh) and RAII machinery (`DependencyTracker`,
  `SuspendDepTracking`, dep hash declarations in dependency-tracker.hh). The
  split eliminates the grab-bag concern. StatHashCache folded into
  dependency-tracker.cc as an anonymous namespace implementation detail.

- [x] **trace-cache-store.cc (603 lines)**: `record()` (~100 lines, BSàlC CT
  recording), `verify()` (~60 lines, BSàlC VT check), `recovery()` (~150 lines,
  BSàlC constructive recovery), trace I/O (~100 lines),
  `storeTrace()`/`loadTrace()` (~80 lines). Well-structured with clear section
  headers. Acceptable as-is.

### Code Patterns

- [x] **primops.cc recordDep calls (184 lines)**: Each built-in that accesses
  files has a `recordDep(...)` call with appropriate type and hash. The calls
  are repetitive but each has type-specific logic (Content vs Directory vs
  Existence, NAR vs raw bytes, etc.). A wrapper macro would obscure the type
  semantics. Acceptable as-is.

- [x] **recordSiblingTrace (trace-cache.cc)**: Large method that serializes
  values and defers writes. The value serialization switch is similar to
  `evaluateFresh`, but the two sites have different error handling and child
  wrapping. Duplication is justified.

- [x] **TracedExpr child wrapping**: Three sites create TracedExpr children
  (Adapton articulation points): `materializeResult()`, `recordSiblingTrace()`,
  and `evaluateFresh()`. Each has subtly different wrapping logic (origExpr vs
  non-origExpr, deferred vs immediate). Consolidation risks losing these
  distinctions. Acceptable as-is.

### Naming

- [x] **`storeAttrPath()` returns null-byte-separated strings**: The name could
  be confused with "store path" (Nix store). However, it is only used internally
  by the store backend for index lookups. **Decision:** Keep as-is -- renaming to
  `serializedAttrPath()` or `indexAttrPath()` would be clearer but the function
  is internal and documented. Low priority for upstream review.

- [x] **`tracePath`**: Good name -- clearly identifies the content-addressed
  trace blob (BSàlC constructive trace) in the store. No change needed.

- [x] **`contextHash`**: The first 8 bytes of BLAKE3 of the stable identity.
  Could be confused with `NixStringContext`. **Decision:** Keep as-is -- the name
  is accurate in the eval trace context and well-documented in
  `trace-cache-store.hh`. Renaming to `identityHash` is an option if reviewers
  flag it, but the term "context" here refers to "evaluation context" (flake
  inputs, expression hash), not string context.

### Dead Code

- [x] Verify no remnants of the intermediate SQLite-based AttrDb design remain
  (all removed in the CAS blob trace commit). **Verified:** No `class AttrDb`
  references in any source files.

- [x] Verify `trace-store.hh/cc` files are fully removed (they were part of
  the intermediate design). **Verified:** Files do not exist.
  `src/libstore/builtins/eval.cc` (marker builders) also confirmed removed.

---

## 4. Interface Documentation Checklist

Files needing documentation review before upstreaming:

- [x] **trace-cache.hh** -- `TraceCache` class has `///@file` marker, method-level
  docs on `getOrEvaluateRoot()` and `getRootValue()`, field docs on
  `storeBackend` and `inputAccessors`. `ResultKind` enum could use per-value docs
  but is self-descriptive. Adequate for review.

- [x] **trace-cache-store.hh** -- Comprehensive doxygen on every public method.
  Module-level comment describes the two-object trace model (BSàlC constructive
  trace structure). `DeferredColdStore` and `TraceCacheStore` both have detailed
  class-level docs. Ready for review.

- [x] **eval-index-db.hh** -- Module-level comment explains index role and how
  losses affect recovery (BSàlC constructive trace recovery degrades to fresh
  evaluation). All public methods documented. `IndexEntry` and `StructGroup`
  structs documented. Ready for review.

- [x] **trace-hash.hh** -- `EvalTrace` struct (BSàlC trace = key + dep hashes +
  result) well-documented with field explanations. Serialization functions have
  format descriptions. `serializeTrace`/`deserializeTrace` document the
  zstd-compressed CBOR format. Ready for review.

- [x] **eval-trace-deps.hh** -- `DepType` enum categorized into Hash Oracle,
  Reference Resolution, and Volatile types with per-value docs. `Dep` struct
  has field-level docs. `Blake3Hash` struct documented. Ready for review.

- [x] **dependency-tracker.hh** -- `DependencyTracker` (Adapton DDG builder)
  has usage guide comment. `SuspendDepTracking` documented. Dep hash function
  declarations documented. StatHashCache is internal (anonymous namespace in
  dependency-tracker.cc) and invisible to consumers. Ready for review.

---

## 5. Duplicate Functionality Check

Analysis of potential overlap with existing Nix code:

| Area | Status | Notes |
|------|--------|-------|
| CBOR | No duplication | Centralized in `trace-hash.cc` via `nlohmann::json::to_cbor`. No other CBOR usage in Nix. |
| BLAKE3 | No duplication | Uses existing `nix::Hash` / `nix::HashSink` throughout. `Blake3Hash` is a lightweight 32-byte wrapper. |
| SQLite | No duplication | Extends existing `sqlite.hh/cc` wrapper. `EvalIndexDb` uses standard `SQLite`/`SQLiteStmt`. |
| File hashing | No duplication | Centralized in `StatHashCache` + `depHash*` functions. No overlap with store-level hashing. |
| Store operations | No duplication | Uses standard `Store::addTextToStore()` for CAS blobs (constructive trace storage). |
| JSON/CBOR library | Existing dependency | `nlohmann/json` is already used by libexpr (no new dependency). |

**No new external dependencies added.**

---

## 6. Pre-Upstreaming Verification

### Functional Tests

```bash
# All functional tests (expects 199 OK, 8 skipped, 0 failures)
nix build -L .#nix-functional-tests

# Individual test suites
nix develop --command bash -c "meson test -C build --suite flakes"
nix develop --command bash -c "meson test -C build --suite main"
nix develop --command bash -c "meson test -C build --suite ca"
```

### C++ Unit Tests

```bash
# All unit tests (expects 677 tests, 0 failures)
nix develop --command bash -c "meson test -C build --suite main"

# Eval trace specific
nix develop --command bash -c "meson test -C build trace-cache"
```

### Performance Sanity Check

```bash
# Fresh evaluation path (first eval -- Adapton demand-driven recomputation)
rm -rf ~/.cache/nix/eval-trace-v1.sqlite
time nix eval nixpkgs#hello.pname
# Expected: ~3-5s (dominated by evaluation + store I/O)

# Verify path (BSàlC verifying trace check -- traced eval)
time nix eval nixpkgs#hello.pname
# Expected: < 200ms (stat validation + deserialization)
```

### Verify No Regressions

```bash
# Full test suite including non-eval-trace tests
nix develop --command bash -c "meson test -C build"

# CA tests (sensitive to store operations)
nix develop --command bash -c "meson test -C build --suite ca"
```

---

## 7. Summary of Changes by Category

| Category | Files | Lines | Description |
|----------|------:|------:|-------------|
| Core eval trace | 12 | ~3,870 | TracedExpr (Adapton articulation points), trace store (BSàlC CT), index, serialization |
| Dep tracking | 4 | ~1,033 | DependencyTracker (Adapton DDG builder), StatHashCache |
| CLI integration | 15 | ~970 | installables, commands, settings, fetcher IDs |
| C++ unit tests | 10 | ~4,508 | Component and integration tests |
| Functional tests | 10 | ~2,780 | End-to-end trace verification and recovery tests |
| Build system | 7 | ~37 | Meson build file updates |
| Test fixes | 3 | ~42 | gc-auto.sh, CA test, common functions |
| Documentation | 3 | ~1,811 | Design (with prior art), implementation, upstreaming docs |
| **Total** | **82 files** | **~13,022 net** | |

---

## 8. Review Guidance

### Recommended Review Order

1. **`eval-trace-deps.hh`** then **`dependency-tracker.hh`** -- Start with dep
   vocabulary types, then the DependencyTracker (Adapton DDG builder). This is
   the conceptual foundation: how dependencies are dynamically discovered and
   recorded during evaluation (cf. Shake's dynamic dependencies, Adapton's DDG
   construction).

2. **`trace-cache.hh`** -- Public API surface (124 lines). Understand what
   consumers see: `TraceCache`, `ResultKind`, `CachedResult`.

3. **`trace-cache.cc`** -- Core eval loop. Focus on `TracedExpr::eval()` and
   `evaluateFresh()`. The three-way dispatch -- verify (BSàlC VT) / record
   (BSàlC CT) / recover (BSàlC constructive recovery) -- is the heart of the
   system.

4. **`trace-cache-store.hh/cc`** -- Trace store backend (BSàlC trace store).
   `record()` (trace recording) and `verify()` (verifying trace check) are the
   two main flows. `recovery()` implements constructive recovery via direct hash
   lookup and structural variant scan.

5. **`trace-hash.hh/cc`** -- CBOR format. Straightforward serialization of
   `EvalTrace` structs (BSàlC trace = key + dep hashes + result).

6. **`eval-index-db.hh/cc`** -- SQLite index. Simple CRUD with 3 tables.
   Supports the constructive recovery phases by mapping trace hashes and
   structural signatures to historical trace entries.

7. **StatHashCache** (internal to `dependency-tracker.cc`) -- Performance
   optimization for the BSàlC VT check. Two-level cache (L1 memory + L2
   bulk-loaded from TraceStore's SQLite) reduces dep verification to `lstat()`
   calls. Dirty entries flushed back to TraceStore at session end. Anonymous
   namespace — review as part of dependency-tracker.cc.

8. **CLI integration** (`src/libcmd/`, `src/nix/`) -- How commands use the
   trace. Focus on `installable-attr-path.cc` and `installable-flake.cc`.

9. **Fetcher changes** (`src/libfetchers/`) -- `getStableIdentity()` additions.
   Small, self-contained.

10. **Tests** -- Read the test names and comments for coverage understanding.
    The regression tests (`eval-trace-impure-regression.sh`) document specific
    bugs that were found and fixed.

### Key Design Decisions to Evaluate

1. **CAS blobs vs SQLite for result storage (BSàlC constructive trace store
   strategy).** The design trades SQLite's query flexibility for store-native GC
   integration and immutability. A BSàlC constructive trace needs to store full
   results; we store them as CAS blobs in the Nix store, gaining GC integration
   for free. Is this the right trade-off for the Nix ecosystem?

2. **11 dep types (BSàlC oracle taxonomy).** Each dep type defines an oracle
   for verifying trace validity. Is this the right granularity? Are there deps
   we're missing (e.g., symlink targets) or deps that are over-specific? See
   design.md Section 4.1 for the full taxonomy.

3. **Two-phase constructive recovery (extending BSàlC CT).** Phase 1 (direct
   hash lookup, with optional Salsa-style parent Merkle chaining for versioned
   queries) provides O(1) recovery. Phase 3 (structural variant scan) handles
   dynamic dep instability (cf. Shake's dynamic dependencies) at the cost of
   O(V) scanning. Is this complexity justified? See design.md Section 6.3.

4. **TracedExpr as Expr subclass (Adapton articulation point strategy).** The
   GC-allocated thunk approach requires understanding of the evaluator's memory
   model. Adapton articulation points are lazy memoized computations; TracedExpr
   implements this pattern within Nix's GC-managed expression tree. Is this the
   right integration point, vs. a separate trace layer?

5. **origExpr dual-mode evaluation (demand-driven vs cached).** The distinction
   between "materialized from trace" (BSàlC CT serving a constructive result)
   and "wrapping a real thunk" (Adapton demand-driven lazy evaluation) adds
   complexity. Is there a simpler way to handle sibling tracing without the
   origExpr/ExprOrigChild indirection?

### Prior Art Mapping for Reviewers

For reviewers familiar with the incremental computation literature, here is
a quick mapping from our implementation to prior art concepts:

| Component | Prior Art | Reference |
|-----------|-----------|-----------|
| `TracedExpr::eval()` dispatch | BSàlC rebuilder: VT check then CT recovery | Mokhov et al. 2020, Section 4 |
| `DependencyTracker` RAII scope | Adapton DDG construction during `force` | Hammer et al. 2014, Section 3 |
| `SuspendDepTracking` | Adapton selective tracking (avoid "fat parent" deps) | -- |
| `record()` in TraceStore | BSàlC constructive trace recording | Mokhov et al. 2020, Definition 5 |
| `verify()` in TraceStore | BSàlC verifying trace check | Mokhov et al. 2020, Definition 3 |
| `recovery()` direct hash | BSàlC constructive recovery with Salsa-style parent Merkle chaining | Mokhov et al. 2020, Def 5; Salsa |
| `recovery()` structural variant | Structural variant recovery (novel extension) | -- |
| Epoch-based staleness | Salsa revision counter | Matsakis et al. |
| Dynamic dep recording | Shake dynamic dependencies | Mitchell 2012, Section 3 |
| `replayTrace()` | Adapton change propagation | Hammer et al. 2014, Section 4 |
| `StatHashCache` L1/L2 | Memoized oracle (optimization of BSàlC VT hash check) | -- |

---

## Appendix A: Terminology Concordance

This table maps the old development terminology (used in commit messages and
some internal comments) to the prior art terminology used throughout the design
and upstreaming documentation. Reviewers encountering older terms in code
comments can use this table to translate.

| Old Term | New Term | Prior Art Source |
|----------|----------|-----------------|
| eval cache | eval trace | BSàlC (constructive trace) |
| warm path | verify path | BSàlC (verifying trace check) |
| cold path / cold store | fresh evaluation and trace recording | BSàlC (recording), Adapton (force) |
| recovery | constructive recovery | BSàlC (constructive trace) |
| dep set | trace | BSàlC (trace = key + dep hashes + result) |
| EvalCacheDb | TraceStore | BSàlC (trace store) |
| ExprCached | TracedExpr | Adapton (articulation point) |
| FileLoadTracker | DependencyTracker | Adapton (DDG builder) |
| SuspendFileLoadTracker | SuspendDepTracking | Adapton (selective tracking) |
| evaluateCold | evaluateFresh | Adapton (demand-driven recomputation) |
| coldStore | record | BSàlC (trace recording) |
| warmPath | verify | BSàlC (verifying trace check) |
| eval_cache namespace | eval_trace namespace | -- |

---

## Appendix B: References

1. Mokhov, Mitchell, Peyton Jones. "Build Systems a la Carte: Theory and Practice."
   *Journal of Functional Programming* 30, e11, 2020.

2. Hammer, Khoo, Hicks, Foster. "Adapton: Composable Demand-Driven Incremental
   Computation." *PLDI 2014.*

3. Matsakis et al. "Salsa: A Framework for On-Demand, Incremental Computation."
   https://github.com/salsa-rs/salsa

4. Mitchell. "Shake Before Building: Replacing Make with Haskell." *ICFP 2012.*
