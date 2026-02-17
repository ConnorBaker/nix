# Nix Eval Trace: Design Document

**Target audience:** Nix core team (assumes familiarity with the evaluator and store).

---

## 1. Motivation

Nix evaluation is expensive. `nix eval nixpkgs#hello.pname` takes ~3–5 seconds on
a cold evaluator because it must parse and evaluate thousands of Nix files even to
reach a single leaf attribute. In typical development workflows — edit-build-test
cycles, CI pipelines, `nix search` — the same flake is evaluated repeatedly with
few or no input changes between runs.

The existing eval trace (`AttrCursor`, ~721 lines in `trace-cache.cc` on `master`)
stores evaluation results in SQLite keyed by a fingerprint derived from the flake
lock file. It has five fundamental limitations:

1. **All-or-nothing invalidation.** The trace key is a hash of the locked flake
   inputs. Any change to any input — even touching a single comment in a single
   file — invalidates the *entire* set of recorded traces. There is no
   per-attribute granularity.

2. **No dependency tracking.** The system records *results* but not *which files
   produced them*. It cannot distinguish between an attribute that depends on the
   changed file and one that does not.

3. **No cross-version recovery.** Because the trace key includes the locked ref
   (which changes every commit), reverting a file to a previous state does not
   recover previously recorded results.

4. **Limited type support.** Only string, bool, int, list-of-strings, and attrset
   are traced. No support for null, float, path, or heterogeneous lists.

5. **No `--file`/`--expr` tracing.** Only flake evaluations are traced.

This design addresses all five limitations with a dependency-tracking eval trace
system that provides per-attribute invalidation, cross-commit recovery, and
transparent integration with the existing CLI.

---

## 2. Prior Art

This system draws on substantial academic and industry literature on incremental
computation and build systems. We adopt standard terminology from the following
works to make the design immediately recognizable and to enable precise discussion
of trade-offs.

### 2.1 Build Systems à la Carte (BSàlC)

> Mokhov, Mitchell, Peyton Jones. "Build Systems à la Carte: Theory and Practice."
> *Journal of Functional Programming*, 2020.

BSàlC provides a taxonomy of build systems based on two axes: **scheduler** (how
tasks are ordered) and **rebuilder** (how staleness is detected). The rebuilder
taxonomy defines:

- **Verifying trace (VT):** Records `(key, [dep_hash], result_hash)`. On lookup,
  recomputes dep hashes and compares. If all match, the result is *valid* —
  but must be recomputed if not available. Our **verify()** path implements this.

- **Constructive trace (CT):** Records `(key, [dep_hash], result)`. Like VT but
  stores the *actual result*, not just its hash. On a match, the result can be
  served directly without recomputation. Our **record()** path stores full results,
  and **recover()** uses historical traces to serve results constructively.

- **Deep constructive trace (DCT):** A CT that records traces at every intermediate
  node, not just leaves. Our system traces at all nesting levels — root attrsets,
  intermediate attrsets, and leaf values.

**Our system is a deep constructive trace store** — it records full results at
every attribute level and can recover them by matching dependency signatures.

### 2.2 Adapton

> Hammer, Khoo, Hicks, Foster. "Adapton: Composable Demand-Driven Incremental
> Computation." *PLDI 2014.*

Adapton introduced demand-driven incremental computation with an explicit
**dependency graph (DDG)** that records which computations depend on which inputs.
Key concepts we adopt:

- **Traced expression (TracedExpr):** A computation whose evaluation is recorded
  in the DDG. Analogous to Adapton's *articulation points*.

- **Dependency tracker (DependencyTracker):** Records the dynamic dependency graph
  during evaluation. Analogous to Adapton's DDG construction during `force`.

- **Fresh evaluation (evaluateFresh):** Demand-driven recomputation when a traced
  result is invalid. Analogous to Adapton's `force` on a dirty node.

- **Trace replay (replayTrace):** Propagating recorded dependencies from a
  previously traced computation to the current tracking context. Analogous to
  Adapton's change propagation.

### 2.3 Salsa

> Matsakis et al. "Salsa: A Framework for On-Demand, Incremental Computation."
> Used in the Rust compiler (rust-analyzer).

Salsa provides versioned queries with memoized results. Our **parent Merkle
chaining** in Phase 1 recovery is analogous to Salsa's versioned query with
context: when a parent's trace_hash is mixed into the child's recovery lookup
key, it disambiguates child traces across different parent versions. The same
child attribute with the same deps but a different parent produces a different
lookup key — analogous to how Salsa's versioned queries produce different
results under different input revisions.

### 2.4 Shake

> Mitchell. "Shake Before Building: Replacing Make with Haskell." *ICFP 2012.*

Shake uses verifying traces with dynamic dependencies — dependencies discovered
during build, not declared statically. Our system shares this property: the
DependencyTracker records deps as they are encountered during evaluation, and the
set of deps can vary between evaluations of the same attribute (e.g., depending
on evaluation order, whether a root file is already cached, etc.).

### 2.5 Terminology Glossary

| This System | Prior Art Term | Definition |
|-------------|---------------|------------|
| **trace** | constructive trace (BSàlC) | A record of `(key, [dep_hash], result)` from an evaluation |
| **verify()** | verifying trace check (BSàlC VT) | Check if recorded dep hashes still match current state |
| **record()** | trace recording (BSàlC CT) | Store evaluation result + dep hashes for future verification |
| **recover()** | constructive trace recovery (BSàlC CT) | Find historical trace with matching deps to reuse its result |
| **verifyTrace()** | verify all deps in a trace (BSàlC) | Validate every dep hash in a trace against current state |
| **TraceStore** | trace store (BSàlC) | Persistent database of recorded traces |
| **TraceCache** | trace-based incremental cache | Cache implemented via constructive traces |
| **TracedExpr** | articulation point (Adapton) | Expression whose evaluation is traced/memoized |
| **DependencyTracker** | DDG builder (Adapton) | Records dynamic dependency graph during evaluation |
| **evaluateFresh()** | demand-driven recomputation (Adapton) | Evaluate an expression from scratch, recording deps |
| **replayTrace()** | change propagation (Adapton) | Replay recorded deps into current tracking context |
| **parent Merkle chaining** | versioned query (Salsa) | Parent trace_hash mixed into child recovery key for disambiguation |

### 2.6 System Classification

**Deep constructive trace store with structural variant recovery.**

- **Constructive** (BSàlC CT): stores result values, not just hashes — can serve
  results without recomputation.
- **Deep** (BSàlC DCT): traces at all nesting levels — root, intermediate attrsets,
  and leaves.
- **Verifying fast path** (BSàlC VT): O(N) dep hash comparison for verification.
- **Dynamic dependencies** (Shake): deps discovered during evaluation, not declared
  statically.
- **Demand-driven** (Adapton): traced expressions are evaluated lazily on access.
- **Structural variant recovery**: novel extension beyond BSàlC — recovers results
  when the dep *structure* (types + sources + keys) is unchanged but the dep
  *values* (hashes) have changed, by scanning historical traces with matching
  structural signatures.

---

## 3. Architecture Overview

The eval trace system is a 3-layer stack with a persistent file hash cache:

```
CLI command (nix eval / nix build / nix search)
  |
  v
TraceCache                                        [trace-cache.hh]
  Public API. Maps a stable identity hash to a root TracedExpr thunk.
  |
  v
TracedExpr thunks                                [trace-cache.cc]
  GC-allocated Expr subclass. eval() dispatches:
  +-- Verify path: DB lookup → dep validation → result decode
  +-- Fresh eval path: navigateToReal → forceValue → DependencyTracker → record
  +-- Recovery: 3-phase trace hash / identity-aware / structural variant
  |
  v
TraceStore                                      [trace-store.cc]
  SQLite backend. Single database at ~/.cache/nix/eval-trace-v1.sqlite.
  +-- verify():       SELECT → verifyTrace → decode CachedResult
  +-- record():       INSERT/UPSERT → trace + recovery index writes
  +-- recover():      3-phase lookup in TraceHistory
  +-- verifyTrace():  validate all deps in a trace against current state
  |
  +=============================================+
  | DependencyTracker    [dep-tracker.cc]       |
  |   RAII thread_local dep recording.          |
  |   Records dynamic dependency graph          |
  |   (Adapton DDG) during evaluation into      |
  |   session-wide vector.                      |
  +=============================================+
  |
  +=============================================+
  | StatHashCache      [stat-hash-cache.cc]     |
  |   Persistent file → BLAKE3 hash cache.      |
  |   L1: concurrent_flat_map (session, 64K cap)|
  |   L2: SQLite (persistent, stat-validated)   |
  +=============================================+
```

Data flows **top-down** for reads (verify path: lookup trace row, validate deps,
decode result) and **bottom-up** for writes (fresh eval path: evaluate, track deps,
compute hashes, upsert trace row). Each layer has clear responsibilities and
a defined interface to the layer below.

---

## 4. Dependency Tracking

### 4.1 Dependency Types

The eval trace tracks 11 dependency types, organized into four categories:

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
| **Structural** | ParentContext (8) | (internal) | Parent trace hash |

**Hash oracle** deps are the most common. They are validated by recomputing a hash
of the current resource content and comparing it to the stored hash — the core
operation of a verifying trace (BSàlC VT). The `StatHashCache` avoids redundant
hashing by caching `(path, stat_metadata) → hash` across sessions.

**Reference** deps validate by re-resolving a reference (computing a store path or
re-fetching a URL) and comparing the result to the stored value. `UnhashedFetch`
respects `tarballTtl` for within-TTL validation.

**Volatile** deps always invalidate. Any attribute that transitively depends on
`currentTime` or `builtins.exec` is re-evaluated every time — these deps make
verification (BSàlC VT check) always fail.

**Structural** deps (ParentContext) are recorded during evaluation but are
**not stored** in the database as separate dep entries. Parent-child
relationships are tracked via the `parentTraceIdHint` parameter passed to
`verify()` and `recovery()`, which enables parent Merkle chaining for
disambiguation (see Section 4.4).

### 4.2 Recording (Adapton DDG Construction)

Dependencies are recorded during evaluation via `DependencyTracker`, an RAII guard
that sets a thread-local pointer on construction and clears it on destruction.
This is analogous to Adapton's DDG construction during `force` — as the evaluator
encounters external resources, it records each as a dependency edge in the graph.

All dep recording goes through a single entry point:

```cpp
void DependencyTracker::record(const Dep & dep);
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

### 4.3 Verification (BSàlC Verifying Trace Check)

During the verify path, each dep is validated by `computeCurrentHash()` in
`trace-store.cc` — this is the core VT verification step from BSàlC. Verification
short-circuits on the first failure: if any dep hash is invalid, the entire trace
is invalid.

For file-based deps (Content, Directory, NARContent), the `StatHashCache` provides
a fast verification path: if the file's stat metadata (device, inode, mtime
nanoseconds, size) matches the cached entry, the stored hash is returned without
re-reading the file. This reduces the verification step to a series of `lstat()`
system calls for unchanged files.

### 4.4 Parent Chain Disambiguation (Salsa Versioned Query)

Parent context is critical for recovery correctness. When a child attribute has
the same deps across two evaluations but its parent has changed, the child's
result may differ. To disambiguate, Phase 1 recovery uses **parent Merkle
chaining**: the parent's `trace_hash` is mixed into the child's recovery lookup
key via `computeTraceHashWithParentFromSorted()`. This is analogous to Salsa's
versioned query with context — the same function with different argument versions
produces different lookup keys.

During verification, `verify()` delegates to `verifyTrace()` which checks all
dep hashes in the trace. There is no separate parent staleness counter; parent
validity is ensured through the dependency chain itself (the child's deps
include all deps inherited from the parent evaluation context via
`DependencyTracker` nesting).

The parent chain is critical for correctness: an attribute like `hello.pname` has
its own deps (the files it directly reads) plus all deps inherited from the
evaluation context that produced the `hello` attrset. Without the parent chain,
changing a file that affects the `hello` derivation itself (but not `pname`
specifically) would not invalidate `hello.pname`.

---

## 5. Storage Model

### 5.1 SQLite Database

All trace data lives in a single SQLite database at
`~/.cache/nix/eval-trace-v1.sqlite`. This includes attribute results, recorded
traces, and recovery indices. The database uses WAL mode, memory-mapped I/O
(30 MB), and a 4 MB page cache for performance.

The choice of a unified SQLite database (rather than separate index + store, or
content-addressed store objects) provides several advantages:

1. **Simplicity.** A single file with well-understood semantics. No coordination
   between multiple storage layers.

2. **Atomic writes.** SQLite's ACID transactions ensure that attribute upserts,
   trace insertions, and recovery index writes are atomic. A crash mid-write
   cannot leave the trace store in an inconsistent state.

3. **Space efficiency.** Traces are deduplicated by `trace_hash` (attributes
   with identical deps share a single `Traces` row). Attribute values are stored
   as SQL columns (type INTEGER, value TEXT, context TEXT) with no serialization
   overhead.

4. **Expendability.** The database is a cache. If deleted or corrupted, fresh
   evaluation re-records everything. Correctness is never at risk.

### 5.2 Schema (6 Tables)

```sql
CREATE TABLE IF NOT EXISTS Strings (
    id    INTEGER PRIMARY KEY,
    value TEXT NOT NULL UNIQUE
) STRICT;

CREATE TABLE IF NOT EXISTS AttrPaths (
    id   INTEGER PRIMARY KEY,
    path BLOB NOT NULL UNIQUE
) STRICT;

CREATE TABLE IF NOT EXISTS Results (
    id      INTEGER PRIMARY KEY,
    type    INTEGER NOT NULL,
    value   TEXT,
    context TEXT,
    hash    BLOB NOT NULL UNIQUE
) STRICT;

CREATE TABLE IF NOT EXISTS Traces (
    id            INTEGER PRIMARY KEY,
    base_trace_id INTEGER REFERENCES Traces(id),
    trace_hash    BLOB NOT NULL UNIQUE,
    struct_hash   BLOB NOT NULL,
    deps_blob     BLOB NOT NULL
) STRICT;

CREATE TABLE IF NOT EXISTS CurrentTraces (
    context_hash  INTEGER NOT NULL,
    attr_path_id  INTEGER NOT NULL,
    trace_id      INTEGER NOT NULL,
    result_id     INTEGER NOT NULL,
    PRIMARY KEY (context_hash, attr_path_id)
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS TraceHistory (
    context_hash  INTEGER NOT NULL,
    attr_path_id  INTEGER NOT NULL,
    trace_id      INTEGER NOT NULL,
    result_id     INTEGER NOT NULL,
    PRIMARY KEY (context_hash, attr_path_id, trace_id)
) WITHOUT ROWID, STRICT;
```

**Strings** and **AttrPaths** are interning tables that deduplicate string values
and attribute path BLOBs via unique constraints, reducing storage for repeated
references.

**Results** deduplicates attribute results by a content hash. Each row stores the
`(type, value, context)` triple encoding a `CachedResult`. Multiple attributes
with the same result share a single Results row.

**Traces** stores deduplicated dependency traces (BSàlC constructive traces),
keyed by `trace_hash` (SHA-256 of the full dep content including hash values).
The `struct_hash` captures the structural signature (dep types + sources + keys,
without hash values) for Phase 3 structural variant recovery. The `deps_blob`
stores delta-encoded dependency entries as a compact BLOB, with `base_trace_id`
pointing to a base trace for delta chain resolution.

**CurrentTraces** maps `(context_hash, attr_path_id)` to the current trace and
result for each attribute. This is the primary lookup table for the verify path.

**TraceHistory** stores all historical `(context_hash, attr_path_id, trace_id,
result_id)` tuples — every trace that has ever been recorded for an attribute.
This is the constructive trace store that enables recovery: when the current
trace is invalid, recovery searches TraceHistory for a historical trace whose
deps match the current state.

### 5.3 One Row Per Attribute, Keys-Only Attrsets

Each attribute in the evaluation tree gets its own row in the `CurrentTraces`
table. For attrsets (`FullAttrs`), the result stores **only the child key names**
(tab-separated), not child values. Each child value lives in a separate row with
its own trace, referencing the parent via the attribute path hierarchy.

This has three important consequences:

1. **Atomic updates.** Each attribute row is independently upserted. Forcing
   additional children of an attrset does not modify the parent's row — only
   new child rows are created.

2. **Partial evaluation is natural.** An attrset can be traced (with its full
   key list) before any of its children's values are traced. When a child is
   later accessed, the verify path serves the parent's key list and creates
   thunks for each child. Only the accessed child triggers a fresh evaluation
   for its individual value.

3. **No reconciliation across sessions.** If session A traces `hello.pname` and
   session B later traces `hello.name`, no coordination is needed. The `hello`
   attrset row (containing the key list) was written once during session A and
   is reused as-is. Session B only adds a new child row for `name`.

### 5.4 CachedResult Encoding

Attribute values are encoded as three SQL columns: `type` (INTEGER), `value`
(TEXT), and `context` (TEXT). The encoding is straightforward:

| ResultKind | type | value | context |
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

`TraceStore` maintains three in-memory session caches to avoid redundant SQLite
reads within a single evaluation session:

- **`validatedAttrIds`** (`std::set<AttrId>`): Attributes whose traces have been
  verified in this session. Skips re-verification on repeated access.
- **`verifiedTraceIds`** (`std::set<int64_t>`): Traces whose dep entries have been
  individually verified (BSàlC VT check passed). Shared across attributes with
  the same trace.
- **`traceCache`** (`std::map<int64_t, std::vector<Dep>>`): Loaded trace entries.
  Avoids re-reading `Traces` rows for the same trace.

These caches are cleared by `clearSessionCaches()` and are not persisted.

---

## 6. Verify / Record / Recover Paths

### 6.1 Verify Path (BSàlC Verifying Trace Check)

The verify path serves a traced result without evaluation — the core VT check:

```
1. Trace lookup
   SELECT FROM CurrentTraces WHERE (context_hash, attr_path_id) → TraceRow
   If not found → fresh evaluation path

2. Verify trace (verifyTrace — BSàlC VT check)
   a. Load trace: trace_id, deps from Traces table
   b. Verify trace (if not already verified in session):
      - Load deps from deps_blob (delta-decode if needed)
      - For each dep: computeCurrentHash(dep, inputAccessors) → currentHash
      - If currentHash != dep.expectedHash → FAIL (trace invalid)
      - Volatile deps (CurrentTime, Exec) always fail
      - All computed hashes cached in currentDepHashes for recovery reuse
   c. On any failure → recovery(oldTraceId, ...), then fresh evaluation path

3. Serve result (constructive trace — BSàlC CT)
   a. Decode CachedResult from Results table
   b. For FullAttrs: create TracedExpr child thunks (deep tracing)
   c. For origExpr FullAttrs: create ExprOrigChild wrappers via SharedParentResult
   d. For leaves: materialize Value directly
   e. Replay trace to DependencyTracker (Adapton change propagation)
```

Typical verify-path latency is dominated by `lstat()` calls for dep verification,
with the `StatHashCache` providing sub-millisecond lookups for unchanged files.

### 6.2 Fresh Evaluation Path (Adapton Demand-Driven Recomputation)

The fresh evaluation path evaluates the real thunk and records the trace:

```
1. Navigate to real tree
   navigateToReal(): walk real eval tree from root to target
   - At each level, wrap sibling thunks with TracedExpr origExpr wrappers
   - Already-forced siblings are speculatively recorded via recordSiblingTrace

2. Force value (Adapton force)
   forceValue(*target): evaluate the real thunk
   - DependencyTracker records dynamic dependency graph (Adapton DDG)
   - For derivations: force drvPath to capture derivationStrict deps

3. Record trace (BSàlC constructive trace recording)
   a. Collect deps from DependencyTracker
   b. Sort and dedup deps
   c. Compute trace_hash (with parent Merkle chaining if parent exists)
      and struct_hash via HashSink
   d. INSERT OR IGNORE INTO Traces (dedup by trace_hash)
   e. Intern result into Results table (dedup by result hash)
   f. UPSERT INTO CurrentTraces (ON CONFLICT: update trace_id, result_id)
   g. INSERT INTO TraceHistory (record for future constructive recovery)
   h. Add to session caches (verifiedTraceIds)

4. Materialize result (Adapton articulation point)
   a. For origExpr attrsets: create ExprOrigChild wrappers
   b. For non-origExpr: create TracedExpr child thunks via materializeResult
   c. Store forced scalar children of derivation targets

5. Record sibling traces
   recordSiblingTrace(): for each already-forced sibling at this level,
   speculatively record its current value (empty deps, parent reference)
   - Skipped if sibling is already recorded (preserves full traces)
```

### 6.3 Recovery (BSàlC Constructive Trace Recovery)

When verification fails (a dep hash has changed), recovery attempts to find a
previously recorded trace whose deps match the *current* state — this is the
key advantage of a constructive trace (BSàlC CT) over a verifying trace (VT):
the CT stores full results, enabling recovery without re-evaluation.

```
Phase 1: Direct hash recovery (with Salsa-style parent chaining)  [O(1)]
  Recompute current dep hashes from old trace's dep keys
  If parentTraceIdHint available:
    Get parent's trace_hash → mix into child trace_hash (Merkle chaining)
    This disambiguates child traces across parent changes
    (Analogous to Salsa's versioned query with context)
  Else:
    Compute plain trace_hash from current dep hashes
  Point lookup in Traces table by trace_hash → candidate trace
  Verify candidate → if valid, update CurrentTraces and serve

Phase 3: Structural variant recovery (novel, beyond BSàlC)        [O(V)]
  Scan TraceHistory for same (context_hash, attr_path_id)
  Group by struct_hash — returns representative traces for each unique
  dep KEY structure (types + sources + keys, without hash values)
  For each representative: load its deps, recompute current hashes
    Retry Phase 1 lookup with recomputed hashes
  Handles dep structure instability (dynamic deps — different deps across
  fresh evaluations depending on evaluation order, per Shake)
```

Recovery is particularly effective for file reverts: reverting a file to a previous
state produces the same dep hashes, and Phase 1 finds the matching candidate in
O(1). This is a direct consequence of the constructive trace property — the
historical result is stored and can be served immediately.

### 6.4 Parent Merkle Chaining (Salsa Versioned Query)

Phase 1 recovery uses `computeTraceHashWithParentFromSorted()` to mix the parent's
`trace_hash` into the child's recovery lookup key. This creates a Merkle chain:

```
traceHash(root)  = SHA256(sorted_deps)
traceHash(child) = SHA256(sorted_deps + "P" + parent.traceHash)
```

The parent's `trace_hash` is itself computed from its deps (and its parent's hash,
recursively), so the child's lookup key transitively captures the entire ancestor
chain. This correctly disambiguates: the same child deps under different parents
produce different lookup keys.

The cost is O(1) per recovery (the parent's trace_hash is already stored in the
Traces table and retrieved via `getTraceFullHash()`).

This is analogous to Salsa's versioned query: the "same function" (same attribute)
with different "input revisions" (different parent state) maps to different
memoization entries.

---

## 7. Partial Tree Invalidation

The key mechanism for per-attribute granularity is `navigateToReal()`:

1. When a fresh evaluation is needed for attribute `A.B.C`, `navigateToReal()`
   walks the real evaluation tree from root to `C`, forcing each intermediate
   attrset.

2. At each level, **sibling thunks are wrapped** with `TracedExpr` wrappers that
   have `origExpr`/`origEnv` set. These wrappers try verification first when
   the sibling is later forced (e.g., as a side effect of `derivationStrict`).

3. This means changing one file typically invalidates only the attributes that
   depend on it. Other attributes at the same level are served from their
   verified traces via their origExpr wrappers.

### 7.1 ExprOrigChild

When the verify path serves a `FullAttrs` result, it creates children as
`ExprOrigChild` thunks. These resolve children by evaluating the **parent's
original expression** (shared across siblings via `SharedParentResult`) rather
than using `navigateToReal()`, which would cycle through the materialized parent
and hit a blackhole.

`ExprOrigChild` uses `SuspendDepTracking` during parent evaluation to prevent
"fat parent" dep explosion. Without suspension, evaluating a parent like
`buildPackages` (= all of nixpkgs) would record 10,000+ file deps into each
child's trace — violating the per-attribute granularity goal.

### 7.2 SharedParentResult

`SharedParentResult` is a GC-allocated struct ensuring the parent is evaluated at
most once across all sibling `ExprOrigChild` instances. The first child to be
forced evaluates the parent; subsequent siblings reuse the cached result.

---

## 8. Trace Identity

### 8.1 Flakes

For flake evaluations, the trace identity is computed via
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

These are also version-independent, enabling tracing and recovery across
modifications to the evaluated file or expression.

---

## 9. Known Limitations and Trade-offs

1. **origExpr wrappers skip eager drvPath forcing.** When a sibling is wrapped
   with an origExpr `TracedExpr` wrapper, it does not eagerly force `drvPath`.
   This means deps from `derivationStrict` env processing (e.g., `readFile` in a
   `buildCommand` string interpolation) are captured only when `drvPath` or
   `outPath` is naturally accessed. Trade-off: avoids infinite recursion through
   nixpkgs' `buildPackages = self` fixed-point.

2. **Verify-served siblings don't replay traces.** When `c = a + b` and `a`, `b`
   are served from the verify path, their deps are not replayed into `c`'s
   `DependencyTracker`. Result: `c`'s trace may be incomplete (missing deps from
   verify-served children). This is conservative — `c` will be re-evaluated more
   often than necessary, not less.

3. **Recovery is single-attribute level.** Recovery works for `nix eval` (leaf
   values) but doesn't cascade through deep trees for `nix build`. A recovered
   parent does not automatically recover its children's trace entries.

4. **StatHashCache same-size file flakiness.** The stat-hash cache keys on
   `(dev, ino, mtime_nsec, size)`. File modifications of the same size within the
   same mtime granularity can produce false cache hits. Mitigated by using
   nanosecond mtime precision.

5. **Dynamic dep instability.** Like Shake, dependencies are discovered during
   evaluation and can vary between evaluations of the same attribute (e.g.,
   depending on whether a root file is already cached). This means hash-based
   recovery (Phases 1–2) cannot be the sole mechanism — Phase 3 structural
   variant recovery is needed as a fallback.

6. **Symlinks not tracked.** Intermediate symlink targets are not recorded as deps.
   Changes to a symlink target without changing the resolved file will not
   invalidate the trace.

7. **Merkle identity hash is O(depth).** `computeIdentityHash` recursively walks
   the parent chain, issuing separate SQLite queries at each level. For deeply
   nested attributes, this can be expensive. Not currently cached across calls
   within a session.

---

## 10. Future Work

1. **Cascading recovery.** Extend recovery to cascade through the attribute tree,
   recovering child entries when a parent is recovered. This would improve
   verify-path performance after input changes.

2. **Trace replay for verify-served siblings.** Implement trace replay in the
   verify path so that `c = a + b` correctly captures deps from verify-served
   `a` and `b`.

3. **Parallel dep verification.** Verify deps concurrently using a thread pool.
   Currently verification is sequential; parallelism would reduce verify-path
   latency for attributes with many deps.

4. **Parent trace_hash caching.** Cache `getTraceFullHash` results in a session
   cache to avoid redundant DB lookups when multiple children use the same
   parent's trace_hash for Merkle chaining during recovery.

5. **Integration with content-addressed derivations.** Content-addressed
   derivations could provide additional optimization opportunities — the eval
   trace could skip re-evaluation when a derivation's output is already available
   regardless of input changes.

6. **Symlink tracking.** Record intermediate symlink targets as Existence deps to
   detect symlink retargeting.

---

## Appendix A: File Layout

| File | Description |
|------|-------------|
| `src/libexpr/include/nix/expr/trace-cache.hh` | TraceCache public API, ResultKind/CachedResult types |
| `src/libexpr/trace-cache.cc` | TracedExpr (verify/record/recover logic), ExprOrigChild, SharedParentResult |
| `src/libexpr/include/nix/expr/trace-store.hh` | TraceStore class declaration |
| `src/libexpr/trace-store.cc` | TraceStore implementation: schema, verify, record, recover, verifyTrace |
| `src/libexpr/include/nix/expr/trace-hash.hh` | Trace hash computation functions (sortAndDedupDeps, computeTraceHash, etc.) |
| `src/libexpr/trace-hash.cc` | Trace hash computation implementations |
| `src/libexpr/include/nix/expr/dep-tracker.hh` | Dep types, DependencyTracker, dep hash helpers |
| `src/libexpr/dep-tracker.cc` | DependencyTracker implementation, stat-cached dep hash variants |
| `src/libexpr/include/nix/expr/stat-hash-cache.hh` | StatHashCache class declaration |
| `src/libexpr/stat-hash-cache.cc` | StatHashCache implementation (L1 concurrent_flat_map + L2 SQLite) |
| `tests/functional/flakes/eval-trace-*.sh` | Functional tests (flake-based) |
| `tests/functional/eval-trace-impure-*.sh` | Functional tests (impure / --file / --expr) |

## Appendix B: References

1. Mokhov, Mitchell, Peyton Jones. "Build Systems à la Carte: Theory and Practice."
   *Journal of Functional Programming* 30, e11, 2020.

2. Hammer, Khoo, Hicks, Foster. "Adapton: Composable Demand-Driven Incremental
   Computation." *PLDI 2014.*

3. Matsakis et al. "Salsa: A Framework for On-Demand, Incremental Computation."
   https://github.com/salsa-rs/salsa

4. Mitchell. "Shake Before Building: Replacing Make with Haskell." *ICFP 2012.*
