#include "nix/expr/eval-trace-deps.hh"
#include "nix/expr/dependency-tracker.hh"
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
#include <filesystem>

namespace nix {

// ═══════════════════════════════════════════════════════════════════════
// DependencyTracker — RAII dep recording (Adapton DDG builder)
// ═══════════════════════════════════════════════════════════════════════

// Per-thread active dependency tracker for dynamic dependency discovery
// (Adapton: the "currently adapting" node whose edges are being recorded).
thread_local DependencyTracker * DependencyTracker::activeTracker = nullptr;
// Per-thread append-only dependency vector (Shake: the "journal" of all
// dependencies observed during this evaluation session).
thread_local std::vector<Dep> DependencyTracker::sessionTraces;

// ═══════════════════════════════════════════════════════════════════════
// Component interning pools — zero-allocation dep key construction
// ═══════════════════════════════════════════════════════════════════════

// StringPool16: intern strings to uint16_t indices. Process-lifetime (not cleared
// per-session) because GC-heap ExprTracedData thunks hold interned IDs.
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

    // Intentionally no clear(): GC-heap ExprTracedData thunks hold interned
    // IDs that must remain valid across DependencyTracker lifetimes. The pool
    // grows monotonically, bounded by unique file paths (typically <100).
};

// DataPathPool: trie of path components. Process-lifetime (same reason as StringPool16).
struct DataPathNode {
    uint32_t parentId;      // 0 = root
    Symbol component;       // object key (interned in SymbolTable)
    int32_t arrayIndex;     // -1 if object key, >=0 if array index
};

struct DataPathPool {
    std::vector<DataPathNode> nodes;
    boost::unordered_flat_map<uint64_t, uint32_t> lookup;

    DataPathPool() {
        // Node 0 is the root sentinel (empty path)
        nodes.push_back({0, Symbol{}, -1});
    }

    uint32_t internChild(uint32_t parentId, Symbol key) {
        uint64_t h = hashValues(uint8_t(0), parentId, key.getId());
        auto [it, inserted] = lookup.try_emplace(h, 0);
        if (!inserted) return it->second;
        uint32_t id = static_cast<uint32_t>(nodes.size());
        nodes.push_back({parentId, key, -1});
        it->second = id;
        return id;
    }

    uint32_t internArrayChild(uint32_t parentId, int32_t index) {
        uint64_t h = hashValues(uint8_t(1), parentId, index);
        auto [it, inserted] = lookup.try_emplace(h, 0);
        if (!inserted) return it->second;
        uint32_t id = static_cast<uint32_t>(nodes.size());
        nodes.push_back({parentId, Symbol{}, index});
        it->second = id;
        return id;
    }

    nlohmann::json toJsonArray(uint32_t nodeId, const SymbolTable & symbols) const {
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
                arr.push_back(std::string(symbols[node.component]));
        }
        return arr;
    }

    // Intentionally no clear(): same lifetime rationale as StringPool16.
    // Bounded by total JSON/TOML/directory structure size across all evaluated files.
};

static thread_local StringPool16 depSourcePool;
static thread_local StringPool16 filePathPool;
static thread_local DataPathPool dataPathPool;
static thread_local const SymbolTable * sessionSymbols = nullptr;

// Cached constant Blake3Hash values used in shape dep recording.
// Function-local statics avoid static initialization order issues across TUs.
static const Blake3Hash & kHashZero()   { static const auto h = depHash("0"); return h; }
static const Blake3Hash & kHashOne()    { static const auto h = depHash("1"); return h; }
static const Blake3Hash & kHashObject() { static const auto h = depHash("object"); return h; }
static const Blake3Hash & kHashArray()  { static const auto h = depHash("array"); return h; }
static const Blake3Hash & kHashEmpty()  { static const auto h = depHash(""); return h; }

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

DataPathId internDataPathChild(DataPathId parentId, Symbol key) {
    return DataPathId(dataPathPool.internChild(parentId.value, key));
}

DataPathId internDataPathArrayChild(DataPathId parentId, int32_t index) {
    return DataPathId(dataPathPool.internArrayChild(parentId.value, index));
}

std::string dataPathToJsonString(DataPathId nodeId) {
    assert(sessionSymbols && "initSessionSymbols() must be called before dataPathToJsonString()");
    return dataPathPool.toJsonArray(nodeId.value, *sessionSymbols).dump();
}

DataPathId jsonStringToDataPathId(std::string_view jsonStr, SymbolTable & symbols) {
    auto arr = nlohmann::json::parse(jsonStr);
    uint32_t id = 0; // root
    for (auto & elem : arr) {
        if (elem.is_number())
            id = dataPathPool.internArrayChild(id, elem.get<int32_t>());
        else
            id = dataPathPool.internChild(id, symbols.create(elem.get<std::string>()));
    }
    return DataPathId(id);
}

void initSessionSymbols(const SymbolTable & symbols) {
    sessionSymbols = &symbols;
}

// ═══════════════════════════════════════════════════════════════════════
// Traced container provenance map — shape dep tracking
// ═══════════════════════════════════════════════════════════════════════

// Thread-local map from stable list identity (first element Value*) to provenance.
// Lists still use this map because list provenance is tracked at access time
// (maybeRecordListLenDep), not via PosIdx. Attrsets use PosIdx-based TracedData
// origin tracking instead.
static thread_local boost::unordered_flat_map<const void*, const TracedContainerProvenance *> tracedContainerMap;

// Memoization: skip re-scanning the same Bindings* in maybeRecordAttrKeysDep.
// Cleared on root DependencyTracker construction.
static thread_local boost::unordered_flat_set<const void *> scannedBindings;

// Stable, non-GC pool for provenance data. std::deque never invalidates
// pointers on push_back, so ProvenanceRef pointers remain valid until clear().
// Cleared alongside tracedContainerMap on root DependencyTracker construction.
static thread_local std::deque<TracedContainerProvenance> provenancePool;

// Precomputed keys hash map: origin offset → {hash, keyCount, interned components}.
// Populated by ExprTracedData::eval() at creation time; consumed by maybeRecordAttrKeysDep
// to skip sort + concat + BLAKE3 when all original keys are visible (common case).
static thread_local boost::unordered_flat_map<uint32_t, PrecomputedKeysInfo> precomputedKeysMap;

// Per-Bindings* cache of TracedData origin info for intersectAttrs.
// Uses interned IDs to avoid string allocation during the scan.
struct IntersectOriginInfo {
    DepSourceId sourceId;
    FilePathId filePathId;
    DataPathId dataPathId;
    StructuredFormat format;
};
static thread_local boost::unordered_flat_map<const Bindings *, std::vector<IntersectOriginInfo>> intersectOriginsCache;

void DependencyTracker::onRootConstruction()
{
    clearTracedContainerMap();
    provenancePool.clear();
    clearReadFileProvenanceMap();
    clearReadFileStringPtrs();
    scannedBindings.clear();
    clearPrecomputedKeysMap();
    intersectOriginsCache.clear();
    // Note: depSourcePool, filePathPool, dataPathPool, and sessionSymbols
    // are NOT cleared here. ExprTracedData thunks on the GC heap hold
    // interned IDs that must remain valid across DependencyTracker lifetimes.
    // The pools grow monotonically for the process lifetime (same as SymbolTable).
    // This is safe: unique file paths and dep sources are O(100), and trie
    // nodes are bounded by the total JSON/TOML/directory structure size.
    sessionTraces.reserve(16384);
}

ProvenanceRef allocateProvenance(DepSourceId sourceId, FilePathId filePathId,
                                 DataPathId dataPathId, StructuredFormat format)
{
    provenancePool.emplace_back(TracedContainerProvenance{sourceId, filePathId, dataPathId, format});
    return &provenancePool.back();
}

void registerTracedContainer(const void * key, const TracedContainerProvenance * prov)
{
    tracedContainerMap.emplace(key, prov);
}

const TracedContainerProvenance * lookupTracedContainer(const void * key)
{
    auto it = tracedContainerMap.find(key);
    return it != tracedContainerMap.end() ? it->second : nullptr;
}

void clearTracedContainerMap()
{
    tracedContainerMap.clear();
}

void registerPrecomputedKeys(uint32_t originOffset, PrecomputedKeysInfo info)
{
    precomputedKeysMap.emplace(originOffset, std::move(info));
}

void clearPrecomputedKeysMap()
{
    precomputedKeysMap.clear();
}

// Record a non-StructuredContent dependency edge (Adapton: "add-edge").
// BSàlC §3.2: during fresh evaluation, the scheduler records each dependency
// into the trace. Hash-based dedup: >90% of record() calls are duplicates,
// so hashing the (type, source, key) triple avoids constructing DepKey strings.
void DependencyTracker::record(Dep dep)
{
    if (activeTracker) {
        uint64_t h = hashValues(std::to_underlying(dep.type), dep.source, dep.key);
        if (!activeTracker->recordedKeyHashes.insert(h).second)
            return;  // Dependency already recorded in this trace scope — skip duplicate
    }
    debug("recording %s (%s) dep: input='%s' key='%s'",
        depTypeName(dep.type), depKindName(depKind(dep.type)), dep.source, dep.key);
    sessionTraces.push_back(std::move(dep));
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
        && !DependencyTracker::activeTracker->recordedKeyHashes.insert(h).second)
        return false;  // Duplicate — no work done

    // 2. Only for non-duplicates: build JSON dep key
    assert(sessionSymbols && "initSessionSymbols() must be called before recordStructuredDep()");
    nlohmann::json key;
    key["f"] = std::string(filePathPool.resolve(c.filePathId.value));
    key["t"] = std::string(1, structuredFormatChar(c.format));
    key["p"] = dataPathPool.toJsonArray(c.dataPathId.value, *sessionSymbols);
    if (c.hasKey)
        key["h"] = std::string((*sessionSymbols)[c.hasKey]);
    else if (c.suffix != ShapeSuffix::None)
        key["s"] = std::string(shapeSuffixName(c.suffix));

    auto source = std::string(depSourcePool.resolve(c.sourceId.value));
    DependencyTracker::sessionTraces.push_back(
        Dep{std::move(source), key.dump(), hash, depType});
    return true;
}

// Replay a child's cached dependency into the session trace without touching the
// active tracker's dedup set (recordedKeyHashes). This prevents parent dedup-set
// pollution: the child's deps land in an excluded range in sessionTraces (so
// they're skipped by collectTraces), but if record() were used instead, the
// parent's recordedKeyHashes would reject a later independent recording of the same
// dep. recordReplay avoids this by only appending to sessionTraces — the dep
// still participates in thunk epoch ranges (recordThunkDeps) for correct
// transitive propagation.
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

// Thread-local map from content hash to ReadFileProvenance. Evaluation is
// single-threaded, so this is safe. Populated by prim_readFile, queried by
// prim_fromJSON/prim_fromTOML to enable lazy structural dep tracking.
// Keyed by content hash so multiple readFile results coexist and the same
// provenance can serve multiple fromJSON/fromTOML calls (non-consuming lookup).
static thread_local boost::unordered_flat_map<Blake3Hash, ReadFileProvenance, Blake3Hash::Hasher> readFileProvenanceMap;

void addReadFileProvenance(ReadFileProvenance prov)
{
    readFileProvenanceMap.insert_or_assign(prov.contentHash, std::move(prov));
}

const ReadFileProvenance * lookupReadFileProvenance(const Blake3Hash & contentHash)
{
    auto it = readFileProvenanceMap.find(contentHash);
    return it != readFileProvenanceMap.end() ? &it->second : nullptr;
}

void clearReadFileProvenanceMap()
{
    readFileProvenanceMap.clear();
}

// ═══════════════════════════════════════════════════════════════════════
// ReadFile string pointer tracking for RawContent deps
// ═══════════════════════════════════════════════════════════════════════

// Thread-local map from string data pointer (Value::c_str()) to content hash.
// Populated by prim_readFile, queried by string builtins that observe raw bytes.
// Key is a raw const char* — valid because readFile strings are GC-allocated
// and stable for the evaluation session. Typically empty or very small.
static thread_local boost::unordered_flat_map<const char *, Blake3Hash> readFileStringPtrs;

void addReadFileStringPtr(const char * ptr, const Blake3Hash & contentHash)
{
    readFileStringPtrs.emplace(ptr, contentHash);
}

void clearReadFileStringPtrs()
{
    readFileStringPtrs.clear();
}

[[gnu::cold]] void maybeRecordRawContentDep(EvalState & state, const Value & v)
{
    if (!DependencyTracker::isActive()) return;
    if (v.type() != nString) return;
    auto it = readFileStringPtrs.find(v.c_str());
    if (it == readFileStringPtrs.end()) return;
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
    std::vector<const Pos::TracedData *> seen;
    for (auto & attr : *v.attrs()) {
        if (!attr.pos.isTracedData()) continue;
        auto * origin = positions.originOfPtr(attr.pos);
        if (!origin) continue;
        auto * df = std::get_if<Pos::TracedData>(origin);
        if (!df) continue;
        bool dup = false;
        for (auto * s : seen) { if (s == df) { dup = true; break; } }
        if (dup) continue;
        if (!parseStructuredFormat(df->format)) continue;
        seen.push_back(df);
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
    if (!scannedBindings.insert(v.attrs()).second) return;

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
        auto pcIt = precomputedKeysMap.find(g.originOffset);
        if (pcIt != precomputedKeysMap.end() && pcIt->second.keyCount == g.keys.size()) {
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

static const std::vector<IntersectOriginInfo> emptyOrigins;

static const std::vector<IntersectOriginInfo> & collectOriginsCached(
    const PosTable & positions, const Value & v)
{
    auto * bindings = v.attrs();
    auto [it, inserted] = intersectOriginsCache.try_emplace(bindings);
    if (!inserted)
        return it->second;

    auto & result = it->second;
    std::vector<const Pos::TracedData *> seen;
    for (auto & attr : *bindings) {
        if (!attr.pos.isTracedData()) continue;
        auto * origin = positions.originOfPtr(attr.pos);
        if (!origin) continue;
        auto * df = std::get_if<Pos::TracedData>(origin);
        if (!df) continue;
        bool dup = false;
        for (auto * s : seen) { if (s == df) { dup = true; break; } }
        if (dup) continue;
        auto fmt = parseStructuredFormat(df->format);
        if (!fmt) continue;
        seen.push_back(df);
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

    // Record exists=false against all pre-computed origins
    auto recordAbsent = [&](Symbol keyName, const std::vector<IntersectOriginInfo> & origins) {
        for (auto & oi : origins) {
            CompactDepComponents c{oi.sourceId, oi.filePathId, oi.format, oi.dataPathId,
                                   ShapeSuffix::None, keyName};
            recordStructuredDep(c, DepHashValue(kHashZero()));
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

[[gnu::cold]] void maybeRecordHasKeyDep(const PosTable & positions, const SymbolTable & symbols,
                          const Value & v, Symbol keyName, bool exists)
{
    if (!DependencyTracker::isActive()) return;
    if (v.type() != nAttrs) return;
    if (!v.attrs()->hasAnyTracedDataLayer()) return;

    if (exists) {
        // Key was found — check tag bit, then look up origin.
        auto * attr = v.attrs()->get(keyName);
        if (!attr || !attr->pos.isTracedData()) return;
        auto * origin = positions.originOfPtr(attr->pos);
        if (!origin) return;
        auto * df = std::get_if<Pos::TracedData>(origin);
        if (!df) return; // Nix-added key (no TracedData provenance) — skip
        auto fmt = parseStructuredFormat(df->format);
        if (!fmt) return;
        CompactDepComponents c{df->sourceId, df->filePathId, *fmt, df->dataPathId,
                               ShapeSuffix::None, keyName};
        recordStructuredDep(c, DepHashValue(kHashOne()));
    } else {
        // Key not found — record against every unique TracedData origin.
        forEachTracedDataOrigin(positions, v, [&](const Pos::TracedData & df) {
            auto fmt = parseStructuredFormat(df.format);
            if (!fmt) return;
            CompactDepComponents c{df.sourceId, df.filePathId, *fmt, df.dataPathId,
                                   ShapeSuffix::None, keyName};
            recordStructuredDep(c, DepHashValue(kHashZero()));
        });
    }
}

} // namespace nix
