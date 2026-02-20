# Nix Eval Trace: Design Document

**Target audience:** Nix core team (assumes familiarity with the evaluator and store).

---

## 1. Motivation

Nix evaluation is expensive. `nix eval nixpkgs#hello.pname` takes ~3–5 seconds on
a cold evaluator because it must parse and evaluate thousands of Nix files even to
reach a single leaf attribute. In typical development workflows — edit-build-test
cycles, CI pipelines, `nix search` — the same flake is evaluated repeatedly with
few or no input changes between runs.

The existing eval trace (`AttrCursor` in `trace-cache.cc` on `master`)
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
  and **recovery()** uses historical traces to serve results constructively.

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

Salsa provides versioned queries with memoized results. Our **ParentContext
deps** are analogous to Salsa's versioned query with context: a child trace
stores a `ParentContext` dep (DepType=8) containing the parent's `trace_hash`,
linking the child to a specific parent version. The same child attribute with
the same own deps but a different parent produces a different `trace_hash`
(because the ParentContext dep hash differs) — analogous to how Salsa's
versioned queries produce different results under different input revisions.

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
| **recovery()** | constructive trace recovery (BSàlC CT) | Find historical trace with matching deps to reuse its result |
| **verifyTrace()** | verify all deps in a trace (BSàlC) | Validate every dep hash in a trace against current state |
| **TraceStore** | trace store (BSàlC) | Persistent database of recorded traces |
| **TraceCache** | trace-based incremental cache | Cache implemented via constructive traces |
| **TracedExpr** | articulation point (Adapton) | Expression whose evaluation is traced/memoized |
| **DependencyTracker** | DDG builder (Adapton) | Records dynamic dependency graph during evaluation |
| **evaluateFresh()** | demand-driven recomputation (Adapton) | Evaluate an expression from scratch, recording deps |
| **replayTrace()** | change propagation (Adapton) | Replay recorded deps into current tracking context |
| **ParentContext dep** | versioned query (Salsa) | Parent trace_hash stored as a regular dep in the child trace for disambiguation |

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
  +-- Recovery: direct hash / structural variant
  |
  v
TraceStore                                      [trace-store.cc]
  SQLite backend. Single database at ~/.cache/nix/eval-trace-v2.sqlite.
  +-- verify():       SELECT → verifyTrace → decode CachedResult
  +-- record():       INSERT/UPSERT → trace + recovery index writes
  +-- recovery():     direct hash + structural variant lookup in TraceHistory
  +-- verifyTrace():  validate all deps in a trace against current state
  |
  +=============================================+
  | DependencyTracker  [dependency-tracker.cc]  |
  |   RAII thread_local dep recording.          |
  |   Records dynamic dependency graph          |
  |   (Adapton DDG) during evaluation into      |
  |   session-wide vector.                      |
  |   Includes internal StatHashCache:          |
  |   L1: concurrent_flat_map (session, 64K cap)|
  |   L2: bulk-loaded from TraceStore's SQLite  |
  |   Dirty entries flushed back on TraceStore  |
  |   destruction (single DB, no separate file) |
  +=============================================+
```

Data flows **top-down** for reads (verify path: lookup trace row, validate deps,
decode result) and **bottom-up** for writes (fresh eval path: evaluate, track deps,
compute hashes, upsert trace row). Each layer has clear responsibilities and
a defined interface to the layer below.

---

## 4. Dependency Tracking

### 4.1 Dependency Types

The eval trace tracks 12 dependency types, organized into four categories:

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
| | StructuredContent (12) | `fromJSON`/`fromTOML`/`readDir` leaf access | BLAKE3 of canonical scalar at data path |
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

**Structural** deps (ParentContext) are stored as regular dep entries in the
trace's dep set, just like any other dep type. A ParentContext dep records
the parent's `trace_hash`, which is looked up via `getCurrentTraceHash()`.
This links the child trace to a specific parent version without merging parent
deps into the child (see Section 4.5).

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
- `json-to-value.cc`: `ExprTracedData::eval()` → StructuredContent dep (scalar leaf access)
- `primops/fromTOML.cc`: `ExprTracedData::eval()` → StructuredContent dep (via shared `ExprTracedData`)
- `primops.cc`: `readDir` + `ExprTracedData::eval()` → StructuredContent dep (directory entry type access, format tag `'d'`)

Each tracker deduplicates deps by `(type, source, key)` within its scope using a
`DepKey` hash set, so the same file read twice during one attribute's evaluation
produces only one dep entry.

### 4.3 Lazy Structural Dependency Tracking (TracedData)

When `fromJSON` or `fromTOML` parses a string that came directly from `readFile`
(validated by matching the string's BLAKE3 hash against the recorded Content dep),
the result is returned as a **lazy attrset/list** whose children are thunks.
Similarly, when `readDir` is called with eval-trace active, the result is an
`ExprTracedData`-wrapped attrset with one thunk per directory entry (backed by
`DirDataNode`/`DirScalarNode`).
Each thunk wraps an `ExprTracedData` node that, when forced:

- **For containers (objects/arrays):** Creates another lazy attrset/list with
  child thunks. No dep is recorded for intermediate containers.
- **For scalar leaves:** Records a `StructuredContent` dep with the BLAKE3 hash
  of the scalar's canonical representation.

This means only **actually accessed** scalar values produce deps. Adding,
removing, or modifying keys/elements that are never accessed does NOT invalidate
the trace.

**Dep key format:** `"filepath\tf:datapath"` where `\t` is the tab separator,
`f` is the format tag (`j` for JSON, `t` for TOML, `d` for directory entries),
and `datapath` uses `.` for object keys and `.[N]` for array indices. Example:
`"/path/to/flake.lock\tj:nodes.nixpkgs.locked.rev"` or
`"/path/to/src\td:main.cc"`.

**Provenance threading:** `readFile` sets a thread-local `ReadFileProvenance`
(path + content hash). `fromJSON`/`fromTOML` consumes it and verifies the
content hash matches the string being parsed. If the hash doesn't match (string
was modified after readFile), eager parsing is used (no structural deps).

**Verification:** During `computeCurrentHash` for a `StructuredContent` dep:

1. Parse the dep key to extract file path, format tag, and data path.
2. Read and parse the file (DOM cached per-file within a `verifyTrace` call).
3. Navigate the DOM to the data path.
4. Hash the leaf's canonical value and compare with the stored hash.

### 4.4 Verification (BSàlC Verifying Trace Check)

Verification operates in two modes depending on the caller:

- **Initial verification** (`verify()` → `verifyTrace(earlyExit=true)`):
  short-circuits on the first dep hash mismatch. This is the fast path for the
  common case where verification succeeds — only `lstat()` calls are needed for
  unchanged files (via StatHashCache). On the first failure, verification aborts
  immediately and hands off to `recovery()`.

- **Recovery hash recomputation** (`recovery()` → iterates deps with
  `computeCurrentHash`): computes current hashes for *all* deps in a trace,
  caching them in `currentDepHashes`. These hashes are populated lazily
  on-demand — `verifyTrace(earlyExit=false)` computes all hashes even on
  mismatch, and recovery reuses the cached values. This avoids redundant
  `lstat()` / hash computation when the same dep appears in multiple
  historical traces during structural variant recovery.

For file-based deps (Content, Directory, NARContent), the `StatHashCache` provides
a fast verification path: if the file's stat metadata (device, inode, mtime
nanoseconds, size) matches the cached entry, the stored hash is returned without
re-reading the file. This reduces the verification step to a series of `lstat()`
system calls for unchanged files.

#### Two-Level Verification (StructuredContent Override)

When `fromJSON(readFile f)` or `fromTOML(readFile f)` is used, the eval trace
records both a whole-file `Content` dep and fine-grained `StructuredContent` deps
for each accessed scalar leaf. Similarly, when `readDir` is used, a whole-listing
`Directory` dep and per-entry `StructuredContent` deps (format tag `'d'`) are
recorded. During verification, if the `Content` or `Directory` dep fails
(file/directory changed) but all `StructuredContent` deps pass (accessed
values unchanged), the trace remains valid. This two-level override enables
traces to survive changes to unused parts of structured data files (e.g.,
changing an unused input's `rev` in `flake.lock`) or directory listings (e.g.,
adding a new file to a directory when only specific entries were checked).

The override logic in `verifyTrace()`:

1. **First pass:** Verify all non-structural deps normally. Defer
   `StructuredContent` deps. Track which `Content` and `Directory` deps failed.
2. **If any non-Content, non-Directory, non-structural dep fails:** trace
   invalid (no override).
3. **If Content/Directory failures exist and structural deps cover those
   files/directories:** verify all structural deps. If all pass,
   Content/Directory failures are overridden.
4. **If Content/Directory failure with no structural coverage:** trace invalid
   (backward compatible with pre-structural behavior).

#### Key-Set Safety with `mapAttrs` and `attrNames`

The lazy attrset design intentionally does NOT track object key sets as deps —
only `Content` (whole file) covers them. This interacts correctly with
`mapAttrs` and `attrNames` because of how the two-level override operates:

**`mapAttrs` with no leaf access** — e.g., `mapAttrs f (fromJSON (readFile f))`
returned as a root attrset. `mapAttrs` iterates keys and creates new thunks but
forces no values. Only the `Content` dep is recorded (no `StructuredContent`
deps). If the file changes, `Content` fails and there are no structural deps to
trigger the override → the trace is invalidated and re-evaluated. The new key set
is correctly reflected in the fresh result.

**`mapAttrs` with specific value access** — e.g.,
`(mapAttrs f (fromJSON (readFile f))).nixpkgs`. The root result is the value at
`.nixpkgs`, not the full attrset. `StructuredContent` deps are recorded for the
accessed leaf values. If the file changes (key added/removed) but the accessed
values are unchanged, the override applies and the cached result (the leaf value)
is served — correctly, because the key set is irrelevant to the result.

**`attrNames` with no leaf access** — e.g.,
`attrNames (fromJSON (readFile f))`. This enumerates keys without forcing any
values. Only `Content` is recorded. Any file change invalidates.

**Array size safety.** The same principle applies to lists. `builtins.length`
reads the list's size field without forcing elements — no `StructuredContent`
deps recorded. If no elements are forced, `Content` alone controls. If specific
elements are forced (e.g., `elemAt list 0`), the override applies to the leaf
value and the list size is irrelevant to that result.

Note: `TracedExpr::evaluateFresh()` eagerly forces all list elements to check if
they're all strings (the `allStrings` optimization). This forcing is wrapped in
`SuspendDepTracking` to prevent `StructuredContent` deps from contaminating the
list trace. Without this, adding an element to a JSON array could incorrectly
trigger the override.

**Why this is correct in general:** The hierarchical trace structure naturally
separates structural concerns (key sets, list sizes) from value concerns.
Container structures are cached at the trace level that produces them, where only
`Content` deps exist. `StructuredContent` deps only appear in child traces
(when individual leaf values are forced), where the cached result is the leaf
value and structural changes do not affect correctness.

#### Directory Two-Level Override

The two-level override extends beyond JSON and TOML to directory listings.
When `readDir` is called with eval-trace active, it returns an
`ExprTracedData`-wrapped attrset whose children are lazy thunks (one per
directory entry). The directory-level `Directory` dep (BLAKE3 of the full
sorted listing) acts as the "coarse" dep, and per-entry `StructuredContent`
deps with format tag `'d'` act as the "fine-grained" deps.

**How it works.** `readDir` records a `Directory` dep covering the entire
listing. The returned attrset has one key per entry; each value is a
`DirScalarNode` thunk. When a specific entry is forced, a
`StructuredContent` dep is recorded with the BLAKE3 hash of the entry's
type string (the canonical value: `"regular"`, `"directory"`, `"symlink"`,
or `"unknown"`). If the directory listing changes (entry added/removed) but
the accessed entries' types are unchanged, the `Directory` dep fails while
all `StructuredContent` deps pass, and the two-level override keeps the
trace valid.

The dep key format mirrors the JSON/TOML pattern:
`"dirpath\td:entryname"` where `\t` is the tab separator, `d` is the
format tag for directory entries, and `entryname` is the directory entry
name. For example: `"/path/to/src\td:main.cc"`.

**Canonical values.** The `dirEntryTypeString()` helper (in
`dependency-tracker.hh/.cc`) maps `SourceAccessor::Type` to a canonical
string: `"regular"`, `"directory"`, `"symlink"`, or `"unknown"`. This
string is BLAKE3-hashed for the `StructuredContent` dep value.

**TracedDataNode implementations.** `DirDataNode` and `DirScalarNode`
(defined in `primops.cc`) implement the `TracedDataNode` virtual interface
for directory listings. `DirDataNode` is the container (one child per
entry); `DirScalarNode` is the leaf (produces the type string as canonical
value). All entry types are known eagerly from the `readDir` result, so
the lazy thunks do not re-read the directory.

**By-name limitation.** When `mapAttrs` is used as `mapAttrs (name: _: ...)`
(accessing only names, ignoring values), no entry thunks are forced, so no
`StructuredContent` deps are recorded. Only the `Directory` dep controls
invalidation. Any directory change invalidates the trace — this is the
correct, conservative behavior because the key set changed. Language
constructs that propagate attrsets transparently (`//`, `with`, `rec`,
`self:`, `mapAttrs`) are transparent to dep tracking — they neither force
nor suppress thunk evaluation.

**Shape observation tracking.** When shape-observing builtins (`length`,
`attrNames`, `hasAttr`) operate on traced data containers, a shape dep is
recorded as a `StructuredContent` dep with a `#len` or `#keys` suffix.
This ensures the two-level override cannot serve stale structural data.
For example, `(toString (length arr)) + "-" + (elemAt arr 0)` correctly
invalidates when the array length changes, even if element 0 is unchanged.

The shape dep key format is `"filepath\tf:datapath#len"` for list length
(BLAKE3 of decimal size string) and `"filepath\tf:datapath#keys"` for
attrset key sets (BLAKE3 of null-byte-joined sorted key names). Shape deps
reuse the `StructuredContent` dep type and participate in two-level
verification unchanged: if the shape changes, the dep fails and the
override is prevented.

Shape deps are recorded **only** when a shape-observing builtin is used.
Point observers (`elemAt`, attribute select `.name`) and shape-preserving
transforms (`mapAttrs`, `map`) do not record shape deps. This preserves
the primary use case: `data.x` still survives key-set changes in the
file because no shape dep is recorded — only the `StructuredContent` dep
for `x`'s value participates in the override.

**Remaining edge case (strict consumers).** Builtins like `sort`, `foldl'`,
`filter`, `any`, `all` observe both shape and all values but are not yet
instrumented. If a single TracedExpr applies such a builtin to traced data,
an element count change could still cause a stale result. These builtins
can be instrumented incrementally by adding `maybeRecordListLenDep()` calls.

### 4.5 Separated Parent and Child Deps

Each trace stores only its **own** deps — deps recorded by the
`DependencyTracker` during that specific thunk's evaluation. Parent deps are
never merged into child traces. This separation has important consequences:

1. **Parent invalidation does not cascade to children.** When a parent attrset's
   Directory dep changes (e.g., a package is added to `pkgs/by-name/`), only the
   parent trace is invalidated. Child traces (e.g., `hello.pname`) retain their
   own (unchanged) deps and pass verification independently.

2. **Correctness is maintained through evaluation ordering.** The parent is
   always forced before its children. If the parent's trace is invalid, it is
   re-evaluated (producing an updated attrset). Children are then accessed from
   the new attrset. A child's own deps capture everything it directly depends on
   — if a file change affects the child's evaluation, the child's own deps will
   reflect that change.

3. **Recovery hit rates improve dramatically.** Without parent dep merging,
   child `trace_hash` values are stable across parent changes. Direct hash
   recovery succeeds for ~10k child traces that would previously have failed
   due to inherited parent dep changes.

Parent-child relationships are now expressed through `ParentContext` deps
(DepType=8), which store the parent's `trace_hash` as a regular dep entry.
This replaces the previous design where `record()` merged all parent deps
into child traces and used Merkle chaining (`computeTraceHashWithParent`)
to disambiguate — that approach caused cascading invalidation because any
parent dep change invalidated all children.

### 4.6 Dependency Over-Approximation

The eval trace system tracks file-level dependencies — a **Content dep** records
that the entire file was read, and a **Directory dep** records the entire sorted
directory listing. For data files consumed by `fromJSON`/`fromTOML`, the
**StructuredContent two-level override** (Section 4.4) provides fine-grained
tracking: only the specific JSON/TOML scalars that were accessed produce deps,
and changes to unaccessed parts of the file do not invalidate the trace.
Similarly, for `readDir` with `ExprTracedData` wrapping (Section 4.4, Directory
Two-Level Override), only accessed directory entries produce deps.

However, for `.nix` code consumed via `import`/`evalFile`, **no fine-grained
override exists**. Nix reads and evaluates the entire `.nix` file as code, and
there is no way to determine which portion of a `.nix` file affects which output
without language-level incremental support (cf. Salsa's query-level tracking in
rust-analyzer, where every function call is a tracked query). The Content dep at
parse time is correct but inherently coarse.

**How it manifests.** `readFile`/`evalFile` records a Content dep covering the
entire file. `readDir` records a Directory dep covering the full listing. The
two-level verification (Section 4.4) mitigates Content and Directory failures for
structured data (JSON/TOML via `fromJSON`/`fromTOML`) and directory entries
(readDir with `ExprTracedData` wrapping). For `.nix` code consumed via `import`,
no override exists — any byte change invalidates the trace.

**Consequences for large shared files.** Nixpkgs contains large shared `.nix`
files — `aliases.nix` (~8,300 lines), `python-packages.nix` (~40,000 lines),
`all-packages.nix` (~50,000 lines) — imported by many evaluation paths. A
semantically irrelevant change (renaming an unused alias, adding an unrelated
Python package) invalidates **all** traces that import the changed file.
Similarly, adding a directory entry to `pkgs/by-name/xx/` invalidates all traces
that enumerate that directory, even if the new package is never used by the
evaluated attribute.

**Comparison with Salsa.** Systems like Salsa (rust-analyzer) achieve
fine-grained invalidation by making every function call a tracked query with its
own dependency set. This requires language-level integration — the Salsa runtime
is tightly coupled to the Rust compiler's query system. Nix's evaluator lacks
this infrastructure; adding query-level tracking would be a fundamental language
change, not a trace system enhancement. The eval trace system operates at the
evaluator boundary (file reads, env vars, store operations) rather than at the
language level.

---

## 5. Storage Model

### 5.1 SQLite Database

All trace data lives in a single SQLite database at
`~/.cache/nix/eval-trace-v2.sqlite`. This includes attribute results, recorded
traces, and recovery indices. The database uses WAL mode, 64 KB page size
(optimized for large BLOB I/O), 256 MB memory-mapped I/O, and a 16 MB page
cache for performance.

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

### 5.2 Schema (8 Tables)

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

CREATE TABLE IF NOT EXISTS DepKeySets (
    id          INTEGER PRIMARY KEY,
    struct_hash BLOB NOT NULL UNIQUE,
    keys_blob   BLOB NOT NULL
) STRICT;

CREATE TABLE IF NOT EXISTS Traces (
    id              INTEGER PRIMARY KEY,
    trace_hash      BLOB NOT NULL UNIQUE,
    dep_key_set_id  INTEGER NOT NULL REFERENCES DepKeySets(id),
    values_blob     BLOB NOT NULL
) STRICT;

CREATE INDEX IF NOT EXISTS idx_traces_dep_key_set ON Traces(dep_key_set_id);

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

CREATE TABLE IF NOT EXISTS StatHashCache (
    path       TEXT NOT NULL,
    dep_type   INTEGER NOT NULL,
    dev        INTEGER NOT NULL,
    ino        INTEGER NOT NULL,
    mtime_sec  INTEGER NOT NULL,
    mtime_nsec INTEGER NOT NULL,
    size       INTEGER NOT NULL,
    hash       BLOB NOT NULL,
    PRIMARY KEY (path, dep_type)
) STRICT;
```

**Strings** and **AttrPaths** are interning tables that deduplicate string values
and attribute path BLOBs via unique constraints, reducing storage for repeated
references.

**Results** deduplicates attribute results by a content hash. Each row stores the
`(type, value, context)` triple encoding a `CachedResult`. Multiple attributes
with the same result share a single Results row.

**DepKeySets** stores content-addressed dep key sets. The `struct_hash` is the
BLAKE3 hash of the structural signature (dep types + sources + keys, without
hash values). Traces with the same dep structure (same types + sources + keys,
different hash values) share a single `DepKeySets` row. The `keys_blob` stores
zstd-compressed 9-byte packed entries `(type[1] + sourceId[4] + keyId[4])`.

**Traces** stores deduplicated dependency traces (BSàlC constructive traces),
keyed by `trace_hash` (BLAKE3 of the full sorted dep content including hash
values). Each trace references a shared `DepKeySets` row via `dep_key_set_id`
and stores its own `values_blob` (zstd-compressed hash values in positional
order matching `keys_blob`). Loading a trace's deps requires a single JOIN +
two zstd decompressions.

**CurrentTraces** maps `(context_hash, attr_path_id)` to the current trace and
result for each attribute. This is the primary lookup table for the verify path.

**TraceHistory** stores all historical `(context_hash, attr_path_id, trace_id,
result_id)` tuples — every trace that has ever been recorded for an attribute.
This is the constructive trace store that enables recovery: when the current
trace is invalid, recovery searches TraceHistory for a historical trace whose
deps match the current state.

**StatHashCache** stores persistent file stat metadata to hash mappings, keyed
by `(path, dep_type)`. This accelerates BSàlC VT dep verification by caching
BLAKE3 hashes of file content, NAR serializations, and directory listings. At
TraceStore construction, all entries are bulk-loaded into the in-memory
StatHashCache singleton. During evaluation, new/updated entries accumulate as
dirty entries. At TraceStore destruction, dirty entries are flushed back to the
database within the same transaction as trace data.

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

This avoids binary serialization for result values (no CBOR). Decoding is a
switch on the integer type with simple string parsing. (Note: `keys_blob` in
DepKeySets and `values_blob` in Traces *are* zstd-compressed for storage
efficiency, but result values in the Results table use plain SQL columns.)

### 5.5 Session Caches

`TraceStore` maintains several in-memory session caches to avoid redundant
SQLite reads and hash computations within a single evaluation session:

- **`verifiedTraceIds`** (`std::unordered_set<TraceId>`): Traces whose dep
  entries have been individually verified (BSàlC VT check passed). Shared across
  attributes with the same trace.
- **`traceDataCache`** (`std::unordered_map<TraceId, CachedTraceData>`): Unified
  per-trace cache. Each `CachedTraceData` holds `traceHash`, `structHash`
  (populated eagerly from a single `getTraceInfo` query), and an optional
  `deps` vector (populated lazily by `loadFullTrace`). The structural hash
  enables recovery short-circuit (skip struct_hash groups already tried by
  direct hash recovery). `hashesPopulated()` detects placeholder (all-zero)
  sentinel state.
- **`traceRowCache`** (`std::unordered_map<std::string, TraceRow>`): Caches
  `lookupTraceRow` results (attrPath → CurrentTraces row). Invalidated when
  CurrentTraces changes for a path (in recovery and record). Makes
  `getCurrentTraceHash()` fully cached after first call per attr path.
- **`depKeySetCache`** (`std::unordered_map<DepKeySetId, std::vector<InternedDepKey>>`):
  Resolved dep key sets. Avoids re-decompressing keys_blob when multiple traces
  share a key set.
- **`currentDepHashes`** (`std::unordered_map<DepKey, std::optional<DepHashValue>>`):
  Current dep hash cache. Persists across verification and recovery within a
  session. Value is `nullopt` if the dep's resource is unavailable (caches
  failure to avoid re-attempting expensive hash computations).
- **`internedStrings`** / **`internedAttrPaths`**: Session-local string/path
  interning caches.
- **`stringTable`**: Reverse lookup (id → string) for BLOB deserialization.

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
      - Load deps from DepKeySets + Traces (keys_blob + values_blob)
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
   b. Sort and dedup own deps (no parent dep inheritance)
   c. Compute trace_hash and struct_hash from own deps via HashSink
   d. UPSERT RETURNING INTO Traces (dedup by trace_hash, single statement)
   e. UPSERT RETURNING INTO Results (dedup by result hash, single statement)
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
Pre-load: single widened scan query                                [O(1)]
  scanHistoryForAttr JOINs TraceHistory→Traces→DepKeySets→Results
  Pre-loads trace_hash + result data for ALL history entries
  Builds in-memory trace_hash → entry map for O(1) candidate matching

Direct hash recovery                                               [O(1)]
  Recompute current dep hashes from old trace's dep keys
  Compute trace_hash from current dep hashes (own deps only)
  In-memory lookup by trace_hash → candidate entry (pre-loaded)
  Accept candidate (hash match proves validity) → update CurrentTraces and serve

Structural variant recovery (novel, beyond BSàlC)                 [O(V)]
  Group pre-loaded history entries by dep_key_set_id
  Skip struct_hash groups already tried by direct hash recovery
    (short-circuit: same dep structure would produce the same trace_hash)
  For each remaining group: load dep KEY SET (no values_blob decompression),
    recompute current hashes, compute candidate trace_hash
    In-memory lookup by candidate trace_hash → candidate entry
  Handles dep structure instability (dynamic deps — different deps across
  fresh evaluations depending on evaluation order, per Shake)
```

Note on direct hash recovery: finding a trace by the computed `trace_hash` IS the
verification. We computed `trace_hash = BLAKE3(sorted current dep hashes)` — a
matching trace in the database was recorded with identical dep values. No dep-by-dep
`verifyTrace` call is needed (probability of false match: 2^-256). This is why
`acceptRecoveredTrace` updates CurrentTraces directly without calling `verifyTrace`.

Recovery is particularly effective for file reverts: reverting a file to a previous
state produces the same dep hashes, and direct hash recovery finds the matching
candidate in O(1). This is a direct consequence of the constructive trace property — the
historical result is stored and can be served immediately.

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

- `--file <path>`: BLAKE3 of the canonical absolute path (plus auto-args, lookup paths, store dir, system)
- `--expr <text>`: BLAKE3 of the expression text (plus auto-args, lookup paths, store dir, system)

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
   depending on whether a root file is already cached). This means direct hash
   recovery cannot be the sole mechanism — structural variant recovery is needed
   as a fallback.

6. **Symlinks not tracked.** Intermediate symlink targets are not recorded as deps.
   Changes to a symlink target without changing the resolved file will not
   invalidate the trace.

7. **(Resolved)** ~~Merkle identity hash is O(depth).~~ Parent Merkle chaining
   has been removed. `trace_hash` is the BLAKE3 of own sorted deps (including
   any ParentContext dep). No recursive parent-chain walk is needed.

8. **Parent-mediated value changes (soundness gap).** Per-sibling ParentContext
   deps track only which siblings a child accessed during evaluation. If a parent
   overlay changes a child's definition without changing any file that the child
   reads **or** any sibling the child accesses, the child's trace incorrectly
   validates. Example: a parent overlay that patches `hello.meta.description` via
   Nix code (not a file change) would not invalidate `hello`'s trace if `hello`
   never accessed the attribute that carries the overlay's dep. This is:
   - **Pre-existing**: origExpr children already lack ParentContext deps entirely.
   - **Extremely rare**: overlays that modify existing packages without changing
     any files are almost nonexistent in real Nixpkgs usage.
   - **Acceptable trade-off**: avoids cascading invalidation of ~60K traces on
     every by-name package addition.
   - **Tracked**: A DISABLED test (`ParentMediatedValueChange_SoundnessGap`) in
     `per-sibling-invalidation.cc` demonstrates this gap.
   - **Future fix**: could be addressed by recording the parent's evaluation
     result hash alongside per-sibling deps, or by adding explicit dep tracking
     for overlay application sites.

9. **Whole-file Content dep over-approximation.** For `.nix` code consumed via
   `import`/`evalFile`, the Content dep covers the entire file. Any byte change
   — even a comment edit in an unused branch — invalidates all traces that
   imported the file. This is the dominant source of unnecessary re-evaluations
   in the 100-commit nixpkgs benchmark (Section 4.6, Section 11.3). The
   StructuredContent two-level override (Section 4.4) mitigates this for data
   files (JSON/TOML) but not for `.nix` code.

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

4. **(Resolved)** ~~Parent trace_hash caching.~~ Parent Merkle chaining has been
   removed. `getCurrentTraceHash()` looks up the parent's `trace_hash` from the
   Traces table, and no recursive chain walk is needed.

5. **Integration with content-addressed derivations.** Content-addressed
   derivations could provide additional optimization opportunities — the eval
   trace could skip re-evaluation when a derivation's output is already available
   regardless of input changes.

6. **Symlink tracking.** Record intermediate symlink targets as Existence deps to
   detect symlink retargeting.

7. **Fine-grained `.nix` file dependency tracking.** The dominant source of
   unnecessary re-evaluations is the whole-file Content dep for `.nix` code
   (Section 4.6, Section 11.3 Pattern A and Pattern C). A future system could
   track dependencies at a finer granularity — e.g., per-binding or per-function
   — to avoid invalidating traces when changes are semantically irrelevant.
   This would require language-level integration similar to Salsa's query system.

8. **Key-set Directory shape deps for `mapAttrs`/`attrNames`.** When `readDir`
   results are consumed only by key-enumerating operations (`mapAttrs`,
   `attrNames`), the current system falls back to the coarse Directory dep
   because no individual entry thunks are forced (Section 4.6, Section 11.3
   Pattern B). A shape dep recording the sorted key set (without entry types)
   could enable traces to survive additions/removals of irrelevant entries.

9. **(Implemented)** ~~Dep key factoring and direct-lookup recovery.~~ Dep keys
   are now stored in a shared `DepKeySets` table (keyed by `struct_hash`),
   separate from per-trace hash values in `Traces.values_blob`. Structural
   variant recovery loads only key sets (no values_blob decompression). All
   dep hashes are computed upfront using warm StatHashCache, enabling O(1)
   direct-lookup recovery as the primary strategy.

---

## Section 11: Benchmark Findings (100-Commit Nixpkgs Analysis)

This section summarizes findings from benchmarking the eval trace system across
100 consecutive nixpkgs commits, evaluating `nixpkgs#hello.pname` for each
commit in both cold (fresh DB) and hot (warmed DB) configurations.

### 11.1 Database Size

After 200 evaluations (100 cold + 100 hot), the eval-trace database grew to
**455 MB**. Dep storage (now split between `DepKeySets.keys_blob` and
`Traces.values_blob`) accounts for **98.8%** of total storage. With dep key
factoring, traces sharing the same dep structure now share a single `DepKeySets`
row. However, each trace still requires its own `values_blob` because at least
one hash value differs between commits. Typical traces contain ~20,000–40,000
deps.

### 11.2 Hot Evaluation Overhead

Hot evaluations (warmed StatHashCache + populated DB) show **25.3% overhead**
compared to baseline Nix evaluation:

| Component | Time | Notes |
|-----------|------|-------|
| Verify (dep checking) | 127s total | 20.5M dep hash comparisons |
| Recovery (direct + structural) | 116s total | 1,228 attempts, 31.6% success |
| Trace loading | 34s total | 679 blob decompresses |
| **Total trace overhead** | **277s** | **25.3% of 1,093s total** |

The recovery time breaks down as: direct hash recovery (142 hits, fast) and
structural variant recovery (246 hits, 840 failures — each failure requires
decompressing 176–451 KB blobs to load dep entries for hash recomputation).

### 11.3 Unnecessary Re-evaluations

**29 out of 100** hot evaluations miss the cache (no verify hit, no recovery
hit) but produce a result identical to an already-recorded trace. These
represent **~980 seconds** of wasted evaluation time (73% of total hot eval
time). Three patterns account for all 29 cases:

**Pattern A: `aliases.nix` changes (26 cases).** `pkgs/top-level/aliases.nix` is
imported by `all-packages.nix`, which is imported by every evaluation path.
Changes to unrelated aliases (e.g., renaming `sddm-kcm` to `kde-sddm`) change
the file's Content hash, invalidating all traces that imported it. The affected
attribute (`hello.pname`) never uses any alias — the entire invalidation is a
consequence of whole-file Content dep over-approximation (Section 4.6).

**Pattern B: `pkgs/by-name/` directory additions (13 cases).** Adding a new
package to `pkgs/by-name/xx/` changes the directory listing hash, invalidating
all traces that enumerate the parent directory. The `hello` package is unaffected
by the addition. The current system records a coarse Directory dep for the
listing; the two-level override only applies when individual entries are forced
via `ExprTracedData` thunks.

**Pattern C: `python-packages.nix` additions (2 cases).** Similar to Pattern A
— adding a new Python package to the ~40,000-line file changes the Content hash,
invalidating traces that import it even though `hello` has no Python dependency.

### 11.4 Recovery Effectiveness

| Strategy | Hits | Failures |
|----------|------|----------|
| Direct hash recovery | 142 | N/A |
| Structural variant recovery | 246 | 840 |
| **Total** | **388 (31.6%)** | **840 (68.4%)** |

Direct hash recovery succeeds when the dep *values* (file hashes) match a
previously-seen trace — i.e., the exact combination of file versions has been
evaluated before. This is O(1) and fast. Structural variant recovery succeeds
when a different dep *structure* (different dep keys) produces a matching trace
— handling dynamic dep instability (Shake-style). Neither strategy helps when
the dep state is genuinely novel (a never-before-seen combination of file
hashes), which is the case for the 840 failures.

### 11.5 Implications

The benchmark findings point to three concrete optimization opportunities:

1. **Dep key factoring** (addresses DB size + recovery overhead): Separating dep
   keys from hash values would enable key-set dedup across structurally-identical
   traces and eliminate blob decompression during structural variant recovery.
   Estimated 30% storage reduction and significant recovery speedup.

2. **Direct-lookup as primary recovery** (addresses verify+recovery overhead):
   With StatHashCache warm (8,535 entries, ~0.5us per dep for stat-only lookup),
   computing all current dep hashes upfront is cheap (~2ms for 4,283 deps).
   Direct hash recovery then becomes O(1). The current two-phase
   verify→recovery pattern is an artifact of an era when hash computation was
   expensive.

3. **Fine-grained `.nix` tracking** (addresses Pattern A+C): This is a
   longer-term goal requiring language-level changes (Section 10, item 7).

---

## Appendix A: File Layout

### `nix::` namespace — evaluator-facing types

| File | Description |
|------|-------------|
| `src/libexpr/include/nix/expr/eval-trace-deps.hh` | Dep vocabulary types: `DepType`, `DepKind`, `Blake3Hash`, `DepHashValue`, `Dep`, `DepKey`, `DepRange`, `StructuredFormat`, `ShapeSuffix` (header-only, includes inline helpers: `depTypeName()`, `depKind()`, `depKindName()`, `isVolatile()`, `isContentOverrideable()`, `buildStructuredDepKey()`, `formatStructuredDepKey()`, etc.) |
| `src/libexpr/include/nix/expr/dependency-tracker.hh` | `DependencyTracker`, `SuspendDepTracking`, dep hash function declarations, `StatHashEntry` bridge API, `ReadFileProvenance` |
| `src/libexpr/dependency-tracker.cc` | DependencyTracker implementation, dep hash functions, `resolveToInput`, `recordDep`, internal StatHashCache (L1 concurrent_flat_map + L2 bulk-loaded from TraceStore), provenance threading |
| `src/libexpr/include/nix/expr/eval-trace-context.hh` | `EvalTraceContext` struct (EvalState integration) |
| `src/libexpr/eval-trace-context.cc` | `EvalTraceContext` methods |
| `src/libexpr/include/nix/expr/traced-data.hh` | `TracedDataNode` virtual interface, `ExprTracedData` Expr subclass for lazy structural dep tracking |

### `nix::eval_trace` namespace — trace system internals

| File | Description |
|------|-------------|
| `src/libexpr/include/nix/expr/trace-result.hh` | Result vocabulary types: `ResultKind`, `CachedResult`, all result structs (header-only) |
| `src/libexpr/include/nix/expr/trace-cache.hh` | TraceCache public API, performance counters |
| `src/libexpr/trace-cache.cc` | TracedExpr (verify/record/recover logic), ExprOrigChild, SharedParentResult |
| `src/libexpr/include/nix/expr/trace-store.hh` | TraceStore class declaration |
| `src/libexpr/trace-store.cc` | TraceStore implementation: schema (including StatHashCache table), verify, record, recover, verifyTrace, stat-hash bulk-load/flush |
| `src/libexpr/include/nix/expr/trace-hash.hh` | Trace hash computation functions (sortAndDedupDeps, computeTraceHash, etc.) |
| `src/libexpr/trace-hash.cc` | Trace hash computation implementations |

### Header dependency graph

```
eval-trace-deps.hh          trace-result.hh
    ↑                            ↑         ↑
    |                            |         |
dependency-                   trace-      trace-
tracker.hh                   store.hh    cache.hh
    ↑                            ↑
    |                            |
eval-trace-                  trace-hash.hh
context.hh

traced-data.hh  (depends on eval-gc.hh, nixexpr.hh, eval-trace-deps.hh)
```

### Tests

| File | Description |
|------|-------------|
| `tests/functional/flakes/eval-trace-*.sh` | Functional tests (flake-based) |
| `tests/functional/eval-trace-impure-*.sh` | Functional tests (impure / --file / --expr) |
| `src/libexpr-tests/eval-trace/traced-data.cc` | GTest unit tests for lazy structural dep tracking |
| `src/libexpr-tests/eval-trace/trace-store.cc` | GTest unit tests for TraceStore (schema, serialization, verify/record/recover) |

## Appendix B: References

1. Mokhov, Mitchell, Peyton Jones. "Build Systems à la Carte: Theory and Practice."
   *Journal of Functional Programming* 30, e11, 2020.

2. Hammer, Khoo, Hicks, Foster. "Adapton: Composable Demand-Driven Incremental
   Computation." *PLDI 2014.*

3. Matsakis et al. "Salsa: A Framework for On-Demand, Incremental Computation."
   https://github.com/salsa-rs/salsa

4. Mitchell. "Shake Before Building: Replacing Make with Haskell." *ICFP 2012.*
