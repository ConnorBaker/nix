# Nix Eval Trace: Upstreaming Plan

**Target audience:** Nix core team reviewing the PR.

This document covers how to prepare the `vibe-coding/file-based-eval-cache`
branch for upstreaming: commit organization, code review checklist,
documentation checklist, and verification.

For terminology and prior art references, see [design.md](design.md) Section 2.

---

## 1. Proposed Commit Organization

The development commits should be squashed and reorganized into clean,
incremental commits. Each commit should be independently buildable.

### Commit 1: BLAKE3 + SQLite BLOB Prerequisites

**Small prerequisite changes needed by later commits.**

**Modified files:**
- `src/libutil/hash.cc` -- BLAKE3 HashSink support
- `src/libutil/include/nix/util/hash.hh` -- BLAKE3 hash type
- `src/libstore/sqlite.cc` -- BLOB binding extension
- `src/libstore/include/nix/store/sqlite.hh` -- BLOB API
- `package.nix` -- Build dependency additions

### Commit 2: DependencyTracker + Dep Types + StatHashCache

**Pure recording infrastructure. No eval trace behavior changes.**

This commit introduces the dependency tracking layer (analogous to Adapton's DDG
construction) without any eval trace changes. It can be reviewed and tested
independently.

**New files:**
- `src/libexpr/include/nix/expr/eval-trace-deps.hh` (header-only)
  - `DepType` enum (12 types), `DepKind` enum, `Dep` struct, `Blake3Hash` struct
  - `DepHashValue` variant, `DepKey` dedup key, `StructuredFormat`, `ShapeSuffix`
  - Inline helpers: `depTypeName()`, `depKind()`, `depKindName()`, `isVolatile()`,
    `isContentOverrideable()`, `buildStructuredDepKey()`, `formatStructuredDepKey()`
- `src/libexpr/include/nix/expr/dependency-tracker.hh`
  - `DependencyTracker` RAII tracker (Adapton DDG builder), `SuspendDepTracking`
  - `depHash*` functions, `recordDep`, `resolveToInput`
- `src/libexpr/dependency-tracker.cc`
  - Dep recording, BLAKE3 hashing, input resolution
  - StatHashCache (anonymous namespace): L1 concurrent_flat_map, L2 bulk-loaded
    from TraceStore, dirty tracking for flush

**Modified files:**
- `src/libexpr/eval.cc` -- `evalFile` Content dep recording
- `src/libexpr/primops.cc` -- `recordDep` calls for all builtins
- `src/libexpr/primops/fetchTree.cc` -- UnhashedFetch dep recording
- `src/libexpr/primops/fetchMercurial.cc` -- UnhashedFetch dep recording
- `src/libexpr/include/nix/expr/eval.hh` -- `EvalTraceContext` (via `traceCtx`)
- `src/libexpr/include/nix/expr/eval-trace-context.hh` -- trace context struct
- `src/libexpr/eval-trace-context.cc` -- trace context implementation
- Meson build files for new sources

### Commit 3: Fetcher Stable Identities

**`getStableIdentity()` methods for all fetcher schemes.**

Provides the `contextHash` partition key used by the trace store (BSàlC trace
store). Small and self-contained -- can be reviewed independently.

**Modified files:**
- `src/libfetchers/fetchers.cc` -- `getStableIdentity()`
- `src/libfetchers/git.cc` -- `git:<url>` identity
- `src/libfetchers/github.cc` -- `<scheme>:<host>/<owner>/<repo>` identity
- `src/libfetchers/mercurial.cc` -- `hg:<url>` identity
- `src/libfetchers/path.cc` -- `path:<path>` identity
- `src/libfetchers/tarball.cc` -- `tarball:<url>` identity
- `src/libfetchers/include/nix/fetchers/fetchers.hh` -- API

### Commit 4: TracedExpr + TraceStore + Recovery + Unit Tests

**Core eval trace: TracedExpr thunks (Adapton articulation points), pure SQLite
trace store (BSàlC constructive traces), two-strategy constructive recovery.**

This is the largest commit. It replaces the existing `AttrCursor` tree
navigation with `TracedExpr` GC-allocated thunks, adds the pure SQLite
`TraceStore` backend, and includes all C++ unit tests.

**New/rewritten files:**
- `src/libexpr/include/nix/expr/trace-cache.hh`
  - `TraceCache` class, performance counters
- `src/libexpr/trace-cache.cc`
  - `TracedExpr` (Adapton articulation point), `ExprOrigChild`,
    `SharedParentResult`
  - `eval()` dispatch: verify / fresh evaluation / constructive recovery paths
  - `navigateToReal()`, `evaluateFresh()` (Adapton demand-driven recomputation),
    `materializeResult()`
  - `recordSiblingTrace()`, `replayTrace()` (Adapton change propagation)
- `src/libexpr/include/nix/expr/trace-result.hh` (header-only)
  - `ResultKind`, `CachedResult`, all result structs
- `src/libexpr/include/nix/expr/trace-store.hh`
  - `TraceStore`: pure SQLite backend (BSàlC trace store) with 8-table schema
- `src/libexpr/trace-store.cc`
  - Schema (Strings, AttrPaths, Results, DepKeySets, Traces, CurrentTraces,
    TraceHistory, StatHashCache)
  - `verify()` (BSàlC VT check), `record()` (BSàlC CT recording),
    `recovery()` (BSàlC constructive recovery) with direct hash + structural
    variant strategies
  - `loadFullTrace()`, `loadKeySet()`, `verifyTrace()`, two-level
    StructuredContent verification, DOM caching
- `src/libexpr/include/nix/expr/trace-hash.hh`
  - `computeTraceHash`, `computeTraceStructHash`, `sortAndDedupDeps`
- `src/libexpr/trace-hash.cc`
  - HashSink-based BLAKE3 trace hashing (domain-separated fields)
- `src/libexpr/include/nix/expr/traced-data.hh` (header-only)
  - `TracedDataNode` virtual interface, `ExprTracedData` Expr subclass

**Modified files:**
- `src/libcmd/installable-attr-path.cc` -- TracedExpr root creation
- `src/libcmd/installable-flake.cc` -- Flake eval trace lifecycle
- `src/libcmd/installables.cc` -- `getRootValue()` integration
- `src/libcmd/common-eval-args.cc` -- Trace option plumbing
- `src/libflake/flake.cc` -- Input accessor mappings
- `src/nix/eval.cc` -- Value-based API
- `src/nix/search.cc` -- Value-based API
- `src/nix/flake.cc` -- Value-based API
- `src/nix/app.cc` -- Value-based API
- `src/nix/main.cc` -- Stats output
- `src/libexpr/include/nix/expr/eval-inline.hh` -- `forceValue` hook
- `src/libexpr/include/nix/expr/eval-settings.hh` -- Settings
- `src/libexpr/print.cc`, `src/libexpr/include/nix/expr/print.hh` -- Print support
- `src/libexpr/primops.cc` -- `ReadFileProvenance` in readFile, `DirDataNode`/
  `DirScalarNode` + `ExprTracedData` wrapping in readDir
- `src/libexpr/primops/fromTOML.cc` -- `TomlDataNode`, `parseTracedTOML`
- `src/libexpr/json-to-value.cc` -- `JsonDataNode`, `parseTracedJSON`,
  `ExprTracedData::eval()` vtable emission
- Various `.hh` files in `src/libcmd/include/` -- Interface changes

**Deleted:**
- `tests/functional/flakes/trace-cache.sh` (replaced by new test suites)

**Unit test files:**
- `src/libexpr-tests/eval-trace/helpers.hh`
- `src/libexpr-tests/eval-trace/helpers.cc`
- `src/libexpr-tests/eval-trace/trace-cache.cc`
- `src/libexpr-tests/eval-trace/trace-store.cc`
- `src/libexpr-tests/eval-trace/traced-data.cc`
- `src/libexpr-tests/eval-trace/trace-hash.cc`
- `src/libexpr-tests/eval-trace/dep-tracker.cc`
- `src/libexpr-tests/eval-trace/stat-hash-cache.cc`
- `src/libexpr-tests/eval-trace/dep-tracking.cc`
- `src/libexpr-tests/eval-trace/integration.cc`
- `src/libexpr-tests/eval-trace/per-sibling-invalidation.cc`

### Commit 5: Functional Tests

**Functional test suites covering all eval trace behavior.**

**New files:**
- `tests/functional/flakes/eval-trace-core.sh`
- `tests/functional/flakes/eval-trace-deps.sh`
- `tests/functional/flakes/eval-trace-output.sh`
- `tests/functional/flakes/eval-trace-recovery.sh`
- `tests/functional/flakes/eval-trace-volatile.sh`
- `tests/functional/eval-trace-impure-core.sh`
- `tests/functional/eval-trace-impure-deps.sh`
- `tests/functional/eval-trace-impure-advanced.sh`
- `tests/functional/eval-trace-impure-output.sh`
- `tests/functional/eval-trace-impure-regression.sh`

**Modified files:**
- `tests/functional/flakes/meson.build` -- Register new test suites
- `tests/functional/meson.build` -- Register new test suites
- `tests/functional/common/functions.sh` -- Shared helpers

### Commit 6: Test Fixes + GC Workaround

**Disable eval trace in GC tests and fix CA test interaction.**

**Modified files:**
- `tests/functional/gc-auto.sh` -- Disable eval trace in GC tests
- `tests/functional/ca/issue-13247.sh` -- Test fix

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

## 2. Code Organization Review

Checklist of items to consider before upstreaming:

### File Size

- [x] **trace-cache.cc**: The `TracedExpr` struct definition + support types
  (`ExprOrigChild`, `SharedParentResult`) + the `eval()` method (implementing
  BSàlC verify/record/recover dispatch) are all in one file. Acceptable --
  layout keeps everything close to usage, which aids understanding.

- [x] **eval-trace-deps.hh + dependency-tracker.hh** (formerly dep-tracker.hh):
  Split into vocabulary types (`DepType`, `Blake3Hash`, `DepHashValue`, `Dep`,
  `DepKey` in eval-trace-deps.hh) and RAII machinery (`DependencyTracker`,
  `SuspendDepTracking`, dep hash declarations in dependency-tracker.hh). The
  split eliminates the grab-bag concern. StatHashCache folded into
  dependency-tracker.cc as an anonymous namespace implementation detail.

- [x] **trace-store.cc**: `record()` (BSàlC CT recording), `verify()` (BSàlC VT
  check), `recovery()` (BSàlC constructive recovery), `loadFullTrace()`,
  `loadKeySet()`, `verifyTrace()` with two-level StructuredContent override,
  `computeCurrentHash()`, DOM caches. Well-structured with clear section
  headers. Acceptable as-is.

### Code Patterns

- [x] **primops.cc recordDep calls**: Each built-in that accesses files has a
  `recordDep(...)` call with appropriate type and hash. The calls are repetitive
  but each has type-specific logic (Content vs Directory vs Existence, NAR vs raw
  bytes, etc.). A wrapper macro would obscure the type semantics. Acceptable
  as-is.

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

- [x] **`buildAttrPath()` returns tab-separated strings**: Used internally for
  SQLite AttrPaths table lookups. The name is accurate.

- [x] **`contextHash`**: The first 8 bytes of BLAKE3 of the stable identity.
  Could be confused with `NixStringContext`. **Decision:** Keep as-is -- the name
  is accurate in the eval trace context and well-documented in `trace-store.hh`.
  Renaming to `identityHash` is an option if reviewers flag it, but the term
  "context" here refers to "evaluation context" (flake inputs, expression hash),
  not string context.

### Dead Code

- [x] Verify no remnants of intermediate designs remain (CAS blob traces,
  EvalIndexDb, AttrDb, TraceCacheStore, CBOR serialization). **Verified:** None
  of these classes or patterns exist in the current codebase.

---

## 3. Interface Documentation Checklist

Files needing documentation review before upstreaming:

- [x] **trace-cache.hh** -- `TraceCache` class with method-level docs.
  `ResultKind` is in a separate header (`trace-result.hh`). Adequate for review.

- [x] **trace-store.hh** -- `TraceStore` struct with comprehensive doxygen on
  `verify()`, `record()`, `recovery()`, `loadFullTrace()`, `loadKeySet()`, and
  `verifyTrace()`. Documents factored serialization (`serializeKeys`,
  `serializeValues`, etc.) and session caches. Ready for review.

- [x] **trace-hash.hh** -- `computeTraceHash`, `computeTraceStructHash`,
  `sortAndDedupDeps`. Domain-separated BLAKE3 hashing. Ready for review.

- [x] **eval-trace-deps.hh** -- `DepType` enum categorized into Hash Oracle,
  Reference Resolution, and Volatile types with per-value docs. `Dep` struct
  has field-level docs. `Blake3Hash` struct documented. Ready for review.

- [x] **dependency-tracker.hh** -- `DependencyTracker` (Adapton DDG builder)
  has usage guide comment. `SuspendDepTracking` documented. Dep hash function
  declarations documented. StatHashCache is internal (anonymous namespace in
  dependency-tracker.cc) and invisible to consumers. Ready for review.

- [x] **traced-data.hh** -- `TracedDataNode` virtual interface and
  `ExprTracedData` Expr subclass for lazy structural dep tracking. Documents
  the two-level verification mechanism. Ready for review.

---

## 4. Duplicate Functionality Check

Analysis of potential overlap with existing Nix code:

| Area | Status | Notes |
|------|--------|-------|
| BLAKE3 | No duplication | Uses existing `nix::Hash` / `nix::HashSink` throughout. `Blake3Hash` is a lightweight 32-byte wrapper. |
| SQLite | No duplication | Extends existing `sqlite.hh/cc` wrapper. `TraceStore` uses standard `SQLite`/`SQLiteStmt`. |
| File hashing | No duplication | Centralized in `StatHashCache` + `depHash*` functions. No overlap with store-level hashing. |
| zstd compression | No duplication | Uses existing `nix/util/compression.hh` for `keys_blob` and `values_blob` compression. |

**No new external dependencies added** (BLAKE3, SQLite, zstd are all existing
Nix dependencies).

---

## 5. Pre-Upstreaming Verification

### Functional Tests

```bash
# Individual test suites
nix develop --command bash -c "meson test -C build --suite flakes"
nix develop --command bash -c "meson test -C build --suite main"
```

### C++ Unit Tests

```bash
# All unit tests
cd build && ./src/libexpr-tests/nix-expr-tests
```

### Performance Sanity Check

```bash
# Fresh evaluation path (first eval -- Adapton demand-driven recomputation)
rm -rf ~/.cache/nix/eval-trace-v2.sqlite
time nix eval nixpkgs#hello.pname
# Expected: ~3-5s (dominated by evaluation)

# Verify path (BSàlC verifying trace check -- traced eval)
time nix eval nixpkgs#hello.pname
# Expected: < 200ms (stat validation + deserialization)
```

### Verify No Regressions

```bash
# Full test suite including non-eval-trace tests
nix develop --command bash -c "meson test -C build"
```

### ASAN Testing

```bash
meson configure build -Db_sanitize=address
meson compile -C build
ASAN_OPTIONS=detect_leaks=0 ./build/src/libexpr-tests/nix-expr-tests
meson configure build -Db_sanitize=none  # restore
```

---

## 6. Review Guidance

### Recommended Review Order

1. **`eval-trace-deps.hh`** then **`dependency-tracker.hh`** -- Start with dep
   vocabulary types, then the DependencyTracker (Adapton DDG builder). This is
   the conceptual foundation: how dependencies are dynamically discovered and
   recorded during evaluation (cf. Shake's dynamic dependencies, Adapton's DDG
   construction).

2. **`trace-result.hh`** -- Result vocabulary: `ResultKind`, `CachedResult`,
   all result struct variants. Header-only, small.

3. **`trace-cache.hh`** -- Public API surface. Understand what consumers see:
   `TraceCache` and performance counters.

4. **`trace-cache.cc`** -- Core eval loop. Focus on `TracedExpr::eval()` and
   `evaluateFresh()`. The verify / record / recover dispatch is the heart of
   the system.

5. **`trace-store.hh/cc`** -- Pure SQLite trace store backend (BSàlC trace
   store). `verify()` (verifying trace check), `record()` (trace recording),
   and `recovery()` (constructive recovery via direct hash lookup + structural
   variant scan) are the three main flows. The 8-table schema and factored
   dep storage (DepKeySets + per-trace values_blob) are documented inline.

6. **`trace-hash.hh/cc`** -- BLAKE3 trace hashing with domain-separated fields.

7. **`traced-data.hh`** + related code in `json-to-value.cc`, `primops/fromTOML.cc`,
   `primops.cc` -- Lazy structural dep tracking via `ExprTracedData` Expr
   subclass. Implements two-level verification (StructuredContent overrides
   Content/Directory failures).

8. **StatHashCache** (internal to `dependency-tracker.cc`) -- Performance
   optimization for the BSàlC VT check. Two-level cache (L1 memory + L2
   bulk-loaded from TraceStore's SQLite) reduces dep verification to `lstat()`
   calls. Dirty entries flushed back to TraceStore at session end. Anonymous
   namespace -- review as part of dependency-tracker.cc.

9. **CLI integration** (`src/libcmd/`, `src/nix/`) -- How commands use the
   trace. Focus on `installable-attr-path.cc` and `installable-flake.cc`.

10. **Fetcher changes** (`src/libfetchers/`) -- `getStableIdentity()` additions.
    Small, self-contained.

11. **Tests** -- Read the test names and comments for coverage understanding.
    The regression tests (`eval-trace-impure-regression.sh`) document specific
    bugs that were found and fixed.

### Key Design Decisions to Evaluate

1. **Pure SQLite for all trace storage (BSàlC constructive trace store
   strategy).** Everything lives in a single `eval-trace-v2.sqlite` cache file.
   Results, dep keys, dep hash values, trace history, and stat hash cache are all
   in SQLite tables. The DB is a pure cache -- deletion causes re-evaluation, not
   data loss. Is this the right trade-off vs. store-integrated CAS blobs?

2. **12 dep types (BSàlC oracle taxonomy).** Each dep type defines an oracle
   for verifying trace validity. Is this the right granularity? Are there deps
   we're missing (e.g., symlink targets) or deps that are over-specific? See
   design.md Section 4.1 for the full taxonomy.

3. **Two-strategy constructive recovery (extending BSàlC CT).** Direct hash
   recovery computes all current dep hashes and does an O(1) trace_hash lookup.
   Structural variant recovery scans TraceHistory for entries with the same dep
   key structure, loading only key sets (no values_blob decompression). Is this
   complexity justified? See design.md Section 6.3.

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

6. **StructuredContent two-level verification.** For JSON/TOML data files and
   directory listings, fine-grained `StructuredContent` deps track individual
   data paths. When a Content/Directory dep fails but all StructuredContent deps
   pass, the trace is still valid. Is this complexity worth the improved
   granularity?

7. **Per-sibling ParentContext deps.** Children record ParentContext deps only
   for siblings they actually access during evaluation, not the full parent.
   This avoids cascading invalidation when unrelated siblings change, at the cost
   of a known soundness gap for parent-mediated value changes (see design.md
   Section 9.8). Is this trade-off acceptable?

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
| `recovery()` direct hash | BSàlC constructive recovery | Mokhov et al. 2020, Def 5 |
| `recovery()` structural variant | Structural variant recovery (novel extension) | -- |
| ParentContext deps | Parent invalidation (replaces Salsa revision counter) | Matsakis et al. |
| Dynamic dep recording | Shake dynamic dependencies | Mitchell 2012, Section 3 |
| `replayTrace()` | Adapton change propagation | Hammer et al. 2014, Section 4 |
| `StatHashCache` L1/L2 | Memoized oracle (optimization of BSàlC VT hash check) | -- |
| `StructuredContent` deps | Fine-grained data file tracking (novel extension) | -- |

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
| TraceCacheStore | TraceStore | BSàlC (trace store) |
| EvalIndexDb | (merged into TraceStore) | -- |
| CAS blob traces | SQLite-stored traces | -- |
| CBOR serialization | zstd-compressed binary blobs | -- |
| epoch / Merkle chaining | ParentContext deps | -- |

---

## Appendix B: References

1. Mokhov, Mitchell, Peyton Jones. "Build Systems a la Carte: Theory and Practice."
   *Journal of Functional Programming* 30, e11, 2020.

2. Hammer, Khoo, Hicks, Foster. "Adapton: Composable Demand-Driven Incremental
   Computation." *PLDI 2014.*

3. Matsakis et al. "Salsa: A Framework for On-Demand, Incremental Computation."
   https://github.com/salsa-rs/salsa

4. Mitchell. "Shake Before Building: Replacing Make with Haskell." *ICFP 2012.*
