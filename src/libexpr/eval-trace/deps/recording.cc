#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/store/stat-hash-store.hh"
#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/value.hh"
#include "nix/expr/attr-set.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/util/file-system.hh"
#include "nix/util/hash.hh"
#include "nix/util/logging.hh"
#include "nix/util/pos-table.hh"
#include "nix/util/source-path.hh"
#include "nix/util/util.hh"

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#include <nlohmann/json.hpp>

#include <deque>

namespace nix::eval_trace {
Counter nrDepTrackerScopes;
Counter nrExcludeChildRangeCalls;
} // namespace nix::eval_trace
#include <filesystem>

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

// Cached constant Blake3Hash values used in shape dep recording.
// Function-local statics avoid static initialization order issues across TUs.
static const Blake3Hash & kHashZero()   { static const auto h = depHash("0"); return h; }
static const Blake3Hash & kHashOne()    { static const auto h = depHash("1"); return h; }
static const Blake3Hash & kHashObject() { static const auto h = depHash("object"); return h; }
static const Blake3Hash & kHashArray()  { static const auto h = depHash("array"); return h; }

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
    InterningPools & pools, DepSourceId srcId, FilePathId fpId, DataPathId dpId, char format)
{
    return Pos::ProvenanceRef{pools.provenanceTable.allocate(srcId, fpId, dpId, format)};
}

const ProvenanceRecord & resolveProvenanceRef(InterningPools & pools, const Pos::ProvenanceRef & ref)
{
    return pools.provenanceTable.resolve(ref.id);
}

// ═══════════════════════════════════════════════════════════════════════
// Per-root-tracker caches — shape dep tracking
// [Lifetime 2: owned by RootTrackerScope, created/destroyed at depth 0↔1]
// ═══════════════════════════════════════════════════════════════════════

// Helper types used by RootTrackerScope fields.

struct DirSetKey {
    std::vector<std::pair<DepSourceId, FilePathId>> sorted;
    bool operator==(const DirSetKey &) const = default;
    struct Hash {
        size_t operator()(const DirSetKey & k) const noexcept {
            size_t seed = k.sorted.size();
            for (auto & [s, f] : k.sorted)
                hash_combine(seed, s.value, f.value);
            return seed;
        }
    };
};

struct IntersectOriginInfo {
    DepSourceId sourceId;
    FilePathId filePathId;
    DataPathId dataPathId;
    StructuredFormat format;
};

/**
 * RAII container for all Lifetime 2 caches. Created when a root
 * DependencyTracker is constructed (depth 0→1), destroyed when the
 * root is destroyed (depth 1→0). All caches are automatically cleared
 * by the destructor — adding a new L2 cache is just adding a field.
 *
 * A single thread_local non-owning pointer provides access from shape
 * dep free functions. Between root tracker scopes, current is nullptr.
 */
struct RootTrackerScope {
    static thread_local RootTrackerScope * current;
    RootTrackerScope * previous;

    // Value* → list provenance. Lists use this (not PosIdx).
    boost::unordered_flat_map<const void*, const TracedContainerProvenance *> tracedContainerMap;

    // Skip re-scanning the same Bindings* in maybeRecordAttrKeysDep.
    boost::unordered_flat_set<const void *> scannedBindings;

    // Stable pool for TracedContainerProvenance data (deque = no pointer invalidation).
    std::deque<TracedContainerProvenance> provenancePool;

    // Origin offset → precomputed keys hash.
    boost::unordered_flat_map<uint32_t, PrecomputedKeysInfo> precomputedKeysMap;

    // Sorted dir-set → BLAKE3 hex hash cache.
    boost::unordered_flat_map<DirSetKey, std::string, DirSetKey::Hash> dirSetHashCache;

    // Bindings* → origin info cache for intersectAttrs bulk recording.
    boost::unordered_flat_map<const Bindings *, std::vector<IntersectOriginInfo>> intersectOriginsCache;

    // Content hash → ReadFileProvenance (from prim_readFile → prim_fromJSON/fromTOML).
    boost::unordered_flat_map<Blake3Hash, ReadFileProvenance, Blake3Hash::Hasher> readFileProvenanceMap;

    // String data pointer → content hash (for RawContent dep tracking).
    boost::unordered_flat_map<const char *, Blake3Hash> readFileStringPtrs;

    RootTrackerScope() : previous(current) { current = this; }
    ~RootTrackerScope() { current = previous; }

    RootTrackerScope(const RootTrackerScope &) = delete;
    RootTrackerScope & operator=(const RootTrackerScope &) = delete;
};

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
        type,
        pools.intern<DepSourceId>(source),
        pools.intern<DepKeyId>(key),
        std::move(hash)});
}

void DependencyTracker::record(const Dep & dep)
{
    if (activeTracker) {
        uint64_t h = hashValues(std::to_underlying(dep.type), dep.sourceId.value, dep.keyId.value);
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
        Dep{depType, c.sourceId, pools.intern<DepKeyId>(keyStr), hash});
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
        type,
        pools.intern<DepSourceId>(source),
        pools.intern<DepKeyId>(key),
        hash});
}

void DependencyTracker::recordReplay(const Dep & dep)
{
    sessionTraces.push_back(dep);
}

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

// ═══════════════════════════════════════════════════════════════════════
// Dep hash functions (BLAKE3 oracle hashing)
// ═══════════════════════════════════════════════════════════════════════

Blake3Hash depHash(std::string_view data)
{
    return Blake3Hash::fromHash(hashString(HashAlgorithm::BLAKE3, data));
}

Blake3Hash depHashPath(const SourcePath & path)
{
    HashSink sink(HashAlgorithm::BLAKE3);
    path.dumpPath(sink);
    return Blake3Hash::fromHash(sink.finish().hash);
}

Blake3Hash depHashDirListing(const SourceAccessor::DirEntries & entries)
{
    HashSink sink(HashAlgorithm::BLAKE3);
    for (auto & [name, type] : entries) {
        sink(name);
        sink(":");
        int typeInt = type ? static_cast<int>(*type) : -1;
        auto typeStr = std::to_string(typeInt);
        sink(typeStr);
        sink(";");
    }
    return Blake3Hash::fromHash(sink.finish().hash);
}

// ═══════════════════════════════════════════════════════════════════════
// Input resolution + dep recording
// ═══════════════════════════════════════════════════════════════════════

// Resolve an absolute filesystem path to an (input name, relative path) pair
// by walking up the directory hierarchy and matching against the flake input
// mount table. This produces stable, relocatable dependency keys (BSàlC §3.1:
// trace keys must be deterministic and reproducible across sessions).
// The subdir prefix is stripped so that deps are relative to the input root,
// not the mount point — ensuring the same trace key regardless of where the
// input is checked out on disk.
std::optional<std::pair<std::string, CanonPath>> resolveToInput(
    const CanonPath & absPath,
    const boost::unordered_flat_map<CanonPath, std::pair<std::string, std::string>> & mountToInput)
{
    auto path = absPath;
    std::vector<std::string> subpathParts;
    while (true) {
        if (auto it = mountToInput.find(path); it != mountToInput.end()) {
            auto & [nodeKey, subdir] = it->second;
            std::reverse(subpathParts.begin(), subpathParts.end());
            CanonPath relPath = CanonPath::root;
            for (auto & part : subpathParts)
                relPath = relPath / part;
            if (!subdir.empty()) {
                auto subdirPath = CanonPath("/" + subdir);
                if (!relPath.isWithin(subdirPath))
                    return std::nullopt; // Path outside the flake's subdir — not a valid dep key
                relPath = relPath.removePrefix(subdirPath);
            }
            return {{nodeKey, relPath}};
        }
        if (path.isRoot())
            break;
        auto bn = path.baseName();
        if (bn)
            subpathParts.push_back(std::string(*bn));
        path.pop();
    }
    return std::nullopt;
}

// Record a dependency edge during fresh evaluation (BSàlC §3.2: "recording
// scheduler"). Resolves the accessed path to a stable trace key, then appends
// the dependency to the active tracker's session dependency vector.
//
// Three recording modes, depending on flake input context:
//   1. Flake input path → (inputName, relativePath) key via resolveToInput
//   2. Out-of-tree real path → ("<absolute>", absPath) key (direct FS oracle)
//   3. Non-flake mode → ("", absPath) key
//
// Virtual files (MemorySourceAccessor, e.g., call-flake.nix, fetchurl.nix)
// are silently dropped — they have no filesystem oracle to verify against
// during trace verification (BSàlC: no hash function for synthetic inputs).
//
// After recording, the stat-hash cache is populated as a side effect
// (Shake: "write-back" of computed file hashes for future early-cutoff
// verification). This MUST happen after record() succeeds — if stat caching
// throws, the dependency is still recorded, preserving trace completeness.
void recordDep(
    InterningPools & pools,
    const CanonPath & absPath,
    const DepHashValue & hash,
    DepType depType,
    const boost::unordered_flat_map<CanonPath, std::pair<std::string, std::string>> & mountToInput)
{
    bool recorded = false;
    // Single lstat — reused for both existence gating and stat-hash-cache population
    std::optional<PosixStat> fileStat;

    if (!mountToInput.empty()) {
        if (auto resolved = resolveToInput(absPath, mountToInput)) {
            DependencyTracker::record(pools, depType, resolved->first, resolved->second.abs(), hash);
            recorded = true;
            // Flake input path — no lstat needed (accessor provides content)
        } else if ((fileStat = maybeLstat(std::filesystem::path(absPath.abs())))) {
            DependencyTracker::record(pools, depType, absolutePathDep, absPath.abs(), hash);
            recorded = true;
        }
        // else: virtual file — no filesystem oracle, skip (see above)
    } else if ((fileStat = maybeLstat(std::filesystem::path(absPath.abs())))) {
        DependencyTracker::record(pools, depType, "", absPath.abs(), hash);
        recorded = true;
    }
    // else: virtual file — no filesystem oracle, skip (see above)

    // Populate stat-hash cache for hashable deps (Shake: write-back of
    // computed hashes for future early-cutoff verification checks).
    // Best-effort — failures are silently ignored to avoid disrupting evaluation.
    if (recorded
        && (depType == DepType::Content || depType == DepType::Directory || depType == DepType::NARContent))
    {
        try {
            if (auto * b3 = std::get_if<Blake3Hash>(&hash))
                eval_trace::StatHashStore::instance().storeHash(
                    std::filesystem::path(absPath.abs()), depType, *b3, fileStat);
        } catch (...) {}
    }
}

// ═══════════════════════════════════════════════════════════════════════
// ReadFile provenance threading
// ═══════════════════════════════════════════════════════════════════════

// Content hash → ReadFileProvenance (field of RootTrackerScope).
// Populated by prim_readFile, queried by prim_fromJSON/prim_fromTOML to
// enable lazy structural dep tracking. Keyed by content hash so multiple
// readFile results coexist and the same provenance can serve multiple
// fromJSON/fromTOML calls (non-consuming lookup).
void addReadFileProvenance(ReadFileProvenance prov)
{
    auto * scope = RootTrackerScope::current;
    if (scope) scope->readFileProvenanceMap.insert_or_assign(prov.contentHash, std::move(prov));
}

const ReadFileProvenance * lookupReadFileProvenance(const Blake3Hash & contentHash)
{
    auto * scope = RootTrackerScope::current;
    if (!scope) return nullptr;
    auto it = scope->readFileProvenanceMap.find(contentHash);
    return it != scope->readFileProvenanceMap.end() ? &it->second : nullptr;
}

void clearReadFileProvenanceMap()
{
    auto * scope = RootTrackerScope::current;
    if (scope) scope->readFileProvenanceMap.clear();
}

// ═══════════════════════════════════════════════════════════════════════
// ReadFile string pointer tracking for RawContent deps
// ═══════════════════════════════════════════════════════════════════════

// String data pointer → content hash (field of RootTrackerScope).
// Populated by prim_readFile, queried by string builtins that observe raw bytes.
// Key is a raw const char* — valid because readFile strings are GC-allocated
// and stable within a root tracker scope. Typically empty or very small.
void addReadFileStringPtr(const char * ptr, const Blake3Hash & contentHash)
{
    auto * scope = RootTrackerScope::current;
    if (scope) scope->readFileStringPtrs.emplace(ptr, contentHash);
}

void clearReadFileStringPtrs()
{
    auto * scope = RootTrackerScope::current;
    if (scope) scope->readFileStringPtrs.clear();
}

[[gnu::cold]] void maybeRecordRawContentDep(EvalState & state, const Value & v)
{
    if (!DependencyTracker::isActive()) return;
    if (v.type() != nString) return;
    auto * scope = RootTrackerScope::current;
    if (!scope) return;
    auto it = scope->readFileStringPtrs.find(v.c_str());
    if (it == scope->readFileStringPtrs.end()) return;
    auto * prov = lookupReadFileProvenance(it->second);
    if (!prov) return;
    auto [source, key] = resolveProvenance(prov->path, state.getMountToInput());
    DependencyTracker::record(DependencyTracker::activeTracker->pools, DepType::RawContent, source, key, DepHashValue(it->second));
}

std::pair<std::string, std::string> resolveProvenance(
    const CanonPath & absPath,
    const boost::unordered_flat_map<CanonPath, std::pair<std::string, std::string>> & mountToInput)
{
    if (!mountToInput.empty()) {
        if (auto resolved = resolveToInput(absPath, mountToInput))
            return {resolved->first, resolved->second.abs()};
        return {std::string(absolutePathDep), absPath.abs()};
    }
    return {"", absPath.abs()};
}

std::string dirEntryTypeString(std::optional<SourceAccessor::Type> type)
{
    if (!type) return "unknown";
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
    switch (*type) {
    case SourceAccessor::tRegular: return "regular";
    case SourceAccessor::tDirectory: return "directory";
    case SourceAccessor::tSymlink: return "symlink";
    default: return "unknown";
    }
#pragma GCC diagnostic pop
}

// ═══════════════════════════════════════════════════════════════════════
// Shape dep recording for traced data containers
// ═══════════════════════════════════════════════════════════════════════

/**
 * Iterate unique TracedData origins in an attrset and invoke a callback for each.
 * The callback receives the TracedData struct (interned IDs + format char).
 * Deduplicates by TracedData pointer identity. Used by maybeRecordTypeDep and
 * maybeRecordHasKeyDep (exists=false) to avoid duplicated scanning loops.
 */
template<typename Fn>
static void forEachTracedDataOrigin(const PosTable & positions, const Value & v, Fn && fn)
{
    boost::unordered_flat_set<uint32_t> seen;
    for (auto & attr : *v.attrs()) {
        if (!attr.pos.isTracedData()) continue;
        auto * origin = positions.originOfPtr(attr.pos);
        if (!origin) continue;
        auto * pr = std::get_if<Pos::ProvenanceRef>(origin);
        if (!pr) continue;
        if (!seen.insert(pr->id).second) continue;
        auto & rec = resolveProvenanceRef(DependencyTracker::activeTracker->pools, *pr);
        if (!parseStructuredFormat(rec.format)) continue;
        fn(rec);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// ═══════════════════════════════════════════════════════════════════════

[[gnu::cold]] void maybeRecordListLenDep(const Value & v)
{
    if (!DependencyTracker::isActive()) return;
    if (v.listSize() == 0) return; // Empty lists can't be tracked (no stable key)
    // Use first element Value* as key (matches registration in ExprTracedData::eval)
    auto * prov = lookupTracedContainer((const void *)v.listView()[0]);
    if (!prov) return;
    auto hash = depHash(std::to_string(v.listSize()));
    auto & pools = DependencyTracker::activeTracker->pools;
    CompactDepComponents c{prov->sourceId, prov->filePathId, prov->format, prov->dataPathId,
                           ShapeSuffix::Len, Symbol{}};
    recordStructuredDep(pools, c, DepHashValue(hash));
}

[[gnu::cold]] void maybeRecordAttrKeysDep(const PosTable & positions, const SymbolTable & symbols, const Value & v)
{
    if (!DependencyTracker::isActive()) return;
    if (v.type() != nAttrs) return;
    if (!v.attrs()->hasAnyTracedDataLayer()) return;

    // Memoize: skip if we've already scanned this exact Bindings*.
    auto * scope = RootTrackerScope::current;
    if (!scope) return;
    if (!scope->scannedBindings.insert(v.attrs()).second) return;

    // Group attrs by their TracedData origin.
    struct OriginKeys {
        const ProvenanceRecord * df;
        uint32_t originOffset;
        std::vector<std::string_view> keys;
    };
    std::vector<OriginKeys> groups;

    for (auto & attr : *v.attrs()) {
        if (!attr.pos.isTracedData()) continue;
        auto resolved = positions.resolveOriginFull(attr.pos);
        if (!resolved) continue;
        auto * pr = std::get_if<Pos::ProvenanceRef>(resolved->origin);
        if (!pr) continue;
        auto & rec = resolveProvenanceRef(DependencyTracker::activeTracker->pools, *pr);
        const ProvenanceRecord * recPtr = &rec;
        OriginKeys * group = nullptr;
        for (auto & g : groups) {
            if (g.df == recPtr) { group = &g; break; }
        }
        if (!group) {
            groups.push_back({recPtr, resolved->offset, {}});
            group = &groups.back();
        }
        group->keys.push_back(symbols[attr.name]);
    }

    auto & pools = DependencyTracker::activeTracker->pools;
    for (auto & g : groups) {
        // Fast path: if all original keys are visible (no shadowing by //),
        // use the precomputed hash from ExprTracedData::eval() creation time.
        auto pcIt = scope->precomputedKeysMap.find(g.originOffset);
        if (pcIt != scope->precomputedKeysMap.end() && pcIt->second.keyCount == g.keys.size()) {
            auto & info = pcIt->second;
            auto fmt = parseStructuredFormat(g.df->format);
            if (!fmt) continue;
            CompactDepComponents c{info.sourceId, info.filePathId, *fmt,
                                   info.dataPathId, ShapeSuffix::Keys, Symbol{}};
            recordStructuredDep(pools, c, DepHashValue(info.hash));
            continue;
        }
        // Slow path REMOVED: when keys are shadowed by //, ImplicitShape
        // (recorded at creation time with ALL source keys) handles verification.
    }
}

[[gnu::cold]] void maybeRecordTypeDep(const PosTable & positions, const Value & v)
{
    if (!DependencyTracker::isActive()) return;

    switch (v.type()) {
    case nAttrs: {
        if (!v.attrs()->hasAnyTracedDataLayer()) return;
        forEachTracedDataOrigin(positions, v, [&](const ProvenanceRecord & df) {
            auto fmt = parseStructuredFormat(df.format);
            if (!fmt) return;
            CompactDepComponents c{df.sourceId, df.filePathId, *fmt, df.dataPathId,
                                   ShapeSuffix::Type, Symbol{}};
            recordStructuredDep(DependencyTracker::activeTracker->pools, c, DepHashValue(kHashObject()));
        });
        break;
    }
    case nList: {
        if (v.listSize() == 0) return;
        auto * prov = lookupTracedContainer((const void *)v.listView()[0]);
        if (!prov) return;
        CompactDepComponents c{prov->sourceId, prov->filePathId, prov->format,
                               prov->dataPathId, ShapeSuffix::Type, Symbol{}};
        recordStructuredDep(DependencyTracker::activeTracker->pools, c, DepHashValue(kHashArray()));
        break;
    }
    case nInt:
    case nBool:
    case nString:
    case nPath:
    case nNull:
    case nFloat:
    case nThunk:
    case nFunction:
    case nExternal:
    case nFailed:
        break;
    }
}

// Forward declarations for DirSet aggregation helpers
static std::string computeDirSetHash(const std::vector<std::pair<DepSourceId, FilePathId>> & dirs);
static std::string buildAggregatedHasKeyJson(
    const std::string & dsHash, std::string_view keyName,
    const std::vector<std::pair<DepSourceId, FilePathId>> & dirs);

static const std::vector<IntersectOriginInfo> emptyOrigins;

static const std::vector<IntersectOriginInfo> & collectOriginsCached(
    const PosTable & positions, const Value & v)
{
    auto * scope = RootTrackerScope::current;
    if (!scope) return emptyOrigins;
    auto * bindings = v.attrs();
    auto [it, inserted] = scope->intersectOriginsCache.try_emplace(bindings);
    if (!inserted)
        return it->second;

    auto & result = it->second;
    boost::unordered_flat_set<uint32_t> seen;
    for (auto & attr : *bindings) {
        if (!attr.pos.isTracedData()) continue;
        auto * origin = positions.originOfPtr(attr.pos);
        if (!origin) continue;
        auto * pr = std::get_if<Pos::ProvenanceRef>(origin);
        if (!pr) continue;
        if (!seen.insert(pr->id).second) continue;
        auto & rec = resolveProvenanceRef(DependencyTracker::activeTracker->pools, *pr);
        auto fmt = parseStructuredFormat(rec.format);
        if (!fmt) continue;
        result.push_back({rec.sourceId, rec.filePathId, rec.dataPathId, *fmt});
    }
    return result;
}

[[gnu::cold]] void recordIntersectAttrsDeps(const PosTable & positions, const SymbolTable & symbols,
                                            const Value & left, const Value & right)
{
    if (!DependencyTracker::isActive()) return;

    bool leftHasData = left.type() == nAttrs && left.attrs()->hasAnyTracedDataLayer();
    bool rightHasData = right.type() == nAttrs && right.attrs()->hasAnyTracedDataLayer();
    if (!leftHasData && !rightHasData) return;

    const auto & leftOrigins = leftHasData ? collectOriginsCached(positions, left) : emptyOrigins;
    const auto & rightOrigins = rightHasData ? collectOriginsCached(positions, right) : emptyOrigins;
    if (leftOrigins.empty() && rightOrigins.empty()) return;

    auto & leftAttrs = *left.attrs();
    auto & rightAttrs = *right.attrs();

    auto & pools = DependencyTracker::activeTracker->pools;

    // Record exists=true for a single attr against its operand's origins
    auto recordExists = [&](const Attr & attr, const std::vector<IntersectOriginInfo> &) {
        if (!attr.pos.isTracedData()) return;
        auto * origin = positions.originOfPtr(attr.pos);
        if (!origin) return;
        auto * pr = std::get_if<Pos::ProvenanceRef>(origin);
        if (!pr) return;
        auto & rec = resolveProvenanceRef(pools, *pr);
        auto fmt = parseStructuredFormat(rec.format);
        if (!fmt) return;
        CompactDepComponents c{rec.sourceId, rec.filePathId, *fmt, rec.dataPathId,
                               ShapeSuffix::None, attr.name};
        recordStructuredDep(pools, c, DepHashValue(kHashOne()));
    };

    // Record exists=false, aggregating directory origins when >1
    auto recordAbsent = [&](Symbol keyName, const std::vector<IntersectOriginInfo> & origins) {
        std::vector<std::pair<DepSourceId, FilePathId>> dirOrigins;
        for (auto & oi : origins) {
            if (oi.format == StructuredFormat::Directory)
                dirOrigins.push_back({oi.sourceId, oi.filePathId});
            else {
                CompactDepComponents c{oi.sourceId, oi.filePathId, oi.format, oi.dataPathId,
                                       ShapeSuffix::None, keyName};
                recordStructuredDep(pools, c, DepHashValue(kHashZero()));
            }
        }
        if (dirOrigins.size() > 1) {
            auto dsHash = computeDirSetHash(dirOrigins);
            auto key = buildAggregatedHasKeyJson(dsHash, symbols[keyName], dirOrigins);
            DependencyTracker::record(pools, DepType::StructuredContent, "", key, DepHashValue(kHashZero()));
        } else {
            for (auto & oi : origins) {
                if (oi.format != StructuredFormat::Directory) continue;
                CompactDepComponents c{oi.sourceId, oi.filePathId, oi.format, oi.dataPathId,
                                       ShapeSuffix::None, keyName};
                recordStructuredDep(pools, c, DepHashValue(kHashZero()));
            }
        }
    };

    for (auto & l : leftAttrs) {
        auto * r = rightAttrs.get(l.name);
        if (r) {
            if (!rightOrigins.empty()) recordExists(*r, rightOrigins);
            if (!leftOrigins.empty()) recordExists(l, leftOrigins);
        } else {
            if (!rightOrigins.empty()) recordAbsent(l.name, rightOrigins);
        }
    }

    if (!leftOrigins.empty()) {
        for (auto & r : rightAttrs) {
            if (!leftAttrs.get(r.name))
                recordAbsent(r.name, leftOrigins);
        }
    }
}

// ── DirSet aggregation for has-key-miss deps ─────────────────────────

/**
 * Compute a deterministic BLAKE3 hash for a set of directory origins.
 * Sorts by (source, filePath) for determinism regardless of // operand order.
 */
[[gnu::cold]]
static std::string computeDirSetHash(const std::vector<std::pair<DepSourceId, FilePathId>> & dirs)
{
    auto & pools = DependencyTracker::activeTracker->pools;
    auto sorted = dirs;
    std::sort(sorted.begin(), sorted.end(),
        [&pools](const auto & a, const auto & b) {
            auto sa = pools.resolve(a.first);
            auto fa = pools.filePathPool.resolve(a.second);
            auto sb = pools.resolve(b.first);
            auto fb = pools.filePathPool.resolve(b.second);
            if (sa != sb) return sa < sb;
            return fa < fb;
        });

    // Cache lookup after sorting (same sorted set → same hash).
    DirSetKey cacheKey{sorted};
    auto * scope = RootTrackerScope::current;
    if (scope) {
        auto cacheIt = scope->dirSetHashCache.find(cacheKey);
        if (cacheIt != scope->dirSetHashCache.end())
            return cacheIt->second;
    }

    HashSink sink(HashAlgorithm::BLAKE3);
    for (auto & [srcId, fpId] : sorted) {
        sink(pools.resolve(srcId));
        sink(std::string_view("\0", 1));
        sink(pools.filePathPool.resolve(fpId));
        sink(std::string_view("\0", 1));
    }
    auto hash = sink.finish().hash;

    std::string hex;
    hex.reserve(64);
    for (size_t i = 0; i < hash.hashSize; i++) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", hash.hash[i]);
        hex += buf;
    }

    if (scope)
        scope->dirSetHashCache.emplace(std::move(cacheKey), hex);
    return hex;
}

/**
 * Build JSON dep key for an aggregated DirSet has-key-miss dep.
 * The dirs array is stored once in pools.dirSets (keyed by dsHash)
 * and persisted to the DirSets table by TraceStore::flush().
 * The dep key itself is compact (~100 bytes) with no embedded dirs.
 */
[[gnu::cold]]
static std::string buildAggregatedHasKeyJson(
    const std::string & dsHash, std::string_view keyName,
    const std::vector<std::pair<DepSourceId, FilePathId>> & dirs)
{
    auto & pools = DependencyTracker::activeTracker->pools;

    // Lazily build and cache the dirs JSON (once per unique dsHash)
    if (!pools.dirSets.contains(dsHash)) {
        nlohmann::json dirArr = nlohmann::json::array();
        for (auto & [srcId, fpId] : dirs) {
            dirArr.push_back({std::string(pools.resolve(srcId)),
                              std::string(pools.filePathPool.resolve(fpId))});
        }
        pools.dirSets[dsHash] = dirArr.dump();
    }

    // Compact key — no dirs embedded (~100 bytes)
    nlohmann::json j;
    j["ds"] = dsHash;
    j["h"] = std::string(keyName);
    j["t"] = "d";
    return j.dump();
}

/**
 * Record has-key-miss deps, aggregating directory origins when >1 exist.
 */
[[gnu::cold]]
static void recordHasKeyMissDeps(
    const PosTable & positions, const SymbolTable & symbols,
    const Value & v, Symbol keyName)
{
    std::vector<std::pair<DepSourceId, FilePathId>> dirOrigins;
    std::vector<const ProvenanceRecord *> nonDirOrigins;

    forEachTracedDataOrigin(positions, v, [&](const ProvenanceRecord & df) {
        auto fmt = parseStructuredFormat(df.format);
        if (!fmt) return;
        if (*fmt == StructuredFormat::Directory)
            dirOrigins.push_back({df.sourceId, df.filePathId});
        else
            nonDirOrigins.push_back(&df);
    });

    auto & pools = DependencyTracker::activeTracker->pools;

    // Non-directory origins: always individual deps
    for (auto * df : nonDirOrigins) {
        auto fmt = parseStructuredFormat(df->format);
        if (!fmt) continue;
        CompactDepComponents c{df->sourceId, df->filePathId, *fmt, df->dataPathId,
                               ShapeSuffix::None, keyName};
        recordStructuredDep(pools, c, DepHashValue(kHashZero()));
    }

    // Directory origins: aggregate when >1
    if (dirOrigins.size() > 1) {
        auto dsHash = computeDirSetHash(dirOrigins);
        auto key = buildAggregatedHasKeyJson(dsHash, symbols[keyName], dirOrigins);
        DependencyTracker::record(pools, DepType::StructuredContent, "", key, DepHashValue(kHashZero()));
    } else {
        forEachTracedDataOrigin(positions, v, [&](const ProvenanceRecord & df) {
            auto fmt = parseStructuredFormat(df.format);
            if (!fmt || *fmt != StructuredFormat::Directory) return;
            CompactDepComponents c{df.sourceId, df.filePathId, *fmt, df.dataPathId,
                                   ShapeSuffix::None, keyName};
            recordStructuredDep(pools, c, DepHashValue(kHashZero()));
        });
    }
}

[[gnu::cold]] void maybeRecordHasKeyDep(const PosTable & positions, const SymbolTable & symbols,
                          const Value & v, Symbol keyName, bool exists)
{
    if (!DependencyTracker::isActive()) return;
    if (v.type() != nAttrs) return;
    if (!v.attrs()->hasAnyTracedDataLayer()) return;

    if (exists) {
        auto * attr = v.attrs()->get(keyName);
        if (!attr || !attr->pos.isTracedData()) return;
        auto * origin = positions.originOfPtr(attr->pos);
        if (!origin) return;
        auto * pr = std::get_if<Pos::ProvenanceRef>(origin);
        if (!pr) return;
        auto & pools = DependencyTracker::activeTracker->pools;
        auto & rec = resolveProvenanceRef(pools, *pr);
        auto fmt = parseStructuredFormat(rec.format);
        if (!fmt) return;
        CompactDepComponents c{rec.sourceId, rec.filePathId, *fmt, rec.dataPathId,
                               ShapeSuffix::None, keyName};
        recordStructuredDep(pools, c, DepHashValue(kHashOne()));
    } else {
        recordHasKeyMissDeps(positions, symbols, v, keyName);
    }
}

} // namespace nix
