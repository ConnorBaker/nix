# Nix Eval Cache: Design Document

**Target audience:** Nix core team (assumes familiarity with the evaluator and store).

---

## 1. Motivation

Nix evaluation is expensive. `nix eval nixpkgs#hello.pname` takes ~3--5 seconds on
a cold evaluator because it must parse and evaluate thousands of Nix files even to
reach a single leaf attribute. In typical development workflows -- edit-build-test
cycles, CI pipelines, `nix search` -- the same flake is evaluated repeatedly with
few or no input changes between runs.

The existing eval cache (`AttrCursor`, ~721 lines in `eval-cache.cc` on `master`)
stores evaluation results in SQLite keyed by a fingerprint derived from the flake
lock file. It has two fundamental limitations:

1. **All-or-nothing invalidation.** The cache key is a hash of the locked flake
   inputs. Any change to any input -- even touching a single comment in a single
   file -- invalidates the *entire* cache. There is no per-attribute granularity.

2. **No dependency tracking.** The cache records *results* but not *which files
   produced them*. It cannot distinguish between an attribute that depends on the
   changed file and one that does not.

3. **No cross-version recovery.** Because the cache key includes the locked ref
   (which changes every commit), reverting a file to a previous state does not
   recover previously cached results.

4. **Limited type support.** Only string, bool, int, list-of-strings, and attrset
   are cached. No support for null, float, path, or heterogeneous lists.

5. **No `--file`/`--expr` caching.** Only flake evaluations are cached.

This design addresses all five limitations with a dependency-tracking eval cache
that provides per-attribute invalidation, cross-commit recovery, and transparent
integration with the existing CLI.

---

## 2. Design Goals

1. **Correctness.** Never serve stale results. Conservative invalidation is
   acceptable (false cache misses are OK; false cache hits are not). Every cached
   result is validated against current file system state before being served.

2. **Partial invalidation.** When one file changes, only attributes that depend
   on that file are re-evaluated. Unaffected attributes are served from cache.

3. **Transparency.** No new CLI flags or API surface. The cache is invisible to
   consumers -- `forceValue()` works as before, and `nix eval`/`nix build`/`nix
   search` get faster without user intervention.

4. **Cross-commit recovery.** After a file revert or structural change, the cache
   recovers previously computed results by matching dependency signatures -- without
   re-evaluation.

5. **Minimal overhead.** The warm path (serving a cached result) should complete
   in under 200ms for typical flake evaluations, dominated by `lstat()` system
   calls for file stat validation and SQLite reads.

---

## 3. Architecture Overview

The eval cache is a 3-layer stack with a persistent file hash cache:

```
CLI command (nix eval / nix build / nix search)
  |
  v
EvalCache                                        [eval-cache.hh]
  Public API. Maps a stable identity hash to a root ExprCached thunk.
  |
  v
ExprCached thunks                                [eval-cache.cc]
  GC-allocated Expr subclass. eval() dispatches:
  +-- Warm path: DB lookup -> dep validation -> result decode
  +-- Cold path: navigateToReal -> forceValue -> FileLoadTracker -> coldStore
  +-- Recovery:  3-phase dep hash / parent-aware / struct-group
  |
  v
EvalCacheDb                                      [eval-cache-db.cc]
  SQLite backend. Single database at ~/.cache/nix/eval-cache-v1.sqlite.
  +-- warmPath():    SELECT -> validate -> decode AttrValue
  +-- coldStore():   INSERT/UPSERT -> dep set + recovery index writes
  +-- recovery():    3-phase lookup in DepHashRecovery + DepStructGroups
  +-- validateAttr(): dep set validation + epoch-based parent chain check
  |
  +=============================================+
  | FileLoadTracker    [file-load-tracker.cc]   |
  |   RAII thread_local dep recording.          |
  |   Captures deps during evaluation into      |
  |   session-wide vector.                      |
  +=============================================+
  |
  +=============================================+
  | StatHashCache      [stat-hash-cache.cc]     |
  |   Persistent file -> BLAKE3 hash cache.     |
  |   L1: concurrent_flat_map (session, 64K cap)|
  |   L2: SQLite (persistent, stat-validated)   |
  +=============================================+
```

Data flows **top-down** for reads (warm path: lookup attribute row, validate deps,
decode result) and **bottom-up** for writes (cold path: evaluate, track deps,
compute hashes, upsert attribute row). Each layer has clear responsibilities and
a defined interface to the layer below.

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
`StatHashCache` avoids redundant hashing by caching `(path, stat_metadata) -> hash`
across sessions.

**Reference** deps validate by re-resolving a reference (computing a store path or
re-fetching a URL) and comparing the result to the stored value. `UnhashedFetch`
respects `tarballTtl` for within-TTL validation.

**Volatile** deps always invalidate. Any attribute that transitively depends on
`currentTime` or `builtins.exec` is re-evaluated every time.

**Structural** deps (ParentContext) are recorded during evaluation but are
**not stored** in the database. Parent-child relationships are instead tracked
via the `parent_id` foreign key and the `epoch`-based staleness mechanism
(see Section 4.4).

### 4.2 Recording

Dependencies are recorded during evaluation via `FileLoadTracker`, an RAII guard
that sets a thread-local pointer on construction and clears it on destruction.
All dep recording goes through a single entry point:

```cpp
void FileLoadTracker::record(const Dep & dep);
```

Recording sites are instrumented throughout the evaluator:
- `eval.cc`: `evalFile` -> Content dep
- `primops.cc`: `readFile`, `readDir`, `pathExists`, `getEnv`, `currentTime`,
  `currentSystem`, `hashFile`, `readFileType` -> appropriate dep types
- `primops/fetchTree.cc`: `fetchTree` (unlocked) -> UnhashedFetch dep
- `builtins.path`: -> CopiedPath or NARContent (filtered) dep

Each tracker deduplicates deps by `(type, source, key)` within its scope using a
`DepKey` hash set, so the same file read twice during one attribute's evaluation
produces only one dep entry.

### 4.3 Validation

On the warm path, each dep is validated by `computeCurrentHash()` in
`eval-cache-db.cc`. Validation short-circuits on the first failure: if any dep
is invalid, the entire dep set is invalid.

For file-based deps (Content, Directory, NARContent), the `StatHashCache` provides
a fast validation path: if the file's stat metadata (device, inode, mtime
nanoseconds, size) matches the cached entry, the stored hash is returned without
re-reading the file. This reduces the warm path to a series of `lstat()` system
calls for unchanged files.

### 4.4 Epoch-Based Parent Chain Validation

Each attribute row in the `Attributes` table stores a `parent_id` (foreign key to
the parent attribute) and a `parent_epoch` (the parent's `epoch` value at the time
the child was cold-stored). The `epoch` column increments on every UPSERT conflict
-- that is, every time an attribute is re-evaluated and re-stored.

During warm-path validation, `validateAttr()` checks:

1. The attribute's own dep set is valid (all deps match current file state).
2. The parent attribute is recursively valid.
3. `storedParentEpoch == parent's current epoch`. If the parent was re-stored
   (bumping its epoch), the child is considered stale even if its own deps pass
   validation. This is correct because a parent's value may change even when its
   deps are unchanged (e.g., a different evaluation order produces different
   intermediate results).

This epoch check is an O(1) integer comparison that detects **any** parent change
(value or deps), providing transitive invalidation without flattening the full
dependency tree at each level.

The parent chain is critical for correctness: an attribute like `hello.pname` has
its own deps (the files it directly reads) plus all deps inherited from the
evaluation context that produced the `hello` attrset. Without the parent chain,
changing a file that affects the `hello` derivation itself (but not `pname`
specifically) would not invalidate `hello.pname`.

---

## 5. Storage Model

### 5.1 SQLite Database

All cache data lives in a single SQLite database at
`~/.cache/nix/eval-cache-v1.sqlite`. This includes attribute results, dependency
sets, and recovery indices. The database uses WAL mode, memory-mapped I/O (30 MB),
and a 4 MB page cache for performance.

The choice of a unified SQLite database (rather than separate index + store, or
content-addressed store objects) provides several advantages:

1. **Simplicity.** A single file with well-understood semantics. No coordination
   between multiple storage layers.

2. **Atomic writes.** SQLite's ACID transactions ensure that attribute upserts,
   dep set insertions, and recovery index writes are atomic. A crash mid-write
   cannot leave the cache in an inconsistent state.

3. **Space efficiency.** Dep sets are deduplicated by `content_hash` (attributes
   with identical deps share a single `DepSets` row). Attribute values are stored
   as SQL columns (type INTEGER, value TEXT, context TEXT) with no serialization
   overhead.

4. **Expendability.** The database is a cache. If deleted or corrupted, the cold
   path re-evaluates and rebuilds it. Correctness is never at risk.

### 5.2 Schema (5 Tables)

```sql
CREATE TABLE IF NOT EXISTS Attributes (
    attr_id      INTEGER PRIMARY KEY,
    context_hash INTEGER NOT NULL,
    attr_path    BLOB NOT NULL,
    parent_id    INTEGER REFERENCES Attributes(attr_id),
    parent_epoch INTEGER,
    epoch        INTEGER NOT NULL DEFAULT 0,
    type         INTEGER NOT NULL,
    value        TEXT,
    context      TEXT,
    dep_set_id   INTEGER REFERENCES DepSets(set_id),
    UNIQUE(context_hash, attr_path)
) STRICT;

CREATE TABLE IF NOT EXISTS DepSets (
    set_id       INTEGER PRIMARY KEY,
    content_hash BLOB NOT NULL UNIQUE,
    struct_hash  BLOB NOT NULL
) STRICT;

CREATE TABLE IF NOT EXISTS DepSetEntries (
    set_id       INTEGER NOT NULL REFERENCES DepSets(set_id),
    dep_type     INTEGER NOT NULL,
    source       TEXT NOT NULL,
    key          TEXT NOT NULL,
    hash_value   BLOB NOT NULL,
    PRIMARY KEY (set_id, dep_type, source, key)
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS DepHashRecovery (
    context_hash INTEGER NOT NULL,
    attr_path    BLOB NOT NULL,
    dep_hash     BLOB NOT NULL,
    dep_set_id   INTEGER NOT NULL REFERENCES DepSets(set_id),
    type         INTEGER NOT NULL,
    value        TEXT,
    context      TEXT,
    PRIMARY KEY (context_hash, attr_path, dep_hash)
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS DepStructGroups (
    context_hash INTEGER NOT NULL,
    attr_path    BLOB NOT NULL,
    struct_hash  BLOB NOT NULL,
    dep_set_id   INTEGER NOT NULL REFERENCES DepSets(set_id),
    PRIMARY KEY (context_hash, attr_path, struct_hash)
) WITHOUT ROWID, STRICT;
```

**Attributes** is the primary table. Each row represents one cached attribute,
identified by `(context_hash, attr_path)`. The `attr_id` (rowid) serves as a
lightweight identifier used for parent references and session caching. The `type`,
`value`, and `context` columns encode the `AttrValue` directly as SQL values -- no
binary serialization. The `epoch` column increments on every UPSERT conflict,
providing a monotonic version counter for staleness detection.

**DepSets** stores deduplicated dependency sets, keyed by `content_hash` (SHA-256
of sorted dep entries including hash values). The `struct_hash` captures the
structural signature (dep types + sources + keys, without hash values) for Phase 3
structural recovery.

**DepSetEntries** is a WITHOUT ROWID table storing individual dependency entries
within a dep set. The composite primary key `(set_id, dep_type, source, key)`
ensures deduplication.

**DepHashRecovery** maps `(context_hash, attr_path, dep_hash)` to a recovery
candidate's dep set and result. Used by Phase 1 (dep content hash) and Phase 2
(dep content hash with parent Merkle identity) recovery.

**DepStructGroups** maps `(context_hash, attr_path, struct_hash)` to a
representative dep set for Phase 3 structural recovery.

### 5.3 One Row Per Attribute, Keys-Only Attrsets

Each attribute in the evaluation tree gets its own row in the `Attributes` table.
For attrsets (`FullAttrs`), the `value` column stores **only the child key names**
(tab-separated), not child values. Each child value lives in a separate row with
its own `attr_id` and dep set, referencing the parent via `parent_id`.

This has three important consequences:

1. **Atomic updates.** Each attribute row is independently upserted. Forcing
   additional children of an attrset does not modify the parent's row -- only
   new child rows are created.

2. **Partial evaluation is natural.** An attrset can be cached (with its full
   key list) before any of its children's values are cached. When a child is
   later accessed, the warm path serves the parent's key list and creates thunks
   for each child. Only the accessed child triggers a cold path for its
   individual value.

3. **No reconciliation across sessions.** If session A caches `hello.pname` and
   session B later caches `hello.name`, no coordination is needed. The `hello`
   attrset row (containing the key list) was written once during session A and
   is reused as-is. Session B only adds a new child row for `name`.

### 5.4 AttrValue Encoding

Attribute values are encoded as three SQL columns: `type` (INTEGER), `value`
(TEXT), and `context` (TEXT). The encoding is straightforward:

| AttrType | type | value | context |
|----------|------|-------|---------|
| FullAttrs (1) | 1 | Tab-separated child names | (empty) |
| String (2) | 2 | String value | Space-separated context elements |
| Bool (6) | 6 | "0" or "1" | (empty) |
| Int (8) | 8 | Integer as decimal string | (empty) |
| Path (9) | 9 | Absolute path string | (empty) |
| Null (10) | 10 | (empty) | (empty) |
| Float (11) | 11 | Float as decimal string | (empty) |
| List (12) | 12 | List size as decimal string | (empty) |
| ListOfStrings (7) | 7 | Tab-separated string elements | (empty) |
| Missing (3) | 3 | (empty) | (empty) |
| Misc (4) | 4 | (empty) | (empty) |
| Failed (5) | 5 | (empty) | (empty) |
| Placeholder (0) | 0 | (empty) | (empty) |

This avoids binary serialization (no CBOR, no zstd). Decoding is a switch on the
integer type with simple string parsing.

### 5.5 Session Caches

`EvalCacheDb` maintains three in-memory session caches to avoid redundant SQLite
reads within a single evaluation session:

- **`validatedAttrIds`** (`std::set<AttrId>`): Attributes whose deps have been
  validated in this session. Skips re-validation on repeated access.
- **`validatedDepSetIds`** (`std::set<int64_t>`): Dep sets whose entries have been
  individually validated. Shared across attributes with the same dep set.
- **`depSetCache`** (`std::map<int64_t, std::vector<Dep>>`): Loaded dep set entries.
  Avoids re-reading `DepSetEntries` rows for the same dep set.

These caches are cleared by `clearSessionCaches()` and are not persisted.

---

## 6. Warm / Cold / Recovery Paths

### 6.1 Warm Path

The warm path serves a cached result without evaluation:

```
1. Attribute lookup
   SELECT FROM Attributes WHERE (context_hash, attr_path) -> AttrRow
   If not found -> cold path

2. Validate attribute (validateAttr)
   a. Load validation info: dep_set_id, parent_id, parent_epoch
   b. Validate dep set (if not already validated in session):
      - SELECT FROM DepSetEntries WHERE set_id = ?
      - For each dep: computeCurrentHash(dep, inputAccessors) -> currentHash
      - If currentHash != dep.expectedHash -> FAIL
   c. Recursively validateAttr(parent_id)
   d. Check parent_epoch matches parent's current epoch
   e. On any failure -> recovery(oldAttrId, ...), then cold path

3. Serve result
   a. Decode AttrValue from (type, value, context) columns
   b. For FullAttrs: create ExprCached child thunks
   c. For origExpr FullAttrs: create ExprOrigChild wrappers via SharedParentResult
   d. For leaves: materialize Value directly
   e. Replay deps to FileLoadTracker (if parent is tracking)
```

Typical warm-path latency is dominated by `lstat()` calls for dep validation,
with the `StatHashCache` providing sub-millisecond lookups for unchanged files.

### 6.2 Cold Path

The cold path evaluates the real thunk and stores the result:

```
1. Navigate to real tree
   navigateToReal(): walk real eval tree from root to target
   - At each level, wrap sibling thunks with ExprCached origExpr wrappers
   - Already-forced siblings are speculatively stored via storeForcedSibling

2. Force value
   forceValue(*target): evaluate the real thunk
   - FileLoadTracker captures all deps during evaluation
   - For derivations: force drvPath to capture derivationStrict deps

3. Store result (coldStore in EvalCacheDb)
   a. Filter out ParentContext deps
   b. Sort and dedup deps
   c. Compute content_hash and struct_hash via HashSink
   d. INSERT OR IGNORE INTO DepSets + SELECT set_id
   e. INSERT OR IGNORE INTO DepSetEntries for each dep
   f. Look up parent's current epoch for parent_epoch column
   g. UPSERT INTO Attributes (ON CONFLICT: epoch = epoch + 1)
   h. Recovery index writes:
      - Phase 1: dep content hash -> DepHashRecovery
      - Phase 2: dep content hash with parent Merkle identity -> DepHashRecovery
      - Phase 3: struct hash -> DepStructGroups
   i. Add to session caches (validatedAttrIds, validatedDepSetIds)

4. Materialize value
   a. For origExpr attrsets: create ExprOrigChild wrappers
   b. For non-origExpr: create ExprCached child thunks via materializeValue
   c. Store forced scalar children of derivation targets

5. Store forced siblings
   storeForcedSibling(): for each already-forced sibling at this level,
   speculatively cache its current value (empty deps, parent reference)
   - Skipped if sibling is already stored (preserves full dep sets)
```

### 6.3 Recovery (Three Phases)

When warm-path validation fails (a dep has changed), recovery attempts to find a
previously stored result whose deps match the *current* state -- avoiding a full
cold-path re-evaluation:

```
Phase 2: Parent-aware dep hash recovery (tried first)       [O(1) + O(depth)]
  Compute parent's Merkle identity hash (computeIdentityHash, recursive)
  Compute depContentHashWithParent from CURRENT dep hashes + parent identity
  Point lookup in DepHashRecovery -> candidate
  Validate candidate dep set -> if valid, upsert Attributes and serve
  Correctly distinguishes child versions across parent changes

Phase 1: Direct dep hash recovery                            [O(1)]
  Compute depContentHash from CURRENT dep hashes (no parent identity)
  Point lookup in DepHashRecovery -> candidate
  Validate candidate -> if valid, upsert Attributes and serve
  Skipped for dep-less children with parent hint (hash([]) is ambiguous)

Phase 3: Structural group scan                               [O(V)]
  Scan DepStructGroups for same (context_hash, attr_path)
  Returns representative dep sets for each unique dep KEY structure
  For each representative: recompute current dep hashes
    Try Phase 1 and Phase 2 lookups with recomputed hashes
  Handles dep structure instability (different deps across cold evals)
```

Phase 2 is tried before Phase 1 when a parent hint is available, because Phase 1
keys do not include parent identity and may return a stale result from a different
parent version.

Recovery is particularly effective for file reverts: reverting a file to a previous
state produces the same dep hashes, and Phase 1 or Phase 2 finds the matching
candidate in O(1).

### 6.4 Merkle Identity Hash (Phase 2)

Phase 2 recovery uses `computeIdentityHash(attrId)` -- a Merkle hash computed
recursively over the attribute's value, deps, and ancestor chain:

```
identityHash(root)  = SHA256("V" + valueHash + "D" + depContentHash)
identityHash(child) = SHA256("V" + valueHash + "D" + depContentHash
                            + "P" + identityHash(parent))
```

Where `valueHash = SHA256("T" + type + "V" + value + "C" + context)`.

This hash is **reproducible** (same state produces the same hash), unlike epoch
which only increments. It **captures the entire ancestor chain**, unlike a plain
dep content hash which cannot distinguish children whose parents have the same
dep structure but different values.

The identity hash is computed **outside lock scopes** in `coldStore()` because it
acquires its own database locks internally (the non-recursive mutex would deadlock
otherwise).

The cost is O(depth) recursive computation per Phase 2 recovery. A future
optimization could cache identity hashes in a session cache.

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
   warm-served children). This is conservative -- `c` will be re-evaluated more
   often than necessary, not less.

3. **Recovery is single-attribute level.** Recovery works for `nix eval` (leaf
   values) but doesn't cascade through deep trees for `nix build`. A recovered
   parent does not automatically recover its children's index entries.

4. **StatHashCache same-size file flakiness.** The stat-hash cache keys on
   `(dev, ino, mtime_nsec, size)`. File modifications of the same size within the
   same mtime granularity can produce false cache hits. Mitigated by using
   nanosecond mtime precision.

5. **No list caching.** Only `FullAttrs` (attrsets) and leaf types are cached at
   intermediate levels. Heterogeneous lists are cached only as leaf values.

6. **Symlinks not tracked.** Intermediate symlink targets are not recorded as deps.
   Changes to a symlink target without changing the resolved file will not
   invalidate the cache.

7. **Merkle identity hash is O(depth).** `computeIdentityHash` recursively walks
   the parent chain, issuing separate SQLite queries at each level. For deeply
   nested attributes, this can be expensive. Not currently cached across calls
   within a session.

---

## 10. Future Work

1. **Cascading recovery.** Extend recovery to cascade through the attribute tree,
   recovering child entries when a parent is recovered. This would improve
   `nix build` warm-path performance after input changes.

2. **Dep replay for warm-served siblings.** Implement dep replay in the warm path
   so that `c = a + b` correctly captures deps from warm-served `a` and `b`.

3. **Parallel dep validation.** Validate deps concurrently using a thread pool.
   Currently validation is sequential; parallelism would reduce warm-path latency
   for attributes with many deps.

4. **Identity hash caching.** Cache `computeIdentityHash` results in a session
   cache (`std::map<AttrId, Hash>`) to avoid redundant recursive computation
   when multiple children trigger Phase 2 recovery against the same parent.

5. **Integration with content-addressed derivations.** Content-addressed
   derivations could provide additional optimization opportunities -- the eval
   cache could skip re-evaluation when a derivation's output is already available
   regardless of input changes.

6. **Symlink tracking.** Record intermediate symlink targets as Existence deps to
   detect symlink retargeting.

---

## Appendix: File Layout

| File | Description |
|------|-------------|
| `src/libexpr/include/nix/expr/eval-cache.hh` | EvalCache public API, AttrType/AttrValue types |
| `src/libexpr/eval-cache.cc` | ExprCached (eval/cache logic), ExprOrigChild, SharedParentResult |
| `src/libexpr/include/nix/expr/eval-cache-db.hh` | EvalCacheDb class declaration |
| `src/libexpr/eval-cache-db.cc` | EvalCacheDb implementation: schema, warm/cold/recovery, validation |
| `src/libexpr/include/nix/expr/eval-result-serialise.hh` | Dep hash computation functions (sortAndDedupDeps, computeDepContentHash, etc.) |
| `src/libexpr/eval-result-serialise.cc` | Dep hash computation implementations |
| `src/libexpr/include/nix/expr/file-load-tracker.hh` | Dep types, FileLoadTracker, dep hash helpers |
| `src/libexpr/file-load-tracker.cc` | FileLoadTracker implementation, stat-cached dep hash variants |
| `src/libexpr/include/nix/expr/stat-hash-cache.hh` | StatHashCache class declaration |
| `src/libexpr/stat-hash-cache.cc` | StatHashCache implementation (L1 concurrent_flat_map + L2 SQLite) |
| `tests/functional/flakes/eval-cache.sh` | Functional tests (flake-based) |
| `tests/functional/eval-cache-impure.sh` | Functional tests (impure / --file / --expr) |
