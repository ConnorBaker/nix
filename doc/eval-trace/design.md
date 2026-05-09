# Nix Eval Trace: Design Document

**Target audience:** Nix core team (assumes familiarity with the evaluator and store).

---

## 1. Motivation

Nix evaluation is expensive. `nix eval nixpkgs#hello.pname` takes several seconds
on a cold evaluator because it must parse and evaluate thousands of Nix files even
to reach a single leaf attribute. In typical development workflows -- edit-build-test
cycles, CI pipelines, `nix search` -- the same flake is evaluated repeatedly with
few or no input changes between runs.

The existing eval cache (`AttrCursor` in `eval-cache.cc` on `master`) stores
evaluation results in SQLite keyed by a fingerprint derived from the flake lock
file. It has five fundamental limitations:

1. **All-or-nothing invalidation.** The cache key is a hash of the locked flake
   inputs. Any change to any input -- even touching a single comment in a single
   file -- invalidates the *entire* set of cached results.

2. **No dependency tracking.** The system records *results* but not *which files
   produced them*. It cannot distinguish between an attribute that depends on the
   changed file and one that does not.

3. **No cross-version recovery.** Because the cache key includes the locked ref
   (which changes every commit), reverting a file to a previous state does not
   recover previously cached results.

4. **Limited type support.** Only string, bool, int, list-of-strings, and attrset
   are cached. No support for null, float, path, or heterogeneous lists.

5. **No `--file`/`--expr` caching.** Only flake evaluations are cached.

This design addresses all five limitations with a dependency-tracking eval trace
system that provides per-attribute invalidation, cross-commit recovery, and
transparent integration with the existing CLI.

---

## 2. Prior Art

This system draws on substantial academic and industry literature on incremental
computation and build systems. We adopt standard terminology from the following
works to make the design immediately recognizable and to enable precise discussion
of trade-offs.

### 2.1 Build Systems a la Carte (BSalC)

> Mokhov, Mitchell, Peyton Jones. "Build Systems a la Carte: Theory and Practice."
> *Journal of Functional Programming*, 2020.

BSalC provides a taxonomy of build systems based on two axes: **scheduler** (how
tasks are ordered) and **rebuilder** (how staleness is detected). The rebuilder
taxonomy defines:

- **Verifying trace (VT):** Records `(key, [dep_hash], result_hash)`. On lookup,
  recomputes dep hashes and compares. If all match, the result is *valid* --
  but must be recomputed if not available. Our **verifyTrace()** path implements this.

- **Constructive trace (CT):** Records `(key, [dep_hash], result)`. Like VT but
  stores the *actual result*, not just its hash. On a match, the result can be
  served directly without recomputation. Our **record()** path stores full results,
  and **recovery()** uses historical traces to serve results constructively.

- **Deep constructive trace (DCT):** A CT that records traces at every intermediate
  node, not just leaves. Our system traces at all nesting levels -- root attrsets,
  intermediate attrsets, and leaf values.

**Our system is a deep constructive trace store** -- it records full results at
every attribute level and can recover them by matching dependency signatures.

### 2.2 Adapton

> Hammer, Khoo, Hicks, Foster. "Adapton: Composable Demand-Driven Incremental
> Computation." *PLDI 2014.*

Adapton introduced demand-driven incremental computation with an explicit
**dependency graph (DDG)** that records which computations depend on which inputs.
Key concepts we adopt:

- **TracedExpr:** A computation whose evaluation is recorded in the DDG. Analogous
  to Adapton's *articulation points*.

- **DepRecordingContext:** Records the dynamic dependency graph during evaluation.
  Analogous to Adapton's DDG construction during `force`.

- **Fresh evaluation (evaluateResolvedTarget):** Demand-driven recomputation when
  a traced result is invalid. Analogous to Adapton's `force` on a dirty node.

- **Trace replay (replayTrace):** Propagating recorded dependencies from a
  previously traced computation to the current tracking context. Analogous to
  Adapton's change propagation.

### 2.3 Salsa

> Matsakis et al. "Salsa: A Framework for On-Demand, Incremental Computation."
> Used in the Rust compiler (rust-analyzer).

Salsa provides versioned queries with memoized results. Our **ParentSlot deps**
are analogous to Salsa's versioned query with context: a child trace stores a
`ParentSlot` dep containing the parent's `trace_hash`, linking the child to a
specific parent version.

### 2.4 Shake

> Mitchell. "Shake Before Building: Replacing Make with Haskell." *ICFP 2012.*

Shake uses verifying traces with dynamic dependencies -- dependencies discovered
during build, not declared statically. Our system shares this property: the
DepRecordingContext records deps as they are encountered during evaluation, and the
set of deps can vary between evaluations of the same attribute.

### 2.5 Ghosts of Departed Proofs (GDP)

> Noonan. "Ghosts of Departed Proofs: Functional Pearls." *Haskell Symposium 2018.*

GDP uses phantom types and scoped proof tokens to encode invariants in the type
system. We use a C++ approximation (`gdp::Proof<Tag>`, `gdp::Certifier<Tag>`) for
scoped local preconditions, and opaque capability types for long-lived
verification facts whose effects escape the proving scope:

- `Proof<BlockingTag>` required for all blocking-thread I/O (SQLite, filesystem, git)
- `Proof<DepCaptureScopeTag>` required to manipulate the dep recording scope stack
- `VerifiedFileDep` required to call `markFileVerified()`
- `ExclusiveTraceStoreAccess` required by primary public mutable `TraceStore`
  methods; residual private/friend paths still use `Proof<BlockingTag>` directly

Proofs are zero-size tokens that exist only as `const &` inside scoped
continuations. They cannot be constructed, copied, or stored outside the
continuation. High-blast-radius verification facts are not exposed as public
proof tags because C++ cannot seal `Certifier<Tag>` inheritance; they use
opaque capabilities with private constructors instead.

### 2.6 Terminology Glossary

| This System | Prior Art Term | Definition |
|-------------|---------------|------------|
| **trace** | constructive trace (BSalC) | A record of `(key, [dep_hash], result)` from an evaluation |
| **verifyTrace()** | verifying trace check (BSalC VT) | Check if recorded dep hashes still match current state |
| **record()** | trace recording (BSalC CT) | Store evaluation result + dep hashes for future verification |
| **recovery()** | constructive trace recovery (BSalC CT) | Find historical trace with matching deps to reuse its result |
| **TraceStore** | trace store (BSalC) | Persistent SQLite database of recorded traces |
| **TraceSession** | trace-based incremental cache | Session managing trace verification and recording |
| **TracedExpr** | articulation point (Adapton) | Expression whose evaluation is traced/memoized |
| **DepRecordingContext** | DDG builder (Adapton) | Records dynamic dependency graph during evaluation |
| **evaluateResolvedTarget()** | demand-driven recomputation (Adapton) | Evaluate an expression from scratch, recording deps |
| **replayTrace()** | change propagation (Adapton) | Replay recorded deps into current tracking context |
| **ParentSlot dep** | versioned query (Salsa) | Parent trace_hash stored as a dep in the child trace |

### 2.7 System Classification

**Deep constructive trace store with structural variant recovery.**

- **Constructive** (BSalC CT): stores result values, not just hashes -- can serve
  results without recomputation.
- **Deep** (BSalC DCT): traces at all nesting levels -- root, intermediate attrsets,
  and leaves.
- **Verifying fast path** (BSalC VT): O(N) dep hash comparison for verification.
- **Dynamic dependencies** (Shake): deps discovered during evaluation, not declared
  statically.
- **Demand-driven** (Adapton): traced expressions are evaluated lazily on access.
- **Type-enforced invariants**: scoped preconditions encoded via GDP proofs and
  long-lived verification facts sealed behind opaque capabilities.
- **Structural variant recovery**: novel extension beyond BSalC -- recovers results
  by trying alternate historical dep key sets for the same attr when the current
  trace's key set misses. This handles dynamic-dependency instability where an
  older or newer trace observed a different dependency structure that matches
  the current state.

---

## 3. Architecture Overview

The eval trace system is a 4-layer stack:

```
CLI command (nix eval / nix build / nix search)
  |
  v
TraceSession                              [cache/trace-session.hh]
  Per-context session. Creates TracedExpr thunks for the root value.
  Owns SemanticRegistry, stamp vectors.
  |
  v
TraceBackend (polymorphic)                [cache/trace-backend.hh]
  +-- StoreTraceBackend: real SQLite + async verification pipeline
  +-- NullTraceBackend: no-op for --no-eval-trace
  |
  v
AsyncRuntime                              [store/async-runtime.hh]
  io_context + dedicated worker threads + BlockingThreadPool
  +-- VerificationOrchestrator (GDP-guarded state via VerificationAccessTag;
  |     dispatches SQLite access via coroBlock -> BlockingThreadPool)
  +-- FiberScheduler (eval thread <-> async bridge via syncAwait)
  |
  v
TraceStore                                [store/trace-store.hh]
  SQLite backend. Single eval-trace DB with recovery_key-partitioned History
  and session_key-partitioned Sessions, namespaced by schema epoch and hash backend.
  GDP: residual Certifier<BlockingTag> for lifecycle/private friend paths;
  primary public data-path store methods additionally require ExclusiveTraceStoreAccess.
  +-- verifyTrace():  validate all deps in a trace (BSalC VT)
  +-- record():       INSERT/UPSERT trace + result (BSalC CT)
  +-- recovery():     direct hash + structural variant (BSalC CT recovery)
```

Structured multi-field eval-trace hashes use `CanonicalHashBuilder`, which
length-frames every field and starts each preimage with a domain string. This is
used for trace hashes, structural hashes, result hashes, session/recovery keys,
directory-listing hashes, NixBinding hashes, runtime-root source keys, and
policy digests. One legacy structured exception is the aggregated DirSet hash,
which is still a raw active-backend digest over NUL-delimited fields in
`shape-deps.cc`. Single-payload dep values such as file bytes, NAR dumps,
environment values, JSON/TOML scalar canonical strings, and small sentinel
values are still hashed as raw payload bytes through the active backend; the dep
key kind/source are framed separately when the trace hash is computed.

The active eval-trace hash backend is selected by the
`eval-trace-hash-algorithm` setting (`blake3` by default, `sha256` also
supported). Both backends currently produce 32-byte digests, stored as
`EvalTraceHash` and wrapped in phantom types such as `DepHash`, `TraceHash`,
`StructHash`, and `ResultHash` so hash domains cannot be mixed accidentally in
C++.

The cache treats these digests as content addresses. There is no secondary
full-preimage equality check after a digest lookup, so collision resistance is a
semantic requirement of any backend enabled here. A faster non-cryptographic or
shorter hash would need an additional collision-resolution scheme in the storage
model before it could be sound.

### 3.1 Recording Path (DepRecordingContext)

Dependencies are recorded during evaluation by the `DepRecordingContext`, a
per-fiber context that manages a scope stack and epoch log. Recording is GDP-
guarded: scope manipulation (`pushScope`/`popScope`/`takeDeps`) requires
`Proof<DepCaptureScopeTag>`. Provenance resolution uses the `SemanticRegistry`
(pre-populated at session open from the resolved flake graph) directly.

Recording sites are instrumented throughout the evaluator:
- `eval.cc`: `evalFile` -> FileBytes dep
- `primops.cc`: `readFile`, `readDir`, `pathExists`, `getEnv`, `currentTime`,
  `currentSystem`, `hashFile`, `readFileType` -> dep kinds matching
  `CanonicalQueryKind` (see Section 4.1)
- `primops.cc`: `fromJSON`, `fromTOML` -> StructuredProjection deps, conditional
  on the argument string carrying TextObject provenance from a preceding `readFile`
- `primops/fetchTree.cc`: `fetchTree` -> RuntimeFetchIdentity dep for unlocked
  inputs; StorePathAvailability + runtime root persistence for locked inputs
- `flake.cc`: `buildResolvedFlakeGraphValue` -> PathObject provenance with
  `DepSource::fromNodeKey` identity on `flakePath` and `outPath` values

### 3.2 Verification Pipeline (Async)

Verification runs on an async pipeline using Boost.Asio coroutines. The eval
thread bridges to async via `syncAwait`, which posts work to the io_context
executor and blocks on a `std::future`. Dedicated worker threads drive the
io_context; the eval thread never calls `run_one()` or `poll()`.

The key components:

1. **VerificationOrchestrator**: dispatches verify-or-recover decisions. Owns
   the `VerificationSession` and `PrefetchPool`. State access is GDP-guarded
   via `VerificationAccessTag`. Serialization is provided by `syncAwait` blocking:
   the eval thread blocks while the worker runs, preventing concurrent access.

2. **Blocking dispatch**: SQLite access goes through `coroBlock` ->
   `BlockingThreadPool` from inside the orchestrator's coroutines. This is
   where `Proof<BlockingTag>` is created for TraceStore method calls.
   (There is no separate "TraceStoreService" class — the orchestrator calls
   TraceStore methods directly under a blocking-pool proof.)

3. **FiberScheduler**: bridges the eval thread to async. When `TracedExpr::eval()`
   needs to verify a trace, it creates an `EvalContext<Suspendable>` and calls
   `ctx.syncAwait(orchestrator->verifyAttr(...))`. `syncAwait` posts the
   coroutine via `co_spawn` and blocks on `future.get()`. Worker threads drive
   the io_context and resume the coroutine.

4. **RecoveryState typestate**: recovery follows an explicit
   `RecoveryUntried -> RecoveryGitMissed -> RecoveryDirectMissed` typestate
   chain (see `RecoveryState` in `store/trace-store-verify.cc`). Protocol
   violations are compile errors via `Linear<T>` single-consumption tagging.
   (Earlier drafts used session-typed `VerifyPipeline`/`VerifyOrRecover`/
   `TraceStoreOp` protocols; those were removed in Bundle 4 — the
   orchestrator now calls TraceStore methods directly under
   `Proof<BlockingTag>`.)

The verification pipeline (in `trace-store-verify.cc`, extracted as `VerifyImpl`
static methods) forms a single pipeline with ordering constraints:

- **Pre-pass (`runStorePathBatch`, `runPrePassDeps`):**
  - Batch StorePathAvailability deps into single `queryValidPaths` call.
  - Resolve GitIdentity + TraceContext (ValueContext, ParentSlot) deps.
  - Runtime roots are already verified and merged into the SemanticRegistry
    at session open via `loadAndVerifyRuntimeRoots`. No per-trace re-fetch.

- **Pass 1 (`runPass1`):** Verify all deps via `DepResolution` typestate.
  ContentOverrideable failures are deferred. StructuredProjection and
  ImplicitStructure deps are deferred. Content/Directory dep matches trigger
  `markFileVerified()` (via `VerifiedFileDep`), which subsumes finer-grained
  deps from that file during `resolveDepHash`.

- **Pass 2 (`runPass2`):** Multi-level override check. When all content deps
  passed, deferred structural/implicit deps are still verified for uncovered
  files. When only ContentOverrideable failures remain, verify
  StructuredProjection deps covering the failed files; ImplicitStructure deps
  (including GitRevisionIdentity) can also provide coverage. Four `VerifyOutcome`
  values: `Valid`, `ValidViaStructuralOverride`, `ValidViaImplicitShapeOverride`,
  `Invalid`.

- **Recovery:** GitIdentity indexed lookup with candidate validation, direct
  hash O(N) recomputation plus indexed lookup, and structural variant scans over
  historical dep-key-set representatives.

### 3.3 Type-Safety Patterns

Five orthogonal patterns compose to cover different bug classes. (A sixth
pattern — session-typed in-process protocols — was used in earlier drafts and
removed in Bundle 4; the `session-types/` library is retained in libutil but
is not consumed by eval-trace. Phase ordering now lives in `Linear<T>`
typestates like `DepResolution` and `RecoveryState`.)

| Pattern | Library | Question | Bug Class Prevented |
|---------|---------|----------|---------------------|
| **Phantom types** | `tagged.hh` | Are values confused? | Passing DepSourceId where DepKeyId expected |
| **Typestate** | `linear.hh` | What order? | Skipping L1 cache check, dropping resolution state |
| **GDP proofs / opaque capabilities** | `gdp/proof.hh`, access control | Precondition proved, or long-lived fact sealed? | markFileVerified without hash comparison |
| **Singleton dispatch** | `singleton/dispatch.hh` | All variants handled? | Missing handler for new CanonicalQueryKind |
| **Linear** | `linear.hh` | Must consume? | Dropping DepResolution without completing |

**Applied vs open.** These patterns are applied across eval-trace's own
code (dep recording, verification, recovery).  One class of bug they
*could* prevent — but currently do not, by convention rather than by
type — is **accessor-identity propagation in libexpr path construction**.
`SourceAccessor::operator==` compares by a monotonic `number`, and any
runtime path construction that rebuilds from a canonical string (via
`EvalState::rootPath`, `EvalState::storePath`, or their equivalents)
silently replaces the accessor.  Two committed fixes (`ExprConcatStrings::
eval`, `EvalState::storePath`) close the two manifestations that
produced user-visible "Filesystem roots are not the same" errors from
`lib.fileset.toSource`; one latent hazard remains at
`eval-trace/cache/materialize.cc`'s `path_t` restoration (OR-11).  A
proposed opaque-capability mechanism (`AccessorForwardingScope`,
following the `OriginScope<CurrentTrace>` pattern already used here)
would move this from convention to type-enforced invariant.  See
`doc/eval-trace/audit/rearchitecture-proposal.md` §14 for the full
hazard-class analysis and the four prevention options (ranging from
per-site fixes to revisiting `SourceAccessor::operator==`'s
handle-vs-filesystem-identity policy).

---

## 4. Dependency Tracking

### 4.1 Dependency Types (CanonicalQueryKind)

The eval trace tracks dependencies via the `CanonicalQueryKind` enum (in
`types.hh`), each mapped to a `QueryBehavior` and `QueryDomainMask` via the
exhaustive `describe(kind)` function (`-Wswitch-enum` enforced). Adding a CQK
variant without a descriptor is a compile error.

| QueryBehavior | CQK | Source Builtins | Validation |
|---------------|-----|-----------------|------------|
| **ContentOverrideable** | FileBytes | `evalFile`, `readFile`, `hashFile` | active eval-trace digest of file bytes |
| | DirectoryEntries | `readDir` | active eval-trace digest of sorted dir listing |
| **Normal** | ExistenceCheck | `pathExists` | exists/missing check |
| | EnvironmentLookup | `getEnv` | Re-read env variable |
| | SessionSystemValue | `currentSystem` | Compare string value |
| | DerivedStorePath | `builtins.path`, `derivationStrict` | Store path comparison |
| | RuntimeFetchIdentity | `fetchTree` (unlocked) | Session-open narHash check |
| | NarIdentity | `builtins.path` (filtered) | active eval-trace digest of NAR |
| | RawBytes | string builtins on readFile result | active eval-trace digest of file bytes |
| | StorePathAvailability | `derivationStrict` | valid/missing |
| **Volatile** | VolatileTime | `currentTime` | Always invalidates |
| | VolatileExec | `builtins.exec` | Always invalidates |
| **Structural** | StructuredProjection | `fromJSON`/`fromTOML`/`readDir` leaf, `.nix` binding access | active eval-trace digest of canonical scalar or AST hash |
| **ImplicitStructural** | ImplicitStructure | Container creation | Skipped during verification |
| | GitRevisionIdentity | Git repo HEAD rev | Fast-path skip |
| **TraceContext** | TraceValueContext | (sibling access) | Upstream trace hash |
| | TraceParentSlot | (parent reference) | Parent trace hash |

### 4.2 QueryBehavior Subsumption Model

| QueryBehavior | Meaning | Subsumption |
|---------------|---------|-------------|
| **Normal** | Standard compute-and-compare | None |
| **ContentOverrideable** | Coarse content dep | Per-file: `markFileVerified()` subsumes finer-grained deps |
| **Structural** | Shape dep (keys/type) | Subsumed when covering file is verified |
| **ImplicitStructural** | Implicit shape dep | Subsumed when covering file is verified |
| **TraceContext** | Trace-internal context dep | Resolved in pre-pass |
| **Volatile** | Always fails verification | None (always invalidates) |

Per-file subsumption is the key mechanism: when a Content or Directory dep
hash matches during verification, `resolveDepHash` manufactures a
`VerifiedFileDep` and calls `markFileVerified()`, which marks that file as
verified. Subsequent StructuredProjection/ImplicitStructure deps from the
same file are subsumed (returned from L1 cache as `VerifiedHash` without
recomputation).

Runtime roots (locked fetchTree inputs) are verified at session open via
`loadAndVerifyRuntimeRoots` and merged into the `SemanticRegistry`. Their
store paths are available for dep resolution without per-trace re-fetching.

### 4.3 Two-Level Verification (StructuredProjection Override)

When `fromJSON(readFile f)` or `fromTOML(readFile f)` is used, the eval trace
records both a whole-file Content dep and fine-grained StructuredProjection deps
for each accessed scalar leaf. During verification, if the Content dep fails
(file changed) but all StructuredProjection deps pass (accessed values unchanged),
the trace remains valid.

This enables traces to survive changes to unused parts of structured data files
(e.g., changing an unused field in a JSON config).

### 4.4 NixBinding StructuredProjection Deps

For `.nix` files consumed via `import`/`evalFile`, a per-binding AST hash system
(NixBinding StructuredProjection deps) provides two-level verification. Each
binding in a non-recursive attrset gets an active-backend eval-trace digest of
its AST representation (via `showForHash`, which normalizes `ExprPath` output
by stripping store path prefixes for cross-store-path stability).

When a `.nix` file changes but the accessed binding's AST is unchanged (e.g.,
only a `description` field was modified), the Content dep fails but the
NixBinding SC dep passes, keeping the trace valid.

### 4.5 Dep Provenance via SemanticRegistry

Dep provenance (which flake input a file belongs to) is resolved via the
`SemanticRegistry`, a mount/provenance registry pre-populated at session open
from the resolved flake graph. It is immutable from the recording and
verification consumers' perspective (`DepCaptureScope` takes `const
SemanticRegistry &`). Runtime root mount points for impure `fetchTree` calls
are added dynamically by `TraceRuntime` during cold evaluation.

The registry provides two indexes sharing the same identity namespace:
- **Forward entries** (`resolve`): `DepSource -> SourcePath`, used during
  verification to map dep sources to current store paths for hash computation.
- **Mount points** (`reverseResolve`): `CanonPath -> [(DepSource, subdir)]`,
  used during recording to map store paths back to logical dep source identities.

Two disjoint identity namespaces exist by construction:
- **Graph node keys** (`DepSource::fromNodeKey`): lock-file node names from
  `ResolvedFlakeGraph`, populated at session creation.
- **Runtime root URLs** (`DepSource::fromRuntimeRoot`): locked fetcher URLs
  from `builtins.fetchTree`, added at session open from `SessionRuntimeRoots`.

### 4.6 Separated Parent and Child Deps

Each trace stores only its **own** deps -- deps recorded during that specific
thunk's evaluation. Parent deps are never merged into child traces. This
separation prevents cascading invalidation: when a parent's dep changes, only
the parent trace is invalidated. Child traces retain their own deps and pass
verification independently.

Parent-child relationships are expressed through `TraceParentSlot` deps,
which store the parent's `trace_hash` as a dep entry. `TraceValueContext` deps
track sibling accesses for finer-grained invalidation.

---

## 5. Storage Model

### 5.1 Database Layout

Trace data is stored across two SQLite databases. The main database filename
encodes the schema version and hash backend (for example,
`eval-trace-v24-blake3.sqlite`) so breaking changes and backend changes force a
fresh DB:

- **Main trace DB** -- traces, results, dep key sets, strings, data paths,
  sessions, history. `session_key` partitions Sessions/current traces;
  `recovery_key` partitions History for cross-session recovery. Neither key is
  part of the filename.
- **`attr-vocab.sqlite`** -- Shared attribute name/path trie (AttrNames,
  AttrPaths). Shared across all session keys. Monotonically growing, never pruned.

Both databases use WAL mode conditionally (respecting `useSQLiteWAL` for
environments like NFS/WSL where WAL is problematic). The main database ATTACHes
vocab for atomic cross-DB commits on clean shutdown. `AttrVocabStore::checkpoint()`
provides crash-safety by flushing vocab via a brief independent connection
before trace entities are written.

### 5.2 Interning and Deduplication

All string data (file paths, dep keys, env var names) is interned in a
`StringInternTable` with O(1) amortized lookup. Attribute paths use a structured
trie (`AttrVocabStore`) with parent/child relationships. Both are persisted to
SQLite and bulk-loaded at session start.

Dep keys are content-addressed in the `DepKeySets` table (keyed by
`struct_hash = canonical framed active-backend digest(dep types + sources +
keys)`, excluding ImplicitStructural deps). Traces with the same dep structure
share a single `DepKeySets` row. Each `Traces` row stores its own `values_blob`
(zstd-compressed hash values in positional order). Non-empty `values_blob`s
include the hash-algorithm tag and are rejected if read under a different active
backend; zero-dep traces store an empty blob and rely on DB/session backend
namespacing for isolation.

Results are deduplicated via the `Results` table, keyed by content hash. Multiple
attributes with the same result share a single Results row.

---

## 6. Verify / Record / Recover Paths

### 6.1 Verify Path (BSalC Verifying Trace Check)

```
1. Trace lookup
   Sessions: (session_key, attr_path_id) -> (trace_id, result_id, node_stamp)
   If not found -> fresh evaluation path

2. Verify trace (BSalC VT check — single pipeline with ordering constraints)
   Pre-pass:  StorePathAvailability batch + GitIdentity + TraceContext deps
   Pass 1:    All remaining deps via DepResolution typestate
              Content/Directory matches trigger markFileVerified (subsumes SC deps)
   Pass 2:    Structural / ImplicitStructure override check (if applicable)

3. Serve result (constructive trace -- BSalC CT)
   Decode CachedResult, create child TracedExpr thunks, replay trace
```

### 6.2 Fresh Evaluation Path

```
1. Evaluate the real expression
   DepCaptureScope uses SemanticRegistry for provenance resolution
   DepRecordingContext records deps via pushScope/popScope/takeDeps (GDP-guarded)

2. Record trace (BSalC CT recording)
   Sort and dedup deps
   Compute trace_hash and struct_hash
   INSERT INTO Traces, Results, Sessions, History

3. Materialize result
   Create child TracedExpr thunks for attrset children
```

### 6.3 Recovery (BSalC Constructive Trace Recovery)

```
GitIdentity recovery                                               [indexed lookup + candidate validation]
  Match current git HEAD + dirty state against historical trace
  Load candidate trace and accept if it is also git-recoverable for the same repo

Direct hash recovery                                               [O(N) recompute + indexed lookup]
  Recompute current dep hashes from old trace's dep keys
  Compute canonical trace_hash with the active backend -> lookup in History
  Accept if found (under the active backend's collision resistance)

Structural variant recovery                                        [O(V)]
  Group history entries by dep_key_set_id
  For each group: load representative key set, recompute hashes, compute trace_hash
  Accept first match
```

Structural-variant recovery does not need historical dependency values for
acceptance: candidate trace hashes are computed from freshly-resolved current
values. Historical values are loaded only when
`eval-trace-structural-recovery-mismatch-telemetry` is enabled for diagnostics.

---

## 7. Flake Integration

### 7.1 Two-Phase Flake Model

Flake evaluation has two phases, and eval-trace only instruments the second:

1. **lockFlake** (graph discovery): Resolves flake references, fetches inputs,
   builds the lock file. Eval-trace does NOT instrument this phase because the
   SemanticRegistry is not yet populated during input resolution. Store mutations
   during graph construction are GDP-guarded via `AuthorityState`.

2. **callFlake** (evaluation): Evaluates the flake's `outputs` function with
   locked inputs. Eval-trace instruments this phase: `DepCaptureScope` provides
   SemanticRegistry-based provenance resolution, and all dep recording happens here.

### 7.2 Resolved Graph and Session-Level Invalidation

`callFlake` does not pass a lock file string to `call-flake.nix`. Instead,
`buildResolvedFlakeGraphValue` constructs a pre-resolved Nix attrset from
`LockedFlake.resolvedGraph` entirely in C++. `call-flake.nix` receives this
attrset and never calls `fromJSON` or reads `flake.lock`. This means
`call-flake.nix` performs zero fetching, registry consultation, or store mutation.

Lock-file change detection operates at session granularity, not per-key:

- `computeResolvedGraphDigest` computes a framed eval-trace digest over the
  resolved graph: node keys, locked version identities, input specs, resolved inputs,
  relativePaths, subdirs, and isFlake flags. Intentional exclusions: `narHash`
  is excluded for the root node and all `path:` type inputs (because the
  accessor type can vary across processes); `carrierPath` and `flakePath`
  store paths are excluded (because they are mutable side-effects of
  `lockFlake`). This digest is part of the session key.
- Any change to a hashed field in any lock entry produces a different graph
  digest, a different session key, and therefore a complete cache miss -- all
  traces for that flake are cold-evaluated.

Per-node source identity is provided by `publishPathProvenance` calls in
`buildResolvedFlakeGraphValue`, which attach `PathObject` with
`DepSource::fromNodeKey` identity to each node's `flakePath` and `outPath`.
This gives intra-session dep recording precision (e.g., reading a file inside
nixpkgs records a dep against the nixpkgs node key), but does not provide
per-lock-entry invalidation across sessions.

---

## 8. Known Limitations

### 8.1 Soundness Gaps

These are cases where the system may serve stale cached results. Each has a
corresponding test that documents the gap (see the test CLAUDE.md files for
test locations).

- **ParentSlot does not capture key set changes.** A parent attrset's trace hash
  depends only on its dep set, not its key set. Removing a child from a parent
  does not invalidate the child's trace.

- **Bare import without attribute access.** `import ./config.nix` with no
  attribute access records no NixBinding SC dep. File changes are invisible to
  the trace.

- **Parent-mediated value changes (Gap P1).** If a parent overlay changes a
  child's definition without changing any file the child reads or any sibling
  the child accesses, the child's trace incorrectly validates. Very low severity.

- **Symlinks not tracked.** Intermediate symlink targets are not recorded as
  deps. Changes to a symlink target without changing the resolved file will
  not invalidate the trace.

### 8.2 Precision Issues

These are cases where the system over-invalidates (result is always correct).

- **Whole-file Content dep for `.nix` code.** Any byte change to a `.nix` file
  invalidates all traces that imported it. NixBinding SC deps mitigate this
  for non-recursive attrset-returning files but not for arbitrary Nix code.

- **Session-level lock-file invalidation.** Any change to any lock entry
  (even an unused input's `rev`) invalidates the entire session. Per-key
  StructuredProjection precision does not exist for lock-file changes.

- **`callFunction` with `...` formals records `#keys`.** Adding an unrelated
  key to a tracked argument attrset causes unnecessary invalidation.
  Per-formal `#has:formalName` deps would be more precise.

- **Dynamic dep instability.** Like Shake, dependencies are discovered during
  evaluation and can vary between evaluations of the same attribute. This means
  direct hash recovery may miss even when the state is identical -- structural
  variant recovery is needed as a fallback.

- **File mutation during one evaluation.** The session-lifetime
  `FileContentHashCache` keys on `SourcePath` and assumes files are stable for
  the duration of an `EvalState`. If file bytes change after the first access
  in the same evaluation, later reads can see the first-observed digest. Nix
  already treats file-change-during-eval as undefined behavior; cross-session
  verification recomputes through fresh state.

- **Provenance propagation is conservative.** Container-reconstructing builtins
  (`mapAttrs`, `filter`, etc.) propagate provenance from the original tracked
  container. When the derived container has a different shape, shape deps
  reference the original path, which may cause false invalidation.

---

## Appendix A: References

1. Mokhov, Mitchell, Peyton Jones. "Build Systems a la Carte: Theory and Practice."
   *Journal of Functional Programming* 30, e11, 2020.

2. Hammer, Khoo, Hicks, Foster. "Adapton: Composable Demand-Driven Incremental
   Computation." *PLDI 2014.*

3. Matsakis et al. "Salsa: A Framework for On-Demand, Incremental Computation."
   https://github.com/salsa-rs/salsa

4. Mitchell. "Shake Before Building: Replacing Make with Haskell." *ICFP 2012.*

5. Noonan. "Ghosts of Departed Proofs: Functional Pearls." *Haskell Symposium 2018.*
