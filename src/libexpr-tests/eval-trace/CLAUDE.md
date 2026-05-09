# Eval-Trace Test Guide

This guide covers the unit test suite under `src/libexpr-tests/eval-trace/`.
See `tests/functional/CLAUDE.md` for the functional (shell-based) tests.

## Section 1: Test Layout

```
src/libexpr-tests/eval-trace/
  helpers.hh / helpers.cc    Test infrastructure (fixtures, helpers, test access)
  semantic-registry-test-access.hh  SemanticRegistry mutation access for tests
  dep/                       Dep recording primitives, epoch log, scope lifecycle
  store/                     TraceStore operations, verification, recovery, session
  traced-data/               Structured data (JSON/TOML/readDir) materialization
  traced-data/dep-precision/ Per-builtin dep precision (soundness + precision)
  traced-data/materialization/ Cross-scope materialization and pointer equality
  nix-binding/               NixBinding SC dep recording and override
  verify/                    Full pipeline integration tests
```

All test files are listed in the `sources = files(...)` block in
`src/libexpr-tests/meson.build`.

## Section 2: Fixture Hierarchy

```
LibExprTest                    Base: EvalState, no tracing
  └─ EvalTraceTest             Adds: ScopedCacheDir, withBs() for BlockingTag proof
       └─ TraceCacheFixture    Adds: TraceSession, makeCache(), forceRoot(), invalidateFileCache()
            ├─ TraceCacheTest      Store/cache.cc: construction, root value, cold/warm, fingerprint isolation
            ├─ DepTrackingTest     dep/oracle-basic.cc, dep/oracle-nested.cc: full pipeline oracle tests
            ├─ ReplayIntegrationTest  dep/replay-integration.cc: replay dedup-pollution
            ├─ EpochBugIntegrationTest  dep/epoch-bugs.cc: BUG-3/7 integration
            ├─ SourceTreeSoundnessTest  dep/source-tree-soundness.cc: DirCopy/FilteredCopy/ThunkMemo/ImportedDirCopy soundness; per-leaf lazy verification contract pin; shared-thunk sibling TraceValueContext replay; warm-hit epochMap asymmetry regression guards
            ├─ TraceCacheIntegrationTest  verify/integration.cc: full pipeline
            ├─ TracedDataTest      traced-data/**/*.cc: TempJsonFile, TempTomlFile, TempDir
            │    └─ DepPrecisionTest    dep-precision/**/*.cc, dirset-verification.cc
            │         └─ MaterializationDepTest  materialization/**/*.cc: getStoredDeps, getStoredResult
            │              └─ NixBindingOverrideTest  nix-binding/override.cc
            └─ TraceStoreFixture   Exposes raw TraceStore (makeDb, recreateDb, vpath)
                 └─ TraceStoreTest     store/**/*.cc: standard store-level tests
                 └─ SessionConfigTest  store/session-config.cc
                 └─ DepOverapproxFixture, NixBindingCoverageFixture  (local in .cc)

::testing::Test (standalone — no EvalState)
  ├─ EpochStabilityTest       dep/epoch-stability.cc: epoch-map scope-boundary stability
  ├─ EpochRecordTest          dep/epoch-record.cc: recordThunkDeps / replayMemoizedDeps
  ├─ SetOnceTest (TEST())     store/set-once.cc: SetOnce<T> write-once semantics
  ├─ CQKLatticeTest (TEST())  store/cqk-lattice.cc: CQK lattice predicate consistency
  ├─ DepSourceEncodingTest    store/dep-source.cc: DepSource serialization round-trip
  ├─ DepSourceInterningTest   store/dep-source.cc: InterningPools intern/resolve
  ├─ SemanticRegistryTest     store/semantic-registry.cc: registry dispatch + reverse resolve
  └─ PolicyDigestTest         store/policy-digest.cc: BUG-11 computePolicyDigest regression
```

### EvalTraceTest

**Provides:**
- `ScopedCacheDir cacheDir` — RAII temp dir, sets `NIX_CACHE_HOME`
- `withBs(f)` — creates a `gdp::Proof<BlockingTag>` for the duration of
  `f`. Satisfies the I1 precondition ("I can block safely"); use for test
  code that calls blocking APIs that are NOT TraceStore methods
  (filesystem, git, daemon IPC).
- `withExclusiveStore(store, f)` — composes `withBs` with
  `store.withExclusiveAccess`, then invokes `f(ea)` with a scoped
  `ExclusiveTraceStoreAccess`. Satisfies both I1 (blocking) and I2
  (exclusive TraceStore access). Preferred helper for test code that
  calls TraceStore public methods — those take `ea`, not `bs`.
- Fresh `EvalState` per test (no trace, `nixPath={}`, dummy store read-write)

**When to use:** Any test that needs a live evaluator but does NOT need a full TraceSession (e.g., dep type unit tests, NixBinding determinism, show-for-hash).

### TraceCacheFixture

**Provides (in addition to EvalTraceTest):**
- `makeCache(nixExpr, loaderCalls*)` — creates a `ref<TraceSession>`, releases previous session
- `forceRoot(session)` — forces the root value and returns it
- `invalidateFileCache(path)` — clears FS accessor lstat cache and file content hash cache
- `testFingerprint` — override in derived test to isolate DB namespaces
- `releaseActiveSession()` — called automatically in destructor; releases SQLite connection

**When to use:** Tests that exercise the full trace→verify→serve pipeline using `TraceSession`. The standard pattern is cold eval (records trace) followed by warm eval (verifies and serves from cache).

**Key method — `makeCache`:**
```cpp
ref<TraceSession> makeCache(const std::string & nixExpr, int * loaderCalls = nullptr)
```
Creates a TraceSession with a loader that evaluates `nixExpr`. Passing `&loaderCalls`
lets the test assert how many times the loader was called (0 = cache hit, 1 = re-eval).

### TracedDataTest

Extends `TraceCacheFixture`. Sets `testFingerprint` to `"traced-data-test"`.
Used by all JSON/TOML/readDir tests. No additional members beyond TraceCacheFixture.

### DepPrecisionTest

Extends `TracedDataTest`. Provides dep-collection helpers for asserting what deps
were recorded during evaluation:

- `evalAndCollectDeps(nixExpr)` — evaluates an expression and returns resolved deps
- `hasDep(deps, type, keySubstr)` — substring match on dep key
- `countDeps(deps, type, keySubstr)` — count deps matching type + key
- `countDepsByType(deps, type)` — count all deps of a type
- `hasJsonDep(deps, type, pred)` / `countJsonDeps(deps, type, pred)` — for StructuredProjection/ImplicitStructure deps
- `hasKeyPred(key)` / `shapePred(suffix)` / `pathContainsPred(path)` — predicates for JSON dep keys
- `dumpDeps(deps)` — diagnostic string for EXPECT failures
- `fj(path)` / `ft(path)` / `rd(path)` — expression builder helpers

**When to use:** Tests that verify which deps are (or are not) recorded for a given Nix expression — i.e., precision and soundness of the dep recording subsystem.

### MaterializationDepTest

Extends `DepPrecisionTest`. Provides cross-session store query helpers:

- `makeQueryDb()` — opens a fresh TraceStore against the same DB (releases active session first)
- `getStoredDeps(attrPath)` — returns deps stored for an attr path (dot-separated string)
- `getStoredResult(attrPath)` — returns the cached result for an attr path
- `pathFromDotted(dotPath)` — converts `"a.b.c"` to an `AttrPathId`

**When to use:** Tests that verify the contents of stored traces after evaluation — cross-scope materialization, dep ownership, stored result values.

### TraceStoreFixture

Extends `EvalTraceTest`. Provides raw `TraceStore` access without `TraceSession`:

- `makeDb()` — creates a `unique_ptr<TraceStore>` with a fixed bootstrap session key
- `recreateDb(db)` — destroys the old store (flushes to SQLite) then creates a fresh one against the same DB file. **Always use this; never use `db = makeDb()` directly.**
- `testVocab()` / `vpath({...})` / `rootPath()` — attr path construction helpers
- `pools()` — returns `InterningPools &` from EvalState

**When to use:** Tests that call TraceStore methods directly (`record`, `verifyTrace`, `recovery`, serialization) without going through TraceSession.

### Standalone fixture classes (no parent hierarchy)

These classes extend `::testing::Test` directly (or use `TEST()` with no fixture).
They do not need a live `EvalState` or `TraceSession`.

#### EpochStabilityTest (`dep/epoch-stability.cc`)

Extends `::testing::Test`. Provides an `InterningPools` and a `DepRecordingContext`
in isolation. Tests epoch-map stability properties: that scope boundaries do not
leak deps across unrelated scopes, and that sibling index maps remain consistent.

**When to use:** Unit tests that exercise epoch-log ordering and scope-boundary
invariants without needing a full evaluator.

#### EpochRecordTest (`dep/epoch-record.cc`)

Extends `::testing::Test`. Provides an `InterningPools`, a mock epoch log, and
direct access to `recordThunkDeps` / `replayMemoizedDeps`. Tests the low-level
recording/replay API in isolation from the evaluator.

**When to use:** Unit tests for dep recording and memoized-dep replay that do not
require a running `EvalState`.

#### SetOnceTest (`store/set-once.cc`)

Uses `TEST()` (no fixture class). Tests `SetOnce<T>` — the write-once wrapper
used to guard singleton initialization in the trace store. Verifies that a second
`set()` call throws `nix::Error`.

**When to use:** Unit tests for `SetOnce<T>` value semantics.

#### CQKLatticeTest (`store/cqk-lattice.cc`)

Uses `TEST()` (no fixture class). Tests `CanonicalQueryKind` lattice predicates:
`queryBehavior`, `isVolatile`, `isCoveredBySessionFingerprint`,
`repoRootAddressingKind`, `isFileContentDep`, and `describe()` must be mutually
consistent across all known query kinds.

**When to use:** Unit tests for CQK metadata consistency.

#### DepSourceEncodingTest / DepSourceInterningTest (`store/dep-source.cc`)

Extends `TraceStoreFixture` (for `InterningPools` access). Tests `DepSource`
serialization (`fromString` / `toString` round-trip) and `InterningPools`
intern/resolve round-trips.

**When to use:** Unit tests for `DepSource` encoding and the interning pool API.

#### SemanticRegistryTest (`store/semantic-registry.cc`)

Extends `TraceStoreFixture` (for `EvalState` + `InterningPools` access). Tests
`SemanticRegistry` forward-resolve dispatch by `DepSourceKind`, mount-point
reverse resolution (`resolveDepPathKey`), and the record→verify round-trip
invariant.

**When to use:** Unit tests for `SemanticRegistry` resolution correctness.

#### PolicyDigestTest (`store/policy-digest.cc`)

Uses a local fixture extending `LibExprTest`. Tests `computePolicyDigest` —
verifies that both `NIX_PATH` env and `settings.nixPath` config affect the
digest (BUG-11 regression guard), and that `nixPath` is ignored in pure mode.

**When to use:** Regression tests for policy-digest computation.

## Section 3: Test Index

### dep/

| File | Fixture | Description |
|------|---------|-------------|
| `dep/hash.cc` | `DepHashBasicTest` (no fixture) | EvalTraceHash, depHash, depHashPath, depHashDirListing, queryKindName, isDigestDep |
| `dep/format.cc` | `DepFormatTest` (no fixture) | StructuredFormat char roundtrip, ShapeSuffix names, Tagged<> type safety |
| `dep/tracker.cc` | `DepRecordingContextTest` | push/pop scope, record, dedup collapse, conflict marks unstable |
| `dep/child-range-exclusion.cc` | `ChildRangeExclusionTest` | Structural scope isolation (inner/outer dep ownership) |
| `dep/oracle-basic.cc` | `DepTrackingTest` (TraceCacheFixture) | Content/EnvVar/Existence/System/Volatile oracle deps: warm hit + invalidation |
| `dep/oracle-nested.cc` | `DepTrackingTest` (TraceCacheFixture) | Nested attrs, sibling isolation, deep nesting, mixed deps, directory deps, NixBindingSC override |
| `dep/copied-path.cc` | `TraceCacheFixture` + `DepPrecisionTest` | CopiedPath dep recording, path coercion, `${./dir}` interpolation, BUG fix (first-call-only guard) |
| ~~`dep/stat-hash-store.cc`~~ | ~~`StatCachedHashTest`~~ | *Removed: StatHashCache was dead code in production (zero read-path hits)* |
| `dep/replay-pollution.cc` | `ReplayPollutionTest` + `ReplayDepPrecisionTest` | Replay dep dedup-pollution: sibling epoch isolation |
| `dep/replay-integration.cc` | `ReplayIntegrationTest` (TraceCacheFixture) | Full-stack replay dedup-pollution fix integration |
| `dep/dep-stability.cc` | `DepStabilityTest` + `DepStabilityIntegrationTest` | Dep interning stability, scope dedup, epoch-log ordering, integration |
| `dep/epoch-bugs.cc` | `EpochBugTest` + `EpochBugIntegrationTest` | BUG-7 (insert_or_assign), BUG-3 (rollback), finalizeAndTakeDeps scope lifecycle |
| `dep/epoch-stability.cc` | `EpochStabilityTest` | Epoch-map stability across scope boundaries |
| `dep/epoch-record.cc` | `EpochRecordTest` | recordThunkDeps and replayMemoizedDeps |
| `dep/nix-binding-determinism.cc` | `NixBindingDeterminismTest` (EvalTraceTest) | NixBinding hash determinism: parse twice, compare hashes; expression variety |
| `dep/show-for-hash.cc` | `ShowForHashTest` (EvalTraceTest) | showForHash path normalization, basePath stripping, store-vs-local accessor stability |
| `dep/source-tree-soundness.cc` | `SourceTreeSoundnessTest` (TraceCacheFixture) | DirCopy / FilteredCopy / ThunkMemo / ImportedDirCopy source-tree soundness; `ParentChild_PerLeafLazyVerification` pins the cache's per-leaf lazy verification contract (forceRoot warm-hits; individual child forces invalidate or hit as dictated by their own deps; precision preserved); `SharedThunk_Siblings_TraceValueContext_InvalidatesBoth` pins the `SiblingReplayCaptureScope::maybeCapture` → `TraceValueContext` replay path for shared-thunk sibling consumption; four warm-hit epochMap asymmetry probes (`WarmHit_Child_NoEpochMapEntry`, `WarmHit_ThenSourceMutation_SiblingsStillInvalidate`, `RecAttrsetSibling_TraceValueContext_InvalidatesOnSourceChange`, `NestedAttrset_WarmHitChain_InnerInvalidatesOnSourceChange`) guarding the finding that the warm-hit-no-epochMap asymmetry is benign under the current architecture (documented in `memo-replay-store.hh::recordThunkDeps` and `trace-session.cc` warm-hit branch). See DEF-6 for the 2026-04-30 reclassification of two formerly DISABLED tests. |

### store/

| File | Fixture | Description |
|------|---------|-------------|
| `store/cache.cc` | `TraceCacheTest` (TraceCacheFixture) | TraceSession construction, root value, cold/warm string/int/bool/float, fingerprint isolation |
| `store/record-verify.cc` | `TraceStoreTest` | record() returns TraceId, attrExists, verifyTrace warm/cold, queryKindName all variants |
| `store/recovery.cc` | `TraceStoreTest` | Phase-1 constructive recovery, child-survives-parent, multi-trace history, RecoveryState typestate |
| `store/serialization.cc` | `TraceStoreTest` | Dep key/value blob roundtrip for all CQK types, bootstrap session key isolation, null-byte attr paths |
| `store/nixpkgs-patterns.cc` | `TraceStoreTest` | Synthesized nixpkgs miss patterns (aliases.nix, overlay chain, stable dep with volatile sibling) |
| `store/nixpkgs-optimization.cc` | `TraceStoreTest` | StructuredContent blob roundtrip, serialization edge cases (matching nixpkgs dep patterns) |
| `store/hash.cc` | `DepHashTest` (LibExprTest) | computeTraceStructHash: order-independent, type sensitivity, dep count impact |
| `store/git-identity.cc` | `TraceStoreTest` | GitRevisionIdentity: QueryBehavior, descriptor, record/load roundtrip, verify pass/fail |
| `store/git-identity-recovery.cc` | `TraceStoreTest` | GitIdentity dep storage and indexed root-trace recovery; adjacent child fixtures cover matching-session CurrentNodes reuse, not GitIdentity-indexed child recovery |
| `store/copied-path.cc` | `TraceStoreTest` | CopiedPath verify (file unchanged/changed), recovery, key format `"sourcePath\tstoreName"` |
| `store/call-arg-nixbinding.cc` | `NixBindingCoverageFixture` (TraceStoreFixture) | NixBinding coverage gap: call-arg attrsets ineligible for SC deps (aliases.nix pattern) |
| `store/dep-overapprox.cc` | `DepOverapproxFixture` (TraceStoreFixture) | Dep over-approximation: Content dep on imported-but-unaccessed file causes hard miss |
| `store/result-encoding.cc` | `TraceStoreTest` | Float encoding round-trip: BUG-10 regression (std::to_chars vs std::to_string precision) |
| `store/set-once.cc` | `SetOnceTest` (no fixture) | SetOnce<T> value type tests |
| `store/cqk-lattice.cc` | `CQKLatticeTest` (no fixture) | CQK lattice property invariants |
| `store/dep-source.cc` | `DepSourceEncodingTest`/`DepSourceInterningTest` (no fixture) | DepSource serialization/interning |
| `store/semantic-registry.cc` | `SemanticRegistryTest` (no fixture) | SemanticRegistry resolution dispatch |
| `store/policy-digest.cc` | `PolicyDigestTest` (local fixture) | computePolicyDigest BUG-11 regression |
| `store/session-config.cc` | `SessionConfigTest` + `InputCopyIdentityTest` + `SessionKeyDeterminismTest` | Session config construction, path-value provenance, lockedInput identity, session key determinism |
| `store/cross-session-subsumption.cc` | `TraceStoreTest` | Cross-session subsumption: pins the origin-gated `canSubsumeShortcut` in `resolveDepHash` so stored hashes cannot leak across `VerificationSession` boundaries for Normal-behaviour deps or `CandidateDep`. 8 tests including `_SubsumptionLeaks_NarIdentityStaleServed`, `_SubsumptionLeaks_ExistenceStaleServed`, and the `_SubsumptionIsolated_FreshSessionFailsCorrectly` positive-control |
| `store/error-handling.cc` | `TraceStoreTest` | DepResolution error-path tests: graceful miss when a dep's accessor throws (DerivedStorePath, RuntimeFetchIdentity, GitRevisionIdentity) |
| `store/large-trace.cc` | `TraceStoreTest` | Large-trace (10k deps) record+verify round-trip performance/soundness floor (OR-10 Group C) |
| `store/multi-session.cc` | `TraceStoreTest` | Cross-session `recreateDb` bulk-load round-trip: a recorded trace survives a session boundary (OR-10 Group C) |
| `store/nar-identity.cc` | `TraceStoreTest` | `NarIdentity` dep record/verify warm-hit + hash-change invalidation round-trip |
| `store/sv-telemetry.cc` | `TraceStoreTest` | Structural-variant byDepKeySet telemetry aggregation and early-exit signal counters |
| `store/trace-parent-slot.cc` | `TraceStoreTest` | `TraceParentSlot` parent-fingerprint encoding + verification (relevant to OR-4) |
| `store/verification-phases.cc` | `TraceStoreTest` | Per-round recovery-pipeline counter assertions across pass-1 / recovery phases (OR-10 Group B); uses the `nrRecoveryGitIdentity*` counter family |

### traced-data/

| File | Fixture | Description |
|------|---------|-------------|
| `traced-data/json-scalar.cc` | `TracedDataTest` | fromJSON scalar access, nested access, unused-key precision, shape dep soundness/precision |
| `traced-data/json-containers.cc` | `TracedDataTest` | fromJSON attrsets, arrays, nested containers, shape dep recording |
| `traced-data/readdir-override.cc` | `TracedDataTest` | readDir SC override: cache hit on dir-listing-only change, invalidation on relevant change |
| `traced-data/readdir-shape.cc` | `TracedDataTest` | readDir shape deps: #keys and #has suffixes, dirset hash stability |
| `traced-data/advanced.cc` | `TracedDataTest` | Mixed JSON/TOML/readDir in one trace, deep paths, nested attrsets, error paths |
| `traced-data/gaps-soundness.cc` | `TracedDataTest` | Soundness coverage for shape-dep gaps: all structured builtins, corner cases |
| `traced-data/gaps-rawcontent.cc` | `TracedDataTest` | Raw content fallback when shape deps are absent |
| `traced-data/gaps-propagation.cc` | `TracedDataTest` | Shape dep gap propagation across trace boundaries |
| `traced-data/dirset-verification.cc` | `DepPrecisionTest` | DirSet aggregated has-key-miss deps: cross-shard absence, Fix 2 regression guard |

### traced-data/dep-precision/

| File | Fixture | Description |
|------|---------|-------------|
| `dep-precision/creation.cc` | `DepPrecisionTest` | StructuredProjection dep created for fromJSON/fromTOML scalar/nested access |
| `dep-precision/typeof.cc` | `DepPrecisionTest` | typeOf on traced data: SC #type dep precision, no spurious Content deps |
| `dep-precision/formals.cc` | `DepPrecisionTest` | Formals matching: SC dep for each required formal attr |
| `dep-precision/formals-comprehensive.cc` | `DepPrecisionTest` | Formals: optional formals, ellipsis, no-match, nested call |
| `dep-precision/formals-cache.cc` | `DepPrecisionTest` | Formals SC dep cache hit/miss across warm verify |
| `dep-precision/attrkeys.cc` | `DepPrecisionTest` | attrNames on traced data: #keys dep, unrelated key change = cache hit |
| `dep-precision/attrkeys-multi.cc` | `DepPrecisionTest` | attrNames across multiple sources, union/intersection patterns |
| `dep-precision/hasattr.cc` | `DepPrecisionTest` | hasAttr: #has:key dep per probed key, absence/presence precision |
| `dep-precision/hasattr-multi.cc` | `DepPrecisionTest` | hasAttr on multiple sources, dep aggregation |
| `dep-precision/hasattr-nix-added.cc` | `DepPrecisionTest` | hasAttr on nix-added keys (not from structured data), no spurious SC dep |
| `dep-precision/listlen.cc` | `DepPrecisionTest` | builtins.length on traced lists: #len dep, unrelated element change = cache hit |
| `dep-precision/listlen-builtins.cc` | `DepPrecisionTest` | length via map/filter/concatMap/flatten: #len dep propagation |
| `dep-precision/preserve.cc` | `DepPrecisionTest` | Dep preservation across Nix operators (//,  ++, map, builtins.seq) |
| `dep-precision/edge-cases.cc` | `DepPrecisionTest` | Edge cases: empty attrset, null-byte paths, non-string keys |
| `dep-precision/pipelines.cc` | `DepPrecisionTest` | Dep precision through evaluation pipelines (lib.attrByPath, lib.getAttrFromPath) |
| `dep-precision/builtins-extra.cc` | `DepPrecisionTest` | Extra builtins: toJSON, toPath, fromJSON(toJSON(x)), concatStringsSep |
| `dep-precision/nixpkgs-patterns.cc` | `DepPrecisionTest` | Nixpkgs access patterns: callPackage-style, overlay application, aliases.nix pattern |
| `dep-precision/function-application.cc` | `DepPrecisionTest` | Function application over traced data: lambda consuming traced attrs, map with predicate |
| `dep-precision/language-features.cc` | `DepPrecisionTest` | Language features: with, inherit, rec attrsets, string interpolation, let-in |
| `dep-precision/dirset-aggregation.cc` | `DepPrecisionTest` | Fix 2: aggregated directory has-key-miss deps across merged readDir origins |
| `dep-precision/haskey-conflict.cc` | `DepPrecisionTest` | hasAttr conflict dep recording: contradictory absence/presence observations |

### traced-data/materialization/

| File | Fixture | Description |
|------|---------|-------------|
| `materialization/single-scope.cc` | `MaterializationDepTest` | Single-scope baseline: shape deps recorded within one DepCaptureScope |
| `materialization/cross-scope-attrkeys.cc` | `MaterializationDepTest` | Cross-scope #keys: attrNames on TracedData child from sibling scope |
| `materialization/cross-scope-haskey.cc` | `MaterializationDepTest` | Cross-scope #has:key: hasAttr across TracedExpr boundary |
| `materialization/cross-scope-typeof.cc` | `MaterializationDepTest` | Cross-scope #type: typeOf across TracedExpr boundary |
| `materialization/cross-scope-cache.cc` | `MaterializationDepTest` | End-to-end cross-scope cache hit/miss cycles, sibling independence |
| `materialization/variant-roundtrip.cc` | `MaterializationDepTest` | attrs_t serialization roundtrip: origin fields survive record/verify |
| `materialization/nested-diagnostic.cc` | `MaterializationDepTest` | Nested cross-scope diagnostic: exact dep dumps for nested TracedExpr |
| `materialization/nested-invalidation.cc` | `MaterializationDepTest` | Nested invalidation propagation through cross-scope materialization boundaries |
| `materialization/multi-source.cc` | `MaterializationDepTest` | Multi-source TracedData: JSON+TOML merge, readDir→fromJSON chain, two JSON files |
| `materialization/standalone-verify.cc` | `MaterializationDepTest` | Standalone dep verification: child traces with only ImplicitShape/SC deps |
| `materialization/error-recovery.cc` | `MaterializationDepTest` | Error paths: tryEval on traced data, recovery after A→B→A revert, simultaneous changes |
| `materialization/pointer-equality.cc` | `TracedDataTest` | Pointer equality for TracedExpr across cache hits: attrs-with-function, list-with-function |

### nix-binding/

| File | Fixture | Description |
|------|---------|-------------|
| `nix-binding/basic.cc` | `NixBindingBasicTest` (DepPrecisionTest) | SC dep recorded on `.attr` access, NixBinding format "n", attrset body eligibility |
| `nix-binding/precision.cc` | `DepPrecisionTest` | NixBinding precision: comment change = SC override = cache hit |
| `nix-binding/override.cc` | `NixBindingOverrideTest` (MaterializationDepTest) | SC override end-to-end: content dep fails, SC passes → cache hit stored |
| `nix-binding/edge-cases.cc` | `NixBindingEdgeCaseTest` (DepPrecisionTest) | Edge cases: ineligible bodies (ExprCall, ExprWith), let-in, rec attrset |

### verify/

| File | Fixture | Description |
|------|---------|-------------|
| `verify/integration.cc` | `TraceCacheIntegrationTest` (TraceCacheFixture) | Full pipeline: evaluator→recording→store→verification→result; flake graph, SemanticRegistry mounts |
| `verify/derivation.cc` | `DerivationIntegrationTest` (TraceCacheFixture) | `builtins.toFile` / `builtins.storePath` / IFD recording + warm-verify; includes `BuiltinsToFile_ContentChange_Invalidation` (OR-1 regression reference) |
| `verify/fetch-tree.cc` | `FetchTreeIntegrationTest` (TraceCacheFixture) | `builtins.fetchTree` / `fetchTarball` trace recording + subdir navigation and invalidation |
| `verify/path-coercion.cc` | `PathCoercionIntegrationTest` (TraceCacheFixture) | Post-OR-3 path-coercion integration: `${./dir}`, dirOf merge-semantic handles, detach-mode warm hits |

## Section 4: Test Naming Conventions

Tests use `TEST_F(FixtureName, TestName)` for fixture-based tests, or
`TEST(SuiteName, TestName)` for standalone tests (no fixture state needed).

### Standard pattern

```cpp
TEST_F(FixtureName, Category_Scenario_ExpectedOutcome)
```

Examples from the codebase:
- `TEST_F(DepTrackingTest, ContentDep_ReadFile_WarmHit)` — category=ContentDep, scenario=ReadFile, outcome=WarmHit
- `TEST_F(DepTrackingTest, ContentDep_ReadFile_Invalidation)` — same category/scenario, outcome=Invalidation
- `TEST_F(TracedDataTest, TracedJSON_UnusedKeyChange)` — category=TracedJSON, scenario=UnusedKeyChange

### ColdWarm pattern

`ColdWarm_*` — cold eval (records trace) followed immediately by warm verify (should hit cache):

```cpp
TEST_F(TraceCacheTest, ColdWarm_String)
TEST_F(TraceCacheTest, ColdWarm_Int)
```

Use this pattern when the primary goal is to confirm the basic record→verify cycle works for a given type or expression.

### Cache hit / miss suffixes

`*_CacheHit` — asserts `loaderCalls == 0` (served from cache)
`*_CacheMiss` — asserts `loaderCalls == 1` (re-evaluated)

### Invalidation suffixes

`*_Invalidation` / `*_Invalidates` — asserts that changing a dep causes re-evaluation.

### DISABLED_ prefix

`DISABLED_TestName` — known limitation or flaky test. Always document WHY in a comment immediately above the test. Before reaching for `DISABLED_`, empirically check whether the test actually fails under the current code; two eval-trace tests were previously DISABLED with claims about soundness bugs that did not hold under the actual cache contract (see DEF-6 for the 2026-04-30 reclassification). Example:

```cpp
// Flaky under shared-fixture RapidCheck harness: can fail the
// pre-mutation warm-hit assertion before any mutation occurs.
// Deterministic coverage lives in traced-data/dep-precision/hasattr.cc.
TEST_F(EvalTraceProperty_HasAttr, DISABLED_RemoveKey_Invalidates)
```

### BUG-N references

When a test guards against a specific bug regression, name it with a `BUG-N` suffix or add a comment:

```cpp
// BUG-10 regression guard: float precision with std::to_chars vs std::to_string
TEST_F(TraceStoreTest, FloatEncoding_PiRoundTrip)
```

## Section 5: Test Categories

### Soundness tests

Verify that no stale results are served. The negative case: change a dep → assert cache miss.

Pattern: record a trace, verify it hits cache, modify the dep, verify it now re-evaluates.

```cpp
auto cache = makeCache(expr);        // cold: record trace
forceRoot(*cache);

int calls = 0;
cache = makeCache(expr, &calls);     // warm: verify hit
forceRoot(*cache);
EXPECT_EQ(calls, 0);                 // soundness pre-condition

modify_the_dep();
invalidateFileCache(path);

calls = 0;
cache = makeCache(expr, &calls);     // warm: verify should now fail
forceRoot(*cache);
EXPECT_EQ(calls, 1);                 // soundness: re-evaluated
```

Always pair soundness with a precision pre-condition (confirm cache hit before the mutation).

### Precision tests

Verify that valid cache hits are preserved when an unrelated dep changes.

Pattern: record a trace, change something the trace does NOT depend on, assert cache hit.

```cpp
// change_unrelated_thing() — modify something outside dep set
EXPECT_EQ(calls, 0);  // precision: cache hit despite unrelated change
```

### Regression tests

Guard against specific bugs identified by a BUG-N number. Reference the bug number in the test name or in a comment. These tests fail under the old code and pass under the fix.

### Integration tests

Exercise the full pipeline: Nix evaluator → dep recording → TraceStore → verification → result serving. Use `TraceCacheFixture` or `TraceCacheIntegrationTest`. Produce real `Value` objects (not just synthetic deps).

### Unit tests

Test a single function or method in isolation. Use standalone `TEST()` (no fixture) or a minimal fixture (`::testing::Test`). Typical: `DepHashBasicTest`, `DepFormatTest`, `DepRecordingContextTest`.

### Protocol tests

Bundle 4 removed the old session-typed verification protocol tests. Validation
now happens through the real verification/recovery store tests instead.

## Section 6: Writing New Tests

### Step 1: Choose the right fixture

Decision tree:

```
Need a live Nix evaluator?
  No  → TEST() with no fixture (or ::testing::Test subclass)
  Yes → need full TraceSession (makeCache/forceRoot)?
    No  → need raw TraceStore (record/verify/recovery)?
      Yes → TraceStoreFixture → TraceStoreTest
      No  → EvalTraceTest (withBs, eval())
    Yes → need dep collection (evalAndCollectDeps)?
      No  → TraceCacheFixture (or TracedDataTest for JSON/TOML/readDir)
      Yes → need cross-session store queries (getStoredDeps)?
        No  → DepPrecisionTest
        Yes → MaterializationDepTest
```

### Step 2: Choose the right directory

- Testing dep recording primitives (DepRecordingContext, InterningPools, epoch log)? → `dep/`
- Testing oracle dep end-to-end (readFile, getEnv, pathExists, etc.)? → `dep/oracle-*.cc`
- Testing TraceStore methods (record, verify, recovery, serialization)? → `store/`
- Testing JSON/TOML/readDir structured data materialization? → `traced-data/`
- Testing per-builtin dep precision (which deps are recorded)? → `traced-data/dep-precision/`
- Testing cross-TracedExpr materialization boundaries? → `traced-data/materialization/`
- Testing NixBinding SC dep recording or override? → `nix-binding/`
- Full end-to-end pipeline with real eval? → `verify/`

### Step 3: Follow the naming convention

```cpp
TEST_F(FixtureName, Category_Scenario_ExpectedOutcome)
```

For soundness tests, always cover BOTH directions:
- `Category_Scenario_CacheHit` — precision pre-condition
- `Category_Scenario_Invalidation` — soundness assertion

### Step 4: Add to meson.build

Add the new file to `src/libexpr-tests/meson.build` in the `sources = files(...)` block, in the correct comment section (e.g., `# eval-trace/dep-precision/`).

### Step 5: git add -f if needed

`nix build` filters files through git. Newly created test files must be staged:

```bash
git add -f src/libexpr-tests/eval-trace/your-new-test.cc
```

Test fixture output files (e.g., files matching `result-*`) may also need `git add -f` if they match `.gitignore` patterns.

### Step 6: Soundness test structure

Always test both sides:

```cpp
// 1. Cold eval — records trace
{ auto cache = makeCache(expr); forceRoot(*cache); }

// 2. Warm verify — confirm cache hit (precision pre-condition)
{ int c = 0; auto cache = makeCache(expr, &c); forceRoot(*cache); EXPECT_EQ(c, 0); }

// 3. Mutate dep — change the oracle value
dep.modify("new-content");
invalidateFileCache(dep.path);

// 4. Warm verify — confirm cache miss (soundness assertion)
{ int c = 0; auto cache = makeCache(expr, &c); EXPECT_EQ(c, 1); }
```

### Step 7: Cross-session tests

Use `recreateDb(db)` (not `db = makeDb()`) to simulate a new evaluation session:

```cpp
auto db = makeDb();
withExclusiveStore(*db, [&](const auto & ea) {
    db->record(ea, rootPath(), string_t{"result_A", {}}, depsA);
});
setenv("VAR", "value_B", 1);
withExclusiveStore(*db, [&](const auto & ea) {
    db->record(ea, rootPath(), string_t{"result_B", {}}, depsB);
});
setenv("VAR", "value_A", 1);
recreateDb(db);  // flush + reopen: simulates new process
auto result = TraceStorageTestAccess::verify(*db, rootPath(), state);
```

## Section 7: Test Helpers Reference

### Test access structs (in helpers.hh)

**`TraceStorageTestAccess`** — friend access to private TraceStore methods:
- `encodeCachedResult(store, value)` / `decodeCachedResult(store, payload)` — codec round-trip
- `verifyTrace(store, traceId, registry, state, session)` — calls public `store.verifyTrace`
- `recovery(store, oldTraceId, pathId, registry, state, session)` — calls public `store.recovery`
- `verify(store, pathId, registry, state)` — convenience: runs orchestrator's inline verify flow
- (Test helpers for the old `verifiedGitRepos_` path were removed in the
  `governingRepoId` refactor.  Tests now drive coverage by attaching a
  repo root to file-content deps via `makeContentDepInRepo` or the
  `governingRepoRoot` parameter of `makeSimpleRecordedDep`, and by
  recording a `GitRevisionIdentity` dep alongside.)
- All methods have overloads that take empty `SemanticRegistry` (pass no registry argument)

Internally `TraceStorageTestAccess` inherits `Certifier<BlockingTag>` and
chains `Certifier::withProof` into `store.withExclusiveAccess` to mint
`ea`. Every verification-path helper is wrapped in
`withExclusiveStore`-equivalent scaffolding, so tests cannot call a
public TraceStore method without first acquiring exclusive access.

**`TestScopeAccess`** — GDP-certifier access to DepRecordingContext private scope methods:
- `pushScope(ctx)` / `popScope(ctx)` / `takeDeps(ctx)` — direct scope manipulation for unit tests

**`SemanticRegistryTestAccess`** — access to SemanticRegistry mutation (in `semantic-registry-test-access.hh`):
- `addEntry(registry, source, path)` / `addMountPoint(registry, path, source, subdir)` — populate registry for tests

### Dep factory helpers (in helpers.hh)

- `makeContentDep(pools, key, content)` — FileBytes dep with `depHash(content)`
- `makeEnvVarDep(pools, key, value)` — EnvironmentLookup dep
- `makeExistenceDep(pools, key, exists)` — ExistenceCheck dep
- `makeSystemDep(pools, system)` — SessionSystemValue dep
- `makeCurrentTimeDep(pools)` — VolatileTime dep with hash `"volatile"`
- `makeExecDep(pools)` — VolatileExec dep
- `makeCopiedPathDep(pools, sourcePath, storeName, storePath)` — DerivedStorePath dep
- `makeNARContentDep(pools, key, hash)` — NarIdentity dep
- `makeDirectoryDep(pools, key, hash)` — DirectoryEntries dep
- `makeGitIdentityDep(pools, repoPath, fingerprint)` — GitRevisionIdentity dep
- `makeSimpleRecordedDep(pools, type, source, key, hash)` — generic simple dep
- `makeStructuredDepForTest(pools, type, source, filePath, format, dataPath, hash, ...)` — StructuredProjection dep

### Temporary file/dir helpers (in helpers.hh)

- `TempTestFile(content)` — RAII `.nix` file in `/tmp/nix-test-eval-trace/`; `modify(newContent)`
- `TempJsonFile(content)` — same with `.json` extension
- `TempTomlFile(content)` — same with `.toml` extension
- `TempTextFile(content)` — same with `.txt` extension
- `TempExtFile(ext, content)` — arbitrary extension
- `TempDir()` — RAII directory; `addFile()`, `addSubdir()`, `addSymlink()`, `removeEntry()`, `changeToSymlink()`, `changeToSubdir()`

### Cache/scope helpers

- `INVALIDATE_DIR(td)` — macro: clears FS accessor lstat cache + file content hash cache for a TempDir
- `invalidateFileCache(path)` — method on TraceCacheFixture: clears FS accessor lstat cache + file content hash cache
- `ScopedEnvVar(key, value)` — RAII env var setter; restores original value on destruction
- `ScopedCacheDir` — RAII temp cache dir that sets `NIX_CACHE_HOME`

### Assertion helpers (defined in libexpr test support)

- `IsIntEq(n)` — gmock matcher: `nInt` type with value `n`
- `IsStringEq(s)` — gmock matcher: `nString` with value `s`
- `IsString()` — gmock matcher: any `nString`
- `IsTrue()` / `IsFalse()` — gmock matcher: `nBool` true/false
- `IsAttrsOfSize(n)` — gmock matcher: `nAttrs` with `n` bindings
- `assertCachedResultEquals(a, b, symbols)` — deep comparison for all CachedResult variant types

### Attr path helpers

- `vocabPath(vocab, {parts...})` — builds an `AttrPathId` from string components
- `makePath({parts...})` — DEPRECATED: null-byte-separated string (use `vocabPath` instead)
- `vpath({parts...})` — method on `TraceStoreFixture`: delegates to `vocabPath`

## Section 8: Blocked Tests and Infrastructure Gaps

Tests that are implemented but skipped at runtime (`GTEST_SKIP()`) or use
weaker assertions (`EXPECT_NO_THROW`) due to missing infrastructure. Each
entry lists the blocker and what's needed to unblock it.

### Skipped tests (GTEST_SKIP)

Resolved and now enabled:

- `Concurrency_ParallelAttrs_NoDataRace` (`store/concurrency.cc`) — enabled
  after the `ExclusiveTraceStoreAccess` capability refactor. Each thread
  mints its own `bs` via `withBs` and then acquires the store-wide
  `storeMutex_` through `store.withExclusiveAccess(bs, ...)`. The mutex
  serializes the N concurrent record/verify calls; the old failure mode
  (threads bypassing serialization by calling `withBs` alone) is no
  longer reachable because the public API now requires `ea`, not `bs`.
  A companion debug-build test `ReEntrantWithExclusiveAccess_Asserts`
  (wrapped in `#ifndef NDEBUG`) guards the `thread_local` re-entrancy
  detector in `trace-store-lifecycle.cc`. In release builds the
  detector is compiled out — re-entry deadlocks directly on the
  non-recursive `std::mutex`; that deadlock path is NOT exercised by
  any test (a release-mode death-test with timeout would be flaky) and
  is understood as the fallback when NDEBUG is defined. The primary
  guard is the debug-build assert; the mutex flip is the secondary
  defense.

| Test | File | Blocker | Status |
|------|------|---------|--------|
| `ConcurrentUpserts_ParallelWrites_NoCorruption` | `store/concurrency.cc` | No subprocess harness | Needs a `fork()`-based test helper that spawns N child processes each writing to the same DB, then parent verifies all rows present.  Exercises SQLite WAL concurrent-writer locking. |
| `IFD_WithTracing_WarmHit` | `verify/derivation.cc` | No real builder | IFD with real `Worker` interaction is architecturally impossible in unit tests (fixture uses `dummy://`).  Functional tests cover IFD end-to-end; `DerivedStorePath_TabDelimitedKey_WarmHit` covers the dep round-trip.  A synthetic-output unit variant would add coverage precision, not completeness.  Remains `GTEST_SKIP`'d. |
| `IndependentExpressions_NoCorruption` | `property/special/concurrent-eval.cc` | No per-thread EvalState fixture | `TraceCacheFixture` shares a single `EvalState` (non-thread-safe GC heap, symbol table, `activeSession_`). Needs a fixture variant that constructs one `EvalState` per worker thread and drives each through its own `makeCache`. |

A previous stale-schema-migration test (`store/schema-migration.cc`
/ `SchemaMigration_StaleDbFile_DoesNotCrash`) was removed 2026-04-22.
It exercised semantics production doesn't provide: production uses
filename-versioning (`eval-trace-v<kSchemaEpoch>-<hash-algorithm>.sqlite`),
not `PRAGMA user_version`, and `tests/functional/flakes/eval-trace-soundness.sh`
Test 2 already covers filename coexistence.  Reintroducing would
require first deciding to add a `user_version` check to production.

`BuiltinsStorePath_ContentUnchanged_WarmHit` (previously skipped
via `GTEST_SKIP`) was enabled 2026-04-22 using `state.store->addToStore`
directly on the write-enabled `dummy://` store — no new helper
needed.  See the test at `verify/derivation.cc`.

### DISABLED_ tests

No DISABLED tests remain in `SourceTreeSoundnessTest` as of 2026-04-30.
Both previously-DISABLED tests were re-classified after empirical
verification:

- **Formerly `DISABLED_ParentChild_ChildInheritsStaleStorePath`,
  now `ParentChild_PerLeafLazyVerification` (enabled).** The
  original test asserted `EXPECT_EQ(loaderCalls, 1)` AFTER
  `forceRoot` and framed the failure (`loaderCalls==0`) as a
  soundness bug. Empirical check showed the cache's actual
  behavior is correct under the project's hard constraint on zero
  precision/performance loss: each leaf's trace is verified when
  forced; siblings with unaffected deps continue to hit. The
  "forceRoot must re-eval" assertion over-specified the contract;
  satisfying it would require either hoisting child source-copy
  deps to the root (precision loss: root invalidation cascades to
  all siblings via ParentSlot) or eagerly forcing every child on
  root force (performance loss: defeats laziness). The test has
  been reformulated to pin the per-leaf-lazy contract explicitly:
  forceRoot warm-hits (`loaderCalls==0`); forcing srcPath
  re-evaluates and increments loaderCalls; childA picks up the
  mutation; childB (whose FileBytes dep is on an unchanged file)
  continues to serve the cached value — precision preserved. See
  the production CLAUDE.md §OR-3 entry for the full rationale.

- **Formerly `DISABLED_BenchmarkRepro_SecondChildServesStaleValue`,
  now `SharedThunk_Siblings_TraceValueContext_InvalidatesBoth`
  (enabled).** This test passes under the current cache behavior:
  cold recording populates `epochMap` for the shared thunk's
  Value; the second sibling's force fires
  `SiblingReplayCaptureScope::maybeCapture` which records a
  `TraceValueContext` dep linking to the first sibling; mutating
  the shared file invalidates the first sibling's trace and the
  recursive `resolveTraceContextHash` check catches it for the
  second sibling. Both siblings correctly invalidate. This is
  NOT the nixpkgs benchmark reproducer (the benchmark uses
  subprocess-per-commit eval with an empty `fileTraceCache` and
  still stale-serves, implying a different root cause); it is
  enabled as a regression guard for the shared-thunk replay
  machinery.

- Historical: `DISABLED_GitIdentitySkip_CounterIncrements` was a
  wrong-direction counter-instrumentation test (asserted skip fires
  with matching content hash, but the skip site is only reached when
  content mutates).  A replacement test named
  `GitIdentitySkip_CounterIncrements_FileMutated` was reported as
  landed in a file `store/git-identity-skip.cc`, but neither the test
  nor the file nor the referenced `nrGitIdentitySkips` counter is
  present in the current tree.  See DEF-8 (resolution artifact absent).

### Weakened assertions (EXPECT_NO_THROW instead of EXPECT_TRUE/FALSE)

The following tests have been strengthened to use `EXPECT_TRUE(result.has_value())`:

- `DerivedStorePath_TabDelimitedKey_WarmHit` (`store/derived-store-path.cc`) — fixed by using a real `TempTestFile` and `computeStorePath` to derive the actual store path string.
- `StorePathAvailability_RecordVerify_WarmHit` (`store/store-path-availability.cc`) — fixed by pre-populating the write-enabled dummy store via `addToStore`, recording `"valid"` string hash.
- `StorePathAvailability_TabDelimitedKey_RoundTrip` (`store/store-path-availability.cc`) — same approach as above.
- `AllCQK_SingleTrace_WarmHit` (`store/record-verify.cc`) — fixed by using correct `DepHashValue` variant types for every CQK (string for store-query deps, EvalTraceHash for content/env/system deps), real files, and a pre-populated store. Volatile deps (VolatileExec, VolatileTime) and RuntimeFetchIdentity are excluded.
- `RuntimeFetchIdentity_RecordVerify_WarmHit` and `RuntimeFetchIdentity_HashChange_WarmMiss` (`store/runtime-fetch-identity.cc`) — strengthened 2026-04-22 (DEF-2) via a locked `path:` input built with `fetchers::Input::fromURL` + `fetchToStore`.  Warm-hit asserts `EXPECT_TRUE(result.has_value())`; hash-change asserts `EXPECT_FALSE(result.has_value())`.
- `Recovery_MultiRepo_StaleGitHash_FileMatches_ValidViaContentDep` and `Recovery_GitIdentity_StaleHash_FileMatches_ValidViaContentDep` (`store/recovery.cc`) — strengthened 2026-04-22 (DEF-1) from `EXPECT_NO_THROW` to `ASSERT_TRUE(result.has_value()) + EXPECT_EQ(value, "v1")`.  Both are genuine HIT scenarios — GitRevisionIdentity mismatch is a short-circuit miss (not a hard failure), so FileBytes match alone is sufficient.
- `Recovery_DirectHash_Miss_StructVariantSameGroup` (`store/recovery.cc`) — strengthened 2026-04-22 (DEF-1) to `EXPECT_FALSE(result.has_value())`.  Also fixed a pre-existing `/tmp` symlink bug.

Remaining weakened tests: none currently tracked.

### Production code changes covered by tests in this directory

| Change | File | Test coverage |
|--------|------|---------------|
| Cycle detection in `verifyTrace`: `VerificationSession::inProgressTraceIds` (`boost::unordered_flat_set<TraceId>`) inserted on entry and RAII-erased on exit; returns `false` (not crash) when a cycle is detected via `TraceValueContext`/`TraceParentSlot` deps pointing back to a trace already on the call stack. | `src/libexpr/eval-trace/store/verifier.cc`, `src/libexpr/include/nix/expr/eval-trace/store/verification-session.hh` | `store/trace-value-context.cc`: `TraceValueContext_HardcodedHashCycle_MissOnMismatch` exercises hash-mismatch cycle paths. A real two-pass setup would be required to test the `inProgressTraceIds` cycle-break code path directly. |

### Infrastructure helpers — status

Built and in use:

| Helper | Purpose |
|--------|---------|
| `TempGitRepo` | RAII git repo: `init()`, `addFile()`, `commit()`, `headHash()`, `dirtyModify()`.  B-1, B-2, C-1, C-2 tests (currently still use synthetic repo paths for cases where a real repo adds no value). |
| `PathCountersSnapshot` | Scoped baseline/delta reads against diagnostic counters. |

Historical DEF-7 items (retired 2026-04-22): `BrokenStoreTest`,
`buildFixtureDrv`, `writeStaleSchemaDb`, `makeDbAtPath`.  See the
DEF-7 entry in §D for the retirement rationale (each either
landed without a helper, duplicated coverage already provided by
functional tests, was architecturally impossible in unit tests,
or tested production semantics that don't exist).

### Property-based testing

Property tests live under `eval-trace/property/`. Top-level infrastructure
sits directly under `property/` (see `property/expr-gen.cc` for the
`DepSlot` method bodies, shared helpers, and the top-level
`makeNixExprGen()` entry point; `property/generator-test.cc` holds
well-formedness smoke tests over each generator — 8 `TEST()` cases
under `EvalTraceProperty_Generator` covering ScalarGen / ReadFileGen /
GetEnvGen / FromJSONGen / AttrAccessGen / PathExistsGen /
MultiSourceAttrGen / ListFromJSONGen). Invariants live under
`property/invariant/`, generators under `property/gen/`, coverage tests
under `property/coverage/`, and composition tests under
`property/composition/`. All property test files are registered in
`meson.build` and run in CI.

RapidCheck's `rc::Arbitrary<TestExpr>` routes through `makeNixExprGen()`
(defined in `property/expr-gen.cc`). Generator coverage includes
FromJSONGen, AttrAccessGen, PathExistsGen, and the nine nixpkgs-fidelity
patterns (import-tree, overlay, rec-attrset, call-package, list-pipeline,
attrset-pipeline, three-source-pipeline, fold-merge-pipeline,
function-chain) in `property/gen/*.cc`.

All four properties are implemented:

- **Soundness** (P1): `∀E: eval(E) == cached_eval(E)` — `property/invariant/soundness.cc`
- **Invalidation** (P2): `∀E, dep ∈ deps(E): mutate(dep) → cached_eval(E) misses` — `property/invariant/invalidation.cc`
- **Precision** (P3): `∀E, f ∉ deps(E): mutate(f) → cached_eval(E) still hits` — `property/invariant/precision.cc`
- **Recovery** (P5): `∀E, v1, v2: record(v1), record(v2), revert(v1) → recover(v1)` — `property/invariant/recovery.cc`
- **Idempotence** (P4): `∀E: cached_eval(cached_eval(E)) == cached_eval(E)` — `property/invariant/idempotence.cc`

Additional property invariants present (not in the P1-P5 taxonomy): `commutativity.cc`, `cross-session-soundness.cc`, `determinism.cc`, `dep-correctness.cc`, `monotonicity.cc`, `structural-override.cc`, `trace-hash-determinism.cc`, `trace-transparency.cc`.

## Section D: Deferred Work Index

Consolidated index of test/infrastructure/production work that was
intentionally deferred rather than completed. One grep target
(`grep -r "Deferred Work Index"`) surfaces every item a future
session should reconsider before starting new work on this subsystem.

Each entry points at where the deferral is documented in full. If
you are resolving one of these, update BOTH the entry here AND the
authoritative source.

### DEF-1: `EXPECT_NO_THROW` in recovery tests — RESOLVED 2026-04-22

- **Where documented:** §N.6 in this file.
- **Tests in `store/recovery.cc`** (renamed in this pass):
  - `Recovery_MultiRepo_StaleGitHash_FileMatches_ValidViaContentDep`
    (was `Recovery_MultiRepo_OneChanges_NoThrow`)
  - `Recovery_GitIdentity_StaleHash_FileMatches_ValidViaContentDep`
    (was `Recovery_GitIdentity_To_DirectHash_Fallthrough`)
  - `Recovery_DirectHash_Miss_StructVariantSameGroup`
    (was `Recovery_DirectHash_To_StructVariant_WithTraceContext_NoThrow`)
- **Actual resolution (2026-04-22):** Two of the three tests turned
  out to be **genuine HIT** cases, not miss cases.  The first
  round-4 audit (2026-04-21) and the earlier §N.6 "implementation-
  dependent" framing (2026-04-20) were both wrong — but in
  different directions.  The right analysis:
  - Tests 1 & 2 (MultiRepo, GitIdentity_To_DirectHash): `GitRevisionIdentity`
    is `QueryBehavior::ImplicitStructural`, so a mismatch in pass-1
    is treated as a short-circuit miss, not a hard failure.
    `recordGitIdentity(matched=false)` does NOT set
    `hasNonContentFailure`.  When the FileBytes dep matches (content
    unchanged), `determineOutcome` returns `Valid` — the trace is
    served correctly via the content dep.  The fixture intent was
    "trace survives a stale git hash when content is unchanged",
    which is working as designed.  Strengthened to
    `ASSERT_TRUE(result.has_value())` + `EXPECT_EQ(value, "v1")`.
  - Test 3 (DirectHash_StructVariant): file content actually changes
    v1 → v2 between record and verify, so FileBytes fails.  No
    `GitRevisionIdentity` dep, so `allDepsGitRecoverable` is false.
    StructVariant same-struct-group skip rejects the single stored
    group.  Verdict is deterministically `std::nullopt`.  Fixed the
    pre-existing `/tmp` symlink bug that the weak assertion had
    been masking (`std::filesystem::canonical(f.path).string()`).
    Strengthened to `EXPECT_FALSE(result.has_value())`.
- **§N.6 has been updated** to reflect the actual per-test
  mechanism.

### DEF-2: RuntimeFetchIdentity tests — RESOLVED 2026-04-22

- **Where documented:** §8 "Weakened assertions" table and §N.6 in
  this file.
- **Tests in `store/runtime-fetch-identity.cc`** (renamed in this pass):
  - `RuntimeFetchIdentity_RecordVerify_WarmHit`
    (was `RuntimeFetchIdentity_RecordVerify_NoThrow`)
  - `RuntimeFetchIdentity_HashChange_WarmMiss`
    (was `RuntimeFetchIdentity_HashChange_NoThrow`)
- **Resolution (2026-04-22):** Rewrote both tests to use a LOCKED
  `path:` input obtained via `fetchers::Input::fromURL(...)` +
  `fetchToStore(...)` — exactly the pattern already used by
  `store/session-config.cc::TraceStore_RuntimeRootPersistence_RoundTripsTypedRows`.
  The LOCKED input has a `narHash` attr, so
  `Input::computeStorePath` inside the verifier succeeds and returns
  a deterministic store-path string; the recorded-vs-current hash
  comparison then drives the test outcome.  Warm-hit test stores
  the expected store-path string and expects verify to return a
  value; hash-change test stores a deliberately-wrong hash and
  expects verify to return `std::nullopt`.  Local helper
  `makeLockedRuntimeFetchDep` in the test file factors the setup.
  Used `std::filesystem::canonical(tempDir.path())` to avoid
  macOS `/tmp → /private/tmp` symlink rejection.
- **Framing correction retained:** The earlier "libfetchers not
  linked" framing was wrong; `libfetchers` IS linked transitively
  via `nix-expr.pc`'s public `Requires:` field (confirmed via
  `otool -L`).  `store/session-config.cc` already calls the same
  fetcher APIs.  Not a linkage issue, so no meson.build change was
  needed.

### DEF-3: Top-level vs nested SC-dep asymmetry — RESOLVED 2026-04-22

- **Previously documented in:** inline TODO on
  `CrossScope_AttrNames_ValueChanged_TopLevelReEval_SymmetryGap`
  (now renamed; see below).
- **Root cause:** **Test-shape asymmetry, not a production bug.**
  The SC #keys dep is recorded symmetrically on the `names` TracedExpr
  child in both cases and resolves to the same hash via
  `resolveShapeSuffix(ShapeSuffix::Keys)` in
  `dep-resolution-service.cc`.  Root/NAMES both HIT warm verify.
  What re-fired the loader was the **WARM block iterating list
  elements** (`names.0`, `names.1`) that the COLD block never
  forced — those child thunks (created by
  `TracedExpr::materializeResult` `list_t` branch in
  `cache/materialize.cc`) had no `CurrentNode` row.
  `verifyAttrImpl` returned `nullopt` → `TracedExpr::eval` fell
  through to `evaluateFresh` → `RootHandle::getRealRoot` →
  rootLoader fired once.  Neither OR-3 nor OR-4 class.
- **Resolution (2026-04-22):** Applied option A.  Removed the
  warm-block element-iteration loop + the per-child
  `getStoredResult("names.<i>")` assertions.  Flipped assertion
  to `EXPECT_EQ(loaderCalls, 0)`.  Renamed test to
  `CrossScope_AttrNames_ValueChanged_TopLevelCacheHit` — now
  structurally matches `NestedValueChange_AttrNames_CacheHit` in
  `nested-invalidation.cc`.

### DEF-4: OR-1 test-level reproducer gone (fixture artifact closed)

- **Where documented:** `src/libexpr/eval-trace/CLAUDE.md` §OR-1.
- **Status:** The pre-§N.1 stale-serve reproducer in
  `verify/derivation.cc::BuiltinsToFile_ContentChange_Invalidation`
  relied on the fixed-fingerprint fixture artifact. §N.1 closed
  that path; the test now asserts `calls == 1`.
- **Production reachability (re-audited 2026-04-30).** Not
  demonstrated. The `ExprParseFile::eval` backstop at
  eval.cc:1432-1434 records `FileBytes(source_file)` whenever a
  cold TracedExpr is evaluating a `.nix` file. An edit to the
  containing file invalidates that FileBytes → fresh eval → new
  `toFile` output. A construct that bypasses the backstop would
  need a trace with zero FileBytes deps on the source; not
  constructible in the current code.
- **Unblock requires:** a demonstrated reachability case beyond
  the retired fixture artifact. Not gated on any pending OR-3
  architectural change — both OR-1 and OR-4 are shielded by the
  same FileBytes backstop in practice.

### DEF-5: Production open-research items (OR-N series)

- **Where documented:** `src/libexpr/eval-trace/CLAUDE.md`
  "Open research: known soundness and precision gaps" section
  (OR-1..OR-11).
- **Retired (moved to "Closure notes: resolved issues retained as
  guidance" in the production CLAUDE.md):** OR-2 (bare-import
  FileBytes backstop — closed by the minimal `else`-removal fix
  at `ExprParseFile::eval` and `scopedImport`; pins live in
  `oracle-nested.cc::BareImport_*`,
  `verify/integration.cc::MapAttrs_ValueChange_CacheMiss`, and
  Tests 6-8 of `tests/functional/eval-trace-impure-soundness.sh`),
  OR-5 (stableRecoveryKey policy-digest — fixed in commit
  following `90e908ad6`; unit-level reproducer added 2026-04-20,
  see §N.7 in this file), OR-6 (pass-2 override patch; working
  as designed), OR-8 (bootstrap fingerprint + missing
  SessionConfig — closed by `BackendParams` fusion), OR-9
  (runtime-gap thread-locals — closed 2026-04-22 via RAII-only
  scope types; see the "RAII-only thread-local scope types"
  Closure note in the production CLAUDE.md), OR-10
  (weak-assertion audit — all items resolved via Group A / B / C /
  functional closure passes on 2026-04-20).
- **Open architectural (re-audited 2026-04-30):**
  - **OR-1** (prim_toFile precision). Storage-layer gap; shielded
    in real evaluation by the `maybeRecordImportedFileContent`
    FileBytes backstop at eval.cc:1432-1434. Not demonstrably
    reachable.
  - **OR-3** (cache-side scope-attribution concern reframed as
    per-leaf lazy verification; benchmark stale-serve at
    `closures.gnome.x86_64-linux` still unreproduced). Cache-side
    concern closed; benchmark concern open, pending
    subprocess-per-commit reproduction.
  - **OR-4** (TraceParentSlot key-set gap). Storage-layer gap pinned
    by synthetic test; shielded in real evaluation by the same
    FileBytes backstop. Not demonstrably reachable.
  - **OR-7** (epoch-log truncation on exception). Performance-only;
    no benchmark motivates landing.
  - **OR-11** (`path_t` materialization rebuilds with `rootFS`).
    Latent accessor-identity drop on the cache-restore path — same
    bug class as the committed `ExprConcatStrings` and
    `state.storePath` fixes, but requires either a `path_t` schema
    extension or a `SemanticRegistry`-based accessor lookup at
    materialization time.  Not demonstrably reachable today.  See
    DEF-9 for the missing round-trip test.
- **Unblock requires:**
  - OR-3: subprocess-per-commit benchmark reproduction to identify
    the failing shape.
  - OR-1, OR-4: a demonstrated reachability case that bypasses the
    FileBytes backstop. Without one, "fixing" the storage-layer
    gap has no observable user benefit.
  - OR-7: a benchmark showing measurable perf regression from
    exception-path partial-epoch retention.
  - OR-11: a reproducer where a cached-then-restored `path_t` value
    compares unequal in a way observable to nixpkgs-lib code or to a
    downstream `SourcePath::operator==` assertion.

### DEF-6: RESOLVED 2026-04-30 — both tests reclassified, enabled

Both tests were reclassified after empirical verification:

- `ParentChild_PerLeafLazyVerification` (formerly
  `DISABLED_ParentChild_ChildInheritsStaleStorePath`): the previous
  DISABLED framing asserted `loaderCalls==1` after forceRoot and
  called the `==0` result a soundness bug. Under the project's
  hard constraint on zero precision/performance loss, the `==0`
  result is correct: the cache operates per-leaf, each leaf
  invalidates when forced, and siblings with unaffected deps
  serve cached. The test now pins the per-leaf lazy contract
  explicitly. Zero code changes were needed — the cache's
  behavior was correct all along; only the test's assertion and
  the documentation around it were wrong.

- `SharedThunk_Siblings_TraceValueContext_InvalidatesBoth`
  (formerly `DISABLED_BenchmarkRepro_SecondChildServesStaleValue`):
  passes under the current cache. Enabled as a regression guard
  for the `SiblingReplayCaptureScope::maybeCapture` →
  `TraceValueContext` path for shared-thunk sibling consumption.
  NOT the nixpkgs benchmark reproducer — the benchmark uses
  subprocess-per-commit eval and still stale-serves, which implies
  a different root cause.

**Status of OR-3's remaining concern.** The original OR-3 entry
conflated two things: the cache-side scope-attribution concern
(which is closed — per-leaf laziness is correct), and the
benchmark stale-serve itself (which remains open as an
unreproduced specific-leaf dep-recording gap). See
`src/libexpr/eval-trace/CLAUDE.md` §OR-3 for the current framing.
Unblocking the remaining concern requires subprocess-per-commit
dep-graph diff to identify which specific leaf is missing which
specific dep.

### DEF-7: RETIRED 2026-04-22 — infrastructure helpers don't close coverage gaps

Retired as a tracked item.  The 2026-04-22 scoping analysis found
that the four originally-listed helpers either (a) landed without
needing a helper at all, or (b) would improve coverage precision
rather than coverage completeness.  Every currently-uncovered
behavior the deferred helpers would have exercised is either
already covered elsewhere (usually at the functional-test level),
architecturally impossible to exercise in unit tests, or tests
semantics production doesn't actually guarantee.

**Landed 2026-04-22 (no helper needed):**
- ✅ `BuiltinsStorePath_ContentUnchanged_WarmHit` in
  `verify/derivation.cc` — enabled using `state.store->addToStore`
  directly on the write-enabled `dummy://` store, matching the
  pattern in `store/store-path-availability.cc`.

**Retired (justification per item):**

1. **`BrokenStoreTest` fixture** (a `LibExprTest` subclass with a
   Store that throws on `isValidPathUncached`/`queryPathInfoUncached`).
   Cost: ~80-100 LoC of `Store`-virtual boilerplate (subclassing
   `DummyStore` directly requires re-implementing ~10 pure virtuals
   because concrete `DummyStoreImpl` is in an anonymous namespace).
   Benefit: covers the `catch (std::exception &) { return nullopt; }`
   branch in `dep-resolution-service.cc`'s `DerivedStorePath` /
   `RuntimeFetchIdentity` cases.  The sibling branch
   (`!maybeLstat() → sentinel(SentinelHash::Missing)`) is already covered by
   `error-handling.cc`'s three `DepResolution_*_GracefulMiss`
   tests, and both branches have identical observable outcome
   (`resolveCurrentDepHash` returns `nullopt`, trace invalidates).
   Net: ~0 behavioral coverage added, ~1 specific invariant
   ("catch block exists and actually swallows").  Not worth the
   boilerplate cost.

2. **`buildFixtureDrv` (synthetic-output variant)** — pre-populate a
   fixed-output drv via `writeDerivation` + `addToStoreFromDump` +
   `registerDrvOutput` to unblock `IFD_WithTracing_WarmHit`.
   Functional tests already cover IFD end-to-end through
   `nix eval` with real derivations.  The `DerivedStorePath`
   record-verify round-trip is covered by
   `DerivedStorePath_TabDelimitedKey_WarmHit` in
   `store/derived-store-path.cc`.  A synthetic-output unit test
   would only add a structural assertion ("this code path
   specifically goes through `realiseCoercedPath`") — coverage
   precision, not coverage completeness.

3. **`buildFixtureDrv` (real-build variant)** — needed to exercise
   `Worker`, sandboxing, output hashing.  Architecturally
   impossible in unit tests (the harness uses `dummy://`).
   Functional tests are the right level.  No unit-test helper can
   close this gap.

4. **`writeStaleSchemaDb(path, ver)` + `makeDbAtPath(path)`** — the
   blocking test `SchemaMigration_StaleDbFile_DoesNotCrash` assumes
   schema-migration semantics that **don't exist in production**.
   Production uses filename-versioning
   (`eval-trace-v<kSchemaEpoch>-<hash-algorithm>.sqlite`), not
   `PRAGMA user_version`;
   `kSchemaEpoch` and the active hash backend are folded into the session key
   hash.  Stale DBs of different versions or hash backends are silently
   ignored (new filename / distinct session key).
   `tests/functional/flakes/eval-trace-soundness.sh` Test 2
   already plants an old-filename stub and verifies coexistence.
   Building the helpers requires first deciding to add a
   `user_version` check to production — a migration-policy
   decision, not test infrastructure.  That call isn't currently
   motivated.

**Under what conditions DEF-7 would reopen.** A concrete bug
report that one of these uncovered invariants would have caught:
- A verifier exception-path regression that propagates to callers
  (would motivate `BrokenStoreTest`).
- An IFD dep-recording regression the functional test misses
  (would motivate `buildFixtureDrv` synthetic variant).
- A stale-DB corruption report, or a decision to add production
  schema-version checking (would motivate `writeStaleSchemaDb` /
  `makeDbAtPath`).

None of these triggers exists today.

**Still built and in use:** `TempGitRepo` (in `helpers.hh` /
`helpers.cc`), `PathCountersSnapshot` (in `helpers.hh`).

### DEF-9: `path_t` round-trip accessor-identity test missing

- **Where documented:** §OR-11 in `src/libexpr/eval-trace/CLAUDE.md`
  (the parent "Open research" entry).
- **What's missing.** No test in
  `src/libexpr-tests/eval-trace/traced-data/materialization/` records a
  cached `path_t` result whose source path has a specific accessor
  (e.g. `state.storeFS`), restores via the cache round trip, and
  asserts that the restored path's accessor compares equal to the
  source's.  Under the current materialize.cc path, such a test would
  fail — see OR-11.
- **Unblock requires.** First, a reachability demonstration: a Nix
  expression whose value is a path, which when cached and restored
  compares unequal in a way observable to nixpkgs-lib (or to direct
  `SourcePath::operator==`).  Once reachable, the fix options are
  documented in OR-11.  Adding the test BEFORE the fix is useful as
  a pin; adding it AFTER is a regression guard.
- **Cross-reference.** Analogous tests exist in
  `src/libexpr-tests/path-accessor-preservation.cc` for
  `ExprConcatStrings` and `state.storePath`.  The materialize.cc site
  can reuse the same harness.

### DEF-8: RETIRED 2026-04-30 — tracks a non-existent feature

- **Previously documented in:** §8 "DISABLED_ tests" subsection.
- **Finding (re-audited 2026-04-30).** The "git-identity skip"
  optimization that `DISABLED_GitIdentitySkip_CounterIncrements`
  was designed to probe does not exist in the current code. A full
  read of `dep-resolution-service.cc::resolveDepHash` (specifically
  the `CanonicalQueryKind::GitRevisionIdentity` branch at lines
  671-711) shows `governingRepoId` is used only for per-repo cache
  scoping, not for a verify-time skip shortcut. A search for
  `nrGitIdentitySkips`, `GitIdentitySkip`, and `gitIdentitySkip`
  across the entire `src/libexpr/` tree returns no hits outside
  documentation prose.
- **Companion coverage.** The git-identity path IS tested via
  `store/git-identity.cc` + `store/git-identity-recovery.cc` (37
  tests covering dep recording, identity-hash round-trip, and
  GitRevisionIdentity-indexed recovery). These cover the actual
  code paths; DEF-8's proposed skip-test was a test for a feature
  that never landed.
- **Status:** Retired as a tracked item. The originally-named
  counter and test file do not exist and should not be
  reintroduced unless a motivated design for a skip optimization
  is first written. `GitIdentity*` tests in the existing test
  files provide the real coverage for git-identity behavior.

---

## Section N: Known test-fixture drift (audit 2026-04-19)

An audit of fixtures vs production code surfaced patterns where tests
pass for reasons other than the invariant they claim to cover. Each
entry below lists the observed-or-suspected drift with concrete file:
line references, so future work can strengthen the tests methodically
instead of re-deriving the analysis.

### N.1 Fixture-level fixed-constant drift (resolved)

**Status: resolved 2026-04-20.** All three drift sources below now
rotate per-test or per-expression.

- **`TraceCacheFixture::testFingerprint` is mixed with the expression
  text inside `makeCache(nixExpr, ...)`** (helpers.hh `makeCache`
  body). Subclasses still set a per-suite `testFingerprint`, but every
  distinct Nix expression lands at its own `(session_key, AttrPathId(0))`
  slot, mirroring production's per-expression `FileEvalExpressionHash =
  active-backend-digest(exprText)` (see `installable-attr-path.cc`). The intentional
  bypass is `TraceCacheFixture::makeCacheWithSessionConfig`, which
  takes an explicit bootstrap fingerprint + `SessionConfig` — tests
  that need to control session-key derivation directly (e.g., the
  OR-5 reproducer) use that overload.

- **`TraceStoreFixture::testBootstrapFingerprint()` rotates per
  gtest test case.** Reads `::testing::UnitTest::GetInstance()->
  current_test_info()` and mixes `TestSuite.TestName` into the hash
  seed. Every TEST_F therefore lands at its own `Sessions.session_key`
  slot, eliminating the cross-test slot collision the old fixed
  literal had.

- **`MaterializationDepTest::makeQueryDb()` follows §N.1's mixing
  automatically.** It bootstraps from `lastPerExprFingerprint_`
  (the per-expression fingerprint from the most recent `makeCache`
  call) rather than the un-mixed `testFingerprint`. Queries land at
  the same slot the record path used.

### N.2 `simulateNewSession()` renamed + `simulateColdProcess()` added (resolved)

**Status: resolved 2026-04-20.** The old `simulateNewSession()`
cleared only the FS accessor lstat cache + TraceRuntime
`fileContentHashes` + cross-session memo tables
(importResolutionCache, fileTraceCache, inputCache,
fileContentHashCache) — misleadingly named because PosTable and
traceCtx were preserved and `fileTraceCache`/`importResolutionCache`
were NOT cleared in the original helper pre-`clearCrossSessionCaches`.
The helper is now split:

- **`TraceCacheFixture::simulateWarmRestart()`** — the old behavior,
  renamed. Preserves PosTable and traceCtx so it is safe to call
  mid-iteration (from inside an RC body, for example). Calls
  `state.clearCrossSessionCaches()` internally. All prior
  `simulateNewSession()` call sites (~40 across `property/**`,
  `verify/integration.cc`, `traced-data/dep-precision/hasattr.cc`)
  were renamed in the same commit.

- **`TraceCacheFixture::simulateColdProcess()`** — new, strictly
  stronger helper that calls `state.resetFileCache()`. Wipes
  PosTable and traceCtx in addition to the cross-session caches, so
  it matches what a real out-of-process subprocess starts with. NOT
  safe mid-iteration — callers holding references into PosTable or
  traceCtx would see them dangle. Use between logical iterations
  only. Intended for tests that must observe recovery/history-
  bootstrap paths a residual cache would otherwise short-circuit.

### N.3 Tests that don't check `loaderCalls` after warm verify (false-positive class)

A large number of "cold then warm" tests in `store/cache.cc`
(`ColdWarm_*` macro family), `verify/integration.cc`
(`Integration_FullFlow_*`), and `traced-data/materialization/*.cc`
(`Nested*_CacheMiss`/`_CacheHit` tests) assert the *result value* after
cold and after warm but NOT the `loaderCalls` counter. A regression
where the cache never fires would still produce the correct result via
re-evaluation — the tests would pass.

Mechanical fix: add a `loaderCalls` counter to each warm `makeCache`
invocation and assert `EXPECT_EQ(calls, 0)` for intended hits,
`EXPECT_EQ(calls, 1)` for intended misses. The `COLD_WARM_TEST` macro
in `store/cache.cc` could thread this automatically if the shape were
standardized.

### N.4 RapidCheck property tests without per-iteration isolation (high severity)

The `TraceCacheFixture` is shared across all iterations of an RC
property test by design (the CLAUDE.md fixture-pattern section warns
against `RC_GTEST_FIXTURE_PROP` destroying the SQLite DB). However,
the shared fixture means:
- `EvalState` persists across iterations
- SQLite trace rows persist across iterations
- `fileTraceCache` / `importResolutionCache` persist across iterations
- `testFingerprint` is constant across iterations

So iteration N writes `(fingerprint, AttrPathId(0))` to the SQLite
Sessions table. Iteration N+1's "cold" eval finds iteration N's row
and may serve or fall through — either way, the cold path of N+1 is
NOT genuinely cold. A precision property test asserting "mutate
unrelated → still hit" is especially susceptible: iteration N+1's
warm-hit assertion may succeed because iteration N's trace happens to
match current state, not because the cache correctly preserved
precision.

Tests that get this right rotate `testFingerprint` per iteration and
call `simulateWarmRestart()` between phases (e.g.,
`property/invariant/trace-transparency.cc`, `path-exists.cc`,
`builtin/list/length.cc::RemoveElement_Invalidates`,
`invariant/dep-correctness.cc`). Tests that don't: most of
`property/invariant/determinism.cc`, `commutativity.cc`,
`monotonicity.cc`, and the vast majority of `property/builtin/**`
and `property/composition/**`. The drift is the norm, not the
exception. `DISABLED_RemoveKey_Invalidates` (in `has-attr.cc`) is the
only one currently marked disabled for this reason; its siblings in
the same file (`UnrelatedKeyChange_CacheHit`,
`AddAbsentKey_Invalidates`) have the same fixture shape.

Mechanical fix: create a helper like `makeIsolatedRCBody()` that
wraps `simulateWarmRestart()` + per-iteration fingerprint rotation.
Adopt it mechanically across every RC property-test body.

### N.5 Shared fingerprints between A and B within one test

Multiple tests evaluate two expressions (e.g. commutativity of A and
B) using the SAME `testFingerprint`. Both end up at
`(session_key, AttrPathId(0))` — B's cold recording overwrites A's.
If the test's assertion depends on A's trace surviving, it's actually
testing History-based recovery, not the stated invariant.

The property tests `commutativity.cc`, `monotonicity.cc`, and some
`composition/*.cc` apply the correct fix (separate `fpA`/`fpB`).
`property/special/concurrent-eval.cc::InterleavedExpressions_NoCrossContamination`
does NOT (it's explicitly a test OF the shared-fingerprint hazard, so
the shared fingerprint is load-bearing — but the test's final warm-hit
assertion for A is sensitive to recovery finding A's prior trace).

### N.6 `EXPECT_NO_THROW` as primary assertion — RESOLVED 2026-04-22

**Status: resolved 2026-04-22.** Every `EXPECT_NO_THROW` wrapper
flagged as drift in prior rounds now carries either a pinned outcome
or a dedicated strengthening; see DEF-1 and DEF-2 for the
2026-04-22 work.

**Strengthened 2026-04-20:**
- `store/trace-value-context.cc` — `TraceValueContext_HardcodedHashCycle_NoThrow`
  renamed to `TraceValueContext_HardcodedHashCycle_MissOnMismatch`;
  replaces `EXPECT_NO_THROW(verify(...))` with
  `EXPECT_FALSE(result.has_value())`. The stored dep hashes (hardcoded
  stubs) cannot match recorded trace hashes, so the miss is intrinsic
  and the stronger assertion is safe.  Counter-based assertions on
  `nrTraceCacheMisses` are NOT used — that counter is incremented by
  the orchestrator path, which `TraceStorageTestAccess::verify`
  bypasses (cross-reference §N.7).
- `verify/integration.cc` — `ImportCycle_TraceValueContext_LoopPrevention`
  gets the same `EXPECT_FALSE(result.has_value())` treatment.

**Strengthened 2026-04-22 (DEF-1):**
- `store/recovery.cc` — three recovery-pathway tests.  Two turned
  out to be genuine HIT cases (FileBytes matches + stale git hash
  → trace served via content dep because GitRevisionIdentity
  mismatch is a short-circuit miss, not a hard failure) and were
  rewritten as `..._StaleGitHash_FileMatches_ValidViaContentDep`
  with `ASSERT_TRUE(result.has_value()) + value equality`.  The
  third is a genuine miss (file content changes + same-struct
  guard skips StructVariant) and became
  `Recovery_DirectHash_Miss_StructVariantSameGroup` with
  `EXPECT_FALSE(result.has_value())`.  Also fixed a pre-existing
  `/tmp` symlink bug in the third test.

**Strengthened 2026-04-22 (DEF-2):**
- `store/runtime-fetch-identity.cc` — 2 tests renamed to
  `RuntimeFetchIdentity_RecordVerify_WarmHit` and
  `RuntimeFetchIdentity_HashChange_WarmMiss`.  Fixture now uses a
  LOCKED `path:` input via `Input::fromURL` + `fetchToStore`
  (matching the pattern already in `store/session-config.cc`), so
  `Input::computeStorePath` inside the verifier succeeds against a
  real `narHash` attr.  Warm-hit test asserts
  `EXPECT_TRUE(result.has_value())`; hash-change test records a
  deliberately-wrong stored hash and asserts
  `EXPECT_FALSE(result.has_value())`.
- Historical note: the earlier "libfetchers not linked" framing
  was wrong — libfetchers IS linked transitively via `nix-expr.pc`'s
  public `Requires:` field (verified via `otool -L`).  Not a
  linkage issue; no `meson.build` change needed.

**Reference for future "intrinsically unresolvable" claims:**
- `Recovery_HistoryBootstrap_StructuralOverride_WhenKeysUnchanged`
  (in `store/recovery.cc`) was previously labeled here but already
  asserts `has_value` (per the "now resolved" note below), so it is
  not in the weak cluster.

Previously in this list, now resolved:
- `MapAttrs_UnrelatedKeyChange_WarmHit` in `verify/integration.cc`
  was flagged as an `EXPECT_NO_THROW` wrapper on 2026-04-19 and
  flipped to `EXPECT_EQ(calls, 0)` in the §N.3 pass on 2026-04-20.
  Kept here as a pointer so the git history remains easy to trace.
- `PassTwo_Structural_And_Implicit_Combined_StructuralFails` in
  `store/record-verify.cc` was flipped from `EXPECT_NO_THROW` to
  `EXPECT_FALSE(result.has_value())` in the Group C closure pass
  (2026-04-20), after tracing the coverage-analysis outcome:
  FileBytes fails → pass-2 covers filePath → StructuredProjection
  fails → not all structural deps cover the file → verdict is Invalid.
- `DepResolution_DerivedStorePath_Throws_GracefulMiss` in
  `store/error-handling.cc` was strengthened to
  `EXPECT_FALSE(result1.has_value())` prior to this audit.

### N.7 Orchestrator access from tests (resolved via canonical pattern)

**Status: resolved 2026-04-20 (pattern-based, not helper-based).**
`test::TraceStorageTestAccess::verifyTrace` / `::verify` call the sync
`TraceStore::{verifyTrace,verify}` entry points in `verifier.cc`.
That sync path is NOT missing the history-bootstrap fallback — it
*duplicates* the `scanHistory(stableRecoveryKey, pathId)` /
`bootstrappedFromHistory` / `nrHistoryBootstraps` logic that
`verifier.cc::verifyAttrImpl` also runs. An
OR-5-class reproducer *can* be written against
`TraceStorageTestAccess::verify`; it is just less ergonomic.

**Why the canonical pattern still matters.** Tests that exercise the
orchestrator path (rather than the duplicated sync path) keep
coverage of the real production code path that user `nix eval`
invocations take, and avoid letting the two copies of
scanHistory drift silently. Go through a `TraceCacheFixture`
subclass and use `makeCacheWithSessionConfig(expr, bootstrapSeed,
sessionConfig)` + `forceRoot(*session)`. That path constructs a
`StoreTraceBackend`, which owns an `AsyncRuntime`, and `forceRoot`
→ `backend->verify()` runs the orchestrator's `verifyAttrImpl`
including its `scanHistory` fallback. Either entry point (sync or
orchestrator) increments `nrHistoryBootstraps`, so
`PathCountersSnapshot::deltaHistoryBootstraps()` is the observable
in both cases.

**Unit-level reference.**
`verify/integration.cc::Integration_OR5Reproducer_ScanHistoryServesCrossSession`
drives two distinct `semanticSessionKey` values (policyA vs policyB)
against the same `stableRecoveryKey` and asserts
`deltaHistoryBootstraps() >= 1`. This is the canonical shape for
future OR-5-style reproducers: pattern consistency with the
orchestrator path and integration-path coverage, not
"only way to reach scanHistory."

**Why no raw-TraceStore helper.** A `verifyAttrViaOrchestrator(store,
pathId, ...)` factored into `TraceStorageTestAccess` would need the
private `async-runtime.hh` header (forward-declared in
`trace-backend.hh` but not exported). Rather than publish that
header, the canonical shape for tests that need the orchestrator is
the session-based path above. The note in
`TraceStorageTestAccess` (`helpers.hh`) cross-references this.

### N.8 `NIX_SHOW_STATS` is the only diagnostic surface for cache-path selection

No test in the project asserts WHICH path produced a warm hit: primary
session key, History-based recovery, structural-variant recovery,
subsumption, or re-evaluation-with-identical-value. The existing
`NIX_ALLOW_EVAL=0` idiom proves "some cache path succeeded," nothing
more. For soundness tests whose semantics depend on a SPECIFIC path
(e.g., "direct hash recovery serves this, not structural variant"),
the `NIX_SHOW_STATS` JSON is the primary observable.

**Helper (added 2026-04-20).** `readEvalTraceCounter` in
`tests/functional/common/functions.sh` parses a dotted key path
from a stats JSON. Counters surfaced: `evalTrace.record.count`,
`evalTrace.hits`, `evalTrace.misses`, `evalTrace.recovery.attempts`,
`evalTrace.recovery.historyBootstraps` (added 2026-04-20),
`evalTrace.recovery.gitIdentityHits`,
`evalTrace.recovery.directHash.hits`,
`evalTrace.recovery.structVariant.hits`. Example usage is in
`eval-trace-deps.sh` Test 25, `eval-trace-recovery.sh` Test 6, and
`eval-trace-core.sh` Tests 7/8.

Future tests should use this idiom where path-selection matters.

### N.9 Functional-test `clearStoreIndex()` is redefined per-file

**Status: resolved 2026-04-20.** `clearStoreIndex` is now defined
once in `tests/functional/common/functions.sh`; per-file copies
were removed. `eval-trace-volatile.sh` and `eval-trace-output.sh`
had missing calls added in Phase B/C of the §N.9 resolution.

### N.10 `Counter::enabled` global-enable in test harness

**Status: landed 2026-04-20.** `src/libexpr-tests/main.cc` sets
`Counter::enabled = true` once at test-suite init. Production
keeps the default-false (only flips when `NIX_SHOW_STATS=1` is in
the env) so non-stats evaluations don't pay the atomic-counter
cost.

**Consequence.** Tests can do raw counter reads (`const auto c =
nrRecoveryGitIdentityAccepted.load();`) without a `PathCountersSnapshot`
wrapper. Forgetting the wrapper previously meant the delta was
silently always 0. `PathCountersSnapshot` is still useful for
scoped baseline/delta; its constructor still executes
`Counter::enabled = true` (and the destructor restores
`wasEnabled_`). Under the `main.cc` global-enable the flip is a
no-op, but the save/restore pair is retained for defense-in-depth
against any future mid-test `Counter::enabled = false` toggle
(see the thread-safety comment above the struct in `helpers.hh`,
which flags the non-atomic `Counter::enabled` store as a hazard
for hypothetical parallel test execution).

**Migration guidance.** Existing tests that use
`PathCountersSnapshot` continue to work unchanged. New tests can
use either the snapshot or raw reads; raw reads are simpler when
you only need one observation.

### N.11 Case-insensitive filesystem coverage delta (landed 2026-04-27)

**Status: landed 2026-04-27.** `makeNixFilesystemIdentifierGen`
(in `property/expr-gen.cc`) restricts generated filenames to
`[a-z0-9_]` on case-insensitive filesystems, delegating to the
full-coverage `makeNixIdentifierGen` on case-sensitive ones.
Detection happens once per process via `tempFsIsCaseSensitive()`
(a probe that writes `<path>-A` and tests whether `<path>-a`
exists; throws on I/O failure rather than defaulting, since
either silent default is unsafe).

**Context.** Original failure (commit on branch
`vibe-coding/file-based-eval-cache`): `makeReadDirMapAttrsGen`
on macOS APFS generated `names = ["_d", "_D", "A"]`, both `_d`
and `_D` collapsed to a single inode, `readDir` returned only
`{_D, A}`, and `."_d"` access threw `attribute '_d' missing`.
RapidCheck counter-example preserved at
`RC_PARAMS="reproduce=BYTR2FGbUJXYjVGUy9GclJHd59lUlFGZElmcvEEZkVkb0JXefVlbyVGbhRXZk91U0lGbshUa0NX7Cu2bD3KgC3ugr92wtCowtL4avNcrAKc7Cu2bD3KgCDIgIAgEiAAAAMwBGcA"`.

**Scope.** Only `property/gen/read-dir-map-attrs.cc` uses the
filesystem variant — it is the only generator that turns random
identifiers into real directory entries. In-memory attrset /
JSON key generators (`makeJsonObjectGen`,
`makeAccessibleJsonObjectGen`, `attrset-pipeline.cc`,
`sibling-trace.cc`, `string-interpolation-json.cc`) still use
`makeNixIdentifierGen` with full upper/lower-case coverage,
since Nix attrset keys are case-sensitive regardless of host FS.

**Coverage delta.** `makeReadDirMapAttrsGen` is mixed into
`makeNixExprGen()` (expr-gen.cc near line 586), so every
property test that draws the general expression pool sees the
narrower name space on case-insensitive FSes. This is a
platform-conditional coverage delta, not a correctness change.
Tests that failed on macOS due to the inode-collision bug now
pass; tests that passed on Linux see unchanged coverage.

**Limitation.** The `collision` fallback in
`read-dir-map-attrs.cc` (around line 71-76) uses exact-equal
matching, which is safe only because `makeNixFilesystemIdentifierGen`
already returns lowercase on case-insensitive FSes. A future
refactor that routes a non-lowercase generator through this
dedup without updating the collision check would reintroduce
the bug.
