# Nix Eval Cache: Upstreaming Plan

**Target audience:** Nix core team reviewing the PR.

This document covers how to prepare the `vibe-coding/file-based-cas-eval-cache`
branch for upstreaming: commit cleanup, code organization review, documentation
checklist, and verification.

---

## 1. Branch Overview

**7 commits, 82 files changed, +14,055/−998 lines.**

The 25 original development commits have been squashed and reorganized into
7 clean, incremental commits. Each commit is independently buildable.

### Commits (oldest to newest)

```
5453c6274 BLAKE3 HashSink support and SQLite BLOB binding
2d1673b1c FileLoadTracker, dep types, and StatHashCache
5365b9e0f Add getStableIdentity() to all fetcher input schemes
2d198c5cf ExprCached thunks, CAS blob traces, and three-phase recovery
fe2b6b8e4 Eval cache functional tests
de8e0dfb7 Disable eval cache in GC and CA tests
bb1642e07 Eval cache design, implementation, and upstreaming documentation
```

---

## 2. Commit Cleanup Strategy

The original 25 development commits have been squashed and reorganized into
7 clean, incremental commits. Each commit is independently buildable and
testable.

### Commit 1: BLAKE3 + SQLite BLOB Prerequisites

**Small prerequisite changes needed by later commits.**

**Modified files:**
- `src/libutil/hash.cc` — BLAKE3 HashSink support (+9)
- `src/libutil/include/nix/util/hash.hh` — BLAKE3 hash type (+8)
- `src/libstore/sqlite.cc` — BLOB binding extension (+9)
- `src/libstore/include/nix/store/sqlite.hh` — BLOB API (+2)
- `package.nix` — Build dependency additions (+2)

**~30 lines.**

### Commit 2: FileLoadTracker + Dep Types + StatHashCache

**Pure recording infrastructure. No cache behavior changes.**

This commit introduces the dependency tracking layer without any eval cache
changes. It can be reviewed and tested independently.

**New files:**
- `src/libexpr/include/nix/expr/file-load-tracker.hh` (390 lines)
  - `DepType` enum (11 types), `Dep` struct, `Blake3Hash` struct
  - `DepHashValue` variant, `DepKey` dedup key
  - `FileLoadTracker` RAII tracker, `SuspendFileLoadTracker`
  - `depHash*` functions, `recordDep`, `resolveToInput`
- `src/libexpr/file-load-tracker.cc` (219 lines)
  - Dep recording, BLAKE3 hashing, input resolution
- `src/libexpr/include/nix/expr/stat-hash-cache.hh` (85 lines)
  - `StatHashCache` singleton API
- `src/libexpr/stat-hash-cache.cc` (339 lines)
  - L1 concurrent_flat_map, L2 SQLite, stat-based validation

**Modified files:**
- `src/libexpr/eval.cc` — `evalFile` Content dep recording (+88 lines)
- `src/libexpr/primops.cc` — `recordDep` calls for all builtins (+184 lines)
- `src/libexpr/primops/fetchTree.cc` — UnhashedFetch dep recording (+12)
- `src/libexpr/primops/fetchMercurial.cc` — UnhashedFetch dep recording (+7)
- `src/libexpr/include/nix/expr/eval.hh` — `mountToInput` map (+41)
- Meson build files for new sources

**~1,700 lines.**

### Commit 3: Fetcher Stable Identities

**`getStableIdentity()` methods for all fetcher schemes.**

Provides the `contextHash` partition key used by the eval cache. Small and
self-contained — can be reviewed independently.

**Modified files:**
- `src/libfetchers/fetchers.cc` — `getStableIdentity()` (+5)
- `src/libfetchers/git.cc` — `git:<url>` identity (+5)
- `src/libfetchers/github.cc` — `<scheme>:<host>/<owner>/<repo>` identity (+24)
- `src/libfetchers/mercurial.cc` — `hg:<url>` identity (+5)
- `src/libfetchers/path.cc` — `path:<path>` identity (+5)
- `src/libfetchers/tarball.cc` — `tarball:<url>` identity (+10)
- `src/libfetchers/include/nix/fetchers/fetchers.hh` — API (+18)

**~72 lines.**

### Commit 4: ExprCached + CAS Store + Recovery + Unit Tests

**Core eval cache: ExprCached thunks, CAS blob traces, three-phase recovery.**

This is the largest commit. It replaces the existing `AttrCursor` tree navigation
with `ExprCached` GC-allocated thunks, adds the CAS store backend with CBOR
serialization, and includes all C++ unit tests.

**New/rewritten files:**
- `src/libexpr/include/nix/expr/eval-cache.hh` (124 lines, rewrite)
  - `EvalCache` class, `AttrType`/`AttrValue` types, performance counters
- `src/libexpr/eval-cache.cc` (888 lines, rewrite)
  - `ExprCached`, `ExprOrigChild`, `SharedParentResult`
  - `eval()` dispatch: warm/cold/recovery paths
  - `navigateToReal()`, `evaluateCold()`, `materializeValue()`
  - `storeForcedSibling()`, `replayDepsToTracker()`
- `src/libexpr/include/nix/expr/eval-cache-store.hh` (262 lines)
  - `EvalCacheStore`: cold/warm/recovery, trace I/O, deferred writes
  - Two-object trace model documentation
- `src/libexpr/eval-cache-store.cc` (603 lines)
  - `coldStore()`, `warmPath()`, `recovery()` with 3 phases
  - `storeDepSet()`, `loadDepSet()`, `loadDepsForTrace()`
- `src/libexpr/include/nix/expr/eval-index-db.hh` (142 lines)
  - `EvalIndexDb`: SQLite index with 3 tables
- `src/libexpr/eval-index-db.cc` (283 lines)
  - Schema creation, prepared statements, batch writes
- `src/libexpr/include/nix/expr/eval-result-serialise.hh` (150 lines)
  - `EvalTrace` struct, CBOR serialize/deserialize, hash functions
  - `serializeDepSet`/`deserializeDepSet` (zstd-compressed CBOR)
- `src/libexpr/eval-result-serialise.cc` (385 lines)
  - CBOR encoding via nlohmann::json, dep hash computation

**Modified files:**
- `src/libcmd/installable-attr-path.cc` — ExprCached root creation (+233)
- `src/libcmd/installable-flake.cc` — Flake eval cache lifecycle (+145)
- `src/libcmd/installables.cc` — `getRootValue()` integration (+66)
- `src/libcmd/common-eval-args.cc` — Cache option plumbing (+27)
- `src/libflake/flake.cc` — Input accessor mappings (+85)
- `src/nix/eval.cc` — Value-based API (+28)
- `src/nix/search.cc` — Value-based API (+55)
- `src/nix/flake.cc` — Value-based API (+121)
- `src/nix/app.cc` — Value-based API (+101)
- `src/nix/main.cc` — Stats output (+10)
- `src/libexpr/include/nix/expr/eval-inline.hh` — `forceValue` hook (+14)
- `src/libexpr/include/nix/expr/eval-settings.hh` — Settings (+10)
- `src/libexpr/print.cc`, `src/libexpr/include/nix/expr/print.hh` — Print support (+2)
- Various `.hh` files in `src/libcmd/include/` — Interface changes

**Deleted:**
- `tests/functional/flakes/eval-cache.sh` (replaced by new test suites)

**Unit test files (all 10 files):**
- `src/libexpr-tests/eval-cache/helpers.hh` (172 lines)
- `src/libexpr-tests/eval-cache/helpers.cc` (64 lines)
- `src/libexpr-tests/eval-cache/eval-cache.cc` (457 lines)
- `src/libexpr-tests/eval-cache/eval-cache-store.cc` (1,123 lines)
- `src/libexpr-tests/eval-cache/eval-index-db.cc` (345 lines)
- `src/libexpr-tests/eval-cache/eval-result-serialise.cc` (492 lines)
- `src/libexpr-tests/eval-cache/file-load-tracker.cc` (456 lines)
- `src/libexpr-tests/eval-cache/stat-hash-cache.cc` (186 lines)
- `src/libexpr-tests/eval-cache/dep-tracking.cc` (789 lines)
- `src/libexpr-tests/eval-cache/integration.cc` (424 lines)

**~8,500 lines.**

### Commit 5: Functional Tests

**10 functional test suites covering all eval cache behavior.**

**New files:**
- `tests/functional/flakes/eval-cache-core.sh` (427 lines)
- `tests/functional/flakes/eval-cache-deps.sh` (286 lines)
- `tests/functional/flakes/eval-cache-output.sh` (192 lines)
- `tests/functional/flakes/eval-cache-recovery.sh` (157 lines)
- `tests/functional/flakes/eval-cache-volatile.sh` (129 lines)
- `tests/functional/eval-cache-impure-core.sh` (200 lines)
- `tests/functional/eval-cache-impure-deps.sh` (422 lines)
- `tests/functional/eval-cache-impure-advanced.sh` (343 lines)
- `tests/functional/eval-cache-impure-output.sh` (351 lines)
- `tests/functional/eval-cache-impure-regression.sh` (273 lines)

**Modified files:**
- `tests/functional/flakes/meson.build` — Register new test suites (+6)
- `tests/functional/meson.build` — Register new test suites (+5)
- `tests/functional/common/functions.sh` — Shared helpers (+5)

**~2,800 lines.**

### Commit 6: Test Fixes + GC Workaround

**Disable eval cache in GC tests and fix CA test interaction.**

**Modified files:**
- `tests/functional/gc-auto.sh` — Disable eval cache in GC tests (+11)
- `tests/functional/ca/issue-13247.sh` — Test fix (+26)

**~37 lines.**

### Commit 7: Documentation

**Design, implementation, and upstreaming documentation.**

**New files:**
- `doc/eval-cache/design.md` — Architecture and design decisions
- `doc/eval-cache/implementation.md` — Implementation phases and details
- `doc/eval-cache/upstreaming.md` — This document

### Key Dependency Constraints

- **Commit 1 before 2:** FileLoadTracker uses BLAKE3 HashSink
- **Commit 2 before 4:** ExprCached depends on FileLoadTracker
- **Commit 3 before 4:** EvalCache uses `getStableIdentity()` for contextHash
- **Commit 4 before 5:** Functional tests exercise the eval cache
- **All unit tests in Commit 4:** `helpers.hh` includes the rewritten `eval-cache.hh`

---

## 3. Code Organization Review

Checklist of items to consider before upstreaming:

### File Size

- [x] **eval-cache.cc (888 lines)**: The `ExprCached` struct definition (~130
  lines) + support types (`ExprOrigChild`, `SharedParentResult`, ~70 lines) + the
  `eval()` method (~200 lines) are all in one file. Acceptable — layout keeps
  everything close to usage, which aids understanding.

- [x] **file-load-tracker.hh (390 lines)**: Contains `DepType` enum, `Blake3Hash`
  struct, `DepHashValue` variant, `Dep` struct, `DepKey`, `FileLoadTracker`, and
  `SuspendFileLoadTracker`. These are tightly coupled — splitting would create
  circular includes. Acceptable as-is.

- [x] **eval-cache-store.cc (603 lines)**: `coldStore()` (~100 lines),
  `warmPath()` (~60 lines), `recovery()` (~150 lines), trace I/O (~100 lines),
  `storeDepSet()`/`loadDepSet()` (~80 lines). Well-structured with clear section
  headers. Acceptable as-is.

### Code Patterns

- [x] **primops.cc recordDep calls (184 lines)**: Each built-in that accesses
  files has a `recordDep(...)` call with appropriate type and hash. The calls
  are repetitive but each has type-specific logic (Content vs Directory vs
  Existence, NAR vs raw bytes, etc.). A wrapper macro would obscure the type
  semantics. Acceptable as-is.

- [x] **storeForcedSibling (eval-cache.cc)**: Large method that serializes
  values and defers writes. The value serialization switch is similar to
  `evaluateCold`, but the two sites have different error handling and child
  wrapping. Duplication is justified.

- [x] **ExprCached child wrapping**: Three sites create ExprCached children:
  `materializeValue()`, `storeForcedSibling()`, and `evaluateCold()`. Each has
  subtly different wrapping logic (origExpr vs non-origExpr, deferred vs
  immediate). Consolidation risks losing these distinctions. Acceptable as-is.

### Naming

- [x] **`storeAttrPath()` returns null-byte-separated strings**: The name could
  be confused with "store path" (Nix store). However, it is only used internally
  by the store backend for index lookups. **Decision:** Keep as-is — renaming to
  `serializedAttrPath()` or `indexAttrPath()` would be clearer but the function
  is internal and documented. Low priority for upstream review.

- [x] **`tracePath`**: Good name — clearly identifies the content-addressed
  trace blob in the store. No change needed.

- [x] **`contextHash`**: The first 8 bytes of BLAKE3 of the stable identity.
  Could be confused with `NixStringContext`. **Decision:** Keep as-is — the name
  is accurate in the eval cache context and well-documented in
  `eval-cache-store.hh`. Renaming to `identityHash` is an option if reviewers
  flag it, but the term "context" here refers to "evaluation context" (flake
  inputs, expression hash), not string context.

### Dead Code

- [x] Verify no remnants of the intermediate SQLite-based AttrDb design remain
  (all removed in the CAS blob trace commit). **Verified:** No `class AttrDb`
  references in any source files.

- [x] Verify `eval-cache-db.hh/cc` files are fully removed (they were part of
  the intermediate design). **Verified:** Files do not exist.
  `src/libstore/builtins/eval.cc` (marker builders) also confirmed removed.

---

## 4. Interface Documentation Checklist

Files needing documentation review before upstreaming:

- [x] **eval-cache.hh** — `EvalCache` class has `///@file` marker, method-level
  docs on `getOrEvaluateRoot()` and `getRootValue()`, field docs on
  `storeBackend` and `inputAccessors`. `AttrType` enum could use per-value docs
  but is self-descriptive. Adequate for review.

- [x] **eval-cache-store.hh** — Comprehensive doxygen on every public method.
  Module-level comment describes the two-object trace model. `DeferredColdStore`
  and `EvalCacheStore` both have detailed class-level docs. Ready for review.

- [x] **eval-index-db.hh** — Module-level comment explains index role and how
  losses affect cache. All public methods documented. `IndexEntry` and
  `StructGroup` structs documented. Ready for review.

- [x] **eval-result-serialise.hh** — `EvalTrace` struct well-documented with
  field explanations. Serialization functions have format descriptions.
  `serializeDepSet`/`deserializeDepSet` document the zstd-compressed CBOR
  format. Ready for review.

- [x] **file-load-tracker.hh** — `///@file` marker. `DepType` enum categorized
  into Hash Oracle, Reference Resolution, and Volatile types with per-value docs.
  `Dep` struct has field-level docs. `FileLoadTracker` has usage guide comment.
  Most comprehensive documentation in the set. Ready for review.

- [x] **stat-hash-cache.hh** — Module-level comment describes two-level (L1/L2)
  architecture. `LookupResult` struct fields explained. All public methods
  documented (`lookupHash`, `storeHash` overloads, `clearMemoryCache`). Ready
  for review.

---

## 5. Duplicate Functionality Check

Analysis of potential overlap with existing Nix code:

| Area | Status | Notes |
|------|--------|-------|
| CBOR | No duplication | Centralized in `eval-result-serialise.cc` via `nlohmann::json::to_cbor`. No other CBOR usage in Nix. |
| BLAKE3 | No duplication | Uses existing `nix::Hash` / `nix::HashSink` throughout. `Blake3Hash` is a lightweight 32-byte wrapper. |
| SQLite | No duplication | Extends existing `sqlite.hh/cc` wrapper. `EvalIndexDb` uses standard `SQLite`/`SQLiteStmt`. |
| File hashing | No duplication | Centralized in `StatHashCache` + `depHash*` functions. No overlap with store-level hashing. |
| Store operations | No duplication | Uses standard `Store::addTextToStore()` for CAS blobs. |
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

# Eval cache specific
nix develop --command bash -c "meson test -C build eval-cache"
```

### Performance Sanity Check

```bash
# Cold path (first eval of a flake)
rm -rf ~/.cache/nix/eval-index-v2.sqlite
time nix eval nixpkgs#hello.pname
# Expected: ~3-5s (dominated by evaluation + store I/O)

# Warm path (cached eval)
time nix eval nixpkgs#hello.pname
# Expected: < 200ms (stat validation + deserialization)
```

### Verify No Regressions

```bash
# Full test suite including non-eval-cache tests
nix develop --command bash -c "meson test -C build"

# CA tests (sensitive to store operations)
nix develop --command bash -c "meson test -C build --suite ca"
```

---

## 7. Summary of Changes by Category

| Category | Files | Lines | Description |
|----------|------:|------:|-------------|
| Core eval cache | 12 | ~3,870 | ExprCached, store backend, index, serialization |
| Dep tracking | 4 | ~1,033 | FileLoadTracker, StatHashCache |
| CLI integration | 15 | ~970 | installables, commands, settings, fetcher IDs |
| C++ unit tests | 10 | ~4,508 | Component and integration tests |
| Functional tests | 10 | ~2,780 | End-to-end caching behavior tests |
| Build system | 7 | ~37 | Meson build file updates |
| Test fixes | 3 | ~42 | gc-auto.sh, CA test, common functions |
| Documentation | 3 | ~1,811 | Design, implementation, upstreaming docs |
| **Total** | **82 files** | **~13,022 net** | |

---

## 8. Review Guidance

### Recommended Review Order

1. **`file-load-tracker.hh`** — Start with dep types and the tracking model.
   This is the conceptual foundation.

2. **`eval-cache.hh`** — Public API surface (124 lines). Understand what
   consumers see.

3. **`eval-cache.cc`** — Core eval loop. Focus on `ExprCached::eval()` and
   `evaluateCold()`. The warm/cold dispatch is the heart of the system.

4. **`eval-cache-store.hh/cc`** — Store backend. `coldStore()` and `warmPath()`
   are the two main flows.

5. **`eval-result-serialise.hh/cc`** — CBOR format. Straightforward serialization.

6. **`eval-index-db.hh/cc`** — SQLite index. Simple CRUD with 3 tables.

7. **`stat-hash-cache.hh/cc`** — Performance optimization. Can be reviewed last.

8. **CLI integration** (`src/libcmd/`, `src/nix/`) — How commands use the cache.
   Focus on `installable-attr-path.cc` and `installable-flake.cc`.

9. **Fetcher changes** (`src/libfetchers/`) — `getStableIdentity()` additions.
   Small, self-contained.

10. **Tests** — Read the test names and comments for coverage understanding.
    The regression tests (`eval-cache-impure-regression.sh`) document specific
    bugs that were found and fixed.

### Key Design Decisions to Evaluate

1. **CAS blobs vs SQLite for result storage.** The design trades SQLite's query
   flexibility for store-native GC integration and immutability. Is this the right
   trade-off for the Nix ecosystem?

2. **11 dep types.** Is this the right granularity? Are there deps we're missing
   (e.g., symlink targets) or deps that are over-specific?

3. **Three-phase recovery.** Phase 3 (struct-group scan) handles dep structure
   instability at the cost of O(V) scanning. Is this complexity justified?

4. **ExprCached as Expr subclass.** The GC-allocated thunk approach requires
   understanding of the evaluator's memory model. Is this the right integration
   point, vs. a separate cache layer?

5. **origExpr dual-mode evaluation.** The distinction between "materialized from
   cache" and "wrapping a real thunk" adds complexity. Is there a simpler way
   to handle sibling caching?
