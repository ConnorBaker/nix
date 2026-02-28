#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
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

#include <boost/unordered/concurrent_flat_map.hpp>
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
// Eval trace thread-local state — lifetime documentation
// ═══════════════════════════════════════════════════════════════════════
//
// The eval trace system uses thread-local state at three lifetime scopes:
//
// ┌─────────────────────────────────────────────────────────────────────┐
// │ LIFETIME 1: Process / thread  (never cleared in production)        │
// │                                                                    │
// │ These pools outlive any single EvalState. GC-heap ExprTracedData   │
// │ thunks hold interned IDs (DepSourceId, FilePathId, DataPathId)     │
// │ that must remain valid as long as the thunk is reachable.          │
// │ In production (one EvalState per process), these grow              │
// │ monotonically and are bounded by unique file paths / trie nodes.   │
// │                                                                    │
// │ In tests (multiple EvalState instances per process), these MUST    │
// │ be cleared between tests via resetEvalTracePools() to prevent      │
// │ cross-EvalState contamination. Any surviving GC thunks holding     │
// │ stale IDs become invalid after a reset.                            │
// │                                                                    │
// │ Members:                                                           │
// │   - depSourcePool      (StringPool16: flake input names)           │
// │   - filePathPool       (StringPool16: file paths)                  │
// │   - dataPathPool       (DataPathPool: JSON/TOML path trie)         │
// │   - depKeyPool         (StringPool32: dep key strings for          │
// │                          CompactDep; grows with unique dep keys)   │
// │   - sessionSymbols     (SymbolTable pointer for hasKey resolution) │
// │   - sessionTraces      (append-only CompactDep vector,             │
// │                          index-addressed by DepRange)              │
// │   - activeTracker      (current RAII tracker in the call stack)    │
// ├─────────────────────────────────────────────────────────────────────┤
// │ LIFETIME 2: Per root DependencyTracker  (owned by RootTrackerScope)│
// │                                                                    │
// │ Caches valid only for a single root tracker scope. Owned as fields │
// │ of RootTrackerScope, created at depth 0→1 and destroyed at 1→0.   │
// │ These use Value*/Bindings* pointers as keys, which are only        │
// │ valid within a single evaluation's GC heap generation.             │
// │ RootTrackerScope::current provides access from free functions.     │
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
// │   - epochMap           (Value* → dep range for thunk memoization)  │
// │   - replayBloom        (fast rejection filter for epochMap)        │
// │   - evalCaches         (flake hash → TraceCache instances)         │
// │   - fileContentHashes  (SourcePath → BLAKE3 for Content deps)     │
// │   - mountToInput       (mount point → (inputName, subdir))         │
// │   - skipEpochRecordFor (TracedExpr anti-contamination guard)       │
// └─────────────────────────────────────────────────────────────────────┘
//
// The StatHashCache singleton is a fourth scope (process-global, not
// thread-local, not per-EvalState). It caches (dev,ino,mtime,size) → BLAKE3
// mappings and is shared across all evaluators and threads.

// ═══════════════════════════════════════════════════════════════════════
// DependencyTracker — RAII dep recording (Adapton DDG builder)
// ═══════════════════════════════════════════════════════════════════════

// [Lifetime 1] Per-thread active dependency tracker pointer.
thread_local DependencyTracker * DependencyTracker::activeTracker = nullptr;
// [Lifetime 1] Per-thread append-only dep vector. Index-addressed by
// DepRange; never shrinks in production (only reserved, not cleared).
thread_local std::vector<CompactDep> DependencyTracker::sessionTraces;
// [Lifetime 1] Nesting depth of live DependencyTracker instances.
thread_local uint32_t DependencyTracker::depth = 0;

// ═══════════════════════════════════════════════════════════════════════
// Component interning pools — zero-allocation dep key construction
// [Lifetime 1: process / thread]
// ═══════════════════════════════════════════════════════════════════════

struct StringPool16 {
    std::vector<std::string> strings;
    boost::unordered_flat_map<std::string, uint16_t> lookup;

    uint16_t intern(std::string_view sv) {
        std::string key(sv);
        auto it = lookup.find(key);
        if (it != lookup.end()) return it->second;
        uint16_t id = static_cast<uint16_t>(strings.size());
        strings.push_back(key);
        lookup.emplace(std::move(key), id);
        return id;
    }

    std::string_view resolve(uint16_t id) const { return strings[id]; }

    void clear() { strings.clear(); lookup.clear(); }

    // In production, pools grow monotonically (GC-heap thunks hold IDs).
    // clear() is only called by resetEvalTracePools() for test isolation.
};

// DataPathPool: trie of path components. Process-lifetime (same reason as StringPool16).
// Stores resolved strings directly (not Symbol IDs) so the pool is self-contained
// and independent of any particular SymbolTable. This is critical because in test
// environments, multiple EvalState instances (each with their own SymbolTable) may
// exist within the same process, and Symbol IDs are only valid within their table.
struct DataPathNode {
    uint32_t parentId;      // 0 = root
    std::string component;  // object key (resolved string, not Symbol)
    int32_t arrayIndex;     // -1 if object key, >=0 if array index
};

struct DataPathPool {
    std::vector<DataPathNode> nodes;
    boost::unordered_flat_map<uint64_t, uint32_t> lookup;

    DataPathPool() {
        // Node 0 is the root sentinel (empty path)
        nodes.push_back({0, "", -1});
    }

    uint32_t internChild(uint32_t parentId, std::string_view key) {
        uint64_t h = hashValues(uint8_t(0), parentId, std::hash<std::string_view>{}(key));
        auto [it, inserted] = lookup.try_emplace(h, 0);
        if (!inserted) return it->second;
        uint32_t id = static_cast<uint32_t>(nodes.size());
        nodes.push_back({parentId, std::string(key), -1});
        it->second = id;
        return id;
    }

    uint32_t internArrayChild(uint32_t parentId, int32_t index) {
        uint64_t h = hashValues(uint8_t(1), parentId, index);
        auto [it, inserted] = lookup.try_emplace(h, 0);
        if (!inserted) return it->second;
        uint32_t id = static_cast<uint32_t>(nodes.size());
        nodes.push_back({parentId, "", index});
        it->second = id;
        return id;
    }

    nlohmann::json toJsonArray(uint32_t nodeId) const {
        nlohmann::json arr = nlohmann::json::array();
        std::vector<uint32_t> path;
        uint32_t cur = nodeId;
        while (cur != 0) {
            path.push_back(cur);
            cur = nodes[cur].parentId;
        }
        std::reverse(path.begin(), path.end());
        for (auto id : path) {
            auto & node = nodes[id];
            if (node.arrayIndex >= 0)
                arr.push_back(node.arrayIndex);
            else
                arr.push_back(node.component);
        }
        return arr;
    }

    void clear() {
        nodes.clear();
        lookup.clear();
        nodes.push_back({0, "", -1}); // re-add root sentinel
    }

    // In production, pools grow monotonically. clear() is for test isolation.
};

// StringPool32: like StringPool16 but with uint32_t IDs for larger key spaces.
struct StringPool32 {
    std::vector<std::string> strings;
    boost::unordered_flat_map<std::string, uint32_t> lookup;

    uint32_t intern(std::string_view sv) {
        std::string key(sv);
        auto it = lookup.find(key);
        if (it != lookup.end()) return it->second;
        uint32_t id = static_cast<uint32_t>(strings.size());
        strings.push_back(key);
        lookup.emplace(std::move(key), id);
        return id;
    }

    std::string_view resolve(uint32_t id) const { return strings[id]; }

    void clear() { strings.clear(); lookup.clear(); }
};

// [Lifetime 1] Interning pools — process-lifetime, cleared only by resetEvalTracePools().
static thread_local StringPool16 depSourcePool;
static thread_local StringPool16 filePathPool;
static thread_local DataPathPool dataPathPool;
// [Lifetime 1] Dep key strings (file paths, JSON SC keys, env var names).
// Unlike depSourcePool/filePathPool (uint16_t, bounded by unique file paths),
// depKeyPool uses uint32_t because unique dep keys can be numerous (one per
// unique StructuredContent JSON key). In production (one EvalState per process),
// this is bounded by the evaluation's dep key space.
static thread_local StringPool32 depKeyPool;
// [Lifetime 1] SymbolTable pointer for resolving hasKey Symbol in recordStructuredDep().
// Updated by initSessionSymbols() when a new evaluation starts.
static thread_local const SymbolTable * sessionSymbols = nullptr;

// Cached constant Blake3Hash values used in shape dep recording.
// Function-local statics avoid static initialization order issues across TUs.
static const Blake3Hash & kHashZero()   { static const auto h = depHash("0"); return h; }
static const Blake3Hash & kHashOne()    { static const auto h = depHash("1"); return h; }
static const Blake3Hash & kHashObject() { static const auto h = depHash("object"); return h; }
static const Blake3Hash & kHashArray()  { static const auto h = depHash("array"); return h; }

// ═══════════════════════════════════════════════════════════════════════
// Pool accessor functions (declared in dependency-tracker.hh)
// ═══════════════════════════════════════════════════════════════════════

DepSourceId internDepSource(std::string_view sv) {
    return DepSourceId(depSourcePool.intern(sv));
}

FilePathId internFilePath(std::string_view sv) {
    return FilePathId(filePathPool.intern(sv));
}

std::string_view resolveDepSource(DepSourceId id) {
    return depSourcePool.resolve(id.value);
}

std::string_view resolveFilePath(FilePathId id) {
    return filePathPool.resolve(id.value);
}

DepKeyId internDepKey(std::string_view sv) {
    return DepKeyId(depKeyPool.intern(sv));
}

std::string_view resolveDepKey(DepKeyId id) {
    return depKeyPool.resolve(id.value);
}

DataPathId internDataPathChild(DataPathId parentId, std::string_view key) {
    return DataPathId(dataPathPool.internChild(parentId.value, key));
}

DataPathId internDataPathArrayChild(DataPathId parentId, int32_t index) {
    return DataPathId(dataPathPool.internArrayChild(parentId.value, index));
}

std::string dataPathToJsonString(DataPathId nodeId) {
    return dataPathPool.toJsonArray(nodeId.value).dump();
}

DataPathId jsonStringToDataPathId(std::string_view jsonStr) {
    auto arr = nlohmann::json::parse(jsonStr);
    uint32_t id = 0; // root
    for (auto & elem : arr) {
        if (elem.is_number())
            id = dataPathPool.internArrayChild(id, elem.get<int32_t>());
        else {
            auto s = elem.get<std::string>();
            id = dataPathPool.internChild(id, s);
        }
    }
    return DataPathId(id);
}

void initSessionSymbols(const SymbolTable & symbols) {
    sessionSymbols = &symbols;
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

void resetEvalTracePools()
{
    // Clear ALL Lifetime 1 state. Only for test isolation — in production
    // these pools must outlive GC-heap ExprTracedData thunks. Any surviving
    // GC thunks holding stale pool IDs become invalid after this call.
    depSourcePool.clear();
    filePathPool.clear();
    dataPathPool.clear();
    depKeyPool.clear();
    sessionSymbols = nullptr;
    DependencyTracker::sessionTraces.clear();
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
void DependencyTracker::record(Dep dep)
{
    if (activeTracker) {
        uint64_t h = hashValues(std::to_underlying(dep.type), dep.source, dep.key);
        if (!activeTracker->depDedup.tryInsert(h))
            return;  // Dependency already recorded in this trace scope — skip duplicate
    }
    debug("recording %s (%s) dep: input='%s' key='%s'",
        depTypeName(dep.type), depKindName(depKind(dep.type)), dep.source, dep.key);
    sessionTraces.push_back(CompactDep{
        dep.type,
        internDepSource(dep.source),
        internDepKey(dep.key),
        std::move(dep.expectedHash)});
}

// ═══════════════════════════════════════════════════════════════════════
// recordStructuredDep — zero-allocation dedup + JSON serialization
// ═══════════════════════════════════════════════════════════════════════

[[gnu::cold]] bool recordStructuredDep(
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
    assert(sessionSymbols && "initSessionSymbols() must be called before recordStructuredDep()");
    nlohmann::json key;
    key["f"] = std::string(filePathPool.resolve(c.filePathId.value));
    key["t"] = std::string(1, structuredFormatChar(c.format));
    key["p"] = dataPathPool.toJsonArray(c.dataPathId.value);
    if (c.hasKey)
        key["h"] = std::string((*sessionSymbols)[c.hasKey]);
    else if (c.suffix != ShapeSuffix::None)
        key["s"] = std::string(shapeSuffixName(c.suffix));

    auto keyStr = key.dump();
    DependencyTracker::sessionTraces.push_back(
        CompactDep{depType, c.sourceId, internDepKey(keyStr), hash});
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
void DependencyTracker::recordReplay(const Dep & dep)
{
    sessionTraces.push_back(CompactDep{
        dep.type,
        internDepSource(dep.source),
        internDepKey(dep.key),
        dep.expectedHash});
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
std::vector<CompactDep> DependencyTracker::collectTraces() const
{
    uint32_t endIndex = mySessionTraces->size();

    if (excludedChildRanges.empty()) {
        // Fast path: no child exclusions needed
        if (replayedRanges.empty())
            return {mySessionTraces->begin() + startIndex, mySessionTraces->begin() + endIndex};

        std::vector<CompactDep> result(mySessionTraces->begin() + startIndex, mySessionTraces->begin() + endIndex);
        for (auto & r : replayedRanges)
            result.insert(result.end(), r.deps->begin() + r.start, r.deps->begin() + r.end);
        return result;
    }

    // Slow path: skip excluded child ranges to prevent parent traces
    // from inheriting children's dependencies.
    std::vector<CompactDep> result;

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
// StatHashCache — persistent (dev, ino, mtime, size, depType) → BLAKE3
// Implementation detail: invisible to all consumers.
// ═══════════════════════════════════════════════════════════════════════

namespace {

// ── Impl ────────────────────────────────────────────────────────────

static constexpr size_t L1_MAX_SIZE = 65536;

// Key for L2 bulk map: path + dep_type
struct L2Key {
    std::string path;
    DepType depType;

    bool operator==(const L2Key &) const = default;

    struct Hash {
        std::size_t operator()(const L2Key & k) const noexcept {
            return hashValues(k.path, std::to_underlying(k.depType));
        }
    };
};

struct StatHashCacheImpl
{
    boost::concurrent_flat_map<StatHashKey, Blake3Hash, StatHashKey::Hash> l1;

    // L2 bulk load cache (populated by loadStatHashEntries from TraceStore)
    std::unordered_map<L2Key, StatHashEntry, L2Key::Hash> l2Bulk;
    bool l2Loaded = false;

    // Dirty entries: newly-stored hashes that TraceStore should flush to SQLite
    std::vector<StatHashEntry> dirtyEntries;

    ~StatHashCacheImpl() = default;
    StatHashCacheImpl() = default;
};

// ── Helpers ─────────────────────────────────────────────────────────

static StatHashKey makeKey(const PosixStat & st, DepType type)
{
    return {
        st.st_dev,
        st.st_ino,
#ifdef __APPLE__
        st.st_mtimespec.tv_sec,
        st.st_mtimespec.tv_nsec,
#else
        st.st_mtim.tv_sec,
        st.st_mtim.tv_nsec,
#endif
        st.st_size,
        type,
    };
}


// ── StatHashCache singleton + public API ─────────────────────────────

struct StatHashCache
{
    struct LookupResult {
        std::optional<Blake3Hash> hash;
        std::optional<PosixStat> stat;
    };

    static StatHashCache & instance()
    {
        static StatHashCache cache;
        return cache;
    }

    LookupResult lookupHash(const std::filesystem::path & physPath, DepType type)
    {
        auto st = maybeLstat(physPath);
        if (!st)
            return {std::nullopt, std::nullopt};

        auto key = makeKey(*st, type);

        // L1: in-memory lookup
        if (auto hit = getConcurrent(impl->l1, key)) {
            debug("stat hash cache: L1 hit for '%s' (%s)", physPath.string(), depTypeName(type));
            return {std::move(hit), *st};
        }

        // L2: bulk-loaded lookup (populated by loadStatHashEntries from TraceStore)
        auto pathStr = physPath.string();
        auto l2It = impl->l2Bulk.find(L2Key{pathStr, type});
        if (l2It != impl->l2Bulk.end()) {
            auto & e = l2It->second;
            // Validate stat metadata
            if (e.stat == key)
            {
                // Promote to L1
                if (impl->l1.size() < L1_MAX_SIZE)
                    impl->l1.emplace(key, e.hash);

                debug("stat hash cache: L2 hit for '%s' (%s)", physPath.string(), depTypeName(type));
                return {e.hash, *st};
            }
        }

        debug("stat hash cache: miss for '%s' (%s)", physPath.string(), depTypeName(type));
        return {std::nullopt, *st};
    }

    void storeHash(
        const std::filesystem::path & physPath, DepType type,
        const Blake3Hash & hash, std::optional<PosixStat> st = std::nullopt)
    {
        if (!st) {
            st = maybeLstat(physPath);
            if (!st)
                return;
        }
        auto key = makeKey(*st, type);

        // L1: store in memory
        if (impl->l1.size() < L1_MAX_SIZE)
            impl->l1.emplace_or_visit(key,
                hash,
                [&](auto & entry) { entry.second = hash; });

        // Track as dirty for TraceStore to flush to SQLite
        impl->dirtyEntries.push_back(StatHashEntry{
            .path = physPath.string(),
            .stat = key,
            .hash = hash,
        });

        // Update L2 bulk cache (reuse the dirty entry we just created)
        if (impl->l2Loaded) {
            auto pathStr = physPath.string();
            impl->l2Bulk[L2Key{pathStr, type}] = impl->dirtyEntries.back();
        }
    }

    void clearMemoryCache()
    {
        impl->l1.clear();
    }

    void bulkLoadEntries(std::vector<StatHashEntry> entries)
    {
        for (auto & e : entries) {
            auto key = L2Key{e.path, e.stat.depType};
            impl->l2Bulk[std::move(key)] = std::move(e);
        }
        impl->l2Loaded = true;
        debug("stat hash cache: loaded %d L2 entries from TraceStore", entries.size());
    }

    void flushDirtyEntries(std::function<void(const StatHashEntry &)> callback)
    {
        for (auto & e : impl->dirtyEntries)
            callback(e);
        impl->dirtyEntries.clear();
    }

private:
    std::unique_ptr<StatHashCacheImpl> impl;

    StatHashCache()
        : impl(std::make_unique<StatHashCacheImpl>())
    {
    }

    ~StatHashCache() = default;

};

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════
// Stat-cached dep hash functions
// ═══════════════════════════════════════════════════════════════════════

// Compute Content dep hash with stat-cache acceleration (Shake: "file
// modification time" early-cutoff check before reading file contents).
// The stat-hash cache acts as an oracle memoization layer — if the file's
// stat metadata (dev, ino, mtime_ns, size) is unchanged since the last
// hashing, the cached BLAKE3 hash is returned without re-reading the file.
//
// Note: hashes the entire file content. For .nix files consumed via
// import/evalFile, this is an over-approximation — a change to any part
// of the file invalidates all traces that depend on it, even if the change
// is semantically irrelevant (e.g., reformatting, adding a comment in an
// unused branch). For data files consumed by fromJSON/fromTOML, the
// StructuredContent two-level override (see design.md Section 4.6) can
// mitigate this. For .nix code, no fine-grained override exists.
Blake3Hash depHashFile(const SourcePath & path)
{
    if (auto physPath = path.getPhysicalPath()) {
        auto result = StatHashCache::instance().lookupHash(*physPath, DepType::Content);
        if (result.hash)
            return *result.hash;
        auto content = path.readFile();
        auto hash = depHash(content);
        if (result.stat)
            StatHashCache::instance().storeHash(*physPath, DepType::Content, hash, *result.stat);
        return hash;
    }
    return depHash(path.readFile());
}

// Stat-cached NAR content hash (Shake: early-cutoff via stat metadata).
// Like depHashFile but hashes the NAR serialization, which captures both
// file content and the executable permission bit — needed for builtins.path
// deps where store path identity depends on permissions.
Blake3Hash depHashPathCached(const SourcePath & path)
{
    if (auto physPath = path.getPhysicalPath()) {
        auto result = StatHashCache::instance().lookupHash(*physPath, DepType::NARContent);
        if (result.hash)
            return *result.hash;
        auto hash = depHashPath(path);
        if (result.stat)
            StatHashCache::instance().storeHash(*physPath, DepType::NARContent, hash, *result.stat);
        return hash;
    }
    return depHashPath(path);
}

// Stat-cached directory listing hash (Shake: early-cutoff via stat metadata).
// Directory stat changes (mtime update on entry add/remove) trigger rehashing;
// unchanged stat metadata serves the cached hash without re-reading the listing.
Blake3Hash depHashDirListingCached(const SourcePath & path, const SourceAccessor::DirEntries & entries)
{
    if (auto physPath = path.getPhysicalPath()) {
        auto result = StatHashCache::instance().lookupHash(*physPath, DepType::Directory);
        if (result.hash)
            return *result.hash;
        auto hash = depHashDirListing(entries);
        if (result.stat)
            StatHashCache::instance().storeHash(*physPath, DepType::Directory, hash, *result.stat);
        return hash;
    }
    return depHashDirListing(entries);
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
    const std::unordered_map<CanonPath, std::pair<std::string, std::string>> & mountToInput)
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
    const CanonPath & absPath,
    const DepHashValue & hash,
    DepType depType,
    const std::unordered_map<CanonPath, std::pair<std::string, std::string>> & mountToInput)
{
    bool recorded = false;
    // Single lstat — reused for both existence gating and stat-hash-cache population
    std::optional<PosixStat> fileStat;

    if (!mountToInput.empty()) {
        if (auto resolved = resolveToInput(absPath, mountToInput)) {
            DependencyTracker::record({resolved->first, resolved->second.abs(), hash, depType});
            recorded = true;
            // Flake input path — no lstat needed (accessor provides content)
        } else if ((fileStat = maybeLstat(std::filesystem::path(absPath.abs())))) {
            // Real path outside flake input tree (e.g., store path from IFD).
            // Record with "<absolute>" sentinel — verification oracle reads
            // directly from the filesystem, bypassing input accessor resolution.
            DependencyTracker::record({std::string(absolutePathDep), absPath.abs(), hash, depType});
            recorded = true;
        }
        // else: virtual file — no filesystem oracle, skip (see above)
    } else if ((fileStat = maybeLstat(std::filesystem::path(absPath.abs())))) {
        DependencyTracker::record({"", absPath.abs(), hash, depType});
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
                StatHashCache::instance().storeHash(
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
    DependencyTracker::record({source, key, DepHashValue(it->second), DepType::RawContent});
}

std::pair<std::string, std::string> resolveProvenance(
    const CanonPath & absPath,
    const std::unordered_map<CanonPath, std::pair<std::string, std::string>> & mountToInput)
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

void clearStatHashMemoryCache()
{
    StatHashCache::instance().clearMemoryCache();
}

void loadStatHashEntries(std::vector<StatHashEntry> entries)
{
    StatHashCache::instance().bulkLoadEntries(std::move(entries));
}

void forEachDirtyStatHash(std::function<void(const StatHashEntry &)> callback)
{
    StatHashCache::instance().flushDirtyEntries(std::move(callback));
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
    boost::unordered_flat_set<const Pos::TracedData *> seen;
    for (auto & attr : *v.attrs()) {
        if (!attr.pos.isTracedData()) continue;
        auto * origin = positions.originOfPtr(attr.pos);
        if (!origin) continue;
        auto * df = std::get_if<Pos::TracedData>(origin);
        if (!df) continue;
        if (!seen.insert(df).second) continue;
        if (!parseStructuredFormat(df->format)) continue;
        fn(*df);
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
    CompactDepComponents c{prov->sourceId, prov->filePathId, prov->format, prov->dataPathId,
                           ShapeSuffix::Len, Symbol{}};
    recordStructuredDep(c, DepHashValue(hash));
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
        const Pos::TracedData * df;
        uint32_t originOffset;
        std::vector<std::string_view> keys;
    };
    std::vector<OriginKeys> groups;

    for (auto & attr : *v.attrs()) {
        if (!attr.pos.isTracedData()) continue;
        auto resolved = positions.resolveOriginFull(attr.pos);
        if (!resolved) continue;
        auto * df = std::get_if<Pos::TracedData>(resolved->origin);
        if (!df) continue;
        OriginKeys * group = nullptr;
        for (auto & g : groups) {
            if (g.df == df) { group = &g; break; }
        }
        if (!group) {
            groups.push_back({df, resolved->offset, {}});
            group = &groups.back();
        }
        group->keys.push_back(symbols[attr.name]);
    }

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
            recordStructuredDep(c, DepHashValue(info.hash));
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
        forEachTracedDataOrigin(positions, v, [&](const Pos::TracedData & df) {
            auto fmt = parseStructuredFormat(df.format);
            if (!fmt) return;
            CompactDepComponents c{df.sourceId, df.filePathId, *fmt, df.dataPathId,
                                   ShapeSuffix::Type, Symbol{}};
            recordStructuredDep(c, DepHashValue(kHashObject()));
        });
        break;
    }
    case nList: {
        if (v.listSize() == 0) return;
        auto * prov = lookupTracedContainer((const void *)v.listView()[0]);
        if (!prov) return;
        CompactDepComponents c{prov->sourceId, prov->filePathId, prov->format,
                               prov->dataPathId, ShapeSuffix::Type, Symbol{}};
        recordStructuredDep(c, DepHashValue(kHashArray()));
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
    boost::unordered_flat_set<const Pos::TracedData *> seen;
    for (auto & attr : *bindings) {
        if (!attr.pos.isTracedData()) continue;
        auto * origin = positions.originOfPtr(attr.pos);
        if (!origin) continue;
        auto * df = std::get_if<Pos::TracedData>(origin);
        if (!df) continue;
        if (!seen.insert(df).second) continue;
        auto fmt = parseStructuredFormat(df->format);
        if (!fmt) continue;
        result.push_back({df->sourceId, df->filePathId, df->dataPathId, *fmt});
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

    // Record exists=true for a single attr against its operand's origins
    auto recordExists = [&](const Attr & attr, const std::vector<IntersectOriginInfo> &) {
        if (!attr.pos.isTracedData()) return;
        auto * origin = positions.originOfPtr(attr.pos);
        if (!origin) return;
        auto * df = std::get_if<Pos::TracedData>(origin);
        if (!df) return;
        auto fmt = parseStructuredFormat(df->format);
        if (!fmt) return;
        CompactDepComponents c{df->sourceId, df->filePathId, *fmt, df->dataPathId,
                               ShapeSuffix::None, attr.name};
        recordStructuredDep(c, DepHashValue(kHashOne()));
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
                recordStructuredDep(c, DepHashValue(kHashZero()));
            }
        }
        if (dirOrigins.size() > 1) {
            auto dsHash = computeDirSetHash(dirOrigins);
            auto key = buildAggregatedHasKeyJson(dsHash, symbols[keyName], dirOrigins);
            DependencyTracker::record(
                Dep{"", std::move(key), DepHashValue(kHashZero()), DepType::StructuredContent});
        } else {
            for (auto & oi : origins) {
                if (oi.format != StructuredFormat::Directory) continue;
                CompactDepComponents c{oi.sourceId, oi.filePathId, oi.format, oi.dataPathId,
                                       ShapeSuffix::None, keyName};
                recordStructuredDep(c, DepHashValue(kHashZero()));
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
    auto sorted = dirs;
    std::sort(sorted.begin(), sorted.end(),
        [](const auto & a, const auto & b) {
            auto sa = resolveDepSource(a.first);
            auto fa = resolveFilePath(a.second);
            auto sb = resolveDepSource(b.first);
            auto fb = resolveFilePath(b.second);
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
        sink(resolveDepSource(srcId));
        sink(std::string_view("\0", 1));
        sink(resolveFilePath(fpId));
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
 * Embeds directory paths so computeCurrentHash can verify independently.
 */
[[gnu::cold]]
static std::string buildAggregatedHasKeyJson(
    const std::string & dsHash, std::string_view keyName,
    const std::vector<std::pair<DepSourceId, FilePathId>> & dirs)
{
    nlohmann::json j;
    j["ds"] = dsHash;
    j["h"] = std::string(keyName);
    j["t"] = "d";
    nlohmann::json dirArr = nlohmann::json::array();
    for (auto & [srcId, fpId] : dirs) {
        dirArr.push_back({std::string(resolveDepSource(srcId)),
                          std::string(resolveFilePath(fpId))});
    }
    j["dirs"] = std::move(dirArr);
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
    std::vector<const Pos::TracedData *> nonDirOrigins;

    forEachTracedDataOrigin(positions, v, [&](const Pos::TracedData & df) {
        auto fmt = parseStructuredFormat(df.format);
        if (!fmt) return;
        if (*fmt == StructuredFormat::Directory)
            dirOrigins.push_back({df.sourceId, df.filePathId});
        else
            nonDirOrigins.push_back(&df);
    });

    // Non-directory origins: always individual deps
    for (auto * df : nonDirOrigins) {
        auto fmt = parseStructuredFormat(df->format);
        if (!fmt) continue;
        CompactDepComponents c{df->sourceId, df->filePathId, *fmt, df->dataPathId,
                               ShapeSuffix::None, keyName};
        recordStructuredDep(c, DepHashValue(kHashZero()));
    }

    // Directory origins: aggregate when >1
    if (dirOrigins.size() > 1) {
        auto dsHash = computeDirSetHash(dirOrigins);
        auto key = buildAggregatedHasKeyJson(dsHash, symbols[keyName], dirOrigins);
        DependencyTracker::record(
            Dep{"", std::move(key), DepHashValue(kHashZero()), DepType::StructuredContent});
    } else {
        forEachTracedDataOrigin(positions, v, [&](const Pos::TracedData & df) {
            auto fmt = parseStructuredFormat(df.format);
            if (!fmt || *fmt != StructuredFormat::Directory) return;
            CompactDepComponents c{df.sourceId, df.filePathId, *fmt, df.dataPathId,
                                   ShapeSuffix::None, keyName};
            recordStructuredDep(c, DepHashValue(kHashZero()));
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
        auto * df = std::get_if<Pos::TracedData>(origin);
        if (!df) return;
        auto fmt = parseStructuredFormat(df->format);
        if (!fmt) return;
        CompactDepComponents c{df->sourceId, df->filePathId, *fmt, df->dataPathId,
                               ShapeSuffix::None, keyName};
        recordStructuredDep(c, DepHashValue(kHashOne()));
    } else {
        recordHasKeyMissDeps(positions, symbols, v, keyName);
    }
}

} // namespace nix
