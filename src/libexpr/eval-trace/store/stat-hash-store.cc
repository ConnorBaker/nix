#include "nix/expr/eval-trace/store/stat-hash-store.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/util/file-system.hh"
#include "nix/util/logging.hh"
#include "nix/util/source-path.hh"
#include "nix/util/util.hh"

#include <filesystem>

namespace nix {

// ═══════════════════════════════════════════════════════════════════════
// StatHashStore — persistent (path, depType) → (FileFingerprint, BLAKE3)
// Implementation detail: invisible to all consumers.
// ═══════════════════════════════════════════════════════════════════════

namespace {

// ── Helpers ─────────────────────────────────────────────────────────

static FileFingerprint makeFingerprint(const PosixStat & st)
{
    auto [sec, nsec] = [&](auto & s) {
        if constexpr (requires { s.st_mtimespec; })
            return std::pair{s.st_mtimespec.tv_sec, s.st_mtimespec.tv_nsec};
        else
            return std::pair{s.st_mtim.tv_sec, s.st_mtim.tv_nsec};
    }(st);
    return {
        .ino = st.st_ino,
        .mtime_sec = sec,
        .mtime_nsec = nsec,
        .size = st.st_size,
        .dev = st.st_dev,
    };
}

// ── StatHashStore singleton + public API ─────────────────────────────

struct StatHashStore
{
    struct LookupResult {
        std::optional<Blake3Hash> hash;
        std::optional<PosixStat> stat;
    };

    static StatHashStore & instance()
    {
        static StatHashStore store;
        return store;
    }

    LookupResult lookupHash(const std::filesystem::path & physPath, DepType type)
    {
        auto st = maybeLstat(physPath);
        if (!st)
            return {std::nullopt, std::nullopt};

        auto fingerprint = makeFingerprint(*st);
        auto key = StatCacheKey{physPath.string(), type};

        auto it = cache.find(key);
        if (it != cache.end()) {
            if (it->second.stat == fingerprint) {
                debug("stat hash store: hit for '%s' (%s)", physPath.string(), depTypeName(type));
                return {it->second.hash, *st};
            }
        }

        debug("stat hash store: miss for '%s' (%s)", physPath.string(), depTypeName(type));
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
        auto fingerprint = makeFingerprint(*st);
        auto key = StatCacheKey{physPath.string(), type};
        auto entry = StatCacheEntry{.stat = fingerprint, .hash = hash};

        cache[key] = entry;
        dirtyEntries[key] = entry;
    }

    void clear()
    {
        cache.clear();
    }

    void bulkLoadEntries(StatCacheMap entries)
    {
        for (auto & [k, e] : entries)
            cache[k] = std::move(e);
        debug("stat hash store: loaded %d entries from TraceStore", entries.size());
    }

    void flushDirtyEntries(
        std::function<void(const StatCacheKey &, const StatCacheEntry &)> callback)
    {
        for (auto & [k, e] : dirtyEntries)
            callback(k, e);
        dirtyEntries.clear();
    }

private:
    StatCacheMap cache;
    StatCacheMap dirtyEntries;

    StatHashStore() = default;
    ~StatHashStore() = default;
};

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════
// Stat-cached dep hash functions
// ═══════════════════════════════════════════════════════════════════════

Blake3Hash depHashFile(const SourcePath & path)
{
    if (auto physPath = path.getPhysicalPath()) {
        auto result = StatHashStore::instance().lookupHash(*physPath, DepType::Content);
        if (result.hash)
            return *result.hash;
        auto content = path.readFile();
        auto hash = depHash(content);
        if (result.stat)
            StatHashStore::instance().storeHash(*physPath, DepType::Content, hash, *result.stat);
        return hash;
    }
    return depHash(path.readFile());
}

Blake3Hash depHashPathCached(const SourcePath & path)
{
    if (auto physPath = path.getPhysicalPath()) {
        auto result = StatHashStore::instance().lookupHash(*physPath, DepType::NARContent);
        if (result.hash)
            return *result.hash;
        auto hash = depHashPath(path);
        if (result.stat)
            StatHashStore::instance().storeHash(*physPath, DepType::NARContent, hash, *result.stat);
        return hash;
    }
    return depHashPath(path);
}

Blake3Hash depHashDirListingCached(const SourcePath & path, const SourceAccessor::DirEntries & entries)
{
    if (auto physPath = path.getPhysicalPath()) {
        auto result = StatHashStore::instance().lookupHash(*physPath, DepType::Directory);
        if (result.hash)
            return *result.hash;
        auto hash = depHashDirListing(entries);
        if (result.stat)
            StatHashStore::instance().storeHash(*physPath, DepType::Directory, hash, *result.stat);
        return hash;
    }
    return depHashDirListing(entries);
}

// ═══════════════════════════════════════════════════════════════════════
// Public API wrappers
// ═══════════════════════════════════════════════════════════════════════

void storeStatHash(
    const std::filesystem::path & physPath, DepType depType,
    const Blake3Hash & hash, std::optional<PosixStat> stat)
{
    StatHashStore::instance().storeHash(physPath, depType, hash, stat);
}

void clearStatHashStore()
{
    StatHashStore::instance().clear();
}

void loadStatHashStore(StatCacheMap entries)
{
    StatHashStore::instance().bulkLoadEntries(std::move(entries));
}

void forEachDirtyStatHashEntry(
    std::function<void(const StatCacheKey &, const StatCacheEntry &)> callback)
{
    StatHashStore::instance().flushDirtyEntries(std::move(callback));
}

} // namespace nix
