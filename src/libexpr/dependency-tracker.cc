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
// Traced container provenance map — shape dep tracking
// ═══════════════════════════════════════════════════════════════════════

// Thread-local map from stable list identity (first element Value*) to provenance.
// Lists still use this map because list provenance is tracked at access time
// (maybeRecordListLenDep), not via PosIdx. Attrsets use PosIdx-based DataFile
// origin tracking instead.
static thread_local std::unordered_map<const void*, const TracedContainerProvenance *> tracedContainerMap;

// Stable, non-GC pool for provenance data. std::deque never invalidates
// pointers on push_back, so ProvenanceRef pointers remain valid until clear().
// Cleared alongside tracedContainerMap on root DependencyTracker construction.
static thread_local std::deque<TracedContainerProvenance> provenancePool;

void DependencyTracker::onRootConstruction()
{
    clearTracedContainerMap();
    provenancePool.clear();
    clearReadFileProvenanceMap();
    clearReadFileStringPtrs();
}

ProvenanceRef allocateProvenance(std::string depSource, std::string depKey,
                                 std::string dataPath, StructuredFormat format)
{
    provenancePool.emplace_back(TracedContainerProvenance{
        std::move(depSource), std::move(depKey), std::move(dataPath), format});
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

// Record a dependency edge in the dynamic dependency graph (Adapton: "add-edge").
// BSàlC §3.2: during fresh evaluation, the scheduler records each dependency
// into the trace for later verification. Deduplication ensures each (type,
// source, key) triple appears at most once per trace, matching Salsa's
// "dependency de-duplication" invariant.
void DependencyTracker::record(const Dep & dep)
{
    if (activeTracker) {
        DepKey key(dep);
        if (!activeTracker->recordedKeys.insert(key).second)
            return;  // Dependency already recorded in this trace scope — skip duplicate
    }
    debug("recording %s (%s) dep: input='%s' key='%s'",
        depTypeName(dep.type), depKindName(depKind(dep.type)), dep.source,
        dep.type == DepType::StructuredContent ? formatStructuredDepKey(dep.key) : dep.key);
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
static thread_local std::unordered_map<Blake3Hash, ReadFileProvenance, Blake3Hash::Hasher> readFileProvenanceMap;

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
static thread_local std::unordered_map<const char *, Blake3Hash> readFileStringPtrs;

void addReadFileStringPtr(const char * ptr, const Blake3Hash & contentHash)
{
    readFileStringPtrs.emplace(ptr, contentHash);
}

void clearReadFileStringPtrs()
{
    readFileStringPtrs.clear();
}

void maybeRecordRawContentDep(EvalState & state, const Value & v)
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

void maybeRecordListLenDep(const Value & v)
{
    if (!DependencyTracker::isActive()) return;
    if (v.listSize() == 0) return; // Empty lists can't be tracked (no stable key)
    // Use first element Value* as key (matches registration in ExprTracedData::eval)
    auto * prov = lookupTracedContainer((const void *)v.listView()[0]);
    if (!prov) return;
    auto fullKey = buildStructuredDepKey(prov->depKey, prov->format, prov->dataPath, ShapeSuffix::Len);
    auto hash = depHash(std::to_string(v.listSize()));
    DependencyTracker::record({prov->depSource, fullKey, DepHashValue(hash), DepType::StructuredContent});
}

void maybeRecordAttrKeysDep(const PosTable & positions, const SymbolTable & symbols, const Value & v)
{
    if (!DependencyTracker::isActive()) return;
    if (v.type() != nAttrs) return;

    // Group attrs by their DataFile origin (pointer identity — all attrs from
    // the same addOrigin() call resolve to the same map entry).
    // Uses originOfPtr() to avoid copying Pos::Origin (which contains 3 strings
    // for DataFile variants).
    struct OriginKeys {
        const Pos::DataFile * df;
        std::vector<std::string_view> keys;
    };
    std::vector<OriginKeys> groups;

    for (auto & attr : *v.attrs()) {
        auto * origin = positions.originOfPtr(attr.pos);
        if (!origin) continue;
        auto * df = std::get_if<Pos::DataFile>(origin);
        if (!df) continue;
        // Find or create group for this origin (pointer identity)
        OriginKeys * group = nullptr;
        for (auto & g : groups) {
            if (g.df == df) { group = &g; break; }
        }
        if (!group) {
            groups.push_back({df, {}});
            group = &groups.back();
        }
        group->keys.push_back(symbols[attr.name]);
    }

    for (auto & g : groups) {
        auto fmt = parseStructuredFormat(g.df->format);
        if (!fmt) continue;
        std::sort(g.keys.begin(), g.keys.end());
        std::string canonical;
        for (size_t i = 0; i < g.keys.size(); i++) {
            if (i > 0) canonical += '\0';
            canonical += g.keys[i];
        }
        auto hash = depHash(canonical);
        auto fullKey = buildStructuredDepKey(g.df->depKey, *fmt, g.df->dataPath, ShapeSuffix::Keys);
        DependencyTracker::record({g.df->depSource, fullKey, DepHashValue(hash), DepType::StructuredContent});
    }
}

void maybeRecordTypeDep(const PosTable & positions, const Value & v)
{
    if (!DependencyTracker::isActive()) return;

    if (v.type() == nAttrs) {
        // Scan attrs for any DataFile origin — if found, record #type.
        // Uses originOfPtr() + pointer identity dedup to avoid string copies.
        auto hash = depHash("object");
        std::vector<const Pos::DataFile *> seen;
        for (auto & attr : *v.attrs()) {
            auto * origin = positions.originOfPtr(attr.pos);
            if (!origin) continue;
            auto * df = std::get_if<Pos::DataFile>(origin);
            if (!df) continue;
            bool dup = false;
            for (auto * s : seen) { if (s == df) { dup = true; break; } }
            if (dup) continue;
            auto fmt = parseStructuredFormat(df->format);
            if (!fmt) continue;
            seen.push_back(df);
            auto fullKey = buildStructuredDepKey(df->depKey, *fmt, df->dataPath, ShapeSuffix::Type);
            DependencyTracker::record({df->depSource, fullKey, DepHashValue(hash), DepType::StructuredContent});
        }
    } else if (v.type() == nList && v.listSize() > 0) {
        // Lists still use the existing tracedContainerMap lookup.
        auto * prov = lookupTracedContainer((const void *)v.listView()[0]);
        if (!prov) return;
        auto hash = depHash("array");
        auto fullKey = buildStructuredDepKey(prov->depKey, prov->format, prov->dataPath, ShapeSuffix::Type);
        DependencyTracker::record({prov->depSource, fullKey, DepHashValue(hash), DepType::StructuredContent});
    }
}

void maybeRecordHasKeyDep(const PosTable & positions, const SymbolTable & symbols,
                          const Value & v, Symbol keyName, bool exists)
{
    if (!DependencyTracker::isActive()) return;
    if (v.type() != nAttrs) return;

    auto keyStr = std::string(std::string_view(symbols[keyName]));
    auto escaped = escapeDataPathKey(keyStr);
    std::string rawSuffix = "#has:";
    rawSuffix += escaped;

    if (exists) {
        // Key was found — look up the Attr to check its PosIdx origin.
        auto * attr = v.attrs()->get(keyName);
        if (!attr) return;
        auto * origin = positions.originOfPtr(attr->pos);
        if (!origin) return;
        auto * df = std::get_if<Pos::DataFile>(origin);
        if (!df) return; // Nix-added key (no DataFile provenance) — skip
        auto fmt = parseStructuredFormat(df->format);
        if (!fmt) return;
        auto fullKey = buildStructuredDepKey(df->depKey, *fmt, df->dataPath, rawSuffix);
        auto hash = depHash("1");
        DependencyTracker::record({df->depSource, fullKey, DepHashValue(hash), DepType::StructuredContent});
    } else {
        // Key not found — record against every unique DataFile origin in this attrset.
        // Each origin represents a data file that could have contained the key.
        // Uses pointer identity for dedup (same addOrigin → same map entry).
        std::vector<const Pos::DataFile *> seen;
        auto hash = depHash("0");
        for (auto & attr : *v.attrs()) {
            auto * origin = positions.originOfPtr(attr.pos);
            if (!origin) continue;
            auto * df = std::get_if<Pos::DataFile>(origin);
            if (!df) continue;
            bool dup = false;
            for (auto * s : seen) {
                if (s == df) {
                    dup = true;
                    break;
                }
            }
            if (dup) continue;
            auto fmt = parseStructuredFormat(df->format);
            if (!fmt) continue;
            seen.push_back(df);
            auto fullKey = buildStructuredDepKey(df->depKey, *fmt, df->dataPath, rawSuffix);
            DependencyTracker::record({df->depSource, fullKey, DepHashValue(hash), DepType::StructuredContent});
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Provenance propagation for list-reconstructing operations
// ═══════════════════════════════════════════════════════════════════════

void propagateTrackedList(const Value & output, const Value & input, bool shapeModifying)
{
    if (!DependencyTracker::isActive()) return;
    if (input.listSize() == 0) return; // Empty input has no stable key
    auto * prov = lookupTracedContainer((const void *)input.listView()[0]);
    if (!prov) return;
    if (output.listSize() == 0) return; // Empty output has no stable key
    // For shapeModifying operations where output size differs, we skip
    // propagation to avoid recording incorrect #len deps.
    if (shapeModifying && output.listSize() != input.listSize())
        return;
    registerTracedContainer((const void *)output.listView()[0], prov);
}

void propagateTrackedListFromAny(const Value & output, size_t nInputs, Value * const * inputs)
{
    if (!DependencyTracker::isActive()) return;
    if (output.listSize() == 0) return;
    for (size_t i = 0; i < nInputs; i++) {
        if (inputs[i]->listSize() == 0) continue;
        auto * prov = lookupTracedContainer((const void *)inputs[i]->listView()[0]);
        if (!prov) continue;
        // For concat: output size always differs, so skip propagation
        // (same as shapeModified=true behavior).
        if (output.listSize() != inputs[i]->listSize())
            return;
        registerTracedContainer((const void *)output.listView()[0], prov);
        return;
    }
}

} // namespace nix
