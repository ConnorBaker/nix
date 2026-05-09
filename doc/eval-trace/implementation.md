# Nix Eval Trace: Implementation Notes

**Target audience:** Nix core team reviewing the implementation.

**System classification:** Deep constructive trace store with structural variant
recovery (BSalC), demand-driven evaluation via articulation points (Adapton),
dynamic dependency discovery (Shake), type-enforced invariants (GDP proofs plus
opaque capabilities), and an async verification pipeline with linear typestates.

> **Terminology note.** This document uses the standard terminology defined in
> `design.md` Section 2. See the glossary there for mappings between this system's
> names and prior art concepts.

---

## 1. Directory Layout

```
src/libexpr/eval-trace/
  cache/        Trace session, materialization, replay (hot path)
  deps/         Dependency recording, hashing, interning, mount resolution
  fiber/        Fiber scheduler, blocking scope (eval thread <-> async bridge)
  store/        SQLite-backed trace store, verification, recovery, async runtime
  context.cc    TraceRuntime (EvalState integration)
  counters.cc   Performance counters

src/libexpr/include/nix/expr/eval-trace/
  cache/        Headers for trace session, backends, root handle
  data/         TracedData (structured JSON/TOML/dir materialization)
  deps/         types.hh, recording.hh, dep-capture-scope.hh, mount resolution
  store/        trace-store.hh, verification-session.hh, verification-protocol.hh

src/libutil/include/nix/util/
  gdp/          GDP proofs: Proof<Tag>, Certifier<Tag>, ProofGuarded<T,Tag>
  session-types/ Session types (ported from Dialectic; retained but not used by eval-trace)
  tagged.hh     Tagged<Tag,T> phantom-typed wrapper
  linear.hh     Linear<Derived> CRTP for exactly-once consumption
  singleton/    Tag<V> for runtime->compile-time dispatch
  guarded.hh    Guarded<T,Mutex> continuation-based sync

src/libexpr-tests/eval-trace/
  dep/          Dependency recording tests
  nix-binding/  Per-binding .nix tracking tests
  store/        Trace store, verification, recovery, protocol tests
  traced-data/  Materialization + dep-precision tests
  verify/       Integration tests

tests/functional/
  flakes/eval-trace-*.sh    Flake-mode functional tests (core, deps, output,
                            recovery, volatile, semantic, graph, soundness)
  eval-trace-impure-*.sh    Impure-mode functional tests (core, deps, advanced,
                            output, regression, soundness)
```

---

## 2. Layer Architecture

### 2.1 TraceSession (cache layer)

`TraceSession` is the per-context entry point. It owns:

- `TraceBackend` (polymorphic: `StoreTraceBackend` or `NullTraceBackend`)
- `SemanticRegistry` (pre-populated at session open for dep resolution)
- Stamp vectors for sibling identity tracking

`TraceSession` creates `RootTraceExpr` thunks for the root value. When forced,
these dispatch to the verify or fresh-eval path.

### 2.2 TracedExpr (Adapton articulation points)

`TracedExpr` is an abstract base class (GC-allocated `Expr` subclass) with two
concrete subclasses:

- **RootTraceExpr:** Root thunk created by `TraceSession`. Verify -> serve or
  evaluate -> record.
- **ChildTraceExpr:** Child thunk materialized from a verified parent's attrset.
  Points to its parent's `TracedExpr*` for tree navigation.

The base class provides common eval dispatch logic. Both concrete subclasses
are `final`.

### 2.3 AsyncRuntime (async layer)

`AsyncRuntime` manages the async infrastructure:

- **io_context** with dedicated worker threads (`IocWorkerPool`). These threads
  call `ioc.run()` and drive all async completions.
- **BlockingThreadPool**. Serializes blocking I/O (SQLite, filesystem) via
  `coroBlock`, which dispatches work to pool threads and resumes the suspended
  coroutine via a timer completion on the io_context. The orchestrator calls
  TraceStore methods directly under a blocking-pool proof; there is no
  separate `TraceStoreService` class.
  Creates `Proof<BlockingTag>` inside the blocking scope.
- **FiberScheduler** bridges the eval thread to async. `syncAwait` posts a
  coroutine via `co_spawn` to the io_context executor, then blocks the eval
  thread on `std::future::get()`. The worker threads drive the io_context --
  the eval thread never calls `run_one()` or `poll()`.

**There are no `asio::strand` objects.** Serialization of orchestrator state
is achieved by `syncAwait` blocking: the eval thread blocks while the coroutine
runs on a worker thread, preventing concurrent access. `VerificationAccessTag`
(a GDP proof tag) guards orchestrator-local state as defense-in-depth.

The `EvalContext<Mode>` phantom type system enforces the `syncAwait` restriction:
- `EvalContext<Suspendable>` -- may call `syncAwait` (eval thread main stack)
- `EvalContext<Critical>` -- must NOT call `syncAwait` (inside lock or handler)

Narrowing (`Suspendable -> Critical`) is free and irreversible. Widening does
not exist. The executor is `any_io_executor` (type-erased), which can post work
but cannot drive the event loop -- structurally preventing re-entrant
`run_one()` calls.

### 2.4 TraceStore (storage layer)

`TraceStore` is the SQLite backend. GDP: privately inherits
`Certifier<BlockingTag>` -- only the constructor, destructor, and lifecycle
methods (`flushExclusive`, `loadRuntimeRootsExclusive`, etc.) can create proofs
directly. The production async path gets proofs via
`BlockingThreadPool::coroBlock`. Primary public data-path methods additionally
require the opaque `ExclusiveTraceStoreAccess` capability, minted by
`TraceStore::withExclusiveAccess`; a few residual lifecycle/private-or-friend
paths still take only `Proof<BlockingTag>`. `lookupCurrentNode`, result decode,
and lifecycle helpers are covered by store/SQLite locking; `publishRecovery`
also updates in-memory CurrentNodes state outside `storeMutex_`, so it is a
remaining EA-migration target. Blocking permission and exclusive mutable store
access are separate facts.

Key methods:
- `verifyTrace()` -- generic pipeline verification (pre-pass/pass1/pass2 + outcome)
- `record()` -- INSERT/UPSERT trace + result + history
- `recovery()` -- direct hash + structural variant recovery
- `resolveCurrentDepHash()` -- typestate pipeline (Unchecked -> CacheMissed -> resolved)

---

## 3. Type-Safety Patterns

### 3.1 GDP Proof Enforcement

Each GDP certifier privately inherits `Certifier<Tag>`. The full set of GDP
proof tags:

| Proof Tag | Certifier(s) | Guards | Purpose |
|-----------|-----------|--------|---------|
| `BlockingTag` | `TraceStore` (lifecycle), `BlockingThreadPool` (async), test helpers | Blocking-thread I/O: filesystem, git, store IPC, SQLite lifecycle | Only blocking-capable threads perform blocking work |
| `AuthorityState` | `AuthorityGate` | `buildResolvedFlakeGraph` and helpers | Phase-1 store mutations during graph construction |
| `DepCaptureScopeTag` | `DepCaptureScope`, `SiblingForceScope`, `PublicationWarmupScope` | `pushScope`/`popScope`/`takeDeps` | Scope management |
| `DedupCheckedTag` | `DedupGate` | `EpochLogRef::append` | Dedup check passed before epoch log write |
| `RecordingScopeActiveTag` | `RecordingScopeGuard` | `replayTrace`, `EpochLogRef::appendReplayed` | Active recording scope exists |
| `FileStrandTag` | `FileStrandGate` (derives from `ExclusiveTraceStoreAccess`) | ParseCaches access, `resolveDepHash` | On a blocking thread with exclusive TraceStore access |
| `VerificationAccessTag` | `VerificationOrchestrator` | `PrefetchPool` access | Orchestrator state isolation |

High-blast-radius verification facts are sealed with opaque capabilities
instead of GDP proofs:

| Capability | Factory | Guards | Purpose |
|------------|---------|--------|---------|
| `VerifiedFileDep` | `VerificationSession::verifiedFile()` (private, friend-gated to `VerifyImpl`) | `markFileVerified()` | Per-file subsumption after content hash match |
| `ExclusiveTraceStoreAccess` | `TraceStore::withExclusiveAccess()` | Primary public data-path `TraceStore` methods and `FileStrandGate` | Store mutex held for this TraceStore |

### 3.2 Typestate: DepResolution

The dep hash resolution pipeline is a two-state machine using `Linear<Derived>`:

```
[*] --> Unchecked: begin()
Unchecked --> resolved_L1: checkCache() -- L1 hit
Unchecked --> CacheMissed: checkCache() -- L1 miss
CacheMissed --> resolved_L3: computeAndCache() -- resolveDepHash
```

Compile errors: `computeAndCache()` on `Unchecked` (requires clause), skipping
L1 (private constructor), reusing consumed state (`&&` qualification).

### 3.3 Session Types (historical)

Earlier drafts of the verification pipeline used session-typed protocols
(`VerifyPipeline`, `VerifyOrRecover`, `TraceStoreOp`, `PrefetchHint`) in an
in-process channel shell. Bundle 4 removed them in favor of direct typed
method calls on TraceStore under `Proof<BlockingTag>`; the
`RecoveryState<Stage>` linear typestate (see §3.5) encodes recovery
ordering now.

The session-types library under `src/libutil/include/nix/util/session-types/`
still exists for other potential uses. The eval-trace pipeline does not
use it directly any more — ordering constraints live in C++ typestates
(`Linear<T>`, `RecoveryState<Stage>`) rather than channel-based
communication sequences.

### 3.4 Phantom Types via Tagged<Tag, T>

All IDs, hashes, and dep provenance use `Tagged<Tag, T>`:
- `TraceId`, `ResultId`, `DepKeySetId`, `AttrPathId`, `DepSourceId`, `DepKeyId`
- `TraceHash`, `StructHash`, `ResultHash`
- `CurrentTraceDep`, `CandidateDep` (origin-tagged deps that gate subsumption
  and L1 writes at compile time)
- `ComputedHash`, `VerifiedHash` (L1 cache provenance via `singleton::Tag<HP>`)
- `EvalContext<Suspendable>`, `EvalContext<Critical>` (mode coloring:
  `syncAwait` only available in Suspendable mode)

### 3.5 Additional Typestates

- **RecoveryState<Stage>:** `RecoveryUntried -> RecoveryGitMissed ->
  RecoveryDirectMissed -> result`. Enforces sequential recovery strategy
  ordering. Each stage is a `Linear` type that must be consumed via the next
  transition. NOT a session type -- recovery is a single coroutine driving a
  state machine, not a communication protocol.

- **`StoreTraceBackend::sessionBound_`:** plain bool checked by a debug
  `assert` in `StoreTraceBackend::verify` (see `context.cc`). Prevents
  calling `verify()` before `bindSession()`. Earlier drafts encoded this
  as an `OrchestratorHandle<Phase>` linear typestate (`Unbound -> Bound`);
  the simpler assertion proved sufficient in practice.

- **Proposed: `AccessorForwardingScope`** (not yet implemented).  Same
  sealed-capability pattern as `OriginScope<CurrentTrace>` in
  `trace-store-verify.cc`: private ctor, friended factory minter,
  one method on the scope (`mkForwardedPath`) as the only way to
  construct a derived path value that inherits the source's accessor
  identity.  Would prevent the accessor-drop hazard class that
  `ExprConcatStrings::eval` and `EvalState::storePath` manifested
  before the committed fixes.  See
  `doc/eval-trace/audit/rearchitecture-proposal.md` §14 for the full
  rationale and alternative options.

### 3.6 Singleton Dispatch

`describe(kind)` maps each `CanonicalQueryKind` to a `QueryBehavior` and
`QueryDomainMask`. Adding a CQK variant without a descriptor is a
`-Wswitch-enum` error. `DepSourceKind` has 2 variants (`Absolute`,
`Registered`) with named factories (`DepSource::fromNodeKey`,
`DepSource::fromRuntimeRoot`) for the two disjoint identity namespaces.

### 3.7 Threading Architecture

```
Eval thread
  +-- FiberScheduler (provides executor for co_spawn)
       +-- syncAwait: co_spawn coroutine, block on future.get()
            |
            v  (posted to io_context, run by worker threads)
       +-- VerificationOrchestrator (GDP VerificationAccessTag)
            +-- coroBlock -> BlockingThreadPool (creates Proof<BlockingTag>)
                 +-- TraceStore (SQLite, filesystem I/O)
                      +-- resolveDepHash (DOM caches, FileContentHashCache)
```

- **Eval thread**: `TracedExpr::eval`, `forceValue`, dep recording (thread-local).
  Blocks on `future.get()` inside `syncAwait`.
- **io_context worker threads**: Drive all async completions. Coroutines
  (`verifyAttrImpl` and the orchestrator's internal store-dispatch helpers)
  run here.
- **BlockingThreadPool**: Blocking I/O (SQLite, filesystem). Creates
  `Proof<BlockingTag>` via `coroBlock`. Posts timer completions back to
  io_context when blocking work finishes.

---

## 4. Verification Pipeline Detail

### 4.1 Session-Open Runtime Root Verification

Runtime roots (locked fetchTree inputs) are verified at session open via
`loadAndVerifyRuntimeRoots`, not during per-trace verification:
1. Load runtime root metadata from the trace backend
2. Verify each root's narHash against the current store path
3. Verified roots are merged into the `SemanticRegistry` via `addEntry()`
4. Failed roots are NOT registered -- traces referencing them fail naturally

### 4.2 Pre-Pass

`runStorePathBatch` and `runPrePassDeps`:
1. Batch StorePathAvailability deps into a single `queryValidPaths` call
2. Resolve GitRevisionIdentity deps (on match, record verified git repo root)
3. Prime TraceContext (TraceValueContext, TraceParentSlot) dep memo cache

### 4.3 Pass 1: Main Dep Verification

For each dep, using the `DepResolution` typestate:
1. Check L1 cache (`VerificationSession::lookupDepHash`)
2. On miss: `resolveDepHash()` -- compute current hash, check subsumption
3. Compare against stored hash
4. ContentOverrideable failures are deferred to Pass 2
5. StructuredProjection and ImplicitStructure deps are deferred to Pass 2
6. Content/Directory hash matches trigger `markFileVerified()` (via
   `VerifiedFileDep`), which subsumes finer-grained deps from that file

### 4.4 Pass 2: Multi-Level Override

Pass 2 produces a `Pass2Result` containing a `VerifyOutcome`. Three paths:

**No failures and no deferred deps:** Immediate `Valid`.

**All Content deps passed, deferred structural/implicit deps exist:** Build a
`coveredFiles` set from all ContentOverrideable deps (these files are already
`markFileVerified`'d). Verify deferred StructuredProjection and ImplicitStructure
deps, skipping any whose file is in `coveredFiles` (already subsumed). Outcome
is `Valid` if all pass, `Invalid` otherwise.

**Only ContentOverrideable failures, structural/implicit deps exist:**
1. Build `passedContentFiles` (content deps that passed) and
   `failedContentFiles` (content deps that failed)
2. Build coverage maps: `structuralCoveredFiles` from StructuredProjection deps,
   `implicitCoveredFiles` from ImplicitStructure deps
3. Check every failed file is covered by at least one map
4. Verify StructuredProjection deps, skipping `passedContentFiles`
5. Verify ImplicitStructure deps only for failed files not already
   structurally covered
6. Outcome: `ValidViaStructuralOverride` if all coverage is structural,
   `ValidViaImplicitShapeOverride` if any file required implicit-only coverage
   (triggers in-memory trace hash recomputation in `applyOutcome`),
   `Invalid` if coverage is incomplete or any dep fails

### 4.5 Recovery

Three strategies, tried in order via `RecoveryState` typestate
(`RecoveryUntried -> RecoveryGitMissed -> RecoveryDirectMissed`):

1. **GitIdentity recovery:** indexed SQL lookup by git repository identity hash.
   If the current git HEAD + dirty state matches a historical trace, load the
   candidate full trace and accept it only if it is also git-recoverable for the
   same repo. This skips normal dep recomputation but is not a pure O(1)
   accept path.

2. **Direct hash recovery:** Recompute all current dep hashes from the old
   trace's dep keys. Compute the canonical framed trace hash over the sorted
   deps with the active backend. Look up in TraceHistory.

3. **Structural variant recovery:** Group history entries by `dep_key_set_id`.
   For each group, load the representative key set, recompute hashes with
   current values, compute candidate `trace_hash`, and look up. Stored
   historical values are loaded only when
   `eval-trace-structural-recovery-mismatch-telemetry` is enabled so telemetry
   can compare historical and recomputed hashes; the winning hash is always
   computed from freshly resolved values. Handles dynamic dep-key-set
   instability. Uses
   `CandidateDep` (not `CurrentTraceDep`) so candidate iteration always computes
   `hash(op(current state))` and cannot reuse a historical stored hash via the
   subsumption shortcut.

---

## 5. Dep Resolution Service

`resolveDepHash<TaggedDepType>()` is the single subsumption enforcement point.
It is templated on the phantom-tagged dep type:

- `CurrentTraceDep`: used by primary verification and direct-hash recovery.
  It enables L1 reads/writes and the structural/implicit subsumption shortcut.
  When `session.isFileVerified(traceId, key)` holds (trace-scoped, not
  session-wide), subsumption returns the stored hash
  as `VerifiedHash` behind the `VerifiedSubsumption` capability.
- `CandidateDep`: used by structural variant recovery. It can read and write
  computed current-state hashes in L1, but the subsumption shortcut is compiled
  out. This prevents historical candidate hashes from poisoning recovery.

The function handles all `CanonicalQueryKind` variants via a `switch`.
`QueryBehavior` and `QueryDomainMask` (from `describe(kind)`) classify deps
for subsumption, but the per-kind hash computation uses direct CQK switching:
- `FileBytes`/`RawBytes`: read bytes and compute the active eval-trace digest
- `DirectoryEntries`: compute the active eval-trace digest of sorted entries
- `NarIdentity`: dump NAR and compute the active eval-trace digest
- `ExistenceCheck`: `maybeLstat()` -> type string or "missing"
- `EnvironmentLookup`: re-read env variable, active eval-trace digest
- `StructuredProjection`/`ImplicitStructure`: DOM navigation (JSON/TOML/dir/Nix AST);
  scalar/sentinel values use active-backend raw payload digests, while key-set
  and NixBinding hashes use canonical framed builders
- `RuntimeFetchIdentity`: resolved via SemanticRegistry (session-open verified)
- `VolatileTime`/`VolatileExec`/`TraceValueContext`/`TraceParentSlot`: returns nullopt

---

## 6. Recording Infrastructure

### 6.1 DepRecordingContext

Per-fiber dep recording state. Manages:
- Scope stack (GDP-guarded push/pop via `Proof<DepCaptureScopeTag>`)
- Epoch log (append-only dep log with two GDP-guarded write paths:
  `append` requires `Proof<DedupCheckedTag>`, `appendReplayed` requires
  `Proof<RecordingScopeActiveTag>`)
- Per-scope dedup state (`seenDeps` map). `DedupGate` is used by the `record()`
  method to certify dedup was checked before creating the append proof.

### 6.2 DepCaptureScope

RAII scope for dep recording. Takes `const SemanticRegistry &` for provenance
resolution. Privately inherits `Certifier<DepCaptureScopeTag>`.

### 6.3 Interning

All string data is interned via `InterningPools`:
- `StringInternTable` for all dep keys, source names, file paths
- `DataPathPool` trie for structured data paths (JSON/TOML navigation)
- `dirSets` for aggregated directory set definitions
- `ProvenanceTable` for value provenance tracking

---

## 7. Key Runtime Components

### 7.1 TraceRuntime (context.cc, context.hh)

`TraceRuntime` is the per-`EvalState` singleton that owns all eval-trace state.
It is non-null when eval-trace is enabled (the default). Key members:

- `InterningPools` (strings, data path trie, provenance table)
- `AttrVocabStore` (shared attribute name/path trie, persisted to SQLite)
- `NixSemanticAnalyzer` (per-binding AST hash cache for NixBinding SC deps)
- `MemoReplayStore` (thunk dep replay memoization with Bloom filter)
- Value identity maps (GC-safe pointer equality for `eqValues` + sibling detection)
- `sessionCache` (Hash -> `ref<TraceSession>` for session reuse across commands)

Dynamic runtime-root mount-point addition during cold eval no longer goes
through `TraceRuntime` globals; it routes through
`TraceSession::registerRuntimeRootMount` and the session's backend's
`recordRuntimeRoot`.

`TraceRuntime` also manages `SiblingReplayCaptureScope` (thread-local scope
for tracking which siblings were accessed during a `ChildTraceExpr` evaluation,
emitting `ParentSlot` + `ValueContext` deps).

### 7.2 FileContentHashCache

`EvalEnvironmentSharedState::fileContentHashCache` is a session-lifetime
`SourcePath -> DepHash` cache shared by eval and dep-resolution paths. It
eliminates repeated content hashing for files read many times in one `EvalState`
(for example, nixpkgs files touched through many bindings).

The cache is not persisted and does not use stat fingerprints. Its contract is
the evaluator's existing stability assumption: files are not mutated during a
single evaluation. Cross-session verification opens fresh state and recomputes
hashes through the active eval-trace backend.

---

## 8. Testing Strategy

### 8.1 Correctness Gate

The mandatory correctness test is a nixpkgs evaluation comparison:
```bash
nix eval -f default.nix asciidoc.nativeBuildInputs --no-eval-trace --json
nix eval -f default.nix asciidoc.nativeBuildInputs --json
```
Both must produce byte-for-byte identical output. Unit tests use synthetic
expressions and do NOT capture the real nixpkgs evaluation path.

### 8.2 Test Categories

- **Functional tests** (`tests/functional/flakes/eval-trace-*.sh` and
  `tests/functional/eval-trace-impure-*.sh`): cover the full CLI pipeline
  including flake input updates, NixBinding SC override, subsumption,
  attribute removal, recovery, volatile deps, semantic provenance, graph
  identity, and soundness regression tests.
- **Unit tests** (`src/libexpr-tests/eval-trace/`): TraceStore record/verify,
  recovery, dep precision, materialization, protocol round-trips, showForHash,
  and integration tests across the `dep/`, `nix-binding/`, `store/`,
  `traced-data/`, and `verify/` subdirectories.
- **Sanitizer builds**: ASAN+UBSAN and TSAN configurations documented in
  `src/libexpr/eval-trace/CLAUDE.md`.

### 8.3 Benchmark Suite

`eval-trace-bench` (flake app, invoked with `nix run .#eval-trace-bench -- ...`)
evaluates across nixpkgs commit sequences in reference (no trace), cold
(persistent cache while walking commits), and hot (cache warmed from cold)
configurations. The main subcommands are `generate`, `runs`, `pairwise`,
`series`, `classify`, `sv`, `logs`, `simulate`, `db-inspect`, and `export`.
`generate` can pass through `--eval-trace-hash-algorithm blake3|sha256`, toggle
structural-variant recovery for selected run modes, and opt selected runs into
`--eval-trace-structural-recovery-mismatch-telemetry` for SV diagnostics.
