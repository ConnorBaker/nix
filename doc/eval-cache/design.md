# Nix Eval Cache: Design Document

**Target audience:** Nix core team (assumes familiarity with the evaluator and store).

---

## 1. Motivation

Nix evaluation is expensive. `nix eval nixpkgs#hello.pname` takes ~3–5 seconds on
a cold evaluator because it must parse and evaluate thousands of Nix files even to
reach a single leaf attribute. In typical development workflows — edit-build-test
cycles, CI pipelines, `nix search` — the same flake is evaluated repeatedly with
few or no input changes between runs.

The existing eval cache (`AttrCursor`, ~721 lines in `eval-cache.cc` on `master`)
stores evaluation results in SQLite keyed by a fingerprint derived from the flake
lock file. It has two fundamental limitations:

1. **All-or-nothing invalidation.** The cache key is a hash of the locked flake
   inputs. Any change to any input — even touching a single comment in a single
   file — invalidates the *entire* cache. There is no per-attribute granularity.

2. **No dependency tracking.** The cache records *results* but not *which files
   produced them*. It cannot distinguish between an attribute that depends on the
   changed file and one that does not.

3. **No cross-version recovery.** Because the cache key includes the locked ref
   (which changes every commit), reverting a file to a previous state does not
   recover previously cached results.

4. **Limited type support.** Only string, bool, int, list-of-strings, and attrset
   are cached. No support for null, float, path, or heterogeneous lists.

5. **No `--file`/`--expr` caching.** Only flake evaluations are cached.

This design addresses all five limitations with a content-addressed, dependency-
tracking eval cache that provides per-attribute invalidation, cross-commit
recovery, and transparent integration with the existing CLI.

---

## 2. Design Goals

1. **Correctness.** Never serve stale results. Conservative invalidation is
   acceptable (false cache misses are OK; false cache hits are not). Every cached
   result is validated against current file system state before being served.

2. **Partial invalidation.** When one file changes, only attributes that depend
   on that file are re-evaluated. Unaffected attributes are served from cache.

3. **Transparency.** No new CLI flags or API surface. The cache is invisible to
   consumers — `forceValue()` works as before, and `nix eval`/`nix build`/`nix
   search` get faster without user intervention.

4. **GC integration.** Cache data lives in the Nix store as content-addressed
   blobs. When the store is garbage-collected, stale cache entries are collected
   naturally. No separate cache GC mechanism is needed.

5. **Cross-commit recovery.** After a file revert or structural change, the cache
   recovers previously computed results by matching dependency signatures — without
   re-evaluation.

6. **Minimal overhead.** The warm path (serving a cached result) should complete
   in under 200ms for typical flake evaluations, dominated by file stat validation.

7. **Remote substitution (not yet implemented).** A remote builder that has already
   evaluated a large closure (e.g., a NixOS system configuration) should be able
   to share its cached evaluation results with local machines via binary cache
   substitution. A developer could then open `nix repl` locally and explore the
   evaluated configuration without re-evaluating — only non-serializable values
   (functions, unevaluated thunks) would trigger local cold evaluation on access.
   This is feasible because traces are content-addressed store objects with
   deterministic paths: two machines evaluating the same flake at the same
   revision produce identical trace store paths. The remaining gap is the local
   `EvalIndexDb`, which maps attribute paths to trace store paths and is not
   currently distributed.

---

## 3. Architecture Overview

The eval cache is a 4-layer stack with a persistent file hash cache:

```
CLI command (nix eval / nix build / nix search)
  │
  ▼
EvalCache                                        [eval-cache.hh: 124 lines]
  Public API. Maps a stable identity hash to a root ExprCached thunk.
  │
  ▼
ExprCached thunks                                [eval-cache.cc: 891 lines]
  GC-allocated Expr subclass. eval() dispatches:
  ├─ Warm path: index lookup → trace validation → result deserialization
  ├─ Cold path: navigateToReal → forceValue → FileLoadTracker → coldStore
  └─ Recovery:  3-phase dep hash / parent-aware / struct-group
  │
  ▼
EvalCacheStore                                   [eval-cache-store.cc: 589 lines]
  Store-based backend. Manages CAS blob traces:
  ├─ storeTrace():    CBOR → Text CA blob in Nix store
  ├─ loadTrace():     store path → CBOR → EvalTrace
  └─ validateTrace(): recursive dep validation + parent chain
  │
  ▼
EvalIndexDb                                      [eval-index-db.cc: 283 lines]
  Lightweight SQLite index (~100KB typical):
  ├─ EvalIndex:        (context_hash, attr_path) → trace_path
  ├─ DepHashRecovery:  dep_hash → trace_path
  └─ DepStructGroups:  struct_hash → representative trace
  │
  ╔══════════════════════════════════════════════╗
  ║ FileLoadTracker    [file-load-tracker.cc: 219 lines]
  ║   RAII thread_local dep recording.
  ║   Captures deps during evaluation into session-wide vector.
  ╚══════════════════════════════════════════════╝
  │
  ╔══════════════════════════════════════════════╗
  ║ StatHashCache      [stat-hash-cache.cc: 339 lines]
  ║   Persistent file → BLAKE3 hash cache.
  ║   L1: concurrent_flat_map (session, 64K cap)
  ║   L2: SQLite (persistent, stat-validated)
  ╚══════════════════════════════════════════════╝
```

Data flows **top-down** for reads (warm path: index → store → result) and
**bottom-up** for writes (cold path: evaluate → track deps → store trace → update
index). Each layer has clear responsibilities and a defined interface to the layer
below.

---

## 4. Dependency Tracking

### 4.1 Dependency Types

The eval cache tracks 11 dependency types, organized into four categories:

| Category | Type | Source Builtins | Validation |
|----------|------|-----------------|------------|
| **Hash oracle** | Content (1) | `evalFile`, `readFile`, `hashFile` | BLAKE3 of file bytes |
| | Directory (2) | `readDir`, filtered `addPath` dirs | BLAKE3 of sorted dir listing |
| | Existence (3) | `pathExists` | `exists?` check |
| | EnvVar (4) | `getEnv` | Re-read env variable |
| | System (6) | `currentSystem` | Compare string value |
| **Reference** | CopiedPath (9) | `builtins.path` (unfiltered) | BLAKE3 of NAR |
| | UnhashedFetch (7) | `fetchTree` (unlocked) | Re-fetch, compare store path |
| | NARContent (11) | `builtins.path` (filtered) | BLAKE3 of NAR (captures exec bit) |
| **Volatile** | CurrentTime (5) | `currentTime` | Always invalidates |
| | Exec (10) | `builtins.exec` | Always invalidates |
| **Structural** | ParentContext (8) | (internal) | Parent dep set hash |

**Hash oracle** deps are the most common. They are validated by recomputing a hash
of the current resource content and comparing it to the stored hash. The
`StatHashCache` avoids redundant hashing by caching `(path, stat_metadata) → hash`
across sessions.

**Reference** deps validate by re-resolving a reference (computing a store path or
re-fetching a URL) and comparing the result to the stored value. `UnhashedFetch`
respects `tarballTtl` for within-TTL validation.

**Volatile** deps always invalidate. Any attribute that transitively depends on
`currentTime` or `builtins.exec` is re-evaluated every time.

**Structural** deps (ParentContext) provide transitive invalidation. Each trace
references its parent trace; if any ancestor's deps are invalid, the entire chain
invalidates.

### 4.2 Recording

Dependencies are recorded during evaluation via `FileLoadTracker`, an RAII guard
that sets a thread-local pointer on construction and clears it on destruction.
All dep recording goes through a single entry point:

```cpp
void FileLoadTracker::record(const Dep & dep);
```

Recording sites are instrumented throughout the evaluator:
- `eval.cc`: `evalFile` → Content dep
- `primops.cc`: `readFile`, `readDir`, `pathExists`, `getEnv`, `currentTime`,
  `currentSystem`, `hashFile`, `readFileType` → appropriate dep types
- `primops/fetchTree.cc`: `fetchTree` (unlocked) → UnhashedFetch dep
- `builtins.path`: → CopiedPath or NARContent (filtered) dep

Each tracker deduplicates deps by `(type, source, key)` within its scope using a
`DepKey` hash set, so the same file read twice during one attribute's evaluation
produces only one dep entry.

### 4.3 Validation

On the warm path, each dep is validated by `computeCurrentHash()` in
`eval-cache-store.cc`. Validation short-circuits on the first failure: if any dep
is invalid, the entire trace is invalid.

For file-based deps (Content, Directory, NARContent), the `StatHashCache` provides
a fast validation path: if the file's stat metadata (device, inode, mtime
nanoseconds, size) matches the cached entry, the stored hash is returned without
re-reading the file. This reduces the warm path to a series of `lstat()` system
calls for unchanged files.

### 4.4 Parent Context Chain

Each trace includes a reference to its parent's trace store path. Validation
recursively walks the parent chain: a child is valid only if all its ancestors
are valid. This provides transitive invalidation without flattening the full
dependency tree at each level.

The parent chain is critical for correctness: an attribute like `hello.pname` has
its own deps (the files it directly reads) plus all deps inherited from the
evaluation context that produced the `hello` attrset. Without the parent chain,
changing a file that affects the `hello` derivation itself (but not `pname`
specifically) would not invalidate `hello.pname`.

---

## 5. Storage Model

### 5.1 Why CAS Blob Traces

The cache stores each attribute's evaluation result as **Text content-addressed
blobs** in the Nix store (a result trace plus a shared dep set blob). This
design was chosen over SQLite-only storage for several reasons:

1. **GC integration.** Traces are regular store objects. When the store is garbage-
   collected, unreferenced traces are collected automatically. No separate cache
   cleanup mechanism is needed.

2. **Immutability.** Once a trace is stored, it never changes. There are no stale
   overwrites, no UPSERT race conditions, no cache coherence issues.

3. **Content addressing.** Dep set blobs are independently CAS-deduplicated:
   attributes that depend on the same set of files share a single dep set store
   object. Result traces reference their dep set blob, coupling their GC
   lifetimes while keeping result data slim.

4. **Reliability.** SQLite databases can become corrupted (power loss, disk errors).
   Store objects are individually content-verified.

### 5.2 Trace Format (v2: Two-Object Model)

Each attribute's cache entry consists of **two** content-addressed store objects:
a slim **result trace** and a separate **dep set blob**. The result trace
references the dep set blob, coupling their lifetimes for GC purposes.

This split was motivated by storage analysis of v1 (single-blob) traces: in a
typical nixpkgs evaluation, 465 MB of trace data lived in an 837 MB store, with
99.99% of trace size consumed by inline dep arrays. Near-zero CAS deduplication
was observed because each trace's deps included the result, making identical dep
sets across attributes extremely rare. By factoring out deps into a separate
blob (keyed only on dep content), dep set blobs achieve meaningful CAS
deduplication across attributes that read the same files. The two-object model
provides an estimated ~89% storage reduction.

#### Result Trace

The result trace is a small CBOR-encoded Text CA blob:

```
{
  "v": 2,                        // Format version (v2)
  "r": <bytes>,                  // Nested CBOR-encoded AttrValue
  "p": <string|null>,            // Parent trace store path (null for root)
  "c": <int|null>,               // Context hash (root only)
  "ds": <string>                 // Dep set blob store path (reference)
}
```

The `"ds"` field is a store path reference to the dep set blob. This reference
serves two purposes: (1) fast dep loading during warm-path validation, and
(2) GC coupling — the result trace keeps the dep set blob alive as long as the
trace itself is reachable.

#### Dep Set Blob

The dep set blob is a zstd-compressed CBOR array stored as a Text CA blob with
the name `"eval-deps"` and no store references (it is self-contained):

```
zstd(cbor([
  {
    "t": <int>,                  // DepType enum value
    "s": <string>,               // Source (flake input name, "" for self, "<absolute>")
    "k": <string>,               // Key (path or identifier)
    "h": <bytes|string>          // Expected hash (BLAKE3 bytes or string)
  },
  ...
]))
```

Deps within the blob are sorted by `(type, source, key)` and deduplicated.
Zstd compression uses a fixed compression level (level 3) to ensure
deterministic output for CAS stability — different compression levels or
non-deterministic compressors would produce different store paths for identical
dep sets, defeating content addressing.

Because the dep set blob does not contain any result or parent information,
attributes that depend on exactly the same set of files produce identical dep
set blobs and share a single store object via CAS deduplication.

#### AttrValue Serialization

The `AttrValue` serialization within `"r"` supports all Nix value types:

```
{
  "v": 1,                        // Format version
  "t": <int>,                    // AttrType enum
  "s": <string>,                 // Value string (type-dependent)
  "c": <string>,                 // String context (space-separated)
  "n": [<string>, ...] | null,   // Child names (FullAttrs only)
  "l": <int|null>                // List size (List only)
}
```

### 5.3 One Trace Per Attribute, Keys-Only Attrsets

Each attribute in the evaluation tree gets its own result trace plus a shared dep
set blob — two store objects per attribute (though dep set blobs are
deduplicated across attributes with identical deps). For attrsets (`FullAttrs`),
the result trace stores **only the child key names**, not child values. Each
child value lives in a separate trace with its own store path and index entry,
referencing the parent trace via the `"p"` field and its dep set via `"ds"`.

This has three important consequences:

1. **No in-place updates.** Traces are immutable CAS blobs. Forcing additional
   children of an attrset does not modify the parent's trace — only new child
   traces are created.

2. **Partial evaluation is natural.** An attrset can be cached (with its full
   key list) before any of its children's values are cached. When a child is
   later accessed, the warm path serves the parent's key list and creates thunks
   for each child. Only the accessed child triggers a cold path for its
   individual value trace.

3. **No reconciliation across sessions.** If session A caches `hello.pname` and
   session B later caches `hello.name`, no coordination is needed. The `hello`
   attrset trace (containing the key list) was written once during session A and
   is reused as-is. Session B only adds a new child trace for `name`.

### 5.4 EvalIndexDb

The `EvalIndexDb` is a lightweight SQLite database (~100KB for a typical flake)
stored at `~/.cache/nix/eval-index-v2.sqlite`. It provides fast lookups without
scanning the store:

| Table | Key | Value | Purpose |
|-------|-----|-------|---------|
| `EvalIndex` | `(context_hash, attr_path)` | `trace_path` | Warm-path lookup |
| `DepHashRecovery` | `(context_hash, attr_path, dep_hash)` | `trace_path` | Phase 1+2 recovery |
| `DepStructGroups` | `(context_hash, attr_path, struct_hash)` | `trace_path` | Phase 3 recovery |

The index is **expendable**. If deleted or corrupted, the cache still functions:
the cold path re-evaluates and rebuilds the index. Recovery is degraded (no
historical candidates to scan) but correctness is unaffected.

### 5.5 Deferred Write Pattern

Traces are written in FIFO order via a deferred write queue. Parent traces are
pushed before children (evaluation is depth-first), ensuring parent trace paths
are resolved before children reference them. The queue is flushed in
`~EvalCacheStore()`, with deduplication via a `stagedAttrPaths` set.

---

## 6. Warm / Cold / Recovery Paths

### 6.1 Warm Path

The warm path serves a cached result without evaluation:

```
1. Index lookup
   index.lookup(contextHash, attrPath) → tracePath
   If not found → cold path

2. Validate trace
   a. Check store path exists (isValidPath)
   b. Load result trace: CBOR → EvalTrace{result, parent, depSetPath}
   c. Load dep set blob: depSetPath → zstd decompress → CBOR → deps[]
   d. For each dep in deps:
      - computeCurrentHash(dep, inputAccessors) → currentHash
      - if currentHash != dep.expectedHash → FAIL
   e. If parent: recursively validateTrace(parent)
   f. On any failure → recovery, then cold path

3. Serve result
   a. Deserialize AttrValue from trace
   b. For FullAttrs: create ExprCached children with origExpr wrappers
   c. For leaves: materialize Value directly
   d. Replay deps to FileLoadTracker (if parent is tracking)
```

Typical warm-path latency is dominated by `lstat()` calls for dep validation,
with the `StatHashCache` providing sub-millisecond lookups for unchanged files.

### 6.2 Cold Path

The cold path evaluates the real thunk and stores the result:

```
1. Navigate to real tree
   navigateToReal(): walk real eval tree from root to target
   - At each level, wrap sibling thunks with ExprCached origExpr wrappers
   - This enables partial tree caching: siblings are independently cacheable

2. Force value
   forceValue(*target): evaluate the real thunk
   - FileLoadTracker captures all deps during evaluation
   - For derivations: force drvPath to capture derivationStrict deps

3. Store result
   coldStore(attrPath, name, value, deps, parentTrace, isRoot):
   a. Sort and dedup deps
   b. Serialize dep set to CBOR, zstd-compress, store as Text CA blob
      ("eval-deps", no references) → depSetPath
   c. Serialize result trace to CBOR (result + parent + contextHash + depSetPath)
      Store as Text CA blob with depSetPath as reference → tracePath
   d. Update index: EvalIndex, DepHashRecovery, DepStructGroups
   e. Wrap children with ExprCached thunks for future cache hits

4. Store siblings
   storeForcedSibling(): for each already-forced sibling at this level,
   speculatively cache its current value (empty deps, parent reference)
```

### 6.3 Recovery (Three Phases)

When warm-path validation fails (a dep has changed), recovery attempts to find a
previously stored trace whose deps match the *current* state — avoiding a full
cold-path re-evaluation:

```
Phase 1: Direct dep hash recovery                               [O(1)]
  Compute depContentHash from CURRENT dep hashes
  Point lookup in DepHashRecovery → candidate tracePath
  Validate candidate → if valid, update index and serve

Phase 2: Parent-aware dep hash recovery                          [O(1)]
  Compute depContentHashWithParent (includes parent identity)
  Point lookup in DepHashRecovery → candidate tracePath
  Finds children whose parent changed but own deps match

Phase 3: Structural group scan                                   [O(V)]
  Scan DepStructGroups for same (context_hash, attr_path)
  Returns representative traces for each unique dep KEY structure
  For each representative: validate its deps against current state
  Handles dep structure instability (different deps across cold evals)
```

Recovery is particularly effective for file reverts: reverting a file to a previous
state produces the same dep hashes, and Phase 1 finds the matching trace in O(1).

---

## 7. Partial Tree Invalidation

The key mechanism for per-attribute granularity is `navigateToReal()`:

1. When a cold path is needed for attribute `A.B.C`, `navigateToReal()` walks the
   real evaluation tree from root to `C`, forcing each intermediate attrset.

2. At each level, **sibling thunks are wrapped** with `ExprCached` wrappers that
   have `origExpr`/`origEnv` set. These wrappers try the warm cache first when
   the sibling is later forced (e.g., as a side effect of `derivationStrict`).

3. This means changing one file typically invalidates only the attributes that
   depend on it. Other attributes at the same level are served from cache via
   their origExpr wrappers.

### 7.1 ExprOrigChild

When the warm path serves a `FullAttrs` result, it creates children as
`ExprOrigChild` thunks. These resolve children by evaluating the **parent's
original expression** (shared across siblings via `SharedParentResult`) rather
than using `navigateToReal()`, which would cycle through the materialized parent
and hit a blackhole.

`ExprOrigChild` uses `SuspendFileLoadTracker` during parent evaluation to prevent
"fat parent" dep explosion. Without suspension, evaluating a parent like
`buildPackages` (= all of nixpkgs) would record 10,000+ file deps into each
child's dep set.

### 7.2 SharedParentResult

`SharedParentResult` is a GC-allocated struct ensuring the parent is evaluated at
most once across all sibling `ExprOrigChild` instances. The first child to be
forced evaluates the parent; subsequent siblings reuse the cached result.

---

## 8. Cache Identity

### 8.1 Flakes

For flake evaluations, the cache identity is computed via
`computeStableIdentity()`, which uses each flake input's `getStableIdentity()`:

- `GitInputScheme`: `git:<url>`
- `GitArchiveInputScheme`: `<scheme>:<host>/<owner>/<repo>`
- `PathInputScheme`: `path:<absolute_path>`
- `TarballInputScheme`: `tarball:<url>`
- `FileInputScheme`: `file:<url>`

This identity is **version-independent**: it identifies the flake *source* but not
a specific revision. The `contextHash` is the first 8 bytes of the BLAKE3 hash of
this identity, used as the index partition key.

### 8.2 `--file` and `--expr`

For non-flake evaluations (`nix eval -f` / `nix eval --expr`):

- `--file <path>`: SHA256 of the canonical absolute path
- `--expr <text>`: SHA256 of the expression text

These are also version-independent, enabling caching and recovery across
modifications to the evaluated file or expression.

---

## 9. Known Limitations and Trade-offs

1. **origExpr wrappers skip eager drvPath forcing.** When a sibling is wrapped
   with an origExpr `ExprCached` wrapper, it does not eagerly force `drvPath`.
   This means deps from `derivationStrict` env processing (e.g., `readFile` in a
   `buildCommand` string interpolation) are captured only when `drvPath` or
   `outPath` is naturally accessed. Trade-off: avoids infinite recursion through
   nixpkgs' `buildPackages = self` fixed-point.

2. **Warm-served siblings don't replay deps.** When `c = a + b` and `a`, `b` are
   served from the warm cache, their deps are not replayed into `c`'s
   `FileLoadTracker`. Result: `c`'s dep set may be incomplete (missing deps from
   warm-served children). This is conservative — `c` will be re-evaluated more
   often than necessary, not less.

3. **Recovery is single-attribute level.** Recovery works for `nix eval` (leaf
   values) but doesn't cascade through deep trees for `nix build`. A recovered
   parent does not automatically recover its children's index entries.

4. **StatHashCache same-size file flakiness.** The stat-hash cache keys on
   `(dev, ino, mtime_nsec, size)`. File modifications of the same size within the
   same mtime granularity can produce false cache hits. Mitigated by using
   nanosecond mtime precision.

5. **CAS traces consume autoGC budget.** Text CA blobs for traces are temporary
   store objects. When the creating process exits, traces become garbage and
   consume the autoGC budget before actual garbage paths. Mitigated by disabling
   eval cache in GC tests.

6. **No list caching.** Only `FullAttrs` (attrsets) and leaf types are cached at
   intermediate levels. Heterogeneous lists are cached only as leaf values.

7. **Symlinks not tracked.** Intermediate symlink targets are not recorded as deps.
   Changes to a symlink target without changing the resolved file will not
   invalidate the cache.

---

## 10. Future Work

1. **Cascading recovery.** Extend recovery to cascade through the attribute tree,
   recovering child index entries when a parent is recovered. This would improve
   `nix build` warm-path performance after input changes.

2. **Dep replay for warm-served siblings.** Implement dep replay in the warm path
   so that `c = a + b` correctly captures deps from warm-served `a` and `b`.

3. **Parallel dep validation.** Validate deps concurrently using a thread pool.
   Currently validation is sequential; parallelism would reduce warm-path latency
   for attributes with many deps.

4. **Remote eval cache substitution.** Traces are content-addressed store objects
   with deterministic paths, so `nix copy` can transfer them between machines.
   The blocker is the `EvalIndexDb`: a local SQLite index that maps
   `(contextHash, attrPath)` to trace store paths. Without an index entry, the
   warm path cannot find a trace even if it exists in the store. Traces do not
   contain their own attribute path, so the index cannot be reconstructed from
   store contents alone. An index export/import mechanism would close this gap.
   Dep validation would still run locally — for pure flake evaluations with the
   same lock file, file-based deps (Content, Directory, Existence) validate
   correctly because flake input store paths are identical across machines.
   Machine-specific deps (EnvVar, currentSystem) would cause validation failures
   on machines with different environments, limiting sharing to same-architecture
   pure evaluations.

5. **Integration with content-addressed derivations.** Content-addressed
   derivations could provide additional optimization opportunities — the eval
   cache could skip re-evaluation when a derivation's output is already available
   regardless of input changes.

6. **Symlink tracking.** Record intermediate symlink targets as Existence deps to
   detect symlink retargeting.
