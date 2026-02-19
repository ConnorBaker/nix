# Nix Eval Trace: Implementation Notes

**Target audience:** Nix core team reviewing the implementation.

**System classification:** Deep constructive trace store with structural variant
recovery (BSàlC), demand-driven evaluation via articulation points (Adapton),
dynamic dependency discovery (Shake), and revision-based staleness detection
(Salsa). See `design.md` Section 2 for the full prior art taxonomy and
terminology glossary.

> **Terminology note.** This document uses the standard terminology defined in
> `design.md` Section 2. In particular: a *trace* is a recorded set of
> dependencies and result for an attribute (BSàlC *constructive trace*); the
> *verify path* checks whether a recorded trace's dep hashes still match current
> state (BSàlC *verifying trace*); *fresh evaluation* evaluates an expression
> from scratch, recording deps (Adapton *demand-driven recomputation*); and
> *record* stores a fresh evaluation's result and deps for future verification
> (BSàlC *trace recording*). *Constructive recovery* reuses a historical trace
> whose deps match the current state (BSàlC *constructive trace recovery*).

This document records what was tried, what failed, what was learned, and how the
current implementation works. It is a companion to `design.md` (architecture and
goals) and `upstreaming.md` (commit cleanup plan).

---

## 1. Development History

The eval trace system was developed across ~48 sessions on the
`vibe-coding/file-based-cas-trace-cache` branch. The implementation went through
several major architectural phases, moving from SQLite to CAS blob traces and
ultimately back to a pure SQLite backend.

### Phase 1: Foundation (Session 1--3)

**Decision: Replace AttrCursor with GC-allocated TracedExpr articulation points.**
The existing `AttrCursor` API requires callers to use cache-specific methods
(`getString()`, `getAttrs()`, etc.). Instead, `TracedExpr` is an `Expr` subclass
-- an *articulation point* in the Adapton sense -- whose `eval()` method
transparently serves verified results or falls through to fresh evaluation. CLI
commands use standard `forceValue()` without knowing whether tracing is active.
This is the Adapton transparency property: tracing is invisible to callers.

**Decision: RAII thread-local dep recording (Adapton DDG construction).**
`DependencyTracker` uses a thread-local pointer set on construction -- analogous
to Adapton's dependency graph (DDG) construction during `force`. All dep
recording goes through `DependencyTracker::record()`, which appends to a session-
wide `std::vector<Dep>`. Each tracker records its start index; the range
`[startIndex, sessionTraces.size())` represents deps recorded during that
tracker's lifetime. Dependencies are discovered dynamically during evaluation
(Shake-style dynamic dependencies), not declared statically.

**Rejected: CachedValue dep type.**
Early prototype had a dep type for "this value was served from a trace." This was
evaluation-order dependent (whether a sibling was verify-served depended on prior
access) and broke deterministic traces -- violating the BSàlC requirement that
traces be reproducible for a given input state.

**Rejected: Single CopiedPath for filtered paths.**
Treating filtered `builtins.path` like unfiltered (single NAR hash for the whole
result) caused over-invalidation: any file change in the filtered directory
invalidated the trace, even if the filter excluded the changed file. Added
`NARContent` (type 11) for per-file NAR hashing that captures both content and
executable bit.

### Phase 2: Sibling Tracing (Session 4--6)

**Problem: Side-effect siblings not traced.**
When `navigateToReal()` walks the eval tree to `A.B.C`, siblings of `B` are
forced as side effects (e.g., `derivationStrict` forcing `stdenv` while building
`hello`). Without tracing, these side-effect evaluations are lost.

**Solution: origExpr/origEnv wrappers (Adapton articulation points).**
`navigateToReal()` wraps sibling thunks with `TracedExpr` instances that have
`origExpr`/`origEnv` set. These are Adapton articulation points: when later
forced, these wrappers try the verify path first (BSàlC VT check), then fall
back to fresh evaluation of the original thunk expression (Adapton demand-driven
recomputation).

### Phase 3: Scale Optimization (Session 7--10)

**Bug: ExprOrigChild caused 6.7M dep recordings on opencv.**
`ExprOrigChild::eval()` evaluates the parent to resolve a child. Without dep
suspension, evaluating `buildPackages` (= all of nixpkgs) as a parent recorded
10,000+ file deps into each child's trace. Combined with redundant trace replay
(Adapton change propagation) after fresh `evaluateFresh`, this compounded to
O(2^K) dep accumulation.

**Fix:** `SuspendDepTracking` RAII guard in `ExprOrigChild::eval()` prevents
recording parent deps into the child's DDG. Redundant trace replay after
origExpr verify miss was removed.

**Bug: Recursive sibling recording caused 88x slowdown.**
`storeForcedChildren()` descended recursively through sibling attrsets, causing
50K--100K+ SQLite operations for nixpkgs-scale evaluations.

**Fix:** Single-level `recordSiblingTrace()` records only immediate children of
forced siblings (BSàlC CT recording at one level only).

### Phase 4: Hashing (Session 11--14)

**Decision: Use Nix's built-in HashSink for BLAKE3.**
Rather than introducing a separate hashing library, all BLAKE3 hashing goes
through `nix::Hash` / `nix::HashSink`. The `Blake3Hash` struct provides a 32-byte
fixed-size, stack-allocated wrapper with zero heap allocation.

**Added: NARContent dep type** for filtered `builtins.path`. Unlike `Content`
(raw file bytes), `NARContent` captures the executable bit via NAR serialization.
This detects `chmod +x` changes that `Content` would miss.

### Phase 5: StatHashCache (Session 15--18)

**Decision: Two-level persistent cache for file hashes (BSàlC VT verification
accelerator).**
L1 is a `boost::concurrent_flat_map` (session-scoped, 64K entry cap). L2 is
bulk-loaded from the `StatHashCache` table in TraceStore's
`eval-trace-v2.sqlite` database at session start, and dirty entries are flushed
back at session end. Both levels are keyed on file stat metadata `(path, dev,
ino, mtime_sec, mtime_nsec, size, dep_type)`. This cache accelerates the BSàlC
VT verification step: validating dep hashes reduces to a series of `lstat()`
calls for unchanged files.

*(Historical note: Prior to Session 56, L2 was a separate SQLite database at
`~/.cache/nix/stat-hash-cache-v2.sqlite`. This was merged into the single
`eval-trace-v2.sqlite` database to simplify the storage model. The
StatHashCache singleton no longer owns any SQLite connection; TraceStore handles
all persistence through its single connection/transaction.)*

**Removed: epochBloom filter.** A 64-bit bloom filter guarded `epochMap.find()`
for trace replay memoization (Adapton change propagation). Measurement across
nixpkgs evals (hello, opencv, firefox, nix search) showed epochMap consistently
holds ~4,000 entries, fully saturating all 64 bits. Result: 97.5% false positive
rate, only 0.9% of calls actually filtered. Even 128 bits would saturate at
N=4,000. Removed entirely -- `boost::unordered_flat_map::find()` miss is already
fast (open addressing, single probe on miss).

### Phase 6--7: Schema Redesign (Session 19--22)

*(These phases applied to an intermediate SQLite-based design that was later
replaced by CAS blob traces, and then replaced again by a pure SQLite backend
in Sessions 49--50. Lessons learned still informed the final design.)*

**DAG-based traces (BSàlC CT with hierarchical structure).** Parent-child dep
relationships encoded via `parent_set_id` FK instead of ParentContext dep entries.
Each trace stores only direct (non-inherited) deps, with parent chain traversal
via recursive CTE.

**String interning.** File paths interned into a `Strings` table, with
`DepEntries` using integer FKs. Reduced unique constraint overhead from long text
B-tree to integer comparison.

**Direct BLAKE3 constructive recovery.** Replaced XOR-based pre-filter and
`diffBasedRecovery` with a single indexed lookup by BLAKE3 hash of current deps
-- a direct BSàlC constructive trace lookup.

### Phase 8: Zero-Alloc Hash Pipeline (Session 23--26)

**Blake3Hash value type.** 32-byte `std::array<uint8_t, 32>` with comparison,
hashing, and hex conversion. Eliminated all heap allocations in the hash pipeline.

**StatHashCache v2.** Stores hashes as 32-byte BLOBs (not hex strings). Bulk L2
load on first access. Stat-passing API: `lookupHash` returns
`LookupResult{hash, stat}`, and `storeHash` accepts the stat to avoid redundant
`lstat()`.

### Phase 9: CAS Blob Traces (Session 27--40)

**Architectural pivot: replace monolithic SQLite with store-based CAS blobs.**
*(Note: This CAS blob approach was itself later replaced by a pure SQLite backend
in Phase 12, Sessions 49--50. See that section for details.)*

The SQLite-based design had persistent correctness issues:
- Cache coherence between session caches and persistent DB
- UPSERT race conditions invalidating traces
- No natural GC integration (separate cleanup needed)
- 91% verify hit rate due to index corruption patterns

CAS blob traces solve all of these:
- Immutable once stored (no overwrites, no races)
- GC'd naturally as store objects
- 100% verify hit rate (if trace exists in store, it's valid)
- Content addressing provides natural dedup

**Deleted:** `builtins/eval.cc` (builtin:eval + builtin:eval-dep marker builders),
`trace-store.hh/cc` (TraceStore SQLite layer).

**Added:** `trace-cache-store.hh/cc`, `eval-index-db.hh/cc`,
`trace-hash.hh/cc`.
*(These files were subsequently replaced in Phase 12.)*

**Constructive recovery redesign (BSàlC CT recovery).** The SQLite design used
derivation path determinism (same deps -> same drv path -> same output). CAS blob
traces embed the result in the trace, so different results produce different
trace paths even with identical deps. Solution: `DepHashRecovery` table maps
`(context_hash, attr_path, dep_hash) -> trace_path`, where `dep_hash` is computed
from deps *without* the result -- a constructive trace recovery index that
enables serving historical results without recomputation.

### Phase 10: Final Optimization (Session 41--48)

**Batched writes.** StatHashCache wraps all writes in a single `SQLiteTxn`
(committed at destructor). Reduces SQLite write overhead from per-dep to per-
session.

**Sort-once deps.** `sortAndDedupDeps()` called once in `record()`. Pre-sorted
variants (`serializeEvalTraceSorted`, `computeTraceHashFromSorted`, etc.)
used for serialization and hashing -- avoids redundant sorting.

**Performance finding: sort-once-on-load anti-pattern.** An attempt to sort deps
in `loadFullTrace()` (so callers could skip sorting) caused a 44.8% regression in
loadTrace time. On warm recovery commits, 326 traces are loaded but only ~31 need
sorted output (for `computeTraceHashFromSorted`). The other 295 loads (for
`verifyTrace`, `computeTraceDelta`, hash recomputation) iterate deps in arbitrary
order. Sorting on load penalized all 326 loads for the benefit of 31. The fix:
sort lazily at the call site.

**Per-tracker dedup (DDG deduplication).** `DependencyTracker::record()`
deduplicates by `(type, source, key)` within the active tracker scope using a
`DepKey` hash set. Note: dedup only when an active tracker exists
(`SuspendDepTracking` sets the tracker to null, but recording must still append
to `sessionTraces` for thunk trace replay -- Adapton change propagation).

**recordDep uses maybeLstat.** Replaced separate `exists()` + `lstat()` calls
with a single `maybeLstat()` that returns `std::optional<struct stat>`.

### Phase 11: Two-Object Trace Model (Session 49+)

*(This phase refined the CAS blob approach that was later replaced by a pure
SQLite backend in Phase 12.)*

**Problem: Storage bloat from inline deps.**
Analysis of a typical nixpkgs evaluation showed 465 MB of trace data in an
837 MB store, with 99.99% of trace size consumed by inline dependency arrays.
Near-zero CAS deduplication was observed because each v1 trace included both
the result and deps -- so even attributes with identical traces produced
different store paths (due to different results). The single-blob model meant
that every attribute paid the full storage cost of its dep array, and no
sharing was possible.

**Solution: Split into result trace + zstd-compressed dep blob.**
Each attribute's trace entry now consists of two store objects:

1. **Result trace** -- a slim CBOR blob containing only the result (BSàlC CT
   result value), parent reference, context hash, and a store path reference
   to the dep blob. Format version bumped to v2:
   `{v:2, r:<result>, p:<parent|null>, c:<ctx|null>, ds:<dep-set-path>}`.

2. **Dep blob** -- a zstd-compressed CBOR array of deps (BSàlC trace = key +
   dep hashes), stored as a Text CA blob with name `"eval-deps"` and no store
   references. Because it contains only deps (no result or parent), attributes
   with identical dep signatures produce identical store paths and share a
   single object via CAS deduplication.

**Storage reduction estimate: ~89%.** The dep blobs compress well (sorted,
repetitive structure), and CAS deduplication eliminates redundant copies across
attributes that read the same files.

**GC coupling.** The result trace lists the dep blob path in its references,
so the dep blob is kept alive as long as any result trace referencing it is
reachable. When all result traces referencing a dep blob are collected, the
dep blob becomes garbage and is collected on the next GC pass.

**v1 format removal.** The v1 single-blob trace format (with inline `"d"` array)
was removed. `loadTrace()` rejects traces with `"v": 1`. Existing v1 traces in
the store are harmless (they become unreferenced garbage) but cannot be loaded.

### Phase 12: Pure SQLite TraceStore (Sessions 49--50)

**Architectural pivot: replace CAS blob traces with a single SQLite TraceStore.**

The CAS blob trace design (Phases 9--11) eliminated the earlier SQLite coherence
bugs by making traces immutable store objects. However, it introduced its own
set of problems:

- **GC budget interference.** CAS blob traces (8--11 Text CA store paths per
  traced expression) consumed autoGC's single-pass deletion budget, causing
  gc-auto test failures (Bug 20). Disabling the trace system in gc-auto was a
  workaround, not a fix.
- **Deferred write complexity.** The `DeferredColdStore` pattern (FIFO queue,
  `stagedAttrPaths` dedup set, `defer()`/`flush()` lifecycle) existed solely
  to ensure parent trace paths were set before children read them. This added
  a non-trivial coordination layer.
- **CBOR + zstd serialization overhead.** Two-object traces required CBOR
  encoding via `nlohmann::json`, zstd compression for dep blobs, and
  store-path-based references between the result trace and its dep blob.
  Each layer added complexity and potential for non-determinism (Pitfall 25).
- **Store coupling.** Trace storage depended on `addTextToStore()` and the
  store's GC and reference graph. Constructive recovery required
  `DepHashRecovery` to map dep hashes back to trace store paths, which is a
  fundamentally store-aware concern leaking into the trace system.

The pure SQLite backend eliminates all of these by storing everything in a
single database at `~/.cache/nix/eval-trace-v2.sqlite`, with no store
dependency for trace storage.

**1. Replaced CAS blob traces with pure SQLite TraceStore (BSàlC trace store).**
The entire `TraceCacheStore` (CAS blob backend) and `EvalIndexDb` (lightweight
SQLite index) were replaced by a single `TraceStore` class. Traces are no
longer store objects -- they are rows in SQLite tables. No `addTextToStore()`,
no store path references, no GC coupling. The trace system is now a fully
self-contained BSàlC trace store.

**2. Eight-table schema.**
`Strings`, `AttrPaths`, `Results`, `DepKeySets`, `Traces`, `CurrentTraces`,
`TraceHistory`, `StatHashCache`.
Dep keys are content-addressed in the `DepKeySets` table (keyed by
`struct_hash = BLAKE3(dep types + sources + keys)`). Each `Traces` row
references a shared `DepKeySets` row via `dep_key_set_id` FK and stores its
own `values_blob` (hash values in positional order). Result values are
deduplicated via the `Results` table.
`CurrentTraces` maps `(context_hash, attr_path_id)` to the current trace and
result (the BSàlC VT lookup table). `TraceHistory` stores all historical
trace-result pairs for each attribute (the BSàlC CT recovery store). See
`design.md` Section 5.2 for the complete schema.

**3. Direct recording (BSàlC CT trace recording).**
No deferred write queue. `record()` executes SQL statements directly and
returns an `AttrId` (integer row ID) immediately. The entire `DeferredColdStore`
struct -- deferred writes vector, `stagedAttrPaths` set, `isStaged()`, `defer()`,
`flush()` -- was eliminated. Parent-child ordering is handled naturally by
evaluation order: parents are always recorded before children because evaluation
is depth-first (Adapton demand-driven traversal).

**4. HashSink-based trace hashing.**
Replaced CBOR-serialized dep hashing with direct `BLAKE3 HashSink` feeding.
Domain-separated fields: `"T"` type, `"S"` source, `"K"` key, `"H"` hash for
trace hash; structural hash omits `"H"`. This eliminated the `nlohmann/json`
dependency from `trace-hash.cc`.

**5. Epoch column for verify-path validation (removed).**
*(Historical note: an epoch column analogous to Salsa's revision counter was
originally added to detect parent staleness via O(1) integer comparison. This
was superseded by ParentContext deps — each child now records the parent's
`trace_hash` as a dep, and parent staleness is detected via normal dep hash
verification. The Attributes table and epoch mechanism no longer exist.)*

**6. Parent Merkle chaining (removed).**
*(Historical note: parent-aware constructive recovery originally used Merkle
chaining — `computeTraceHashWithParent()` mixed the parent's `trace_hash` into
the child's recovery lookup key. This was removed when dep ownership was
separated: each trace now stores only its own deps, and parent invalidation
no longer cascades to children. The `computeTraceHashWithParent*` functions
and `getTraceFullHash()` / `traceHashCache` were deleted.)*

**7. Dep key factoring (DepKeySets).**
Dep storage is split into shared key sets and per-trace hash values. The
`DepKeySets` table stores content-addressed dep key sets (keyed by
`struct_hash = BLAKE3(dep types + sources + keys)`). Each `Traces` row
references a `DepKeySets` row and stores its own `values_blob`. Traces with the
same dep structure share a single `DepKeySets` row. Structural variant recovery
loads only key sets (no `values_blob` decompression needed).

**Files changed.**
- **Created:** `trace-store.cc`, `trace-store.hh`
- **Deleted:** `trace-cache-store.cc`, `trace-cache-store.hh`, `eval-index-db.cc`,
  `eval-index-db.hh`
- **Modified:** `trace-cache.cc` (AttrId replaces StorePath, direct recording),
  `trace-cache.hh` (`dbBackend` replaces `storeBackend`),
  `trace-hash.cc/hh` (removed CBOR, kept HashSink hashing),
  `meson.build` files

**Bugs found and fixed during this phase:**

*Bug A: base_trace_id fails for 0-dep parents.*
Trace IDs are content-addressed by hash. Zero deps always produces the same
trace ID. When a parent's value changes but it has zero deps, child
verification incorrectly passed because `base_trace_id` matched (same empty
trace). Fix: epoch mechanism (Salsa revision counter, described above).

*Bug B: Epoch breaks parent-aware constructive recovery.*
Epochs only increment -- they are not reproducible across sessions (unlike
Salsa's revision counter, which is deterministic). When a root attribute is
recovered and UPSERTed (epoch increments), recovery entries stored with old
epochs never match. Fix: Merkle identity hash (described above) replaces epoch
in recovery keys -- it is reproducible (same state produces the same hash).

*Bug C: Parent value hash fails for FullAttrs.*
FullAttrs encodes only child names, not child values. Two attrsets with the
same child names but different child values produce the same value hash. Fix:
Merkle identity hash chains through ancestors, propagating root dep changes
through the entire tree.

*Bug D: computeIdentityHash deadlocks in record.*
`record()` holds `_state->lock()`, then calls `computeIdentityHash()` which
tries to acquire the same non-recursive lock. Fix: compute the identity hash
outside the lock scope.

### Phase 14: Lazy Structural Dependency Tracking (Session 60+)

**Problem: Whole-file Content deps cause over-invalidation for structured data.**
When `fromJSON(readFile "flake.lock")` is used, a `Content` dep is recorded on
the entire file. Any change — even to an unused input's `rev` — invalidates the
trace. Similarly, a `Directory` dep on a `readDir` result invalidates whenever
any entry is added, removed, or changes type — even if only specific entries
are accessed. This is unnecessarily conservative for structured data where only
specific scalar values are accessed.

**Solution: StructuredContent deps with two-level verification override.**
Added `StructuredContent` (DepType 12), a Hash Oracle dep that records the
BLAKE3 hash of a scalar value at a specific data path within a JSON file,
TOML file, or directory listing. During verification, if the whole-file
`Content` dep or whole-listing `Directory` dep fails but all
`StructuredContent` deps pass (accessed values unchanged), the trace remains
valid.

**Implementation: Lazy thunks via `ExprTracedData` Expr subclass.**
When `fromJSON`/`fromTOML` parses a string whose BLAKE3 hash matches the
`ReadFileProvenance` set by the preceding `readFile`, the result is returned
as a lazy attrset/list. Each child is a thunk wrapping an `ExprTracedData`
node backed by a format-agnostic `TracedDataNode` virtual interface
(`JsonDataNode` wraps `nlohmann::json`, `TomlDataNode` wraps `toml::value`).

- **Containers:** Create nested lazy attrsets/lists. No dep recorded.
- **Scalar leaves:** Record `StructuredContent` dep with BLAKE3 of canonical
  value. Dep key: `"filepath\tf:datapath"` (tab separator, `f` = format tag:
  `'j'` for JSON, `'t'` for TOML, `'d'` for directory entries).

**Provenance threading.** Thread-local `ReadFileProvenance` set by `readFile`,
consumed by `fromJSON`/`fromTOML`. Content hash validation ensures structural
deps are only used when the string came directly from `readFile` (not modified).

**DOM caching in verifyTrace.** Thread-local parsed DOM caches avoid re-parsing
when multiple `StructuredContent` deps reference the same file.

**Two-level verification in verifyTrace.** First pass verifies non-structural
deps and defers `StructuredContent` deps. If only `Content` and/or `Directory`
failures exist and structural deps cover those files/directories, structural
deps are verified. If all pass, `Content`/`Directory` failures are overridden.

**Key-set safety with `mapAttrs`/`attrNames`.** The lazy attrset eagerly
materializes the full key set but records no dep for it — only the `Content`
dep from `readFile` covers the key set. The two-level override only activates
when `StructuredContent` deps exist in the trace. If code only iterates keys
(e.g., `mapAttrs` without forcing values, `attrNames`), no `StructuredContent`
deps are recorded and `Content` alone controls → any file change invalidates.
If specific values are accessed, the cached result is the leaf value (not the
attrset) and key-set changes are irrelevant. This is the correct behavior for
flake-compat patterns where `mapAttrs` is used over lock file inputs and
adding/removing unused inputs should not invalidate traces.

**List allStrings fix.** `TracedExpr::evaluateFresh()` eagerly forces all list
elements to check if they're all strings (for compact storage). This forcing is
wrapped in `SuspendDepTracking` to prevent `ExprTracedData` thunks from recording
`StructuredContent` deps into the list trace's `DependencyTracker`. Without this,
adding an element to a JSON array could make the `Content` dep fail while all
existing `StructuredContent` deps pass, incorrectly overriding the list's trace.

**Shape observation tracking.** Shape-observing builtins (`length`,
`attrNames`, `hasAttr`) record `StructuredContent` shape deps when
operating on traced data containers. Implementation uses a thread-local
`tracedContainerMap` (in `dependency-tracker.cc`, anonymous namespace)
mapping `const void*` → `TracedContainerProvenance` (depSource, depKey,
dataPath, format). The map key is a stable internal pointer that
survives Value copies: `Bindings*` for attrsets, first-element `Value*`
for lists. Empty lists cannot be tracked (no stable internal pointer)
but this is safe — they have no leaf `StructuredContent` deps.
`ExprTracedData::eval()` registers containers; builtins call
`maybeRecordListLenDep()` / `maybeRecordAttrKeysDep()` in `primops.cc`.
The map is cleared on root `DependencyTracker` construction via
`onRootConstruction()`. `escapeDataPathKey()` now quotes `#` to prevent
ambiguity with shape suffixes (`#len`, `#keys`). `computeCurrentHash()`
in `trace-store.cc` detects `#len`/`#keys` suffixes and computes shape
hashes during verification.

**Data path key escaping.** Object keys containing `.`, `[`, `]`, `"`, `\`, or `#`
are quoted using `"..."` with `\"` and `\\` escaping (matching Nix attr-path
conventions). `parseDataPath` in `trace-store.cc` handles bare keys, quoted
keys (with unescape), and array indices `[N]`.

**Key bug: SQLite TEXT truncation at null bytes.** The original dep key format
used `\0` as separator (`"filepath\0f:datapath"`). SQLite TEXT columns truncate
at null bytes, so the interned key contained only the filepath. Fixed by
using `\t` (tab) as separator.

**Directory two-level override (readDir).** Extended the StructuredContent
two-level override to directory listings. `readDir` now returns an
`ExprTracedData`-wrapped attrset when eval-trace is active. The `Directory`
dep (BLAKE3 of sorted listing) is the coarse dep; per-entry
`StructuredContent` deps with format tag `'d'` are the fine-grained deps.

- `DirDataNode` / `DirScalarNode` (in `primops.cc`): `TracedDataNode`
  subclasses for directory entries. `DirDataNode` is the root container
  (one child per entry name). `DirScalarNode` is the leaf — its
  `canonicalValue()` returns the type string (`"regular"`, `"directory"`,
  `"symlink"`, `"unknown"`) via `dirEntryTypeString()`.
- `dirEntryTypeString()` (in `dependency-tracker.hh/.cc`): shared helper
  mapping `std::optional<SourceAccessor::Type>` to a canonical string.
  Used by both `DirScalarNode::canonicalValue()` and `computeCurrentHash`
  `'d'` format handler.
- `dirListingCache` (in `trace-store.cc`): thread-local
  `std::unordered_map<std::string, SourceAccessor::DirEntries>`, alongside
  `jsonDomCache` and `tomlDomCache`. Avoids re-reading directory listings
  when multiple `StructuredContent` deps with format `'d'` reference the
  same directory within a single `verifyTrace` call. Cleared by
  `clearDomCaches()`.
- `computeCurrentHash` `'d'` handler: re-reads the directory listing
  (cached via `dirListingCache`), looks up the entry by name, and hashes
  the type string via `dirEntryTypeString()`.
- `verifyTrace` decision tree: `Directory` dep failures are now
  override-eligible alongside `Content` failures. If a `Directory` dep
  fails but all `StructuredContent` deps covering that directory pass,
  the `Directory` failure is overridden.

**Files added/modified:**
- `traced-data.hh` (new): `TracedDataNode` + `ExprTracedData`
- `eval-trace-deps.hh`: `StructuredContent = 12`
- `dependency-tracker.hh/cc`: `ReadFileProvenance`, `resolveProvenance`, `TracedContainerProvenance`, `registerTracedContainer`/`lookupTracedContainer`/`clearTracedContainerMap`, `dirEntryTypeString`
- `json-to-value.hh/cc`: `JsonDataNode`, `parseTracedJSON`, `ExprTracedData::eval()`
- `primops/fromTOML.cc`: `TomlDataNode`, `parseTracedTOML`
- `primops.cc`: provenance wiring in `readFile`/`fromJSON`, `DirDataNode`/`DirScalarNode` for `readDir`
- `trace-store.cc`: `computeCurrentHash` for StructuredContent (including `'d'` format), two-level `verifyTrace` (Content + Directory override), DOM caches (`jsonDomCache`/`tomlDomCache`/`dirListingCache`), `navigateJson`/`navigateToml` helpers
- `traced-data.cc` (new test): 94 GTest unit tests (62 JSON/TOML + 32 directory)

---

## 2. File Layout

### Source Files

*(Note: This table reflects the state after Phase 13 file reorganization.
The dep-tracker and stat-hash-cache modules have been split and consolidated.)*

#### `nix::` namespace — evaluator-facing types

| File | Description |
|------|-------------|
| `src/libexpr/include/nix/expr/eval-trace-deps.hh` | Dep vocabulary types: `DepType`, `DepKind`, `Blake3Hash`, `DepHashValue`, `Dep`, `DepKey`, `DepRange`, `StructuredFormat`, `ShapeSuffix`, inline helpers (header-only, includes `depTypeName()`, `depKind()`, `depKindName()`, `isVolatile()`, `isContentOverrideable()`, `buildStructuredDepKey()`, `formatStructuredDepKey()`, etc.) |
| `src/libexpr/include/nix/expr/dependency-tracker.hh` | `DependencyTracker` (Adapton DDG builder), `SuspendDepTracking`, dep hash function declarations, `StatHashEntry` bridge API, `ReadFileProvenance`, `resolveProvenance`, `dirEntryTypeString` |
| `src/libexpr/dependency-tracker.cc` | Dep recording, hashing, input resolution + internal StatHashCache (L1 concurrent_flat_map + L2 bulk-loaded from TraceStore, dirty tracking for flush) + provenance threading + `dirEntryTypeString` |
| `src/libexpr/include/nix/expr/traced-data.hh` | `TracedDataNode` virtual interface, `ExprTracedData` Expr subclass for lazy structural dep tracking |
| `src/libexpr/include/nix/expr/eval-trace-context.hh` | `EvalTraceContext` struct (evalCaches, fileContentHashes, mountToInput, epochMap) |
| `src/libexpr/eval-trace-context.cc` | `EvalTraceContext` methods (recordThunkDeps, replayMemoizedDeps, reset, flush) |

#### `nix::eval_trace` namespace — trace system internals

| File | Description |
|------|-------------|
| `src/libexpr/include/nix/expr/trace-result.hh` | Result vocabulary types: `ResultKind`, `CachedResult`, all result structs (header-only) |
| `src/libexpr/include/nix/expr/trace-cache.hh` | Public API: `TraceCache`, performance counters |
| `src/libexpr/trace-cache.cc` | `TracedExpr` (Adapton articulation point), `ExprOrigChild`, `SharedParentResult`, verify/record/recover loop |
| `src/libexpr/include/nix/expr/trace-store.hh` | `TraceStore`: pure SQLite backend (BSàlC trace store) |
| `src/libexpr/trace-store.cc` | SQLite backend: schema, verify, record, constructive recovery, verifyTrace (BSàlC VT/CT operations), `computeCurrentHash` ('j'/'t'/'d' formats), DOM caches |
| `src/libexpr/include/nix/expr/trace-hash.hh` | HashSink-based trace hashing |
| `src/libexpr/trace-hash.cc` | HashSink trace hashing implementations |

**Removed in Phase 12:** `trace-cache-store.hh/cc` (CAS blob backend),
`eval-index-db.hh/cc` (SQLite index for trace store paths).

**Removed in Phase 13 (file reorganization):** `dep-tracker.hh/cc` (split into
header-only `eval-trace-deps.hh` + `dependency-tracker.hh/cc`),
`stat-hash-cache.hh/cc` (folded into `dependency-tracker.cc` as an
anonymous-namespace implementation detail).

### Modified Files

| File | Description |
|------|-------------|
| `src/libexpr/eval.cc` | DependencyTracker integration (Adapton DDG), `evalFile` Content dep |
| `src/libexpr/eval-trace-context.cc` | `EvalTraceContext` methods (recordThunkDeps, replayMemoizedDeps, reset, flush) |
| `src/libexpr/primops.cc` | `recordDep` calls for all file-accessing builtins + `ReadFileProvenance` in readFile, provenance consumption in fromJSON, `DirDataNode`/`DirScalarNode` + ExprTracedData path in readDir |
| `src/libexpr/primops/fetchTree.cc` | UnhashedFetch dep recording |
| `src/libexpr/primops/fromTOML.cc` | `TomlDataNode`, `parseTracedTOML`, provenance consumption |
| `src/libexpr/json-to-value.cc` | `JsonDataNode`, `parseTracedJSON`, `ExprTracedData::eval()` vtable |
| `src/libexpr/include/nix/expr/eval-trace-context.hh` | `EvalTraceContext` struct (evalCaches, fileContentHashes, mountToInput, epochMap) |
| `src/libexpr/include/nix/expr/eval-settings.hh` | `trace-cache`, `verify-trace-cache` settings |
| `src/libcmd/installable-attr-path.cc` | `NIX_ALLOW_EVAL` guard, TracedExpr root articulation point creation |
| `src/libcmd/installable-flake.cc` | Trace lifecycle, `computeStableIdentity()` |
| `src/libflake/flake.cc` | Input accessor mappings, `traceCtx->mountToInput` |
| `src/nix/eval.cc` | `getRootValue()` instead of `getRoot()` |
| `src/nix/search.cc` | Value-based API instead of AttrCursor |
| `src/nix/flake.cc` | Value-based API instead of AttrCursor |

### Test Files

| File | Description |
|------|-------------|
| `tests/functional/flakes/eval-trace-core.sh` | Core verify/record cycle, per-attr invalidation, GC resilience |
| `tests/functional/flakes/eval-trace-deps.sh` | pathExists, readDir, flake input, partial tree |
| `tests/functional/flakes/eval-trace-output.sh` | Scalar types, JSON output, write-to |
| `tests/functional/flakes/eval-trace-recovery.sh` | Dep revert, alternating versions, constructive recovery |
| `tests/functional/flakes/eval-trace-volatile.sh` | currentTime, mixed volatile/non-volatile |
| `tests/functional/eval-trace-impure-core.sh` | --file, --expr, selective invalidation |
| `tests/functional/eval-trace-impure-deps.sh` | getEnv, currentSystem, pathExists, hashFile, addPath |
| `tests/functional/eval-trace-impure-advanced.sh` | Deep origExpr, dep suspension, fat parent |
| `tests/functional/eval-trace-impure-output.sh` | Cursor eval, JSON, revert constructive recovery |
| `tests/functional/eval-trace-impure-regression.sh` | Specific bug regressions |
| `src/libexpr-tests/eval-trace/traced-data.cc` | GTest: lazy structural dep tracking (JSON/TOML + directory; covers scalar access, two-level override, shape deps, provenance, directory entries) |
| `src/libexpr-tests/eval-trace/trace-store.cc` | GTest: TraceStore (schema, serialization, verify/record/recover) |

---

## 3. Critical Implementation Details

### 3.1 TracedExpr Eval Loop -- Adapton Articulation Point (trace-cache.cc)

The `TracedExpr::eval()` method is the core dispatch point. As an Adapton
articulation point, it intercepts `forceValue()` calls and dispatches between
verification (BSàlC VT), fresh evaluation (Adapton demand-driven recomputation),
and constructive recovery (BSàlC CT):

```
eval(state, env, v):
  1. If origExpr set and verify path succeeds (BSàlC VT check):
     -> serve traced result, replay trace to parent DDG (Adapton change propagation)
  2. If origExpr set and verify path fails (VT miss):
     -> evaluate origExpr directly (Adapton fresh eval, not via navigateToReal)
     -> record result as trace (BSàlC CT recording), wrap children
     -> v = *target (real value, not materialized)
  3. If no origExpr, verify path succeeds (BSàlC VT check):
     -> serve traced result
  4. If no origExpr, fresh evaluation path (Adapton demand-driven recomputation):
     -> navigateToReal() to get real tree Value*
     -> forceValue(*target)
     -> if derivation: force drvPath (Shake dynamic dep discovery)
     -> record: compute trace hash + record trace (BSàlC CT recording)
     -> materializeResult: create TracedExpr child articulation points
     -> recordSiblingTrace: speculatively record already-forced siblings
```

Key correctness guards:
- `if (origExpr) v = *target;` -- origExpr wrappers produce real values, not
  materialized TracedExpr children, to prevent infinite recursion through
  navigateToReal on subsequent accesses.
- `if (!origExpr && isDerivation) forceValue(drvPath)` -- eager drvPath forcing
  triggers `derivationStrict`, capturing env-processing deps (Shake-style
  dynamic dependency discovery). Skipped for origExpr wrappers to avoid
  infinite recursion via `buildPackages = self`.
- `dynamic_cast<TracedExpr*>` guard -- prevents double-wrapping due to
  `derivation.nix` Value* aliasing (`default.out` shares Value* with `default`).

### 3.2 DependencyTracker Session Model -- Adapton DDG Builder (dependency-tracker.hh/cc)

The `DependencyTracker` constructs the dynamic dependency graph (DDG) during
evaluation, analogous to Adapton's DDG construction during `force`. Dependencies
are discovered dynamically (Shake-style), not declared statically.

```
Thread-local state:
  activeTracker: DependencyTracker* (null when suspended)
  sessionTraces:   vector<Dep>      (all deps from this session)

DependencyTracker:
  previous:       saved activeTracker (restored on destruction)
  mySessionTraces:  &sessionTraces
  startIndex:     sessionTraces.size() at construction
  replayedRanges: index ranges from pre-existing thunks (Adapton change propagation)
  recordedKeys:   DepKey dedup set for this tracker's scope

record(dep):
  if activeTracker:
    if dep.key not in activeTracker->recordedKeys:
      recordedKeys.insert(dep.key)
      sessionTraces.push_back(dep)
  else:
    sessionTraces.push_back(dep)  // no dedup during suspension

collectTraces():
  result = sessionTraces[startIndex..current]
  for range in replayedRanges:
    result += range.deps[range.start..range.end]
  return result
```

`SuspendDepTracking` is an RAII guard that temporarily sets `activeTracker` to
null. This is used in `ExprOrigChild::eval()` to prevent "fat parent" dep
explosion -- without suspension, evaluating a parent like `buildPackages` (= all
of nixpkgs) would record 10,000+ deps into each child's DDG, violating the
per-attribute trace granularity goal.

### 3.3 StatHashCache Architecture -- BSàlC VT Verification Accelerator (internal to dependency-tracker.cc)

The StatHashCache accelerates the BSàlC VT verification step by caching file
hashes keyed on stat metadata. This reduces dep verification to a series of
`lstat()` system calls for unchanged files. It is an implementation detail
inside `dependency-tracker.cc` (anonymous namespace) — no public header exists.
Consumers access it through the `depHashFile`, `depHashPathCached`, and
`depHashDirListingCached` functions, which handle stat-cache lookup/store
internally.

The StatHashCache singleton has no SQLite connection of its own. TraceStore
owns the `StatHashCache` table in its single `eval-trace-v2.sqlite` database
and handles all persistence:

- **Construction:** TraceStore bulk-loads all `StatHashCache` rows into the
  in-memory L2 map via `loadStatHashEntries()`.
- **Evaluation:** New/updated hashes are stored in L1 and tracked as dirty
  entries (no SQLite writes during evaluation).
- **Destruction:** TraceStore calls `forEachDirtyStatHash()` to upsert all
  dirty entries back to the database within the same transaction as trace data.

```
StatHashCache (anonymous namespace singleton in dependency-tracker.cc):
  L1: boost::concurrent_flat_map<StatHashKey, Blake3Hash>
      Key: (dev, ino, mtime_sec, mtime_nsec, size, depType)
      Session-scoped, 64K entry cap

  L2: unordered_map<(path, depType), L2Entry>
      Bulk-loaded from TraceStore's SQLite at session start
      L1 promoted on L2 hit

  dirtyEntries: vector<StatHashEntry>
      Accumulated during evaluation, flushed by TraceStore at session end

  lookupHash(path, depType):
    lstat(path) -> stat (returned even on miss)
    if L1.find(key) -> return {hash, stat}
    if L2.find(path, depType) and stat matches -> promote to L1, return
    return {nullopt, stat}

  storeHash(path, depType, hash, stat):
    L1.insert(key, hash)
    dirtyEntries.push_back(entry)  // for TraceStore to flush later
    L2.update(path, depType, entry)  // keep L2 in sync
```

The bridge API between TraceStore and StatHashCache consists of two free
functions declared in `dependency-tracker.hh`:

- `loadStatHashEntries(vector<StatHashEntry>)`: populates L2 from DB rows
- `forEachDirtyStatHash(callback)`: iterates dirty entries for DB flush

This design ensures a single SQLite file (`eval-trace-v2.sqlite`) with a
single connection and transaction for all trace and stat-hash data.

### 3.4 Trace Hashing (trace-hash.hh/cc)

*(Note: In Phases 9--11, this module handled CBOR serialization using
`nlohmann::json` for `json::to_cbor()`/`json::from_cbor()` encoding, plus
zstd compression for dep blobs stored as Text CA store objects. In Phase 12
(Sessions 49--50), CBOR serialization, zstd compression, and the nlohmann::json
dependency were removed. The module now contains only HashSink-based trace
hashing.)*

**Trace hash computation** hashes sorted deps by feeding each dep's fields into a
`HashSink(HashAlgorithm::BLAKE3)` with domain-separated fields:

```
For each dep (sorted by type, source, key):
  sink("T")  sink(to_string(dep.type))
  sink("S")  sink(dep.source)
  sink("K")  sink(dep.key)
  sink("H")  hashDepValue(sink, dep.expectedHash)
```

Two hash variants serve the two constructive recovery strategies
(BSàlC CT recovery pipeline):
- `computeTraceHash`: deps with hashes (direct hash recovery key)
- `computeTraceStructHash`: dep keys only, no hashes (structural variant
  recovery grouping key — handles Shake-style dynamic dep instability)

### 3.5 Deferred Write Pattern (trace-cache-store.cc) -- Removed in Phase 12

*(This pattern existed in the CAS blob trace design, Phases 9--11. It was
eliminated in Phase 12 when `record()` was changed to execute SQL statements
directly and return an `AttrId` immediately. Retained here for historical
context.)*

Fresh-evaluation trace recording used a FIFO queue to ensure correct ordering:

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

~TraceCacheStore():
  flush()
```

Parent lambdas were pushed before children because evaluation is depth-first
(Adapton demand-driven order). This guaranteed that `parentEC->tracePath` was
set before children read it. In the pure SQLite backend, this coordination is
unnecessary -- parent rows exist in the database before children are recorded,
and `AttrId` (integer row ID) is available immediately after `INSERT`.

---

## 4. Bugs and Pitfalls Catalog

### 4.1 Infinite Recursion (4 bugs)

**Bug 1: Eager drvPath forcing for origExpr articulation points.**
*Symptom:* Stack overflow evaluating `blas.provider.pname` in nixpkgs.
*Root cause:* `evaluateFresh`'s `if (isDerivation) forceValue(drvPath)` triggers
`derivationStrict` for every derivation in the chain. With origExpr wrappers and
nixpkgs' `buildPackages = self` fixed-point: `blas -> perl -> libxcrypt ->
buildPackages.perl -> self.perl -> drvPath -> blackhole`.
*Fix:* `if (!origExpr && isDerivation) forceValue(drvPath)`. Also: `if (origExpr)
v = *target;` early blackhole exit.
*Lesson:* origExpr articulation points must never trigger eager side effects that
could recurse through the fixed-point.

**Bug 2: navigateToReal blackholes on materialized children.**
*Symptom:* Infinite loop when accessing verified FullAttrs children.
*Root cause:* Naively materializing TracedExpr children into the real tree causes
navigateToReal to walk through materialized thunks -> force parent -> blackhole.
*Fix:* `ExprOrigChild` resolves via parent's original expression, producing fresh
Value* objects (not the materialized ones).
*Lesson:* Materialized and real trees must be kept separate.

**Bug 3: materializeResult on origExpr paths.**
*Symptom:* Infinite recursion when accessing children of origExpr fresh-evaluation
results.
*Root cause:* `materializeResult()` replaces children in the real tree with
TracedExpr articulation points. Subsequent `navigateToReal()` walks through these
modified children and hits blackholed Values.
*Fix:* origExpr fresh evaluations use `v = *target` (raw values), not
`materializeResult()`.
*Lesson:* Never modify the real tree from origExpr paths.

**Bug 4: ExprOrigChild dep suspension missing.**
*Symptom:* 6.7M dep recordings for opencv, causing OOM.
*Root cause:* Without `SuspendDepTracking`, evaluating `buildPackages` as
parent recorded 10K+ deps into each child's DDG. Combined with redundant
trace replay (Adapton change propagation): O(2^K) compounding.
*Fix:* `SuspendDepTracking` RAII guard + remove redundant trace replay.
*Lesson:* Parent evaluation during child resolution must be isolated from the
child's DDG (dependency tracker scope must be suspended).

### 4.2 Dependency Tracking (6 bugs)

**Bug 5: Trace structure instability across fresh evaluations (Shake dynamic deps).**
*Symptom:* Hash-based constructive recovery misses for attributes that should match.
*Root cause:* A child's trace can have different structure between fresh
evaluations depending on evaluation order. `navigateToReal -> getOrEvaluateRoot
-> evalFile` runs inside the child's DependencyTracker when root is NOT cached,
but outside when cached. So the first fresh evaluation may record [Content
revert-dep.txt, ParentCtx] while the second records [Content test-revert.nix,
Content revert-dep.txt, ParentCtx]. This is a direct consequence of Shake-style
dynamic dependencies: the dep set is not statically known.
*Fix:* Hash recovery (O(1)) as fast path + structural variant recovery (O(V))
as fallback.
*Lesson:* Trace structure is not deterministic across evaluations.

**Bug 6: recordSiblingTrace overwrites traces with dep-less entries.**
*Symptom:* Verify path sees 0 deps for previously fully-traced attributes.
*Root cause:* When sibling `a` is already forced, `recordSiblingTrace` calls
`record` with empty deps `{}`, overwriting `a`'s trace entry (which had
full deps from its own `evaluateFresh`).
*Fix:* Check `index.lookup()` before `record` -- skip if already recorded.
*Lesson:* Speculative BSàlC CT recordings must not overwrite authoritative entries.

**Bug 7: recordSiblingTrace on non-derivation attrsets.**
*Symptom:* "access to absolute path is forbidden in pure evaluation mode" for
flake inputs.
*Root cause:* Recording children of non-derivation attrsets (flake inputs like
`inputs.flake1`) as bare strings bypasses SourceAccessor setup. The verify path
serves them directly, but flake input resolution requires real-tree evaluation
for AllowListSourceAccessor.
*Fix:* Guard forced-children loop with `!origExpr && isDerivation(*target)`.
*Lesson:* Not all attrset children can be independently traced.

**Bug 8: Virtual files from MemorySourceAccessor.**
*Symptom:* Dep verification (BSàlC VT check) crashes on non-existent paths.
*Root cause:* Virtual files (`/fetchurl.nix`, `/derivation-internal.nix`) exist
in `MemorySourceAccessor` but not on the real filesystem. Recording them as deps
fails on verification (`lstat()` -> ENOENT).
*Fix:* Guard with `std::filesystem::exists()` before recording deps.
*Lesson:* Not all evaluated files are real filesystem paths.

**Bug 9: Stat-hash cache population placement.**
*Symptom:* Incomplete traces causing stale verify hits (BSàlC VT false positives).
*Root cause:* `cacheStatHash()` before `recordDep()` could throw, skipping dep
recording. Trace had incomplete deps -> stale BSàlC VT verification passed.
*Fix:* Populate stat cache inside `recordDep()` itself, after the dep is recorded.
*Lesson:* Dep recording must be infallible relative to hash caching.

**Bug 10: navigateToReal wraps siblings, stealing deps from DDG.**
*Symptom:* Incomplete traces for attributes whose children are TracedExpr.
*Root cause:* When navigateToReal wraps a sibling with TracedExpr, forcing that
wrapped value creates a nested DependencyTracker (nested Adapton DDG scope) that
captures deps instead of the outer tracker.
*Fix:* In evaluateFresh, detect TracedExpr origExpr thunks and unwrap by evaluating
the original expression in the current tracker's scope.
*Lesson:* Nested DDG scopes from wrapped thunks steal deps from outer scopes.

### 4.3 Performance (4 bugs)

**Bug 11: Lazy trace replay O(N*M) explosion.**
*Symptom:* 132 seconds, 8GB memory for nixpkgs `hello.pname`.
*Root cause:* Eager DFS trace replay (Adapton change propagation). Each thunk
replayed all its deps into the parent tracker, and the parent replayed those
plus its own into the grandparent. O(N*M) where N = thunk count, M = avg dep
count.
*Fix:* Store `DepRange` pointers (lazy references). Flatten deps once at
`collectTraces()` time.
*Lesson:* Trace replay (Adapton change propagation) must be O(1) per thunk,
not O(deps).

**Bug 12: epochBloom filter saturation.**
*Symptom:* No measurable performance improvement despite bloom filter overhead.
*Root cause:* 64-bit bloom filter with ~4,000 entries -> 100% bit saturation ->
97.5% false positive rate. Only 0.9% of calls actually filtered.
*Fix:* Removed entirely. `boost::unordered_flat_map::find()` miss is already fast.
*Lesson:* Bloom filters need ~10x more bits than entries to be useful.

**Bug 13: Recursive sibling recording.**
*Symptom:* 88x slowdown for nixpkgs evaluations.
*Root cause:* `storeForcedChildren()` recursively descended through sibling
attrsets -> 50K--100K+ SQLite operations.
*Fix:* Single-level `recordSiblingTrace()` (BSàlC CT recording at one level).
*Lesson:* Recursive tree recording is catastrophically expensive.

**Bug 14: Fresh-evaluation redundant sorting.**
*Symptom:* Unnecessary CPU overhead in fresh evaluation.
*Root cause:* Deps sorted separately for serialization, trace hash, structural hash.
*Fix:* `sortAndDedupDeps()` once; pre-sorted variants for all consumers.
*Lesson:* Sort once, use many.

### 4.4 Storage and SQLite (5 bugs)

**Bug 15: internString deadlock in setTrace.**
*Symptom:* Deadlock during BSàlC CT trace recording.
*Root cause:* `internString()` acquires `_state->lock()`, but `setTrace()` already
holds that lock via `doSQLite`.
*Fix:* Lambda inside setTrace accesses already-locked state; separate public
`internString()` for external callers.
*Lesson:* SQLite helper methods must not re-acquire locks.

**Bug 16: sessionTraceIds cache staleness.**
*Symptom:* Missing dep verification, stale BSàlC VT results.
*Root cause:* `insertAttribute`'s UPSERT sets `trace_id = null` on conflict,
making the session cache stale.
*Fix:* Never use sessionTraceIds to skip DB writes.
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
*Lesson:* Not all dep hashes are BLAKE3 -- some are store paths or status strings.

**Bug 19: STRICT SQLite BLOB binding.**
*Symptom:* Type mismatch error on EvalIndex operations.
*Root cause:* `attr_path` is `BLOB` in a `STRICT` table. Using
`operator()(string_view)` binds as TEXT, which SQLite STRICT mode rejects.
*Fix:* Use `operator()(const unsigned char*, size_t)` for BLOB binding.
*Lesson:* SQLite STRICT tables enforce type affinity at bind time.

### 4.5 Store Integration (5 bugs)

**Bug 20: CAS traces consume autoGC budget.**
*(Note: This bug is specific to the CAS blob trace design, Phases 9--11.
The pure SQLite TraceStore (Phase 12) stores no objects in the Nix store, so
this class of issue no longer applies.)*
*Symptom:* gc-auto test fails (expects 2 of 3 garbage paths deleted).
*Root cause:* `nix build --impure --expr` creates 8--11 Text CA traces per
expression. When the process exits, traces become garbage. autoGC's single-pass
budget gets consumed deleting traces before garbage derivation paths.
*Fix:* `--option trace-cache false` in gc-auto.sh.
*Lesson:* Test isolation requires disabling tracing when testing GC behavior.

**Bug 21: derivation.nix lazy derivationStrict (Shake dynamic dep discovery).**
*Symptom:* Missing deps from env var processing (e.g., readFile in buildCommand).
*Root cause:* `derivation.nix` uses `let strict = derivationStrict drvAttrs`
lazily. Forcing a derivation attrset to WHNF does NOT call `derivationStrict`.
Deps are only captured when `drvPath`/`outPath` is accessed -- a Shake-style
dynamic dependency that is discovered late.
*Fix:* In evaluateFresh, after forceValue, detect derivation attrsets and force
`drvPath` to trigger derivationStrict.
*Lesson:* derivation.nix's laziness means derivationStrict deps are discovered
dynamically, only on demand.

**Bug 22: derivation.nix Value* aliasing.**
*Symptom:* Double-wrapping of TracedExpr articulation points, assertion failures.
*Root cause:* `derivation` returns `(head outputsList).value`, so `default.out`
and `default` share the **same Value***. Wrapping `default.drvPath` also wraps
`default.out.drvPath`.
*Fix:* `dynamic_cast<TracedExpr*>` guard before wrapping.
*Lesson:* Nix Values can alias -- always check before wrapping with articulation
points.

**Bug 23: .drv Existence deps are unsafe.**
*Symptom:* CA test failures when .drv files are deleted mid-test.
*Root cause:* Recording Existence deps for .drv store paths means fresh
re-evaluation calls `pathDerivationModulo()` on deleted inputs.
*Fix:* Skip recording Existence deps for .drv store paths.
*Lesson:* Store-internal paths should not be treated as external deps.

**Bug 24: StatHashCache file modification flakiness.**
*Symptom:* Tests pass on first run but fail on fast re-runs.
*Root cause:* File modifications of SAME SIZE within the same mtime granularity
cause L1/L2 cache hits returning stale hashes. Key includes size and mtime but
not content -- the stat-based BSàlC VT approximation is inherently lossy.
*Fix:* Always use content of DIFFERENT SIZE in test modifications. Call
`invalidateFileCache()` to clear both PosixSourceAccessor and StatHashCache L1.
*Lesson:* Stat-based caching is vulnerable to same-size, same-timestamp writes.

### 4.6 Structural Dep Tracking (1 bug)

**Bug 25: SQLite TEXT truncation at null bytes.**
*Symptom:* All StructuredContent dep keys were truncated to just the filepath,
causing two-level verification to never find structural deps covering failed
Content dep files.
*Root cause:* The original dep key format used `\0` (null byte) as separator
between the filepath and the format/datapath suffix. SQLite TEXT columns
truncate strings at null bytes, so the interned key in the `Strings` table
contained only the filepath portion, losing the structural information.
*Fix:* Changed separator from `\0` to `\t` (tab character) throughout all
code: `ExprTracedData::eval()`, `computeCurrentHash()`, `verifyTrace()`
two-level logic, and doc comments.
*Lesson:* Never use null bytes in SQLite TEXT column values. Tab is a safe
alternative that doesn't appear in file paths or data paths.

### 4.7 Two-Object Trace Model (3 pitfalls) -- CAS Blob Era Only

*(These pitfalls are specific to the CAS blob trace design, Phases 9--11.
The pure SQLite TraceStore does not use CAS blobs or store-path-based references,
so none of these apply to the current design. Note: the current TraceStore does
use zstd compression for `keys_blob` and `values_blob`, but determinism is not
required since these are SQLite BLOBs, not content-addressed store paths.)*

**Pitfall 25: Zstd determinism for CAS stability.**
*Symptom:* Dep blobs for identical traces produce different store paths.
*Root cause:* Zstd compression output can vary across library versions or when
using non-default compression parameters. Different output bytes -> different
content hash -> different store path -> no CAS deduplication.
*Fix:* Use a fixed compression level (level 3) with default zstd parameters.
This produces deterministic output for identical input within a given zstd
library version. Cross-version determinism is not guaranteed but is acceptable
since CAS dedup is an optimization, not a correctness requirement.
*Lesson:* CAS stability requires deterministic serialization at every layer.

**Pitfall 26: Dep blob GC coupling.**
*Symptom:* Dep blob collected while result traces still reference it.
*Root cause:* If the dep blob is stored without being listed in the result
trace's references, the GC has no way to know the result trace depends on it.
The dep blob becomes unreferenced garbage and is collected, causing
`loadTrace()` to fail with a missing store path.
*Fix:* The result trace lists the dep blob's store path in its `references`
set when calling `addTextToStore()`. This creates a proper GC root chain:
result trace -> dep blob. The dep blob itself has no references
(self-contained).
*Lesson:* Store object references are the GC coupling mechanism -- always
declare dependencies explicitly.

**Pitfall 27: validatedTraces cache must be cleared alongside validatedTraces.**
*Symptom:* Stale dep verification results (BSàlC VT false positive) after trace
invalidation.
*Root cause:* `validatedTraces` is a session cache of trace paths known to be
valid. When a trace is invalidated (e.g., during constructive recovery), the
cache was not cleared. If the old dep blob path happened to be reused (same
deps, different result trace after recovery), verification was incorrectly
skipped.
*Fix:* Clear `validatedTraces` whenever trace invalidation occurs.
*Lesson:* Coupled caches must be invalidated together.

---

## 5. Repeated Reasoning Failure Patterns

These patterns of mistakes recurred across multiple sessions:

### 5.1 Assuming Evaluation Determinism (violates Shake dynamic deps)

The trace for an attribute can have different structure across fresh evaluations
depending on evaluation order. This is a direct consequence of Shake-style
dynamic dependency discovery -- the dep set is not statically determined. This
broke hash-based constructive recovery (which assumed identical structure ->
identical hash) and required adding structural variant recovery as a fallback
(scanning TraceHistory for traces with matching structural signatures).

### 5.2 Forgetting Value* Aliasing

`derivation.nix` returns `(head outputsList).value`, making `default.out` and
`default` the same Value*. This caused double-wrapping bugs multiple times.
Every Value* mutation must check for pre-existing TracedExpr articulation points.

### 5.3 Underestimating Fixed-Point Recursion

nixpkgs' `buildPackages = self` creates chains where forcing one derivation's
drvPath transitively forces arbitrary other derivations. This invalidated
assumptions about bounded evaluation depth and required careful guards on eager
drvPath forcing for origExpr articulation points.

### 5.4 Session Cache Coherence (Salsa revision invalidation)

Multiple bugs arose from session caches becoming stale after DB writes.
The pattern: cache value X -> write to DB (which changes X) -> read cache
(returns stale X). Must either invalidate caches after writes or avoid caching
write-affected values. In the current schema (v2), parent staleness is detected
via trace dep verification rather than session caches, avoiding this class of
bugs entirely.

### 5.5 Testing with Same-Size Files

StatHashCache validates by stat metadata, not content. Early tests used
modifications like "hello" -> "world" (same size, 5 bytes), which could produce
false cache hits (stat-based BSàlC VT check passing incorrectly). Must use
different-size content for reliable invalidation testing.

---

## 6. Integration Points

### 6.1 EvalState

`EvalState` holds a `std::unique_ptr<EvalTraceContext> traceCtx` that is non-null
when eval-trace is enabled (the default). The `EvalTraceContext` struct contains
`evalCaches`, `fileContentHashes`, `mountToInput`, and `epochMap` -- all state
needed for trace-aware incremental evaluation. Each unique trace identity (flake
source or file/expr hash) gets its own `TraceCache` instance via
`traceCtx->evalCaches`. Integration is transparent: `TracedExpr` articulation
points are placed in the Value tree and `forceValue()` triggers
`TracedExpr::eval()` automatically -- this is the Adapton demand-driven model
where computation is memoized and re-verified on access.

### 6.2 Flake Subsystem

- `computeStableIdentity()`: computes version-independent identity for trace key
- `inputAccessors`: maps flake input names to SourcePath for dep verification
  (BSàlC VT dep hash recomputation)
- `mountToInput`: maps mount points to (inputName, subpath) for path resolution

### 6.3 CLI Commands

- `nix eval`: uses `getRootValue()` instead of `getRoot()` (AttrCursor)
- `nix build`: same eval path, derivations traced via TracedExpr articulation points
- `nix search`: navigates Value tree instead of AttrCursor tree
- `nix flake show/check`: same pattern

All commands use `forceValue()` on Values -- no trace-specific API calls. This
is the Adapton transparency property: tracing is invisible to callers.

### 6.4 Settings

- `trace-cache` (bool, default true): enables/disables the eval trace system
- `verify-trace-cache` (bool, default false): evaluates both verify and fresh
  paths, compares results, logs warnings on mismatch. For debugging only.

---

## 7. Testing Strategy

### 7.1 Test Pattern

Every test follows the same structure, exercising the BSàlC verify/record cycle:

```bash
# 1. Fresh eval + record: populate trace store (BSàlC CT recording)
nix build "$dir#attr"

# 2. Verify: serve from trace without evaluation (BSàlC VT check)
NIX_ALLOW_EVAL=0 nix build "$dir#attr"

# 3. Modify input (Shake: dynamic dep changes)
echo "new content!!" > "$dir/file.txt"  # DIFFERENT SIZE
git add file.txt && git commit -m "modify"

# 4. Verify invalidation: verify path fails (BSàlC VT check fails)
NIX_ALLOW_EVAL=0 expectStderr 1 nix build "$dir#attr" \
  | grepQuiet "not everything is cached"

# 5. Fresh re-eval + re-record (BSàlC CT recording)
nix build "$dir#attr"

# 6. Verify new result traced (BSàlC VT check passes)
NIX_ALLOW_EVAL=0 nix build "$dir#attr"
```

`NIX_ALLOW_EVAL=0` is the key mechanism: it causes the root loader to throw
if evaluation is attempted, proving the result was served entirely from the
trace store -- a BSàlC constructive trace serving without recomputation.

### 7.2 Test Categories

- **Core tests**: verify/record cycle, per-attr invalidation, GC resilience, errors
- **Dep tests**: pathExists, readDir, flake inputs, partial tree, addPath
- **Output tests**: scalar types, JSON derivation, write-to, cursor paths
- **Recovery tests**: dep revert, alternating versions, constructive recovery
  (BSàlC CT recovery across all 3 phases)
- **Volatile tests**: currentTime, mixed volatile/stable (always-failing VT check)
- **Regression tests**: specific bug reproductions (lazy derivationStrict,
  recordSiblingTrace overwrite, DDG dep stealing)
- **Structural dep tests** (GTest): lazy JSON/TOML structural dep tracking,
  two-level Content override, scalar access, nested access, array access,
  provenance validation, unused key change survival

### 7.3 Known Test Limitations

1. **Same-size file modification**: must use different-size content to avoid
   StatHashCache false hits (stat-based BSàlC VT approximation).
2. **Verify-served sibling trace incompleteness**: not tested because `c = a + b`
   with verify-served `a`/`b` produces incomplete traces for `c` (Adapton change
   propagation not implemented for verify path).
3. **autoGC interaction**: eval tracing was disabled in gc-auto.sh due to CAS
   blob traces consuming GC budget (Bug 20). With the Phase 12 pure SQLite
   TraceStore, this may no longer be necessary since no store objects are created.
4. **Parallel evaluation**: tests are single-threaded; no concurrent eval tests.
