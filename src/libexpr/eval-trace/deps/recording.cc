#include "nix/expr/eval-trace/deps/recording.hh"
#include "root-tracker-scope.hh"
#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/util/logging.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace {
Counter nrDepTrackerScopes;
Counter nrExcludeChildRangeCalls;
} // namespace nix::eval_trace

namespace nix {

// ═══════════════════════════════════════════════════════════════════════
// Eval trace state — lifetime documentation
// ═══════════════════════════════════════════════════════════════════════
//
// The eval trace system uses state at three lifetime scopes:
//
// ┌─────────────────────────────────────────────────────────────────────┐
// │ LIFETIME 1: Thread-local recording state                           │
// │                                                                    │
// │ Thread-local variables for the single-threaded dep recording       │
// │ machinery. These are independent of any particular EvalState.      │
// │                                                                    │
// │ Members:                                                           │
// │   - sessionTraces      (append-only Dep vector,                    │
// │                          index-addressed by DepRange)              │
// │   - activeTracker      (current RAII tracker in the call stack)    │
// │   - depth              (nesting depth of live trackers)            │
// ├─────────────────────────────────────────────────────────────────────┤
// │ LIFETIME 2: Per root DependencyTracker  (owned by RootTrackerScope)│
// │                                                                    │
// │ Caches valid only for a single root tracker scope. Owned as fields │
// │ of RootTrackerScope, created at depth 0→1 and destroyed at 1→0.   │
// │ These use Value*/Bindings* pointers as keys, which are only        │
// │ valid within a single evaluation's GC heap generation.             │
// │ RootTrackerScope::current provides access from shape dep helpers.  │
// │                                                                    │
// │ Fields (all in RootTrackerScope):                                  │
// │   - tracedContainerMap   (Value* → list provenance)                │
// │   - provenancePool       (stable deque of TracedContainerProv.)    │
// │   - readFileProvenanceMap (content hash → ReadFileProvenance)      │
// │   - readFileStringPtrs   (char* → content hash for RawContent)    │
// │   - scannedBindings      (Bindings* memoization for #keys)         │
// │   - precomputedKeysMap   (origin offset → precomputed hash)        │
// │   - intersectOriginsCache (Bindings* → origin info for //∩)        │
// │   - dirSetHashCache     (sorted dir-set → BLAKE3 hex hash)         │
// ├─────────────────────────────────────────────────────────────────────┤
// │ LIFETIME 3: Per EvalState  (owned by EvalTraceContext)             │
// │                                                                    │
// │ State tied to a specific evaluator instance. Destroyed with the    │
// │ EvalState. Not thread-local — owned as a member of EvalState.      │
// │                                                                    │
// │ Members (in EvalTraceContext):                                     │
// │   - pools              (InterningPools: depSource, filePath,       │
// │                          dataPath, depKey pools + provenanceTable)  │
// │   - epochMap           (Value* → dep range for thunk memoization)  │
// │   - replayBloom        (fast rejection filter for epochMap)        │
// │   - evalCaches         (flake hash → TraceCache instances)         │
// │   - fileContentHashes  (SourcePath → BLAKE3 for Content deps)     │
// │   - mountToInput       (mount point → (inputName, subdir))         │
// │   - skipEpochRecordFor (TracedExpr anti-contamination guard)       │
// └─────────────────────────────────────────────────────────────────────┘
//
// The StatHashStore singleton (stat-hash-store.hh/cc) is a fourth scope
// (process-global, not thread-local, not per-EvalState). It caches
// (path,depType) → (FileFingerprint, BLAKE3) mappings and is shared across
// all evaluators and threads.

// ═══════════════════════════════════════════════════════════════════════
// DependencyTracker — RAII dep recording (Adapton DDG builder)
// ═══════════════════════════════════════════════════════════════════════

// [Lifetime 1] Per-thread active dependency tracker pointer.
thread_local DependencyTracker * DependencyTracker::activeTracker = nullptr;
// [Lifetime 1] Per-thread append-only dep vector. Index-addressed by
// DepRange; never shrinks in production (only reserved, not cleared).
thread_local std::vector<Dep> DependencyTracker::sessionTraces;
// [Lifetime 1] Nesting depth of live DependencyTracker instances.
thread_local uint32_t DependencyTracker::depth = 0;

// ═══════════════════════════════════════════════════════════════════════
// DataPath JSON helpers
// ═══════════════════════════════════════════════════════════════════════

static nlohmann::json dataPathToJsonArray(const DataPathPool & pool, DataPathId nodeId) {
    nlohmann::json arr = nlohmann::json::array();
    for (auto & node : pool.collectPath(nodeId)) {
        if (node.arrayIndex >= 0)
            arr.push_back(node.arrayIndex);
        else
            arr.push_back(node.component);
    }
    return arr;
}

std::string dataPathToJsonString(InterningPools & pools, DataPathId nodeId) {
    return dataPathToJsonArray(pools.dataPathPool, nodeId).dump();
}

DataPathId jsonStringToDataPathId(InterningPools & pools, std::string_view jsonStr) {
    auto arr = nlohmann::json::parse(jsonStr);
    DataPathId id = pools.dataPathPool.root();
    for (auto & elem : arr) {
        if (elem.is_number())
            id = pools.dataPathPool.internArrayChild(id, elem.get<int32_t>());
        else {
            auto s = elem.get<std::string>();
            id = pools.dataPathPool.internChild(id, s);
        }
    }
    return id;
}

Pos::ProvenanceRef allocateProvenanceRef(
    InterningPools & pools, DepSourceId srcId, FilePathId fpId, DataPathId dpId, StructuredFormat format)
{
    return Pos::ProvenanceRef{pools.provenanceTable.allocate(srcId, fpId, dpId, format)};
}

const ProvenanceRecord & resolveProvenanceRef(InterningPools & pools, const Pos::ProvenanceRef & ref)
{
    return pools.provenanceTable.resolve(ref.id);
}

// ═══════════════════════════════════════════════════════════════════════
// RootTrackerScope — thread_local definitions and root lifecycle
// ═══════════════════════════════════════════════════════════════════════

thread_local RootTrackerScope * RootTrackerScope::current = nullptr;

// Storage for the active RootTrackerScope. emplaced at depth 0→1, reset at 1→0.
static thread_local std::optional<RootTrackerScope> rootScopeStorage;

void DependencyTracker::onRootConstruction()
{
    rootScopeStorage.emplace();
    // Lifetime 1 state (pools, sessionTraces) is NOT cleared here.
    sessionTraces.reserve(16384);
}

void DependencyTracker::onRootDestruction()
{
    rootScopeStorage.reset();
}

// ═══════════════════════════════════════════════════════════════════════
// Lifetime 2 accessors
// ═══════════════════════════════════════════════════════════════════════

ProvenanceRef allocateProvenance(DepSourceId sourceId, FilePathId filePathId,
                                 DataPathId dataPathId, StructuredFormat format)
{
    auto * scope = RootTrackerScope::current;
    if (!scope) return nullptr;
    scope->provenancePool.emplace_back(TracedContainerProvenance{sourceId, filePathId, dataPathId, format});
    return &scope->provenancePool.back();
}

void registerTracedContainer(const void * key, const TracedContainerProvenance * prov)
{
    auto * scope = RootTrackerScope::current;
    if (scope) scope->tracedContainerMap.emplace(key, prov);
}

const TracedContainerProvenance * lookupTracedContainer(const void * key)
{
    auto * scope = RootTrackerScope::current;
    if (!scope) return nullptr;
    auto it = scope->tracedContainerMap.find(key);
    return it != scope->tracedContainerMap.end() ? it->second : nullptr;
}

void clearTracedContainerMap()
{
    auto * scope = RootTrackerScope::current;
    if (scope) scope->tracedContainerMap.clear();
}

void registerPrecomputedKeys(uint32_t originOffset, PrecomputedKeysInfo info)
{
    auto * scope = RootTrackerScope::current;
    if (scope) scope->precomputedKeysMap.emplace(originOffset, std::move(info));
}

void clearPrecomputedKeysMap()
{
    auto * scope = RootTrackerScope::current;
    if (scope) scope->precomputedKeysMap.clear();
}

// ═══════════════════════════════════════════════════════════════════════
// DependencyTracker::record / recordReplay
// ═══════════════════════════════════════════════════════════════════════

// Record a non-StructuredContent dependency edge (Adapton: "add-edge").
// BSàlC §3.2: during fresh evaluation, the scheduler records each dependency
// into the trace. Hash-based dedup: >90% of record() calls are duplicates,
// so hashing the (type, source, key) triple avoids constructing DepKey strings.
void DependencyTracker::record(InterningPools & pools, DepType type,
                               std::string_view source, std::string_view key,
                               DepHashValue hash)
{
    if (activeTracker) {
        uint64_t h = hashValues(std::to_underlying(type), source, key);
        if (!activeTracker->depDedup.tryInsert(h))
            return;
    }
    debug("recording %s (%s) dep: input='%s' key='%s'",
        depTypeName(type), depKindName(depKind(type)), source, key);
    sessionTraces.push_back(Dep{
        {type, pools.intern<DepSourceId>(source), pools.intern<DepKeyId>(key)},
        std::move(hash)});
}

void DependencyTracker::record(const Dep & dep)
{
    if (activeTracker) {
        uint64_t h = hashValues(std::to_underlying(dep.key.type), dep.key.sourceId.value, dep.key.keyId.value);
        if (!activeTracker->depDedup.tryInsert(h))
            return;
    }
    sessionTraces.push_back(dep);
}

// ═══════════════════════════════════════════════════════════════════════
// recordStructuredDep — zero-allocation dedup + JSON serialization
// ═══════════════════════════════════════════════════════════════════════

[[gnu::cold]] bool recordStructuredDep(
    InterningPools & pools,
    const CompactDepComponents & c,
    const DepHashValue & hash,
    DepType depType)
{
    // 1. Hash integer components — zero allocation
    uint64_t h = hashValues(
        uint8_t(depType), c.sourceId.value, c.filePathId.value,
        uint8_t(c.format), c.dataPathId.value, uint8_t(c.suffix),
        c.hasKey.getId());
    if (DependencyTracker::activeTracker
        && !DependencyTracker::activeTracker->depDedup.tryInsert(h))
        return false;  // Duplicate — no work done

    // 2. Only for non-duplicates: build JSON dep key
    assert(pools.sessionSymbols && "sessionSymbols must be set before recordStructuredDep()");
    nlohmann::json key;
    key["f"] = std::string(pools.filePathPool.resolve(c.filePathId));
    key["t"] = std::string(1, structuredFormatChar(c.format));
    key["p"] = dataPathToJsonArray(pools.dataPathPool, c.dataPathId);
    if (c.hasKey)
        key["h"] = std::string((*pools.sessionSymbols)[c.hasKey]);
    else if (c.suffix != ShapeSuffix::None)
        key["s"] = std::string(shapeSuffixName(c.suffix));

    auto keyStr = key.dump();
    DependencyTracker::sessionTraces.push_back(
        Dep{{depType, c.sourceId, pools.intern<DepKeyId>(keyStr)}, hash});
    return true;
}

// Replay a child's cached dependency into the session trace without touching the
// active tracker's dedup filter (depDedup). This prevents parent dedup
// pollution: the child's deps land in an excluded range in sessionTraces (so
// they're skipped by collectTraces), but if record() were used instead, the
// parent's depDedup would reject a later independent recording of the same
// dep. recordReplay avoids this by only appending to sessionTraces — the dep
// still participates in thunk epoch ranges (recordThunkDeps) for correct
// transitive propagation.
void DependencyTracker::recordReplay(InterningPools & pools, DepType type,
                                     std::string_view source, std::string_view key,
                                     const DepHashValue & hash)
{
    sessionTraces.push_back(Dep{
        {type, pools.intern<DepSourceId>(source), pools.intern<DepKeyId>(key)},
        hash});
}

void DependencyTracker::recordReplay(const Dep & dep)
{
    sessionTraces.push_back(dep);
}

// ═══════════════════════════════════════════════════════════════════════
// collectTraces / clearSessionTraces
// ═══════════════════════════════════════════════════════════════════════

// Collect the complete trace for this evaluation scope (BSàlC §3.1: a trace
// is the ordered sequence of (key, hash) pairs observed during evaluation).
// Combines two sources:
//   1. Directly recorded deps from this scope [startIndex, endIndex)
//   2. Replayed deps from previously-verified thunks (Adapton: "demanded
//      computations" whose cached traces are transitively included)
// When excludedChildRanges is non-empty, session deps within those ranges
// are skipped, and replayed ranges fully contained within an excluded range
// are filtered out. This prevents parent TracedExpr traces from inheriting
// children's deps (children have their own traces + ParentContext deps).
// The result is the flattened dependency vector for trace storage.
std::vector<Dep> DependencyTracker::collectTraces() const
{
    uint32_t endIndex = mySessionTraces->size();

    if (excludedChildRanges.empty()) {
        // Fast path: no child exclusions needed
        if (replayedRanges.empty())
            return {mySessionTraces->begin() + startIndex, mySessionTraces->begin() + endIndex};

        std::vector<Dep> result(mySessionTraces->begin() + startIndex, mySessionTraces->begin() + endIndex);
        for (auto & r : replayedRanges)
            result.insert(result.end(), r.deps->begin() + r.start, r.deps->begin() + r.end);
        return result;
    }

    // Slow path: skip excluded child ranges to prevent parent traces
    // from inheriting children's dependencies.
    std::vector<Dep> result;

    // Copy session deps [startIndex, endIndex) skipping excluded ranges
    uint32_t pos = startIndex;
    for (auto & [exStart, exEnd] : excludedChildRanges) {
        uint32_t s = std::max(exStart, startIndex);
        uint32_t e = std::min(exEnd, endIndex);
        if (s > pos)
            result.insert(result.end(), mySessionTraces->begin() + pos, mySessionTraces->begin() + s);
        if (e > pos)
            pos = e;
    }
    if (pos < endIndex)
        result.insert(result.end(), mySessionTraces->begin() + pos, mySessionTraces->begin() + endIndex);

    // Append replayed ranges, filtering out any fully within an excluded range
    for (auto & r : replayedRanges) {
        bool excluded = false;
        if (r.deps == mySessionTraces) {
            for (auto & [exStart, exEnd] : excludedChildRanges) {
                if (r.start >= exStart && r.end <= exEnd) {
                    excluded = true;
                    break;
                }
            }
        }
        if (!excluded)
            result.insert(result.end(), r.deps->begin() + r.start, r.deps->begin() + r.end);
    }

    return result;
}

// Clear the session dependency vector between evaluation sessions.
// Called when the file cache is reset.
void DependencyTracker::clearSessionTraces()
{
    sessionTraces.clear();
}

} // namespace nix
