# Eval-trace / eval-environment surface inventory

Produced as part of the redesign audit. Every entry below was read end-to-end in the .hh/.cc file named. No identifier is mentioned that I did not verify in code. The plan file (`you-are-going-to-enumerated-creek.md`) references this inventory by section; they must be read together.

## 0. Files that were read in full

- Every `.hh` under `src/libexpr/include/nix/expr/eval-trace/**`.
- Every `.hh` under `src/libexpr/include/nix/expr/eval-environment/**` and `src/libexpr/include/nix/expr/eval-environment.hh`.
- Every `.hh` under `src/libexpr/eval-trace/cache/` and `src/libexpr/eval-trace/fiber/` and `src/libexpr/eval-trace/store/`.
- Every `.cc` under `src/libexpr/eval-trace/` (cache, deps, fiber, store, context, counters, hash-spec).
- Every `.cc` under `src/libexpr/eval-environment/` (authority, request-builder, session-builders) plus `src/libexpr/eval-environment.cc` and `src/libexpr/eval-environment/private-errors.hh`.
- `src/libexpr/eval.cc` (full 4838 lines) and `src/libflake/flake.cc` (full 2886 lines).

## 1. `eval-trace/ids.hh`

Public types:

- 13 `Tagged<Tag_, uint32_t>` aliases: `DepSourceId`, `FilePathId`, `RepoRootId`, `DataPathId`, `DepKeyId`, `SimpleDepKeyId`, `StringId`, `AttrNameId`, `AttrPathId`, `DefinitionStamp`, `SlotStamp`, `NodeStamp`, `ValueIdentityStamp`.
- 3 near-identical structs each wrapping `DepKeyId value` with `=default` `==` and a `Hash` forwarder: `DerivedStorePathDepKeyId`, `StorePathAvailabilityDepKeyId`, `RuntimeFetchIdentityDepKeyId`.
- `TypedDepKeyId = std::variant<SimpleDepKeyId, DerivedStorePathDepKeyId, StorePathAvailabilityDepKeyId, RuntimeFetchIdentityDepKeyId>` plus `static_assert(std::variant_size_v<TypedDepKeyId> == 4, ...)` and `eraseDepKeyType(const TypedDepKeyId &)`.
- 2 single-`AttrPathId value` structs with manual `==` + `Hash`: `ParentSlot`, `ValueContext`.
- `SiblingIdentity { ParentSlot, DefinitionStamp, SlotStamp, uint32_t canonicalSiblingIdx; }`.
- `invalidSiblingIndex` constant.

## 2. `eval-trace/hash-spec.{hh,cc}`

- `enum class EvalTraceHashAlgorithm { Blake3, Sha256 }`.
- `kEvalTraceDigestSize` (32).
- 7 free functions: `evalTraceHashAlgorithmName`, `evalTraceHashAlgorithmSlug` (identical body to `Name` — `Slug` literally returns `Name` in `.cc`), `toHashAlgorithm`, `evalTraceHashAlgorithmTag`, `parseEvalTraceHashAlgorithmTag`, `getEvalTraceHashAlgorithm`, `setEvalTraceHashAlgorithm`.
- `BaseSetting<EvalTraceHashAlgorithm>` specializations: `parse`, `to_string`, `trait`.

## 3. `eval-trace/canonical-hash.hh`

- `class CanonicalHashBuilder` with **11 `field` overloads** (6 concrete + 5 templated) on arity 2: `(string_view, string_view)`, `(string_view, EvalTraceHash)`, `field<Tag>(string_view, Tagged<Tag,EvalTraceHash>)`, `field<Tag>(string_view, Tagged<Tag,std::string>)`, `(string_view, CanonPath)`, `field<Tag>(string_view, Tagged<Tag,CanonPath>)`, `field<UInt>(string_view, UInt)` (unsigned), `field<SInt>(string_view, SInt)` (signed), `field<Enum>(string_view, Enum)`, `(string_view, bool)`, `optionalField<T>(string_view, optional<T>)`, plus `finish()`.
- Free fn `taggedEvalTraceHashHex<Tag>(Tagged<Tag,EvalTraceHash>)`.

## 4. `eval-trace/deps/types.hh` (1179 LoC)

- `enum class CanonicalQueryKind : uint8_t` — 17 variants: `FileBytes, DirectoryEntries, ExistenceCheck, EnvironmentLookup, SessionSystemValue, RuntimeFetchIdentity, DerivedStorePath, VolatileExec, NarIdentity, StructuredProjection, ImplicitStructure, RawBytes, StorePathAvailability, GitRevisionIdentity, TraceValueContext, TraceParentSlot, VolatileTime`.
- `enum class QueryBehavior : uint8_t` (6 variants), `enum class QueryDomain : uint32_t` (7 flags), `using QueryDomainMask = uint32_t`, `enum class RepoRootAddressingKind { None, DirectPath, StructuredPath }`.
- `struct QueryDescriptor` + `makeQueryDescriptor(CanonicalQueryKind)` (17-arm constexpr switch); alias `describe(kind)`.
- 12 free predicate fns on CQK: `queryKindName, isVolatile, isContentOverrideable, isDigestDep, isTraceContext, queryBehavior, contributesToTraceHash, queryBehaviorName, isStructuredQueryKind, isTraceContextQueryKind, isCoveredBySessionFingerprint, isFileContentDep`, plus `repoRootAddressingKind`.
- `struct EvalTraceHash` with `operator==`, `<=>`, `data`, `size`, `view`, `fromHash`, `fromBlob`, nested `Hasher`, `toHex`.
- 14 `Tagged<Tag_, ...>` hash/identity aliases: `DepHash, TraceHash, StructHash, ResultHash, FullTraceHash, DepKeySetHash, StoredGitIdentityHash, CurrentGitIdentityHash, RuntimeRootSourceKey, RuntimeRootNarHash, RuntimeRootStorePath, GraphNodeDepSourceKey, GitRepoRoot, RegistryMountSubdir`.
- `template<T> evalTraceHashFromSink<T>(HashSink &)`, `evalTraceHashFromBlob<T>(data, len)`.
- `struct EvalTraceHasher` — `HashSink` wrapper with `operator()(string_view)`, `finish`, `finishAs<T>` (duplicates `evalTraceHashFromSink<T>`).
- `using DepHashValue = std::variant<DepHash, std::string>`, `blobData(const DepHashValue &)`.
- `enum class HashProvenance { Computed, Verified }`, `template<HP> using ProvenancedHash = Tagged<singleton::Tag<HP>, DepHashValue>`, aliases `ComputedHash`, `VerifiedHash`.
- `enum class StructuredFormat : char { Json='j', Toml='t', Directory='d', Nix='n' }` + `structuredFormatChar`, `parseStructuredFormat`, `structuredFormatName`.
- `enum class ShapeSuffix : uint8_t { None, Len, Keys, Type }` + `shapeSuffixName`.
- `enum class DepSourceKind { Absolute, Registered }`.
- `absolutePathDep`, `runtimeRootSourceKeyFromDebugString`.
- `struct AbsoluteDepSource {}`, `struct DepSource { std::variant<Absolute, GraphNode, RuntimeRoot>; ... }` with factories `makeAbsolute, fromNodeKey(std::string), fromNodeKey(GraphNodeDepSourceKey), fromRuntimeRoot` and accessors `kind, graphNodeKey, runtimeRootKey, operator==`, nested `Hash`.
- `using EncodedDepSourceBlob = Tagged<Tag, std::string>` + `parseDepSource, serializeDepSource, encodeDepSourceBlob, decodeDepSourceBlob, isAbsoluteDepSource`.
- `struct StructuredPathComponent` + `makeKey, makeIndex, isIndex, isKey`; `using StructuredPath = std::vector<StructuredPathComponent>`; `displayStructuredPath`.
- `struct Dep::Key` — the central type:
  - private `InitTag`, private `TypedDepKeyId typedKeyId_`.
  - Public fields: `kind, sourceId, filePathId, dataPathId, hasKeyId, dirSetHashId, attrPathId, governingRepoId, suffix, format`.
  - 6 factories: `makeSimple, makeDerivedStorePath, makeStorePathAvailability, makeRuntimeFetchIdentity, makeStructured, makeTraceContext`.
  - 3 predicates: `isSimple, isStructured, isTraceContext`.
  - 7 accessors (assert on kind): `structuredFormat, simpleKeyId, depKeyId, derivedStorePathKeyId, storePathAvailabilityKeyId, runtimeFetchIdentityKeyId, typedKeyId`.
  - `operator<=>` that hand-rolls a pseudo-variant ordering (0=simple, 1=structured, 2=trace-context), plus `operator==`, nested `Hash`.
- `struct Dep` + 2 factories `makeValueContext, makeParentSlot`, accessor `traceContextPath`.
- `struct DepRange { std::vector<Dep> * deps; uint32_t start, end; }`.
- `struct ProvenanceRecord { DepSourceId sourceId, FilePathId filePathId, DataPathId dataPathId, StructuredFormat format; }`.
- `struct ProvenanceTable { vector<ProvenanceRecord>; allocate, resolve, clear; }`.

## 5. `eval-trace/semantic-objects.hh`

- 4 objects: `PathObject`, `TextObject`, `StructuredObject`, `IdentityObject`.
- `enum class SemanticKind : uint8_t { None, Path, Text, PathText, Structured }`.
- `struct SemanticHandle { SemanticKind kind; optional<PathObject> path; optional<TextObject> text; optional<StructuredObject> structured; optional<IdentityObject> identity; }` with 5 factories (`forPath, forText, forPathText, forStructured, withIdentity`) + predicates.
- `class ContextObject` — 3 private variant alternatives (`PlainString`, `PreservedString`, `DetachedStorePathString`), public `isDetached`, `view`. Forward-declared friend `EvalState`.

## 6. `eval-trace/producer-origin.hh`

- `using ProducerOrigin = StructuredObject;` plus `inline const StructuredObject & getStructuredObject(const ProducerOrigin & origin) { return origin; }`. The comment in the file says this was collapsed from a single-variant std::variant.

## 7. `eval-trace/child-meta.hh`

- `struct TracedContainerMeta { optional<ProducerOrigin> producerOrigin; optional<ValueIdentityStamp> valueIdentityStamp; }`.
- `struct CachedAttrEntry { Symbol name; optional<ProducerOrigin> producerOrigin; uint32_t aliasOf = invalidSiblingIndex; }`.
- `struct CachedListEntry { uint32_t aliasOf = invalidSiblingIndex; }`.

## 8. `eval-trace/result.hh` + `trace-result-codec.cc`

- `enum class ResultKind : uint8_t` — 12 variants (`Placeholder=0, FullAttrs=1, String=2, Missing=3, Misc=4, Failed=5, Bool=6, Int=8, Path=9, Null=10, Float=11, List=12`; note non-contiguous 7).
- 11 per-variant structs (`placeholder_t, missing_t, misc_t, failed_t, int_t, path_t, null_t, float_t, list_t, string_t, attrs_t`) — some empty, some with payload. `bool` is in the variant directly (no `bool_t`).
- `CachedResult = std::variant<12 alternatives>`.
- `struct EncodedResultPayload { ResultKind; uint32_t encodingVersion; std::string payload; std::string auxContext; }`.
- `kSemanticResultEncodingVersion = 2`.
- `resultKindName(ResultKind)` free fn.
- `trace-result-codec.cc`: `TraceStore::encodeCachedResult` uses `std::visit(overloaded{12 lambdas})`; `decodeCachedResult` uses a 12-arm switch on `ResultKind`. Per-variant JSON encode/decode helpers (`encodeAttrEntries`, `decodeCanonicalAttrEntries`, `encodeListEntries`, etc.).

## 9. `eval-trace/toml-canonical.hh`

- `inline std::string tomlCanonical(const toml::value & v) { ostringstream ss; ss << v; return ss.str(); }` (3 lines).

## 10. `eval-trace/strand-local.hh`

- `template<Tag> using StrandToken = gdp::Proof<Tag>;`
- `template<T, Tag> using StrandLocal = gdp::ProofGuarded<T, Tag>;`
- Two tag structs `FileStrandTag`, `VerificationAccessTag`.

## 11. `eval-trace/deps/recording.hh` + `deps/recording.cc`

- Free fns `allocateProvenanceRef(pools, srcId, fpId, dpId, format) -> Pos::ProvenanceRef`, `resolveProvenanceRef(pools, ref)` (wrappers around `pools.provenanceTable.allocate/resolve`).
- `recording.cc` also implements: `InterningPools::internGoverningRepo` (walks ancestors looking for `.git`), `resolveStructuredPath`, `internStructuredPath`, `recordStructuredDep` (tests TraceAccess::current() → fiberCtx → standaloneCtx chain; dropping when none active increments `nrDepRecordNoActiveContext`), TraceFrame registry implementations (`ContainerProvenanceRegistry`, `ScannedBindingsRegistry`, `PrecomputedKeysRegistry`, `IntersectOriginsRegistry`, `DirSetHashRegistry`), `TraceAccess` forwarders (~15 methods), `DepCaptureScope` out-of-line ctors/dtor (production + test overloads, 2 ctor arity shapes differing only in `registry`).

## 12. `eval-trace/deps/input-resolution.hh` + `deps/input-resolution.cc`

- `using SimpleDepKeyAtom = Tagged<Tag_, std::string>`.
- 3 POD structs: `DerivedStorePathDepKey { CanonPath pathKey; SimpleDepKeyAtom storeName; }`, `StorePathAvailabilityDepKey { StorePath storePath; }`, `RuntimeFetchIdentityDepKey { fetchers::Attrs inputAttrs; }`.
- Free fns: `recordDep`, `maybeRecordRawContentDep`, `recordFileBytesDepViaCache`, `resolveDepPathKey` (3-tier lookup; increments `nrResolveViaRegistry`/`nrResolveViaPathObject`/`nrResolveViaAbsolute`), `resolveProvenanceViaRegistry` (×2 overloads — with/without `TraceAccess`), `resolvePathObjectViaRegistry` (×2 overloads), `decodeDerivedStorePathDepKey`, `decodeStorePathAvailabilityDepKey`, `decodeRuntimeFetchIdentityDepKey`, `makeRuntimeFetchIdentityInput`, `feedCanonicalDepKeyMaterial` (17-arm switch — `TraceValueContext`/`TraceParentSlot` `unreachable()`, `StructuredProjection`/`ImplicitStructure` `unreachable()`), `renderSimpleDepKeyDisplay` (3-arm switch on derived/availability/runtime-fetch, fall-through to `pools.resolve(simpleKeyId)`), `makeRuntimeRootSourceKey`, `renderRuntimeFetchIdentityDisplay`, `ResolvedDepPath` struct.
- `input-resolution.cc` also defines 3 parallel encoders (`encodeDerivedStorePathDepKey` → magic `"dsp1"` + 2 length-prefixed fields; `encodeStorePathAvailabilityDepKey` → magic `"spa1"` + 1 length-prefixed field; `encodeRuntimeFetchIdentityDepKey` → magic `"rfi1"` + attr count + framed name/value). 3 parallel decoders with identical shape (magic check, read framed strings, fail-on-trailing-bytes). All use local `appendUint64`, `readUint64`, `readFramedString`, `appendFetcherAttr`, `readFetcherAttr` helpers duplicated from `trace-serialize.cc`.

## 13. `eval-trace/deps/input-resolution-internal.hh`

- 3 Tagged blob types: `EncodedDerivedStorePathDepKeyBlob`, `EncodedStorePathAvailabilityDepKeyBlob`, `EncodedRuntimeFetchIdentityDepKeyBlob`.
- 3 encode free fns whose bodies are in `input-resolution.cc`.

## 14. `eval-trace/deps/interning-pools.hh`

- `struct DataPathNode { uint32_t parentId; std::string component; int32_t arrayIndex; }`.
- `struct DataPathNodeKey { parentId; component; arrayIndex; } + Hash`.
- `struct DataPathPool` with `root, fromRaw, internChild, internArrayChild, collectPath, bulkLoad, nextId`.
- `struct DirSetOrigin { DepSourceId sourceId; FilePathId filePathId; }`.
- `using DirSetDefinition = std::vector<DirSetOrigin>`.
- `struct InterningPools`:
  - private `StringInternTable strings_`.
  - private `template<Id> internDepKeyBlob<Id>(string_view)`.
  - private `resolveEncodedDepKeyBlobForPersistence(DepKeyId)`.
  - 3 friend decls for `decode*DepKey`.
  - public `dataPathPool`, `provenanceTable`, `dirSets` (flat map), `governingRepoCache_`.
  - `template<Id> intern(std::string_view)` (asserts `!same_as<Id, DepSourceId>`).
  - specialized `intern<DepSourceId>(const DepSource &)`.
  - **4 concrete `intern` overloads**: `(SimpleDepKeyAtom)`, `(DerivedStorePathDepKey)`, `(StorePathAvailabilityDepKey)`, `(RuntimeFetchIdentityDepKey)`. The latter 3 each do `encode*` → `internDepKeyBlob<*Id>(empty-safe view)`.
  - `template<Id> resolve(Id)`.
  - **4 concrete `resolve` overloads** for the 4 key-id types.
  - `resolveDepSource(DepSourceId)`, `resolveRepoRoot(RepoRootId)`, `bulkLoadString`, `nextStringId`, `resolveRawString`, `internGoverningRepo(string_view)`, `clearGoverningRepoCache`.

## 15. `eval-trace/deps/hash.hh` + `deps/hash.cc`

- `template<F> concept DepKeyFeeder` requiring `f(CanonicalHashBuilder &, const Dep::Key &) -> void`.
- Free fn `sortAndDedupDeps(deps)` (sorts by `(key, hash)`, uniques on the same key).
- `detail::hashableDepCount`, `detail::feedDepValue` (visit DepHashValue variant with 2 arms), `detail::feedDep(ordinal, dep, includeHash, feedKey)`.
- **4 primary templates** differing only in (domain, filter, include-value):
  - `computeTraceHashFromSorted<F>` — domain `"eval-trace.trace-hash.v2"`, filter `contributesToTraceHash`, include hash, return `TraceHash`.
  - `computeFullTraceHashFromSorted<F>` — domain `"eval-trace.full-trace-hash.v1"`, filter none, include hash, return `FullTraceHash`.
  - `computeTraceStructHashFromSorted<F>` — domain `"eval-trace.trace-struct-hash.v2"`, filter `contributesToTraceHash`, NO hash, return `StructHash`.
  - `computeDepKeySetHashFromSorted<F>` — domain `"eval-trace.dep-key-set-hash.v1"`, filter none, NO hash, return `DepKeySetHash`.
- **8 InterningPools-based convenience wrappers** (4 × fromSorted/non-sorted), bodies in `.cc` are one-line lambdas threading `feedCanonicalDepKeyMaterial` into the primary template.
- `canonicalKeysHash(vector<string>)` — sort + `CanonicalHashBuilder` domain `"canonical-keys-hash"`.

## 16. `eval-trace/deps/dep-hash-fns.hh` + `deps/dep-hash-fns.cc`

- `DepHash depHash(string_view)` (raw digest of bytes via active backend).
- `DepHash depHashPath(const SourcePath &)` (NAR dump).
- `DepHash depHashDirListing(const DirEntries &)` (canonical-framed).
- `optional<CurrentGitIdentityHash> computeGitIdentityHash(const fs::path &)` (canonical-framed, HEAD-rev + modified/deleted file lists).
- `dirEntryTypeString(optional<Type>)`.
- 6 sentinel accessors (function-local statics): `kHashZero, kHashOne, kHashObject, kHashArray, kHashEmpty, kHashMissing`. Bodies defined in `shape-deps.cc`.

## 17. `eval-trace/deps/dep-recording-context.hh` + `.cc`

- `struct DedupCheckedTag {}` + `class DedupGate : private gdp::Certifier<DedupCheckedTag>` with single static `ifPassed<F>(bool cond, F && f)` (friend: `DepRecordingContext`). `withProof` (unconditional) is not exposed.
- `struct EpochLogRef` — wrapper around `std::vector<Dep> & log_`. Methods: `size`, `storage` (const ref), `truncate(size)`, `append(Proof<DedupCheckedTag>, dep)` (push_back), `appendReplayed(Proof<RecordingScopeActiveTag>, dep)` (push_back). No raw push_back.
- `struct DepRecordingContext`:
  - `InterningPools & pools`.
  - Nested `struct Scope { registry; ownDeps; seenDeps (flat_map Dep::Key → DepHashValue); replayedValues (traceable flat_set<const Value *>); epochLogStartIndex; unstable; }`.
  - `vector<Scope> scopeStack`.
  - `EpochLogRef epochLog`.
  - Nested `struct DepCaptureScopeTag {}`.
  - ctor `(InterningPools &, std::vector<Dep> & epochLog)` — does NOT push a root scope.
  - public read-only: `currentScope, isActive, depth, scopeContainsDepKey`.
  - private `recordInterned`.
  - public **4 `record` overloads** (simple CQK + 3 typed-key-specific) + 1 `record(const Dep &)` general = **5 overloads**.
  - public `replayMemoizedRange(value, range)`, `isStable`.
  - private `observeRecordedDep`, `pushScope(Proof)`, `popScope(Proof)`, `takeDeps(Proof)`. Proof-guarded by `DepCaptureScopeTag`.
  - Friend: `DepCaptureScope`, `SiblingForceScope`, `PublicationWarmupScope`, `test::TestScopeAccess`.

## 18. `eval-trace/deps/dep-capture-scope.hh`

- `DepRecordingContext * currentFiberDepCtx()` + `DepRecordingContext * currentStandaloneDepCtx()` free fns.
- `struct StandaloneDepCtxGuard` — RAII ctor/dtor saves/restores the file-static `standaloneDepCtx_`.
- `struct DepCaptureScope : private gdp::Certifier<DepCaptureScopeTag>`: fields `registry`, `ctx`, `fallbackEpochLog_`, `ownedCtx`, `standaloneGuard`, `isRoot`, `ownsTraceFrame`, `scopePopped`. **2 constructors** (`InterningPools &, SemanticRegistry const &`) and (`InterningPools &`). Methods `finalizeAndTakeDeps`, `isStable`.
- `struct RecordingScopeActiveTag {}`.
- `class RecordingScopeGuard : private gdp::Certifier<RecordingScopeActiveTag>` — single `ifActive<F>` static.

## 19. `eval-trace/deps/sibling-force-scope.hh`

- `struct SiblingForceScope : private gdp::Certifier<DepCaptureScopeTag>` — RAII, `commit()`.

## 20. `eval-trace/deps/replay-publish-scope.hh`

- `template<F> struct ReplayPublishScope` — RAII stored-inline, `commit()`, CTAD guide.

## 21. `eval-trace/deps/nix-binding.hh` + `deps/nix-binding.cc`

- `using NixScopeHash = Tagged<Tag_, EvalTraceHash>`, `NixBindingHash`.
- `struct NixBindingEntry { DataPathId dataPathId; NixBindingHash bindingHash; SourcePath resolvedFile; optional<PathObject> origin; }`.
- Free fns: `findNonRecExprAttrs(Expr*)`, `computeNixScopeHash`, `computeNixBindingHash`, `registerNixBindings`, **2 `maybeRecordNixBindingDep` overloads** (with/without `TraceAccess`).

## 22. `eval-trace/deps/semantic-analyzer.hh`

- `class NixSemanticAnalyzer` — `std::unordered_map<PosIdx, NixBindingEntry> nixBindings`, `clear, rememberBinding, lookupBinding`. One-hash-one-lookup class.

## 23. `eval-trace/deps/shape-recording.hh` + `deps/shape-deps.cc`

- `struct TracedContainerProvenance { DepSourceId sourceId; FilePathId filePathId; DataPathId dataPathId; StructuredFormat format; }`.
- `using ProvenanceRef = const TracedContainerProvenance *`.
- `struct CompactDepComponents { DepSourceId; FilePathId; StructuredFormat; DataPathId; ShapeSuffix; StringId hasKeyId; StringId dirSetHashId; }`.
- `struct PrecomputedKeysInfo { DepHash hash; uint32_t keyCount; DepSourceId; FilePathId; DataPathId; StructuredFormat; }`.
- Free fns: `resolveStructuredPath`, `internStructuredPath`, `recordStructuredDep` (rejects Directory+Len combination), and **5 shape recorders**: `maybeRecordListLenDep`, `maybeRecordAttrKeysDep`, `recordIntersectAttrsDeps`, `maybeRecordHasKeyDep`, `maybeRecordTypeDep`. All `[[gnu::cold]]`.
- `deps/shape-deps.cc` also defines `forEachTracedDataOrigin` (private template), `collectOriginsCached`, `computeDirSetHashAndDefinition`, `recordHasKeyMissDeps`, plus the 6 sentinel hash constants.

## 24. `eval-trace/deps/trace-access.hh`

- `struct TraceAccess` — **5 `record` overloads** mirroring `DepRecordingContext`; plus `recordStructured` one-arg shape; plus ~15 forwarder methods: `registerPrecomputedKeys`, `erasePrecomputedKeys`, `allocateProvenance`, `registerTracedContainer`, `unregisterTracedContainer`, `lookupTracedContainer`, `markBindingsScanned`, `isBindingsScannedForTest`, `lookupPrecomputedKeys`, `lookupIntersectOrigins`, `cacheIntersectOriginsIfScoped`, `lookupDirSetHash`, `cacheDirSetHash`, `dirSetHashCacheSizeForTest`, `intersectOriginsCacheSizeForTest`, `replayMemoizedRange`, `scopeContainsDepKey`. Factories `current()`, `forRecording`, `tracingPools`, `depRecordingContext`, `isActive`, `operator bool`.

## 25. `eval-trace/deps/trace-frame.hh`

- `struct TransparentStringHash`, `struct DirSetKey + Hash`, `struct IntersectOriginInfo`.
- `struct TraceFrame` — private struct, all members accessible only through 5 friends (`TraceFrameScope`, `eval_trace::FiberTraceFrameScope`, `FiberEvalContext`, `DepCaptureScope`, `TraceAccess`). 5 nested registries (each with its own methods): `ContainerProvenanceRegistry`, `ScannedBindingsRegistry`, `PrecomputedKeysRegistry`, `IntersectOriginsRegistry`, `DirSetHashRegistry`. Plus nested `ContainerRefKind`, `ContainerRef`, `BindingsRef` — each with a Hash. ~15 outer forwarder methods.

## 26. `eval-trace/deps/trace-activation-scope.hh`

- `struct TraceActivationScope` — ctor increments `state.traceActiveDepth`, dtor decrements it.

## 27. `eval-trace/deps/memo-replay-store.hh`

- `struct MemoReplayStore` — `epochLog_ (vector<Dep>)`, `epochMap (traceable flat_map<Value *, DepRange>)`, `replayBloom (PointerBloomFilter<1<<23, 16>)`. Methods: `clearReplayIndex`, `clear`, `epochSize`, `epochEntriesForTest`, `recordThunkDeps(value, epochStart)` (uses `insert_or_assign`), `rollbackEpoch(epochStart)`, `getReplayRange`.

## 28. `eval-trace/counters.hh` + `counters.cc` (inventory of `Counter` externs)

- 7 cache hit/miss.
- 13 timing accumulators (`nrVerifyTimeUs`, `nrVerifyTraceTimeUs`, `nrRecoveryTimeUs`, ..., `nrDbCloseTimeUs`).
- 5 event counters.
- 4 key-set loading.
- 5 TracedExpr creation.
- 2 data-file nodes.
- 3 DepRecordingContext scope.
- 4 replayMemoizedDeps.
- 12 per-dep-type hash timing (Content/Directory/Existence/StructuredJson/StructuredToml/StructuredDir/StructuredNix/StorePath/StructuredOuter/GitIdentity/GitIdentityMisses/Other).
- 4 L1 cache tracking (hits/misses/structuredMisses/contentSubsumptionSkips).
- 2 recovery dep recompute.
- 6 recovery lookup (latestHistory / directHash / gitIdentity / scanHistory — each Count/Rows/Us triad where applicable).
- 5 git-identity recovery (Attempts/Candidates/Accepted/Rejected/TimeUs).
- 5 implicit-guard (Candidates/FullTraceLoads/Checks/Failures/TimeUs).
- 2 SC/IS detail (DirSetMisses, JsonParseUs).
- 1 GitIdentityHits.
- 1 nrHistoryBootstraps.
- 5 structural variant (Candidates/DepsResolved/LoadKeySetUs/HashUs/DepResolveUs).
- 5 observability (BackendSetupFailed, ResolveViaRegistry, ResolveViaPathObject, ResolveViaAbsolute, DepRecordNoActiveContext).
- `struct SVCandidateStats { uint32_t tried, succeeded, abortedEarly, hashMismatch; uint64_t depsResolvedSum, depResolveUsSum; uint32_t bothSetCount, earlierHashMismatchCount; uint64_t earlierHashMismatchSavedDeps; uint32_t hashMismatchOnlyCount; uint64_t hashMismatchOnlySavedDeps; }` — 11 per-ID fields for a measurement-only feature.
- `using SVCandidateStatsMap = flat_map<uint32_t, SVCandidateStats>`.
- Free fns: `recordSVCandidate(10 args)`, `snapshotSVCandidateStats`, `renderSVCandidateStatsJson`, `clearSVCandidateStats`.

Total `Counter` externs: **~85**. Printed in `EvalState::printStatistics` under `evalTrace.*` subtrees: `db`, `hits/misses`, `loadTrace`, `loadKeySet`, `record.*`, `recovery.*` (with nested `acceptance`, `directHash`, `gitIdentity`, `historyBootstraps`, `structVariant`, `lookups` sub-object with 4 nested lookup objects), `verify.*`, `verifyTrace`, `thunks.*`, `dataFile.*`, `depTracker.*`, `replay.*`, `depHash.*` (20+ fields). Plus `structVariant.byDepKeySet` array.

## 29. `eval-trace/context.hh` + `context.cc`

- `struct SiblingReplayCaptureScope` — per-thread scope; `thread_local` pointer `current`. ctor/dtor, `appendDeps(deps)`, static `shouldCapture`, static `maybeCapture`, private `recordAccess`, `ctx()`, `appendRecordedValueContextDeps`. Friend `TraceRuntime`, `appendActiveReplayCaptureDeps`, `currentReplayCaptureScope`, `setCurrentReplayCaptureScope` (all free fns).
- `struct PublishedMaterializedIdentity { Value * key; Bindings * bindings; Value *const* listBacking; optional<ValueIdentityStamp> stamp; optional<ValueIdentitySnapshot> previousValueIdentity; optional<ValueIdentitySnapshot> previousStampedIdentity; optional<ValueIdentityStamp> previousBindingsStamp; optional<ValueIdentityStamp> previousListBackingStamp; }` + nested `ValueIdentitySnapshot` with 6 fields.
- `struct TraceRuntime`:
  - Private nested `ValueIdentity`, `ListIdentityStamp`, `ReadOptimizedMap<Map>`.
  - Public fields: `vocabStore (unique_ptr)`, `pools`, `fileContentHashes`, `semanticAnalyzer`, `replayStore`, `valueIdentityMap`, `stampedValueIdentityVec`, `listValueIdentityStampMap`, `listBackingValueIdentityStampMap`, `nextValueIdentityStamp`.
  - Public non-test methods: ctor, `currentReplayEpochSize`, `rollbackReplayEpoch`, `makeTraceBackend`.
  - **14 test-only public methods**: `epochLogForTest`, `lookupReplayRangeForTest`, `hasReplayEntriesForTest`, `clearReplayEntriesForTest`, `registerValueIdentityForTest`, `hasValueIdentityForTest`, `hasBindingsValueIdentityForTest`, `resetForTest`, `recordThunkDepsForTest`, `replayMemoizedDepsForTest`, `clearFileContentHashes`, `epochEntriesForTest`, `isBindingsScannedForTest` (via TraceAccess), `hasValueIdentityForTest` on EvalState.
  - Private methods (friended to EvalState, TracedExpr, SiblingReplayCaptureScope): `tracingPools, lookupFileContentHash, cacheFileContentHash, registerTracedValueIdentity, registerTracedBindingsValueIdentity, rememberNixBinding, lookupNixBinding, registerMaterializedValueIdentity, publishRootMaterializedValueIdentity, rollbackRootMaterializedValueIdentity, shouldIsolateSiblingForce, recordThunkDeps, replayMemoizedDeps, sameValueIdentity, reset, lookupCapturedValueIdentity, lookupValueIdentity, lookupValueIdentityStamp, makeValueIdentity (static), makeSiblingIdentity (static), makeListIdentityStamp (static), lookupOrCreateBindingsIdentityStamp, lookupOrCreateListValueIdentityStamp, getVocabStore`.

## 30. `eval-trace/eval-context.hh`

- `struct Suspendable {}`, `struct Critical {}` — mode tags.
- `[[noreturn]] evalContextViolation(const char *)`.
- `template<Mode> class EvalContext` — private Suspendable ctor (placement-new site for `SuspendableCtxScope`), public Critical ctor, `state()`, `ownerThreadId()`, `executor()`, `syncAwait<T>(awaitable<T>)` + `void` specialization, `critical()`, `withLock<Mutex,F>`.
- Forward decls to fiber-scheduler: `FiberThreadLocals, saveThreadLocals, restoreThreadLocals, FiberScheduler, SuspendableCtxScope`.
- `class SuspendableCtxScope` — RAII, private `thread_local current_`; friends to `EvalContext` (for placement-new) and `saveThreadLocals`/`restoreThreadLocals` (for `snapshotPointer`/`restorePointer`). Methods `ctx()`, static `innermost()`.

## 31. `eval-trace/data/traced-data.hh`

- `struct TracedDataNode : gc` (virtual interface): `kind, formatTag, objectKeys, objectGet, arraySize, arrayGet, materializeScalar, canonicalValue` — 8 virtuals.
- `struct ExprTracedData : Expr, gc` — 4 fields, `eval`, `show`, `showForHash`, `bindVars`.

## 32. `eval-trace/cache/trace-backend.hh`

- `using TraceInputAccessors = flat_map<DepSource, SourcePath, DepSource::Hash>`.
- `class TraceBackend` — 12 virtuals: `verify, record, loadFullTrace, getCurrentTraceHash, getScheduler, flush, submitPrefetchHints, bindSession, setSessionConfig, resetVerificationState, recordRuntimeRoot, loadAndVerifyRuntimeRoots`. Nested `struct RuntimeRootResult { verifiedRoots; expectedCount; rejectedCount; }`.
- `class NullTraceBackend final : public TraceBackend` — **5 stub overrides** (others inherit the `= 0` → no, wait: re-reading, all 12 are `virtual` not `= 0`; `NullTraceBackend` only overrides `verify, record, loadFullTrace, getCurrentTraceHash, flush`. The rest inherit the default no-op bodies from the base class).
- `class StoreTraceBackend final : public TraceBackend` — overrides every virtual (all 12 + `getStore, asyncRuntime`). Field `std::shared_ptr<TraceStore> store`, `std::unique_ptr<AsyncRuntime> asyncRuntime_`, `bool sessionBound_`.

## 33. `eval-trace/cache/trace-session.hh`

- `class TraceSession : public std::enable_shared_from_this<TraceSession>`.
- Nested `struct RootLoadDep { CanonicalQueryKind; DepSource; SimpleDepKeyAtom; DepHashValue; }`.
- Nested `struct BackendParams { reference_wrapper<const Hash> fingerprint; optional<SessionConfig> sessionConfig; }`.
- Fields: `backend (unique_ptr)`, `state`, `root`, `tracedRoot`, `inputAccessors`, `rootLoadDeps_`, `registry_`, `childDefinitionStamps`, `childSlotStamps`, `nextDefinitionStamp, nextSlotStamp`, `registeredSiblingParents`.
- **2 constructors**: primary (`optional<BackendParams>` + full args) + convenience (`optional<reference_wrapper<const Hash>>`); convenience delegates to primary wrapping `BackendParams{*useCache, std::nullopt}`. Header comment states the convenience ctor is for "test fixtures that deliberately exercise the bootstrap path".
- Public: `traceBackend`, `registry` (×2 const/non-const), `registerRuntimeRootMount`, `withDepCaptureScope<F>` (accepts both `F(const TraceAccess &)` and `F()`), `getRealRoot`, `getRootValue`, `flush`, `releaseBackend`.
- Free fn `currentTraceSession()`.

## 34. `eval-trace/cache/root-handle.hh`

- `using RootLoader = std::function<Value *()>`.
- `class RootHandle` — 3 methods (ctor, `getRealRoot`, `reset`).

## 35. `eval-trace/cache/derived-container-builder.hh`

- `struct ShapePreservingReview { explicit constexpr ShapePreservingReview() = default; }` — the header itself states "This is NOT a GDP proof and provides no compile-time guarantee."
- `class DerivedContainerBuilder` — `DerivedContainerBuilder() = delete`; requires `ShapePreservingReview{}`. Methods `addShapePreservingSource`, `willRegisterContainer`, `registerContainer`, `finishList`, `finishAttrs`.

## 36. `eval-trace/cache/{traced-expr,child-trace-expr,root-trace-expr}.hh` + `cache/trace-session.cc` + `cache/materialize.cc`

- `struct TracedExpr : Expr, gc`: `cache (raw TraceSession *)`, `pathId`, `canonicalSiblingIdx`, nested `struct LazyState : gc { optional<TraceId> traceId; Value * resolvedTarget; }`, `lazy (ptr)`, `ensureLazy()`. Virtuals: `evaluateFresh, evaluateDirect, navigateToReal, getResolvedTarget, parentSlot, definitionStamp, slotStamp`. Methods: `eval` (the uncolored virtual), `show, showForHash, bindVars` (no-op), `attrPathStr`, `valueContext`, `peekResolvedTarget`, `cacheResolvedTarget`, `resolvedAttrBindingsHint`, `materializeResult`, `replayTrace`, `installChildThunk`, `traceBackend`, `vocab`. Protected ctor + `evaluateResolvedTarget`.
- `struct RootTraceExpr final : TracedExpr` — 4 overrides + static `make`.
- `struct ChildTraceExpr final : TracedExpr` — fields `parentExpr, name, listIndex, parentSlot_, definitionStamp_, slotStamp_`. 4 overrides (`evaluateFresh, evaluateDirect, navigateToReal, getResolvedTarget`) + `parentSlot, definitionStamp, slotStamp` + `tracePathFromRoot, traceChainFromRoot, traverseRealTree` + static `make`.
- Free fn `buildCachedResult(state, target)` in `materialize.cc`.
- `TracedExpr::eval` in `trace-session.cc` (outermost entry): checks `FiberScheduler::insideTask`, if not and a scheduler exists, runs body inside `scheduler->run`. `SuspendableCtxScope scope(state, *sched)` or null ctx. If no ctx, falls through to `evaluateDirect`. Otherwise `backend->verify(ctx, pathId)` → `classifyReplayPolicy` → `materializeResult` + `replayTrace` or `evaluateFresh`.

## 37. `eval-trace/cache/materialization-scope.hh`

- `struct PendingPrecomputedKey { uint32_t originOffset; PrecomputedKeysInfo info; }`.
- `class MaterializationScope` — staging container with 4 methods: `stageValueIdentity`, `stageContainerProvenance`, `stagePrecomputedKeys`, `commit`.

## 38. `eval-trace/cache/node-locator.hh`

- `struct AttrSelector { Symbol name; }`, `struct ListSelector { size_t index; }`, `using ChildSelector = std::variant<...>`, `using NodeLocator = std::vector<ChildSelector>`.

## 39. `eval-trace/cache/replay-policy.hh`

- `struct FailurePayload { const std::string * errorMessage; }`, `struct FreshOnlyPayload {}`, `struct MaterializablePayload { const CachedResult * value; }`.
- `using ReplayPolicy = std::variant<Failure, FreshOnly, Materializable>`.
- `inline ReplayPolicy classifyReplayPolicy(const CachedResult &)` (3-arm branch: Failed, Misc/Missing/Placeholder, else).

## 40. `eval-trace/cache/prefetch-pool.hh`

- `struct PrefetchToken { AttrPathId pathId; bool completed; optional<TraceStore::VerifyResult> result; }`.
- `class PrefetchPool` — `maxOutstanding` ctor; `lookup, submit, complete, remove, clear, size, maxOutstanding`.

## 41. `eval-trace/store/session-policy.{hh,cc}`

- `EvalTraceHash computePolicyDigest(const EvalSettings &)` — `CanonicalHashBuilder` domain `"eval-trace.policy-digest.settings"`, fields `pure-eval, restrict-eval, current-system, enable-import-from-derivation, nix-path-env (optional), nix-path-count + nix-path-entry* (pure-eval branches), allowed-uris-count + allowed-uri*`.
- **Parallel `computePolicyDigest(const EvalPolicySnapshot &)`** in `session-builders.cc` (anonymous-namespace), domain `"eval-policy-snapshot"`, same set of fields but with the snapshot's typed fields.

## 42. `eval-trace/store/semantic-registry.{hh,cc}`

- `class SemanticRegistry` — `entries_ (flat_map<DepSource, SourcePath>)`, `mountPoints_ (flat_map<CanonPath, vector<pair<DepSource, RegistryMountSubdir>>>)`. Private `addEntry, addMountPoint` (friended to `TraceSession`, `TraceRuntime`, `SemanticRegistryTestAccess`). Public `resolve, reverseResolve, resolvePathObject, contains, size, mountPointCount, entries`. Constructors: default + `explicit(entries flat_map)`.

## 43. `eval-trace/store/attr-vocab-store.{hh,cc}`

- `struct AttrVocabStore` — fields `symbols`, `dbPath`, private `nameTable (StringInternTable)`, `paths (vector<PathEntry>)`, `pathByKey (flat_map<uint64_t, AttrPathId>)`, `nameToSymbol`, `symbolToName`, `maxLoadedNameId`, `maxLoadedPathId`.
- Static `rootName()`, `rootPath()`.
- **2 `internName` overloads**: `(string_view)`, `(Symbol)`.
- `internPath(parent, child)`, `extendPath(parent, Symbol)`.
- `resolveName, childName, parentPath, childSymbol, displayPath`.
- `hashPath(HashSink &, AttrPathId)` — raw length-prefixed bytes via recursive feed.
- `feedPath(CanonicalHashBuilder &, AttrPathId)` — framed `dep.key.trace-context.attr-path.count` + `dep.key.trace-context.attr-path.component*`.
- `lookupName, lookupPath, flushTo, checkpoint, getDbPath, seedRootSentinels, loadFrom`.

## 44. `eval-trace/store/verification-protocol.hh`

- `enum class VerifyOutcome { Valid, ValidViaStructuralOverride, ValidViaImplicitShapeOverride, Invalid }`.
- 3 tag structs: `RecoveryUntried`, `RecoveryGitMissed`, `RecoveryDirectMissed`.

## 45. `eval-trace/store/verification-session.hh`

- `struct VerificationSession`:
  - Nested `using VerifiedTraceIds`, `InProgressTraceIds` (flat_set<TraceId>), `CurrentDepHashes` (flat_map Dep::Key → optional<DepHashValue>), `TraceContextMemoEntry { NodeStamp; optional<EvalTraceHash>; }`, `TraceContextMemo`.
  - Public fields: `verifiedTraceIds, inProgressTraceIds, traceContextMemo, parseCaches (shared_ptr)`, `storePathValid`, `gitIdentityCache`.
  - Public `lookupDepHash, depHashCount, isFileVerified`.
  - Private `cacheComputedHash(optional<ComputedHash>)` + `cacheVerifiedHash(VerifiedHash, VerifiedSubsumption)` + `verifiedContentFilesByTrace_` + `clearTraceVerifiedFiles, markFileIdentityVerified`, 2 `contentFileKey` overloads (one from Dep::Key, one from (DepSourceId, StringId)).
  - Friends: `resolveDepHash<T>` free fn template, `TraceStore`, `VerifyImpl`, `test::TraceStoreTestAccess`.

## 46. `eval-trace/store/dep-resolution-service.hh` + `.cc`

- Single template `template<TaggedDepType> std::optional<DepHashValue> resolveDepHash(EvalState &, VerificationSession &, const TaggedDepType &, const SemanticRegistry &, const InterningPools &, ParseCaches &, const StrandToken<FileStrandTag> &)`. Explicit instantiations for `CurrentTraceDep` and `CandidateDep`.
- `.cc` body: 17-arm switch on CQK with per-kind bodies. Includes `TrackedSource` abstraction (JsonSource/TomlSource/DirSource) + `navigateJson`, `navigateToml`, `resolveShapeSuffix`, `resolveDepPathCached`, `attributeStructuredFormatTime` (4-arm StructuredFormat switch), `attributeDepHashTime` (17-arm CQK switch).
- Subsumption: `if constexpr (is_same_v<Origin, CurrentTrace>)` block runs `canSubsumeShortcut` check (behavior ∈ {Structural, ImplicitStructural} + `session.isFileVerified(traceId, key)`) and on hit calls `cacheVerifiedHash(key, VerifiedHash{dep.hash}, grantVerifiedSubsumption(taggedDep))` and returns `dep.hash`.

## 47. `eval-trace/store/parse-caches.hh`

- `struct FileCacheKey { DepSourceId; FilePathId; } + Hash`.
- `struct StructuredSourceCacheKey { FileCacheKey; StructuredFormat; } + Hash`.
- `struct ParseCaches` — 6 `StrandLocal<..., FileStrandTag>` caches: `jsonDomCache, tomlDomCache, dirListingCache, nixAstCache, sourcePathCache, structuredSourceFailureCache`.

## 48. `eval-trace/store/trace-store.hh` (1116 LoC) + `store/trace-store.cc`, `trace-store-verify.cc` (1946), `trace-store-lifecycle.cc` (727), `trace-serialize.cc` (552), `trace-result-codec.cc` (551)

- Constants: `kSchemaEpoch = 24`, `kProviderEpoch = 1`.
- Tagged aliases: `SessionSourceIdentity, SessionRecoveryKey, SessionExternalRoot` (all in `eval_trace` namespace — note name collision with `nix::SessionSourceIdentity` variant in `eval-environment/session-types.hh`).
- `struct SemanticSessionKey { EvalTraceHash digest; static fromSerialized, fromDigest, toHex; }`.
- `struct SessionConfig { policyDigest, optional<graphDigest>, sourceIdentity (Tagged), externalRoots, stableRecoveryKey; buildSemanticSessionKey, semanticSessionDigest, forTest; }`.
- `template<T> class SetOnce` — `set, has_value, operator*`, throws on second set.
- `using TraceId, ResultId, DepKeySetId` — all `Tagged<Tag_, uint32_t>`.
- Origin typing machinery:
  - `dep_origin::CurrentTrace`, `dep_origin::HistoricalCandidate` — empty structs, private to `OriginScope<Origin>`.
  - `template<Origin> class OriginDep` — private ctor, friend `OriginScope<Origin>`, `value()`, `traceId() requires same_as<Origin, CurrentTrace>`. Copy-ctor deleted.
  - `struct OriginScopeFactory` — forward-declared, defined in `trace-store-verify.cc` only.
  - `template<> class OriginScope<CurrentTrace>` — private ctor friended to `OriginScopeFactory`, `tag(Dep)`.
  - `template<> class OriginScope<HistoricalCandidate>` — same.
  - Aliases `CurrentTraceScope, CandidateScope, CurrentTraceDep, CandidateDep`.
- `class VerifiedSubsumption` — private default ctor, sole friend `grantVerifiedSubsumption<O> requires same_as<O, CurrentTrace>`.
- Free fn `grantVerifiedSubsumption(const OriginDep<O> &)`.
- `struct AuthorityState {}`, `class AuthorityGate : private gdp::Certifier<AuthorityState>` with single `withAuthority<F>` static.
- Inert marker structs `TraceState {}`, `ControlContext {}` — no usages in function signatures anywhere I read.
- `class ExclusiveTraceStoreAccess` — private ctor, friend `TraceStore`, `blockingProof()`. Copy-ctor, move-ctor all deleted.
- `struct TraceStore : private gdp::Certifier<BlockingTag>`:
  - Friend `VerifyImpl`, `VerificationOrchestrator`.
  - Nested `struct State` — 21 `SQLiteStmt` members (getAllStrings, insertStringWithId, getAllDataPaths, insertDataPathWithId, getAllResults, insertResultWithId, getResult, getAllDepKeySets, insertDepKeySetWithId, getDepKeySet, getAllTraces, insertTraceWithId, lookupTraceByFullHash, getTraceInfo, lookupAttr, upsertAttr, insertHistory, lookupHistoryByTrace, lookupLatestHistoryForAttr, lookupHistoryByTraceHash, scanHistoryForAttr, lookupHistoryByGitIdentity, insertRuntimeRoot, loadRuntimeRoots, insertDirSet, getAllDirSets, insertVocabName, insertVocabPath — 28 actually).
  - Fields: `storeMutex_ (recursive)`, `_state (BlockingSync<State>)`, `symbols, pools, vocab, semanticSessionKey, stableRecoveryKey`, 3 content-addressed dedup maps (`resultByHash, depKeySetByHash, traceByFullHash`), `traceHeaderCache, traceFullCache, currentNodeIndex, resultPayloadCache, depKeySetCache, deferredTraceBlobs`, 4 ID counters, `sessionConfig (SetOnce)`, `sessionVerification (unique_ptr)`, 2 high-water marks, 3 pending-write vectors.
  - Nested structs: `CurrentNodeRef { TraceId, ResultId, NodeStamp }`, `ResultPayload`, `TraceHeader { TraceHash, DepKeySetHash, DepKeySetId }`, `PendingResult, PendingDepKeySet, PendingTrace, RuntimeRootRecord, VerifiedRuntimeRootRecord, VerifyResult { CachedResult, TraceId }, RecordResult { TraceId }, TraceHistoryEntry, RuntimeRootLoadResult, TraceKeysAndHeader { TraceHeader, shared_ptr<const vector<Dep::Key>> }, DeferredTraceBlob, ResolvedDep { source, key, expectedHash, type, optional<StructuredKey>, optional<TraceContextKey> }`.
  - Public API (counted):
    - `withExclusiveAccess<F>(bs, f)`.
    - `flushExclusive, recordRuntimeRootExclusive, loadRuntimeRootsExclusive, bulkLoadAll`.
    - Constructor, destructor, `resetVerificationSession`, `currentSemanticSessionKey, currentStableRecoveryKey`, `flush(ea)`.
    - `record(ea, pathId, value, deps)`.
    - `loadFullTrace(ea, traceId), loadKeySet(ea, depKeySetId), scanHistory(ea, pathId), loadTraceKeysAndHeader(ea, traceId)`.
    - `attrExists, getCurrentTraceHash, getCurrentNodeStamp, allocateNodeStamp, setSessionConfig`.
    - `extractGoverningRepoId, allDepsGitRecoverable, extractGitIdentityHash`.
    - 2 `serializeKeys` (static), `deserializeKeys`, `serializeValues` (static), `deserializeValues` (static).
    - `resolveDep(const Dep &)`, 2 `decodeCachedResult` overloads (`ResultPayload &`, `bs + ResultId`), `encodeCachedResult`.
    - `verifyTrace, recovery, verify`.
    - `feedKey(CanonicalHashBuilder &, Dep::Key)`.
    - `recordRuntimeRoot(ea, record, store), loadRuntimeRoots(ea, store)`.
  - Private: `withExclusiveAccessReentrancyCheckEnter/Exit` (thread_local depth map), `patchTraceHashInMemory, bulkLoadAllLocked, lookupCurrentNode, lookupLatestHistoryForAttr, loadResultPayload, ensureTraceHeader, doInternResult, getOrCreateTrace, getOrCreateDepKeySet, publishRecord, publishRecovery, publishStateChange, computeSortedTraceHash, computePresortedTraceHash`, and templates `resolveCurrentDepHash<TaggedDepType>, resolveTraceContextHash`.
- `trace-store-verify.cc`:
  - `OriginScopeFactory::{enterCurrentTrace, enterCandidate}` definitions.
  - `struct StorePathBatch` — `collect, flush`.
  - `class FileStrandGate : private gdp::Certifier<FileStrandTag>` — static `ifPassed<F>(const ExclusiveTraceStoreAccess &, F &&)` (mints a `FileStrandTag` proof from the `ea` capability).
  - `struct FileIdentity { DepSourceId; StringId; }` + 3 ctor overloads (`SimpleDepKeyId`, `FilePathId`, `StringId`) + `Hash`. Plus `scFileIdentity` and `contentFileIdentity` switch helpers.
  - `enum class DepAction { Done, CheckTraceContext, CheckNormal }`.
  - `struct VerificationState` (Pass 1 accumulator): 5 flags + 2 deferred index vectors + 2 FileIdentitySets (`failedContentFiles`, `passedContentFiles`). Methods `classifyDep, recordTraceContext, recordNormal, determineOutcome`.
  - `struct VerifyContext { ea, bs, store, pools, registry, state, session; }`.
  - `struct Pass2Result { outcome; passedContentFiles; structuralCoveredFiles; implicitCoveredFiles; uncoveredCount; }`.
  - `struct HistoryEntry { DepKeySetId; TraceId; ResultId; TraceHash; }`.
  - `using TraceHashLookup = flat_map<TraceHash, vector<size_t>>`.
  - `struct DirectHashResult { optional<VerifyResult> result; vector<Dep> currentTraceHashDeps; bool allComputable; }`.
  - `struct VerifyImpl` with nested `RecoveryAcceptance` (private-ctor wrapper around `VerifyResult`). Static methods: `runStorePathBatch, runPass1, runPass2, applyOutcome, verifyDepsExcluding, verifyDepsOnlyFor, loadHistory, loadHistoryByTraceHash, buildTraceHashLookup, lookupCandidates, contributesToTraceHash, traceHashDepCount, hasImplicitStructureGuard, extractGoverningRepoIdFromKeys, allKeysGitRecoverable, validateImplicitStructureGuards, acceptRecoveredTraceUnchecked, acceptRecoveredTraceWithLoadedDeps, acceptRecoveredTraceWithKeySet, acceptRecoveredTrace, tryGitIdentityRecovery, tryDirectHashRecovery, tryStructuralVariantRecovery`. ~25 static methods.
  - `template<State> class RecoveryState : public Linear<...>` — begin, tryGitIdentity, tryDirectHash, tryStructVariant (`&&`-qualified, requires specific `State`).

## 49. `eval-trace/store/verification-orchestrator.{hh,cc}`

- `class VerificationOrchestrator : private gdp::Certifier<VerificationAccessTag>`.
- Config `{ uint32_t maxPrefetchHints; }`, `defaultConfig`.
- Ctor(`store, blockingPool, Config`), dtor, `sessionForTest, bindSession, resetVerificationState, verifyAttr (awaitable), submitPrefetchHint, submitPrefetchHints`.
- Private `store_, blockingPool_, session_, config_, registry_ (ptr), state_ (ptr), prefetchPool_ (StrandLocal<PrefetchPool, VerificationAccessTag>)`, `verifyAttrImpl`.

## 50. `eval-trace/store/async-runtime.hh`

- `struct IocWorkerPool` RAII.
- `struct AsyncRuntime` — `Config`, fields: `ioc_, scheduler (FiberScheduler), workGuard_, iocWorkers_, blockingPool (BlockingThreadPool), orchestrator (unique_ptr<VerificationOrchestrator>)`. Ordered for destruction: orchestrator first, iocWorkers, blockingPool, ioc_ last.

## 51. `eval-trace/store/trace-serialize.{hh,cc}`

- 2 Tagged blob types: `PersistedHashBlob (std::string)`, `EncodedStorePathBlob (std::string)`.
- **9 bind helpers**: `bindBlobStringView, bindBlobVec, bindEncodedDepSourceBlob, bindRuntimeRootSource, bindRuntimeRootFetchIdentity, bindRuntimeRootNarHash, bindRuntimeRootStorePath, bindPersistedHashBlob, bindEncodedStorePathBlob`. Most are one-line wrappers.
- 2 templates `bindTaggedEvalTraceHash<Tag>`, `bindEvalTraceHash` (inline).
- Encoders: `encodePersistedHashBlob, encodeStorePathBlob`.
- Decoders: `decodePersistedHashBlob, decodeRuntimeRootSourceBlob, decodeRuntimeRootFetchIdentityBlob, decodeRuntimeRootNarHashBlob, decodeRuntimeRootStorePathBlob, decodeStorePathBlob`.
- `computeResultHash(ResultKind, encodingVersion, payload, auxContext) -> ResultHash`.
- Blob formats:
  - `SimpleDepKeyBlobEntry` (13 bytes packed), `StructuredDepKeyBlobEntry` (27 bytes), `TraceContextDepKeyBlobEntry` (5 bytes).
  - `TraceStore::serializeKeys/deserializeKeys` dispatch on isStructured/isTraceContext/simple then per-kind sub-dispatch. zstd compressed.
  - `TraceStore::serializeValues/deserializeValues` — schema-grouped blob with magic `"vals2"` + hash algorithm tag. Separate digest and string blocks (255-byte-limited strings).
- Local duplicate helpers for `readUint64`, `readFramedString`, `readFetcherAttr` (same as in `input-resolution.cc`).

## 52. `eval-trace/store/trace-store-lifecycle.cc`

- `thread_local flat_map<TraceStore *, uint32_t> heldStoreDepths` (re-entrancy detection).
- `SemanticSessionKey::fromSerialized/fromDigest`, `SessionConfig::buildSemanticSessionKey/forTest`.
- Schema (SQL string with 8 CREATE TABLE + 3 CREATE INDEX).
- `TraceStore` ctor: opens `eval-trace-v24-<algo>.sqlite`, 28 SQLiteStmt preparations, pragmas, vocab ATTACH, bulkLoadAll.
- `DirSetBlobEntry` (packed 8 bytes) + `serializeDirSet, deserializeDirSet`.
- `bulkLoadAllLocked` loads Strings, DataPaths, Results, DepKeySets, Traces, DirSets.

## 53. `eval-trace/fiber/blocking-scope.{hh,cc}`

- `struct BlockingTag {}`.
- `class BlockingThreadPool : private gdp::Certifier<BlockingTag>` — ctor(ioc, numThreads), `post<F>`, `stop`. Internal move-only type-erased `Work` wrapper.
- `template<F> coroBlock(pool, f) -> awaitable<R>` — async_initiate-based. Separate specializations for `void` and non-void R.
- `template<T> class BlockingSync` — `lock(Proof<BlockingTag>) -> WriteLock`.

## 54. `eval-trace/fiber/fiber-scheduler.{hh,cc}`

- `struct FiberEvalContext { DepRecordingContext depCtx; bool ownsTraceFrame; ctor(pools, epochLog); dtor; }`.
- Free fns `currentFiberDepCtx, currentStandaloneDepCtx`.
- `struct FiberThreadLocals` — 6 fields: `insideTask, current, currentEvalCtx, standaloneDepCtx, suspendableCtxScope, captureScope`.
- `saveThreadLocals`, `restoreThreadLocals`.
- `class FiberScheduler` — ctor(ioc), `run<F>(func, pools, epochLog)`, `insideTask (static)`, `current (static)`, `currentEvalCtx (static)`, `executor`, `onOwnerThread`, `ownerThreadId`. 3 private `thread_local` statics (`insideTask_, current_, currentEvalCtx_`).
- File-static `thread_local standaloneDepCtx_`.
- `struct FiberTraceFrameScope` + `static thread_local optional<FiberTraceFrameScope> fiberTraceFrameStorage` — RAII push/pop around `TraceFrame::swapCurrent`.
- `SuspendableCtxScope::{snapshotPointer, restorePointer}`, ctor, dtor.
- `StandaloneDepCtxGuard` ctor/dtor.

## 55. `eval-environment/domains.hh`

- 18 `Tagged<Tag_, ...>` aliases: `FlakeSourceIdentity, StableRecoveryKey, ExternalRootIdentity, FileEvalAbsoluteFilePath, FileEvalExpressionHash, FileEvalLogicalSourceIdentity, FileEvalAutoArgsHash, StoreDirIdentity, SessionCurrentSystem, FlakeGraphNodeKey, FlakeInputSubdir, LookupPathPrefix, LookupPathRawValue, FlakeSessionReuseKey, FileEvalSessionReuseKeyValue, OriginalFetchInput, ResolvedLockedInput, FinalizedLockedInput`. Stream operator for `FlakeGraphNodeKey`.

## 56. `eval-environment/request-types.hh`

- 5 enums: `PathObservationMode, StorePathPublicationMode, UriPolicyScope, LookupPathAccessControlMode, StringRealisationMode`.
- **14 request structs**: `CoercedPathRequest, PathObservationRequest, ReadFileRequest, ReadDirectoryRequest, StorePathPublishRequest, CopyPathToStoreRequest, AuthorizedStorePathRenderRequest (with 3 static factories), LookupPathRequest, UriPolicyRequest, RealiseContextRequest, FetchIdentityRequest, EnsureMountedStorePathRequest, GitIdentityRequest, DerivedStorePathRequest`.

## 57. `eval-environment/observation-types.hh`

- 13 observation structs: `EnvVarObservation, LookupPathObservation (with MatchedResolution/CorepkgsFallbackResolution), UriPolicyObservation, ContextRealisationObservation, PathStatusObservation, FileReadObservation, DirectoryReadObservation, StorePathObservation, StoreClosureObservation, RuntimeFetchIdentityObservation, GitIdentityObservation, DerivedStorePathObservation, SessionSystemObservation`.
- `enum class LookupPathOrigin { Existing, Downloaded, HookResolved, Missing }`.
- 2 conditional-field structs: `LookupPathResolvedRootFields`, `LookupPathMaterializedField`.
- `template<O> struct LookupPathResolution : ConditionalBase<...>` + 4 aliases.
- `using LookupPathResolvedEntry = std::variant<Existing, Downloaded, HookResolved, Missing>`.
- `class PublishedStorePathString` — 3 private variant alternatives, 3 static factories (`preserve, detach, plain`), 4 accessors.
- `class ResolvedFetchIdentity : public Linear<...>` — `MaterializationPayload { FetchIdentityRequest; ResolvedLockedInput; ref<SourceAccessor>; optional<string> materializationFingerprint; }`.
- `struct FetchIdentityResolution { fetchers::Input resolvedInput; Attrs extraAttrs; SourcePath phase1Root; ResolvedFetchIdentity identity; }`.
- `struct RuntimeRootCandidate { DepSource; RuntimeFetchIdentityDepKey; RuntimeRootNarHash; RuntimeRootStorePath; }`.
- `class FetchedInput : public Linear<...>` — `MountPayload { OriginalFetchInput; ResolvedLockedInput; ref<SourceAccessor>; optional<string> materializationFingerprint; }`.
- `struct MountedStorePath { StorePath; optional<PathObject>; }`, `using DetachedMountedStorePath = MountedStorePath`.
- `enum class MountMode : uint8_t { DetachedStandalone, DetachedGraph, BoundLocked, BoundUnlocked }`.
- 3 field structs: `MountedInputGraphField, MountedInputNarHashField, MountedInputFinalizedLockedInputField`.
- `namespace detail { template<M> using MountedInputLinearityBase = conditional_t<DetachedStandalone, MoveOnly, Linear<...>>; template<M> class MountedInput; }`.
- 4 aliases (`DetachedStandaloneMountedInput, DetachedGraphMountedInput, BoundLockedMountedInput, BoundUnlockedMountedInput`).
- `struct GraphFetchCompletion : MountedStorePath` with `promotedGraphSource` field.
- `enum class RuntimeFetchLockMode : uint8_t { Locked, Unlocked }`.
- 2 field structs: `LockedRuntimeFetchField { RuntimeRootCandidate; }`, `UnlockedRuntimeFetchField { DepSource; }`.
- `namespace detail { template<L> class PublishedRuntimeFetch; }`. Always-present `FinalizedLockedInput lockedInput`. `runtimeSource()` method branches on `L`.
- 2 aliases, `RuntimeFetchResult = variant<2>`.
- ~30 `static_assert(is_base_of_v<...>)` invariant checks.

## 58. `eval-environment/capabilities.hh`

- `struct ObserveOnlyTag` + `constexpr observeOnly` instance.
- `enum class EffectMode : uint8_t { Detached, Bound }`.
- `struct BoundSessionField { ref<eval_trace::TraceSession> session_; }`.
- `template<E> class EffectScope : MoveOnly, ConditionalBase<E==Bound, BoundSessionField>` — private ctors, 2 `friend EvalEnvironment`. Aliases `DetachedEffectScope, BoundEffectScope`.
- 2 enums: `LookupPathEntryDetail {Identity, Full}`, `LookupPathRealization {Unrealized, Realized}`.
- `struct LookupPathEntryContextFields { NixStringContext; optional<PathObject>; }`.
- `template<D, R> class LookupPathEntry : ConditionalBase<D==Full, LookupPathEntryContextFields>` — private default ctor, cross-instantiation friend. Methods `toIdentity, realize, realize(raw)`. Public fields `prefix, rawValue`.
- Free fn `buildLookupPathEntrySpec(contextFields, prefix, rawValue)`.
- 3 aliases: `UnrealizedFullLookupPathEntry, UnrealizedLookupPathIdentity, RealizedLookupPathIdentity`.
- `struct EvalEnvironmentAuthority` — 13 fields: `evalState, store, buildStore, fetchSettings, evalSettings, repair, storeFS, rootFS, corepkgsFS, internalFS, inputCache, lookupPathHookResolver, traceSessionFactory, sharedState`.
- `class RootLoaderHolder` — virtual `loadRoot()`.
- `class RootLoaderCapability : public Linear<...>` — `create, discardUnused, intoRootLoader`.

## 59. `eval-environment/session-types.hh`

- `using SessionSourceIdentity = variant<FlakeSourceIdentity, CurrentGitIdentityHash, FileEvalLogicalSourceIdentity>`. **Name collision with `eval_trace::SessionSourceIdentity` (which is a Tagged<Tag, EvalTraceHash>).** `toTraceSessionConfig` (in session-builders.cc) converts one to the other by visiting.
- 4 enums: `EvalPurityMode, EvalRestrictionMode, ImportFromDerivationMode, FileEvalReuseKeyUncacheableReason`.
- Structs: `EvalTraceInputAccessorBinding, EvalTraceMountBinding, EvalTraceSessionConfigInput, EvalPolicySnapshot (sealed ctor, 9 fields, friend EvalEnvironment)`, `FlakeTraceSessionConfigRequest, FileEvalGitIdentitySnapshot, NoSessionReuseRequested, FileEvalTraceSessionReuseKey, FileEvalTraceSessionReuseKeyInputs, RootLoadDepObservation, FlakeGraphAuthorityNodeSpec, TraceSessionReuseSlotKey`.
- `using FlakeSessionReuseDecision = variant<NoSessionReuseRequested, FlakeSessionReuseKey>`, `FileEvalReuseSource = variant<FileEvalAbsoluteFilePath, FileEvalExpressionHash, monostate>`, `FileEvalSessionReuseDecision = variant<NoSessionReuseRequested, FileEvalTraceSessionReuseKey, FileEvalReuseKeyUncacheableReason>`, `FlakeEvalSessionOpen = tuple<EvalTraceSessionAuthority, FlakeSessionReuseDecision>`, `FileEvalSessionOpen = tuple<EvalTraceSessionAuthority, FileEvalSessionReuseDecision>`.
- 4 Linear carrier types: `CommonTraceSessionAuthorityInputs, FlakeGraphTraceSessionAuthorityRequest, CapturedSessionOpenInputs, EvalTraceSessionAuthority`. Each has `AssemblyPayload` / `OpenPayload` nested struct, private ctor, narrow friend list, `create(...)`, `consumeForAssembly() &&` or `consumeForTraceSessionOpen() &&`.
- 2 Tagged aliases `EvalTraceFlakeEvaluationRootPath, EvalTraceFlakeCarrierRootPath`.

## 60. `eval-environment/environment.hh`

- `class EvalEnvironment final` — `shared_ptr<void> pImpl`.
- Lifecycle (6): ctor, dtor, `openDetachedEffectScope, tryBindCurrentEvalSession, initializeLookupPathAccessControl, captureSessionOpenInputs`.
- Session open (2 + 1): `openEvalSession(FlakeEvalSessionOpen), openEvalSession(FileEvalSessionOpen), traceSession(BoundEffectScope)`.
- **Auto-dispatch public overloads (13 methods)**: `readEnvVar, resolveLookupPath, authorizeUri, realiseContext, observePath, readFile, readDirectory, publishStorePath, copyPathToStore, authorizeStorePath, authorizeStoreClosure, renderAuthorizedStorePath, observeSessionSystem`.
- **Observe-only overloads (8)**: `readEnvVar(ObserveOnlyTag), observePath, readFile, readDirectory, observeRuntimeFetchIdentity, observeGitIdentity, observeDerivedStorePath, observeSessionSystem`.
- **Detached overloads (17 public + 2 Bound variants shared for `resolveFetchIdentity` and `materializeFetch`)**: `readEnvVar, resolveLookupPath, authorizeUri, realiseContext, observePath, readFile, readDirectory, publishStorePath, copyPathToStore, authorizeStorePath, authorizeStoreClosure, renderAuthorizedStorePath, resolveFetchIdentity (Detached+Bound), materializeFetch (Detached+Bound), ensureMountedStorePath, mountFetchedInput, mountGraphFetchedInput, completeGraphFetch, mountAndCompleteRuntimeFetch (Bound), observeGitIdentity`.
- **Private Bound overloads (15)**: `readEnvVar, resolveLookupPath, authorizeUri, realiseContext, observePath, readFile, readDirectory, publishStorePath, copyPathToStore, authorizeStorePath, authorizeStoreClosure, renderAuthorizedStorePath, observeGitIdentity, observeDerivedStorePath, observeSessionSystem`.
- **Private TraceAccess overloads (12)**: `observePath, readFile, readDirectory, realiseContext, publishStorePath, copyPathToStore, observeSessionSystem, readEnvVar, resolveLookupPath, authorizeStorePath, authorizeStoreClosure, renderAuthorizedStorePath`.
- Private `completeLockedRuntimeFetch, completeUnlockedRuntimeFetch, openEvalSessionImpl<SessionReuse>, makePublishedStorePathString (static)`.

Total: **~80 methods / overloads** (with the 5 `readEnvVar` overloads alone summing all 4 dispatch tiers + observeOnly).

## 61. `eval-environment/request-builder.hh` + `.cc`

- `class EvalRequestBuilder final` — 14 `build*Request` methods:
  - `buildCoercedPathRequest`, `buildPathObservationRequest`, `buildReadFileRequest`, `buildReadDirectoryRequest`, `buildStorePathPublishRequest`.
  - 3 `buildLookupPathRequest` overloads (string_view / const LookupPath & + string_view / PosIdx + Value &).
  - `buildUriPolicyRequest`, `buildRealiseContextRequest`.
  - 2 `buildGitIdentityRequest` overloads (Value & coerced / SourcePath + origin).
  - `buildDerivedStorePathRequest`.
  - `buildFetchIdentityRequest`, `buildEnsureMountedStorePathRequest`.

## 62. `eval-environment.hh` top-level + `authority.cc` + `session-builders.cc`

- Free fns: `makeDetachedEvalEnvironmentAuthority, makeSessionEvalEnvironmentAuthority, clearEvalEnvironmentState, releaseSessionEvalEnvironmentState`.
- `allowPath(EvalEnvironmentAuthority &, StorePath), allowClosure(EvalEnvironmentAuthority &, StorePath)` (free fns, not methods).
- `toTraceSessionConfig(EvalTraceSessionConfigInput) -> eval_trace::SessionConfig`.
- `buildFileEvalSessionConfigInputs(GitIdentityObservation) -> optional<FileEvalGitIdentitySnapshot>`.
- `assembleFlakeTraceSessionOpen, assembleFileEvalTraceSessionOpen`.
- `copyPathToStoreViaEvalEnvironment, realiseStringViaEvalEnvironment`.
- `class TraceSessionFactory` (abstract) — `openTraceSession(optional<TraceSessionReuseSlotKey>, EvalTraceSessionAuthority)`.

`authority.cc` defines (file-static, anonymous namespace):
- `semanticSessionDigest(const optional<EvalTraceSessionConfigInput> &)` — 2-line helper.
- `buildBackendFingerprint(optional<EvalTraceHash>)` — 5 lines.
- `toRootLoadDeps(vector<RootLoadDepObservation>)` — 7 lines.
- `makeTraceSession(state, OpenPayload, semanticDigest)` — the actual ctor-dispatch with the `assert(!backendFingerprint == !sessionConfig)` production invariant.
- `class EvalStateTraceSessionFactory final : public TraceSessionFactory` — holds `flat_map<TraceSessionReuseSlotKey, CacheEntry>` keyed session cache; `openTraceSession` releases old backend if semantic digest changed (only for file-eval `trackBackend` slots, not flake).
- `sessionFactoryMutex`, `sessionFactories` (EvalState * → factory) singletons.
- `makeBaseEvalEnvironmentAuthority`, `getOrCreateSessionFactory`.

`session-builders.cc` defines:
- Anonymous-namespace `computePolicyDigest(const EvalPolicySnapshot &)` — parallel to `session-policy.cc`'s `computePolicyDigest(const EvalSettings &)` but in a different namespace.
- `computeFileEvalLogicalIdentityHash(domain, request, lookupPathEntries, optional policyDigest)` — core hash used 3x with different domains.
- `buildFileEvalLogicalSourceIdentity`, `buildFileEvalStableRecoveryKey`.
- `toTraceSessionConfig(EvalTraceSessionConfigInput)`.
- `buildFlakeSessionConfig, buildFileEvalSessionConfig, buildFlakeSessionReuseKey, buildFileEvalSessionReuseKey`.
- `assembleFlakeTraceSessionOpen, assembleFileEvalTraceSessionOpen`.

## 63. Integration: `eval.cc` (4838 LoC) and `flake.cc` (2886 LoC)

- `eval.cc`:
  - `EvalEnvironmentSharedState` ctor initializes 5 caches (`inputCache, srcToStore, importResolutionCache, fileTraceCache, fileContentHashCache, lookupPathResolved`).
  - `getOrStoreFileContentHash`, `getOrReadFileContentHash` free fns (memoize depHash in the shared cache).
  - `copyPathToStoreViaEvalEnvironment` (handled `isDerivation`; fast path for store-rooted paths).
  - `realiseStringViaEvalEnvironment`.
  - `readFileViaEvalEnvironment` (file-static).
  - `EvalState` ctor — sets `eval_trace::setEvalTraceHashAlgorithm(settings.evalTraceHashAlgorithm)`; if `useTraceCache`, allocates `TraceRuntime`.
  - `EvalState` dtor calls `releaseSessionEvalEnvironmentState`.
  - `struct ExprParseFile : Expr, gc` — has the `ExprParseFile::eval` hook that calls `registerNixBindings` + `maybeRecordImportedFileContent` under `traceActiveDepth`.
  - `struct PublicationWarmupScope : private gdp::Certifier<DepCaptureScopeTag>` — pushes/pops a scope, `close, mergeIntoParent, discard`. Used by `coerceToContextObjectForUnsafeDiscard`.
  - Semantic handle API on `EvalState`: `lookupSemanticHandle, setSemanticHandle, mergeSemanticHandle`. Plus `publishPathProvenance, publishTextProvenance, publishStructuredProvenance, mkStorePathStringWithProvenance, mkOutputStringWithProvenance, mkSingleDerivedPathStringWithProvenance`.
  - `coerceToStringWithProvenance`, `coerceToContextObject`, `coerceToCoercedPath`, `publishContextObject`, `publishCoercedPath`, `coerceToContextObjectForUnsafeDiscard`, `captureContextObject`, `captureCoercedPath`, `capturePathWithObject`.
  - `forceThunkValue` — 3 versions: `Critical` (skip TracedExpr), `Suspendable` (bridge via ReplayPublishScope or SiblingForceScope), and the backward-compat non-colored bridge (same body as Suspendable).
  - `SiblingForceScope` ctor/commit.
  - ExprOpUpdate uses `DerivedContainerBuilder` on alias paths (when one operand is empty).
  - `concatLists` — intentionally does NOT use DerivedContainerBuilder (set union with sum length — not shape-preserving).
  - `ExprConcatStrings` — handles path provenance with "ambiguous" detection across sources.
  - 75+ `maybeRecord*` call sites to ExprSelect/ExprOpHasAttr/callFunction/eqValues/assertEqValues/forceListObserved/coerceToString*.
  - `EvalState::printStatistics` builds the `evalTrace.*` JSON tree (section 28 maps 1:1 to this).
  - TraceRuntime accessors on EvalState: `makeTraceBackend, tracingPools, replayEpochLog, vocabStore, registerTracedValueIdentity, registerTracedBindingsValueIdentity, rememberNixBinding, lookupNixBinding, registerMaterializedValueIdentity, publishRootMaterializedValueIdentity, rollbackRootMaterializedValueIdentity, lookupValueIdentityStamp, hasValueIdentityForTest, hasBindingsValueIdentityForTest, recordThunkDeps, rollbackReplayEpoch, replayMemoizedDeps, mayHaveMemoizedDeps, sameValueIdentity`.

- `flake.cc`:
  - Uses `eval_trace::AuthorityGate::withAuthority(...)` to scope `lockFlake`. The `gdp::Proof<AuthorityState>` is threaded through `ensureMountedStorePath`, `tryMountLockedInputRoot`, `buildResolvedFlakeGraph` — but per the doc comment on `AuthorityState` in the header, `mountInput` itself does NOT require the proof (phase-2 callers like `prim_fetchTree` and `getFlake` need to call it).
  - `depSourceForFlakeGraphNode` wrapper around `DepSource::fromNodeKey`.
  - `computeLockedVersionIdentity` uses `CanonicalHashBuilder` with domain `"flake-locked-version-identity"` + 5-arm discrimination (relative-path / git-rev / dirty-rev / stable-identity / fingerprint) + `addFetcherAttrsFlake` fallback.
  - `openTraceCache` — the flake-side entry point. Builds `FlakeGraphTraceSessionAuthorityRequest` via `buildFlakeAuthorityNodeSpecs` (from `eval-trace-session-open-adapter.hh`), captures session inputs, calls `assembleFlakeTraceSessionOpen` and `openEvalSession`, returns `environment.traceSession(session)`.
  - `LockedFlake::getFingerprint` uses `CanonicalHashBuilder` domain `"locked-flake-fingerprint"` with ~17 per-node fields (input-fingerprint, locked-subdir, lock-file-unlocked, lock-file, resolved-graph-root-key, one field per resolved-graph-node-* property, rev-count, last-modified).
  - `callFlake` is phase-2-only; reads no lock-file, performs no fetches.
  - `buildResolvedFlakeGraphValue` emits `outPath` with `PathObject{source=depSourceForFlakeGraphNode(key), rootPath=logicalRootPath}` via `publishPathProvenance`.
