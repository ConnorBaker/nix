# Nix Eval Cache: Implementation Notes

**Target audience:** Nix core team reviewing the implementation.

This document records what was tried, what failed, what was learned, and how the
current implementation works. It is a companion to `design.md` (architecture and
goals) and `upstreaming.md` (commit cleanup plan).

---

## 1. Development History

The eval cache was developed across ~48 sessions on the
`vibe-coding/file-based-cas-eval-cache` branch. The implementation went through
several major architectural phases before converging on the current CAS blob
trace design.

### Phase 1: Foundation (Session 1–3)

**Decision: Replace AttrCursor with GC-allocated ExprCached thunks.**
The existing `AttrCursor` API requires callers to use cache-specific methods
(`getString()`, `getAttrs()`, etc.). Instead, `ExprCached` is an `Expr` subclass
whose `eval()` method transparently serves cached results or falls through to
real evaluation. CLI commands use standard `forceValue()` without knowing whether
caching is active.

**Decision: RAII thread-local dep recording.**
`FileLoadTracker` uses a thread-local pointer set on construction. All dep
recording goes through `FileLoadTracker::record()`, which appends to a session-
wide `std::vector<Dep>`. Each tracker records its start index; the range
`[startIndex, sessionDeps.size())` represents deps recorded during that tracker's
lifetime.

**Rejected: CachedValue dep type.**
Early prototype had a dep type for "this value was served from cache." This was
evaluation-order dependent (whether a sibling was warm-cached depended on prior
access) and broke deterministic dep sets.

**Rejected: Single CopiedPath for filtered paths.**
Treating filtered `builtins.path` like unfiltered (single NAR hash for the whole
result) caused over-invalidation: any file change in the filtered directory
invalidated the cache, even if the filter excluded the changed file. Added
`NARContent` (type 11) for per-file NAR hashing that captures both content and
executable bit.

### Phase 2: Sibling Caching (Session 4–6)

**Problem: Side-effect siblings not cached.**
When `navigateToReal()` walks the eval tree to `A.B.C`, siblings of `B` are
forced as side effects (e.g., `derivationStrict` forcing `stdenv` while building
`hello`). Without caching, these side-effect evaluations are lost.

**Solution: origExpr/origEnv wrappers.**
`navigateToReal()` wraps sibling thunks with `ExprCached` instances that have
`origExpr`/`origEnv` set. When later forced, these wrappers try the warm cache
first, then fall back to evaluating the original thunk expression.

### Phase 3: Scale Optimization (Session 7–10)

**Bug: ExprOrigChild caused 6.7M dep recordings on opencv.**
`ExprOrigChild::eval()` evaluates the parent to resolve a child. Without dep
suspension, evaluating `buildPackages` (= all of nixpkgs) as a parent recorded
10,000+ file deps into each child's dep set. Combined with redundant dep replay
after cold `evaluateCold`, this compounded to O(2^K) dep accumulation.

**Fix:** `SuspendFileLoadTracker` RAII guard in `ExprOrigChild::eval()` prevents
recording parent deps. Redundant dep replay after origExpr cold miss was removed.

**Bug: Recursive sibling storage caused 88x slowdown.**
`storeForcedChildren()` descended recursively through sibling attrsets, causing
50K–100K+ SQLite operations for nixpkgs-scale evaluations.

**Fix:** Single-level `storeForcedSibling()` stores only immediate children
of forced siblings.

### Phase 4: Hashing (Session 11–14)

**Decision: Use Nix's built-in HashSink for BLAKE3.**
Rather than introducing a separate hashing library, all BLAKE3 hashing goes
through `nix::Hash` / `nix::HashSink`. The `Blake3Hash` struct provides a 32-byte
fixed-size, stack-allocated wrapper with zero heap allocation.

**Added: NARContent dep type** for filtered `builtins.path`. Unlike `Content`
(raw file bytes), `NARContent` captures the executable bit via NAR serialization.
This detects `chmod +x` changes that `Content` would miss.

### Phase 5: StatHashCache (Session 15–18)

**Decision: Two-level persistent cache for file hashes.**
L1 is a `boost::concurrent_flat_map` (session-scoped, 64K entry cap). L2 is a
SQLite database at `~/.cache/nix/stat-hash-cache-v2.sqlite`. Both are keyed on
file stat metadata `(path, dev, ino, mtime_sec, mtime_nsec, size, dep_type)`.

**Removed: epochBloom filter.** A 64-bit bloom filter guarded `epochMap.find()`
for dep replay memoization. Measurement across nixpkgs evals (hello, opencv,
firefox, nix search) showed epochMap consistently holds ~4,000 entries, fully
saturating all 64 bits. Result: 97.5% false positive rate, only 0.9% of calls
actually filtered. Even 128 bits would saturate at N=4,000. Removed entirely —
`boost::unordered_flat_map::find()` miss is already fast (open addressing, single
probe on miss).

### Phase 6–7: Schema Redesign (Session 19–22)

*(These phases applied to an intermediate SQLite-based design that was later
replaced by CAS blob traces. Lessons learned still informed the final design.)*

**DAG-based dep sets.** Parent-child dep relationships encoded via
`parent_set_id` FK instead of ParentContext dep entries. Each dep set stores only
direct (non-inherited) deps, with parent chain traversal via recursive CTE.

**String interning.** File paths interned into a `Strings` table, with
`DepEntries` using integer FKs. Reduced unique constraint overhead from long text
B-tree to integer comparison.

**Direct BLAKE3 recovery.** Replaced XOR-based pre-filter and `diffBasedRecovery`
with a single indexed lookup by BLAKE3 hash of current deps.

### Phase 8: Zero-Alloc Hash Pipeline (Session 23–26)

**Blake3Hash value type.** 32-byte `std::array<uint8_t, 32>` with comparison,
hashing, and hex conversion. Eliminated all heap allocations in the hash pipeline.

**StatHashCache v2.** Stores hashes as 32-byte BLOBs (not hex strings). Bulk L2
load on first access. Stat-passing API: `lookupHash` returns
`LookupResult{hash, stat}`, and `storeHash` accepts the stat to avoid redundant
`lstat()`.

### Phase 9: CAS Blob Traces (Session 27–40)

**Architectural pivot: replace monolithic SQLite with store-based CAS blobs.**

The SQLite-based design had persistent correctness issues:
- Cache coherence between session caches and persistent DB
- UPSERT race conditions invalidating dep sets
- No natural GC integration (separate cleanup needed)
- 91% warm hit rate due to index corruption patterns

CAS blob traces solve all of these:
- Immutable once stored (no overwrites, no races)
- GC'd naturally as store objects
- 100% warm hit rate (if trace exists in store, it's valid)
- Content addressing provides natural dedup

**Deleted:** `builtins/eval.cc` (builtin:eval + builtin:eval-dep marker builders),
`eval-cache-db.hh/cc` (AttrDb SQLite layer).

**Added:** `eval-cache-store.hh/cc`, `eval-index-db.hh/cc`,
`eval-result-serialise.hh/cc`.

**Recovery redesign.** The SQLite design used derivation path determinism (same
deps → same drv path → same output). CAS blob traces embed the result in the
trace, so different results produce different trace paths even with identical
deps. Solution: `DepHashRecovery` table maps `(context_hash, attr_path, dep_hash)
→ trace_path`, where `dep_hash` is computed from deps *without* the result.

### Phase 10: Final Optimization (Session 41–48)

**Batched writes.** StatHashCache wraps all writes in a single `SQLiteTxn`
(committed at destructor). Reduces SQLite write overhead from per-dep to per-
session.

**Sort-once deps.** `sortAndDedupDeps()` called once in `coldStore()`. Pre-sorted
variants (`serializeEvalTraceSorted`, `computeDepContentHashFromSorted`, etc.)
used for serialization and hashing — avoids redundant sorting.

**Per-tracker dedup.** `FileLoadTracker::record()` deduplicates by `(type, source,
key)` within the active tracker scope using a `DepKey` hash set. Note: dedup only
when an active tracker exists (`SuspendFileLoadTracker` sets the tracker to null,
but recording must still append to `sessionDeps` for thunk dep replay).

**recordDep uses maybeLstat.** Replaced separate `exists()` + `lstat()` calls
with a single `maybeLstat()` that returns `std::optional<struct stat>`.

### Phase 11: Two-Object Trace Model (Session 49+)

**Problem: Storage bloat from inline deps.**
Analysis of a typical nixpkgs evaluation showed 465 MB of trace data in an
837 MB store, with 99.99% of trace size consumed by inline dependency arrays.
Near-zero CAS deduplication was observed because each v1 trace included both
the result and deps — so even attributes with identical dep sets produced
different store paths (due to different results). The single-blob model meant
that every attribute paid the full storage cost of its dep array, and no
sharing was possible.

**Solution: Split into result trace + zstd-compressed dep set blob.**
Each attribute's cache entry now consists of two store objects:

1. **Result trace** — a slim CBOR blob containing only the result, parent
   reference, context hash, and a store path reference to the dep set blob.
   Format version bumped to v2: `{v:2, r:<result>, p:<parent|null>,
   c:<ctx|null>, ds:<dep-set-path>}`.

2. **Dep set blob** — a zstd-compressed CBOR array of deps, stored as a Text
   CA blob with name `"eval-deps"` and no store references. Because it contains
   only deps (no result or parent), attributes with identical dep sets produce
   identical store paths and share a single object via CAS deduplication.

**Storage reduction estimate: ~89%.** The dep set blobs compress well (sorted,
repetitive structure), and CAS deduplication eliminates redundant copies across
attributes that read the same files.

**GC coupling.** The result trace lists the dep set blob path in its references,
so the dep set blob is kept alive as long as any result trace referencing it is
reachable. When all result traces referencing a dep set blob are collected, the
dep set blob becomes garbage and is collected on the next GC pass.

**v1 format removal.** The v1 single-blob trace format (with inline `"d"` array)
was removed. `loadTrace()` rejects traces with `"v": 1`. Existing v1 traces in
the store are harmless (they become unreferenced garbage) but cannot be loaded.

---

## 2. File Layout

### Source Files

| File | Lines | Description |
|------|------:|-------------|
| `src/libexpr/include/nix/expr/eval-cache.hh` | 124 | Public API: `EvalCache`, `AttrType`, `AttrValue`, counters |
| `src/libexpr/include/nix/expr/eval-cache-store.hh` | 187 | `EvalCacheStore`: cold/warm/recovery, trace I/O |
| `src/libexpr/include/nix/expr/eval-index-db.hh` | 142 | `EvalIndexDb`: SQLite index for warm-path + recovery |
| `src/libexpr/include/nix/expr/eval-result-serialise.hh` | 141 | CBOR serialization: `EvalTrace`, `AttrValue`, hash functions |
| `src/libexpr/include/nix/expr/file-load-tracker.hh` | 390 | Dep types, `Dep`, `Blake3Hash`, `FileLoadTracker`, `SuspendFileLoadTracker` |
| `src/libexpr/include/nix/expr/stat-hash-cache.hh` | 85 | `StatHashCache`: L1/L2 persistent file hash cache |
| `src/libexpr/eval-cache.cc` | 891 | `ExprCached`, `ExprOrigChild`, `SharedParentResult`, eval loop |
| `src/libexpr/eval-cache-store.cc` | 589 | Store backend: `coldStore`, `warmPath`, `recovery`, trace I/O |
| `src/libexpr/eval-index-db.cc` | 283 | SQLite index: schema, prepared statements, CRUD |
| `src/libexpr/eval-result-serialise.cc` | 355 | CBOR encoding/decoding via nlohmann::json; dep set zstd compression/decompression; v2 trace format (v1 removed) |
| `src/libexpr/file-load-tracker.cc` | 219 | Dep recording, hashing, input resolution |
| `src/libexpr/stat-hash-cache.cc` | 339 | L1 concurrent map, L2 SQLite, stat validation |

**Total new code:** ~3,745 lines across 12 files.

### Modified Files

| File | Changes | Description |
|------|--------:|-------------|
| `src/libexpr/eval.cc` | +88 | FileLoadTracker integration, `evalFile` Content dep |
| `src/libexpr/primops.cc` | +184 | `recordDep` calls for all file-accessing builtins |
| `src/libexpr/primops/fetchTree.cc` | +12 | UnhashedFetch dep recording |
| `src/libcmd/installable-attr-path.cc` | ~200 | `NIX_ALLOW_EVAL` guard, ExprCached root creation |
| `src/libcmd/installable-flake.cc` | ~200 | Flake eval cache lifecycle, `computeStableIdentity()` |
| `src/libflake/flake.cc` | +85 | Input accessor mappings, `mountToInput` |
| `src/nix/eval.cc` | ~50 | `getRootValue()` instead of `getRoot()` |
| `src/nix/search.cc` | ~50 | Value-based API instead of AttrCursor |
| `src/nix/flake.cc` | ~50 | Value-based API instead of AttrCursor |
| `src/libexpr/include/nix/expr/eval-settings.hh` | +20 | `eval-cache`, `verify-eval-cache` settings |

### Test Files

| File | Lines | Description |
|------|------:|-------------|
| `tests/functional/flakes/eval-cache-core.sh` | ~350 | Core cold/warm, per-attr invalidation, GC resilience |
| `tests/functional/flakes/eval-cache-deps.sh` | ~300 | pathExists, readDir, flake input, partial tree |
| `tests/functional/flakes/eval-cache-output.sh` | ~250 | Scalar types, JSON output, write-to |
| `tests/functional/flakes/eval-cache-recovery.sh` | ~200 | Dep revert, alternating versions, recovery |
| `tests/functional/flakes/eval-cache-volatile.sh` | ~150 | currentTime, mixed volatile/non-volatile |
| `tests/functional/eval-cache-impure-core.sh` | ~350 | --file, --expr, selective invalidation |
| `tests/functional/eval-cache-impure-deps.sh` | ~350 | getEnv, currentSystem, pathExists, hashFile, addPath |
| `tests/functional/eval-cache-impure-advanced.sh` | ~350 | Deep origExpr, dep suspension, fat parent |
| `tests/functional/eval-cache-impure-output.sh` | ~300 | Cursor eval, JSON, revert recovery |
| `tests/functional/eval-cache-impure-regression.sh` | ~200 | Specific bug regressions (3 tests) |

**Total test code:** ~2,800 lines across 10 files.

---

## 3. Critical Implementation Details

### 3.1 ExprCached Eval Loop (eval-cache.cc)

The `ExprCached::eval()` method is the core dispatch point:

```
eval(state, env, v):
  1. If origExpr set and warm path succeeds:
     → serve cached result, replay deps to parent tracker
  2. If origExpr set and warm path fails:
     → evaluate origExpr directly (not via navigateToReal)
     → store result in cache, wrap children
     → v = *target (real value, not materialized)
  3. If no origExpr, warm path succeeds:
     → serve cached result
  4. If no origExpr, cold path:
     → navigateToReal() to get real tree Value*
     → forceValue(*target)
     → if derivation: force drvPath (unless origExpr wrapper)
     → coldStore: serialize + store trace + update index
     → materializeValue: create ExprCached children
     → storeForcedSibling: cache already-forced siblings
```

Key correctness guards:
- `if (origExpr) v = *target;` — origExpr wrappers produce real values, not
  materialized ExprCached children, to prevent infinite recursion through
  navigateToReal on subsequent accesses.
- `if (!origExpr && isDerivation) forceValue(drvPath)` — eager drvPath forcing
  triggers `derivationStrict`, capturing env-processing deps. Skipped for
  origExpr wrappers to avoid infinite recursion via `buildPackages = self`.
- `dynamic_cast<ExprCached*>` guard — prevents double-wrapping due to
  `derivation.nix` Value* aliasing (`default.out` shares Value* with `default`).

### 3.2 FileLoadTracker Session Model (file-load-tracker.hh/cc)

```
Thread-local state:
  activeTracker: FileLoadTracker* (null when suspended)
  sessionDeps:   vector<Dep>      (all deps from this session)

FileLoadTracker:
  previous:       saved activeTracker (restored on destruction)
  mySessionDeps:  &sessionDeps
  startIndex:     sessionDeps.size() at construction
  replayedRanges: epoch ranges from pre-existing thunks
  recordedKeys:   DepKey dedup set for this tracker's scope

record(dep):
  if activeTracker:
    if dep.key not in activeTracker->recordedKeys:
      recordedKeys.insert(dep.key)
      sessionDeps.push_back(dep)
  else:
    sessionDeps.push_back(dep)  // no dedup during suspension

collectDeps():
  result = sessionDeps[startIndex..current]
  for range in replayedRanges:
    result += range.deps[range.start..range.end]
  return result
```

### 3.3 StatHashCache Architecture (stat-hash-cache.hh/cc)

```
StatHashCache (singleton):
  L1: boost::concurrent_flat_map<StatHashKey, Blake3Hash>
      Key: (dev, ino, mtime_sec, mtime_nsec, size, depType)
      Session-scoped, 64K entry cap, cleared on invalidateFileCache()

  L2: SQLite (persistent, ~/.cache/nix/stat-hash-cache-v2.sqlite)
      Schema: (path TEXT, dep_type INT) → hash BLOB(32)
      Plus stat metadata columns for validation
      Bulk-loaded on first L1 miss, L1 promoted on L2 hit

  lookupHash(path, depType, stat):
    if L1.find(key) → return {hash, stat}
    if L2.query(path, depType) and stat matches → promote to L1, return
    return nullopt

  storeHash(path, depType, hash, stat):
    L1.insert(key, hash)
    L2.upsert(path, depType, hash, stat)  // deferred in SQLiteTxn
```

All L2 writes are wrapped in a single `SQLiteTxn` committed at destructor time.
This reduces SQLite write overhead from per-dep to per-session.

### 3.4 CBOR Serialization (eval-result-serialise.hh/cc)

Uses `nlohmann::json` for CBOR encoding (`json::to_cbor()`) and decoding
(`json::from_cbor()`). This was chosen over a dedicated CBOR library because
nlohmann::json is already a dependency.

**Two-object serialization.** Result traces and dep set blobs are serialized
independently. The dep set blob is CBOR-encoded and then zstd-compressed at a
fixed compression level (level 3) before being stored as a Text CA blob. The
result trace is a slim CBOR blob referencing the dep set blob's store path via
the `"ds"` field. On load, `loadTrace()` reads the result trace, resolves the
`"ds"` path, reads and decompresses the dep set blob, and returns the combined
`EvalTrace` struct.

**Dep hash computation** hashes sorted deps by feeding each dep's fields into a
`HashSink(HashAlgorithm::SHA256)`:

```
For each dep (sorted by type, source, key):
  sink("T")  sink(to_string(dep.type))
  sink("S")  sink(dep.source)
  sink("K")  sink(dep.key)
  sink("H")  hashDepValue(sink, dep.expectedHash)
```

Three hash variants:
- `computeDepContentHash`: deps only (Phase 1 recovery key)
- `computeDepContentHashWithParent`: deps + "P" + parent path (Phase 2)
- `computeDepStructHash`: deps keys only, no hashes (Phase 3 grouping)

### 3.5 Deferred Write Pattern (eval-cache-store.cc)

Cold-path trace storage uses a FIFO queue to ensure correct ordering:

```
deferredWrites: vector<function<void()>>
stagedAttrPaths: unordered_set<string>

defer(attrPath, fn):
  if attrPath in stagedAttrPaths: return  // dedup
  stagedAttrPaths.insert(attrPath)
  deferredWrites.push_back(fn)

flush():
  for fn in deferredWrites: fn()
  deferredWrites.clear()
  stagedAttrPaths.clear()

~EvalCacheStore():
  flush()
```

Parent lambdas are pushed before children because evaluation is depth-first.
This guarantees that `parentEC->tracePath` is set before children read it.

---

## 4. Bugs and Pitfalls Catalog

### 4.1 Infinite Recursion (4 bugs)

**Bug 1: Eager drvPath forcing for origExpr wrappers.**
*Symptom:* Stack overflow evaluating `blas.provider.pname` in nixpkgs.
*Root cause:* `evaluateCold`'s `if (isDerivation) forceValue(drvPath)` triggers
`derivationStrict` for every derivation in the chain. With origExpr wrappers and
nixpkgs' `buildPackages = self` fixed-point: `blas → perl → libxcrypt →
buildPackages.perl → self.perl → drvPath → blackhole`.
*Fix:* `if (!origExpr && isDerivation) forceValue(drvPath)`. Also: `if (origExpr)
v = *target;` early blackhole exit.
*Lesson:* origExpr wrappers must never trigger eager side effects that could
recurse through the fixed-point.

**Bug 2: navigateToReal blackholes on materialized children.**
*Symptom:* Infinite loop when accessing cached FullAttrs children.
*Root cause:* Naively materializing ExprCached children into the real tree causes
navigateToReal to walk through materialized thunks → force parent → blackhole.
*Fix:* `ExprOrigChild` resolves via parent's original expression, producing fresh
Value* objects (not the materialized ones).
*Lesson:* Materialized and real trees must be kept separate.

**Bug 3: materializeStoreValue on origExpr paths.**
*Symptom:* Infinite recursion when accessing children of origExpr cold-path
results.
*Root cause:* `materializeStoreValue()` replaces children in the real tree with
ExprCached thunks. Subsequent `navigateToReal()` walks through these modified
children and hits blackholed Values.
*Fix:* origExpr cold evals use `v = *target` (raw values), not
`materializeStoreValue()`.
*Lesson:* Never modify the real tree from origExpr paths.

**Bug 4: ExprOrigChild dep suspension missing.**
*Symptom:* 6.7M dep recordings for opencv, causing OOM.
*Root cause:* Without `SuspendFileLoadTracker`, evaluating `buildPackages` as
parent recorded 10K+ deps into each child's dep set. Combined with redundant
dep replay: O(2^K) compounding.
*Fix:* `SuspendFileLoadTracker` RAII guard + remove redundant dep replay.
*Lesson:* Parent evaluation during child resolution must be isolated from
child dep recording.

### 4.2 Dependency Tracking (6 bugs)

**Bug 5: Dep set structure instability across cold evals.**
*Symptom:* Hash-based recovery misses for attributes that should match.
*Root cause:* A child's dep set can have different structure between cold evals
depending on evaluation order. `navigateToReal → getOrEvaluateRoot → evalFile`
runs inside the child's FileLoadTracker when root is NOT cached, but outside
when cached. So first cold eval may record [Content revert-dep.txt, ParentCtx]
while second records [Content test-revert.nix, Content revert-dep.txt, ParentCtx].
*Fix:* Hash recovery (O(1)) as fast path + struct-group scan (O(V)) as fallback.
*Lesson:* Dep set structure is not deterministic across evaluations.

**Bug 6: storeForcedSibling overwrites index with dep-less entries.**
*Symptom:* Warm path sees 0 deps for previously fully-cached attributes.
*Root cause:* When sibling `a` is already forced, `storeForcedSibling` calls
`coldStore` with empty deps `{}`, overwriting `a`'s index entry (which had
full deps from its own `evaluateCold`).
*Fix:* Check `index.lookup()` before `coldStore` — skip if already stored.
*Lesson:* Speculative writes must not overwrite authoritative entries.

**Bug 7: storeForcedSibling on non-derivation attrsets.**
*Symptom:* "access to absolute path is forbidden in pure evaluation mode" for
flake inputs.
*Root cause:* Storing children of non-derivation attrsets (flake inputs like
`inputs.flake1`) as bare strings bypasses SourceAccessor setup. The warm path
serves them directly, but flake input resolution requires real-tree evaluation
for AllowListSourceAccessor.
*Fix:* Guard forced-children loop with `!origExpr && isDerivation(*target)`.
*Lesson:* Not all attrset children can be independently cached.

**Bug 8: Virtual files from MemorySourceAccessor.**
*Symptom:* Dep validation crashes on non-existent paths.
*Root cause:* Virtual files (`/fetchurl.nix`, `/derivation-internal.nix`) exist
in `MemorySourceAccessor` but not on the real filesystem. Recording them as deps
fails on validation (`lstat()` → ENOENT).
*Fix:* Guard with `std::filesystem::exists()` before recording deps.
*Lesson:* Not all evaluated files are real filesystem paths.

**Bug 9: Stat-hash cache population placement.**
*Symptom:* Incomplete dep sets causing stale cache hits.
*Root cause:* `cacheStatHash()` before `recordDep()` could throw, skipping dep
recording. Cache had incomplete deps → stale warm-path hits.
*Fix:* Populate stat cache inside `recordDep()` itself, after the dep is recorded.
*Lesson:* Dep recording must be infallible relative to hash caching.

**Bug 10: navigateToReal wraps siblings, stealing deps.**
*Symptom:* Incomplete dep sets for attributes whose children are ExprCached.
*Root cause:* When navigateToReal wraps a sibling with ExprCached, forcing that
wrapped value creates a nested FileLoadTracker that captures deps instead of
the outer tracker.
*Fix:* In evaluateCold, detect ExprCached origExpr thunks and unwrap by evaluating
the original expression in the current tracker's scope.
*Lesson:* Nested trackers from wrapped thunks steal deps from outer scopes.

### 4.3 Performance (4 bugs)

**Bug 11: Lazy dep replay O(N*M) explosion.**
*Symptom:* 132 seconds, 8GB memory for nixpkgs `hello.pname`.
*Root cause:* Eager DFS replay of thunk deps. Each thunk replayed all its deps
into the parent tracker, and the parent replayed those plus its own into the
grandparent. O(N*M) where N = thunk count, M = avg dep count.
*Fix:* Store `EpochRange` pointers (lazy references). Flatten deps once at
`collectDeps()` time.
*Lesson:* Dep replay must be O(1) per thunk, not O(deps).

**Bug 12: epochBloom filter saturation.**
*Symptom:* No measurable performance improvement despite bloom filter overhead.
*Root cause:* 64-bit bloom filter with ~4,000 entries → 100% bit saturation →
97.5% false positive rate. Only 0.9% of calls actually filtered.
*Fix:* Removed entirely. `boost::unordered_flat_map::find()` miss is already fast.
*Lesson:* Bloom filters need ~10x more bits than entries to be useful.

**Bug 13: Recursive sibling storage.**
*Symptom:* 88x slowdown for nixpkgs evaluations.
*Root cause:* `storeForcedChildren()` recursively descended through sibling
attrsets → 50K–100K+ SQLite operations.
*Fix:* Single-level `storeForcedSibling()`.
*Lesson:* Recursive tree storage is catastrophically expensive.

**Bug 14: Cold-path redundant sorting.**
*Symptom:* Unnecessary CPU overhead in cold path.
*Root cause:* Deps sorted separately for serialization, content hash, struct hash.
*Fix:* `sortAndDedupDeps()` once; pre-sorted variants for all consumers.
*Lesson:* Sort once, use many.

### 4.4 Storage and SQLite (5 bugs)

**Bug 15: internString deadlock in setDepSet.**
*Symptom:* Deadlock during cold-path writes.
*Root cause:* `internString()` acquires `_state->lock()`, but `setDepSet()` already
holds that lock via `doSQLite`.
*Fix:* Lambda inside setDepSet accesses already-locked state; separate public
`internString()` for external callers.
*Lesson:* SQLite helper methods must not re-acquire locks.

**Bug 16: sessionDepSetIds cache staleness.**
*Symptom:* Missing dep validation, stale cache hits.
*Root cause:* `insertAttribute`'s UPSERT sets `dep_set_id = null` on conflict,
making the session cache stale.
*Fix:* Never use sessionDepSetIds to skip DB writes.
*Lesson:* Session caches are unsafe after DB writes that modify the cached column.

**Bug 17: setAttrs orphaning children.**
*Symptom:* Children and grandchildren lost after parent update.
*Root cause:* `INSERT OR REPLACE` deletes and re-inserts the row with a new rowId,
orphaning FK references from children.
*Fix:* `UPDATE` for parent + `INSERT OR IGNORE` for children + `DELETE` stale.
*Lesson:* `INSERT OR REPLACE` is a delete+insert, not an update.

**Bug 18: hash_value BLOB encoding.**
*Symptom:* NULL constraint violation on CopiedPath/UnhashedFetch deps.
*Root cause:* `hexToBytes()` was applied to store paths (`/nix/store/...`), which
are not hex-encoded BLAKE3 hashes. Store paths are variable-length strings.
*Fix:* Bind raw UTF-8 bytes directly as BLOB.
*Lesson:* Not all dep hashes are BLAKE3 — some are store paths or status strings.

**Bug 19: STRICT SQLite BLOB binding.**
*Symptom:* Type mismatch error on EvalIndex operations.
*Root cause:* `attr_path` is `BLOB` in a `STRICT` table. Using
`operator()(string_view)` binds as TEXT, which SQLite STRICT mode rejects.
*Fix:* Use `operator()(const unsigned char*, size_t)` for BLOB binding.
*Lesson:* SQLite STRICT tables enforce type affinity at bind time.

### 4.5 Store Integration (5 bugs)

**Bug 20: CAS traces consume autoGC budget.**
*Symptom:* gc-auto test fails (expects 2 of 3 garbage paths deleted).
*Root cause:* `nix build --impure --expr` creates 8–11 Text CA traces per
expression. When the process exits, traces become garbage. autoGC's single-pass
budget gets consumed deleting traces before garbage derivation paths.
*Fix:* `--option eval-cache false` in gc-auto.sh.
*Lesson:* Test isolation requires disabling cache when testing GC behavior.

**Bug 21: derivation.nix lazy derivationStrict.**
*Symptom:* Missing deps from env var processing (e.g., readFile in buildCommand).
*Root cause:* `derivation.nix` uses `let strict = derivationStrict drvAttrs`
lazily. Forcing a derivation attrset to WHNF does NOT call `derivationStrict`.
Deps are only captured when `drvPath`/`outPath` is accessed.
*Fix:* In evaluateCold, after forceValue, detect derivation attrsets and force
`drvPath` to trigger derivationStrict.
*Lesson:* derivation.nix's laziness means derivationStrict deps are captured
only on demand.

**Bug 22: derivation.nix Value* aliasing.**
*Symptom:* Double-wrapping of ExprCached thunks, assertion failures.
*Root cause:* `derivation` returns `(head outputsList).value`, so `default.out`
and `default` share the **same Value***. Wrapping `default.drvPath` also wraps
`default.out.drvPath`.
*Fix:* `dynamic_cast<ExprCached*>` guard before wrapping.
*Lesson:* Nix Values can alias — always check before wrapping.

**Bug 23: .drv Existence deps are unsafe.**
*Symptom:* CA test failures when .drv files are deleted mid-test.
*Root cause:* Recording Existence deps for .drv store paths means cold-path
re-evaluation calls `pathDerivationModulo()` on deleted inputs.
*Fix:* Skip recording Existence deps for .drv store paths.
*Lesson:* Store-internal paths should not be treated as external deps.

**Bug 24: StatHashCache file modification flakiness.**
*Symptom:* Tests pass on first run but fail on fast re-runs.
*Root cause:* File modifications of SAME SIZE within the same mtime granularity
cause L1/L2 cache hits returning stale hashes. Key includes size and mtime but
not content.
*Fix:* Always use content of DIFFERENT SIZE in test modifications. Call
`invalidateFileCache()` to clear both PosixSourceAccessor and StatHashCache L1.
*Lesson:* Stat-based caching is vulnerable to same-size, same-timestamp writes.

### 4.6 Two-Object Trace Model (3 pitfalls)

**Pitfall 25: Zstd determinism for CAS stability.**
*Symptom:* Dep set blobs for identical dep sets produce different store paths.
*Root cause:* Zstd compression output can vary across library versions or when
using non-default compression parameters. Different output bytes → different
content hash → different store path → no CAS deduplication.
*Fix:* Use a fixed compression level (level 3) with default zstd parameters.
This produces deterministic output for identical input within a given zstd
library version. Cross-version determinism is not guaranteed but is acceptable
since CAS dedup is an optimization, not a correctness requirement.
*Lesson:* CAS stability requires deterministic serialization at every layer.

**Pitfall 26: Dep set blob GC coupling.**
*Symptom:* Dep set blob collected while result traces still reference it.
*Root cause:* If the dep set blob is stored without being listed in the result
trace's references, the GC has no way to know the result trace depends on it.
The dep set blob becomes unreferenced garbage and is collected, causing
`loadTrace()` to fail with a missing store path.
*Fix:* The result trace lists the dep set blob's store path in its `references`
set when calling `addTextToStore()`. This creates a proper GC root chain:
result trace → dep set blob. The dep set blob itself has no references
(self-contained).
*Lesson:* Store object references are the GC coupling mechanism — always
declare dependencies explicitly.

**Pitfall 27: validatedDepSets cache must be cleared alongside validatedTraces.**
*Symptom:* Stale dep validation results after trace invalidation.
*Root cause:* `validatedTraces` is a session cache of trace paths known to be
valid. When a trace is invalidated (e.g., during recovery), `validatedTraces`
is cleared. But `validatedDepSets` — a separate cache of dep set blob paths
whose deps have been validated — was not cleared. If the old dep set blob path
happened to be reused (same deps, different result trace after recovery), the
validation was incorrectly skipped.
*Fix:* Clear `validatedDepSets` whenever `validatedTraces` is cleared.
*Lesson:* Coupled caches must be invalidated together.

---

## 5. Repeated Reasoning Failure Patterns

These patterns of mistakes recurred across multiple sessions:

### 5.1 Assuming Evaluation Determinism

The dep set for an attribute can have different structure across cold evals
depending on evaluation order. This broke hash-based recovery (which assumed
identical structure → identical hash) and required adding struct-group scanning
as a fallback.

### 5.2 Forgetting Value* Aliasing

`derivation.nix` returns `(head outputsList).value`, making `default.out` and
`default` the same Value*. This caused double-wrapping bugs multiple times.
Every Value* mutation must check for pre-existing ExprCached thunks.

### 5.3 Underestimating Fixed-Point Recursion

nixpkgs' `buildPackages = self` creates chains where forcing one derivation's
drvPath transitively forces arbitrary other derivations. This invalidated
assumptions about bounded evaluation depth and required careful guards on eager
drvPath forcing.

### 5.4 Session Cache Coherence

Multiple bugs arose from session caches becoming stale after DB writes.
The pattern: cache value X → write to DB (which changes X) → read cache
(returns stale X). Must either invalidate caches after writes or avoid caching
write-affected values.

### 5.5 Testing with Same-Size Files

StatHashCache validates by stat metadata, not content. Early tests used
modifications like "hello" → "world" (same size, 5 bytes), which could produce
false cache hits. Must use different-size content for reliable invalidation
testing.

---

## 6. Integration Points

### 6.1 EvalState

`EvalState` holds a `std::map<const Hash, ref<eval_cache::EvalCache>> evalCaches`
map. Each unique cache identity (flake source or file/expr hash) gets its own
`EvalCache` instance. Integration is transparent: `ExprCached` thunks are placed
in the Value tree and `forceValue()` triggers `ExprCached::eval()` automatically.

### 6.2 Flake Subsystem

- `computeStableIdentity()`: computes version-independent identity for cache key
- `inputAccessors`: maps flake input names to SourcePath for dep validation
- `mountToInput`: maps mount points to (inputName, subpath) for path resolution

### 6.3 CLI Commands

- `nix eval`: uses `getRootValue()` instead of `getRoot()` (AttrCursor)
- `nix build`: same eval path, derivations cached via ExprCached thunks
- `nix search`: navigates Value tree instead of AttrCursor tree
- `nix flake show/check`: same pattern

All commands use `forceValue()` on Values — no cache-specific API calls.

### 6.4 Settings

- `eval-cache` (bool, default true): enables/disables the eval cache
- `verify-eval-cache` (bool, default false): evaluates both warm and cold paths,
  compares results, logs warnings on mismatch. For debugging only.

---

## 7. Testing Strategy

### 7.1 Test Pattern

Every test follows the same structure:

```bash
# 1. Cold eval: populate cache
nix build "$dir#attr"

# 2. Warm eval: verify cache serves without evaluation
NIX_ALLOW_EVAL=0 nix build "$dir#attr"

# 3. Modify input
echo "new content!!" > "$dir/file.txt"  # DIFFERENT SIZE
git add file.txt && git commit -m "modify"

# 4. Verify invalidation
NIX_ALLOW_EVAL=0 expectStderr 1 nix build "$dir#attr" \
  | grepQuiet "not everything is cached"

# 5. Cold re-eval
nix build "$dir#attr"

# 6. Verify new result cached
NIX_ALLOW_EVAL=0 nix build "$dir#attr"
```

`NIX_ALLOW_EVAL=0` is the key mechanism: it causes the root loader to throw
if evaluation is attempted, proving the result was served entirely from cache.

### 7.2 Test Categories

- **Core tests**: cold/warm cycle, per-attr invalidation, GC resilience, errors
- **Dep tests**: pathExists, readDir, flake inputs, partial tree, addPath
- **Output tests**: scalar types, JSON derivation, write-to, cursor paths
- **Recovery tests**: dep revert, alternating versions, three-way cycling
- **Volatile tests**: currentTime, mixed volatile/stable
- **Regression tests**: specific bug reproductions (lazy derivationStrict,
  storeForcedSibling overwrite, dep stealing)

### 7.3 Known Test Limitations

1. **Same-size file modification**: must use different-size content to avoid
   StatHashCache false hits.
2. **Warm sibling dep incompleteness**: not tested because `c = a + b` with
   warm-served `a`/`b` produces incomplete dep sets for `c`.
3. **autoGC interaction**: eval cache must be disabled in gc-auto.sh.
4. **Parallel evaluation**: tests are single-threaded; no concurrent eval tests.
