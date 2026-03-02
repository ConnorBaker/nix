#include "nix/expr/eval-trace/store/stat-hash-store.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/util/file-system.hh"
#include "nix/util/logging.hh"
#include "nix/util/source-path.hh"
#include "nix/util/util.hh"

#include <filesystem>

namespace nix {

// ── StatHashStore singleton + methods ───────────────────────────────

StatHashStore & StatHashStore::instance()
{
    static StatHashStore store;
    return store;
}

std::optional<StatHashStore::LookupResult> StatHashStore::lookupHash(const std::filesystem::path & physPath, DepType type)
{
    auto st = maybeLstat(physPath);
    if (!st)
        return std::nullopt;

    auto fingerprint = FileFingerprint::fromStat(*st);
    auto key = Key{physPath.string(), type};

    auto it = cache.find(key);
    if (it != cache.end()) {
        if (it->second.stat == fingerprint) {
            debug("stat hash store: hit for '%s' (%s)", physPath.string(), depTypeName(type));
            return LookupResult{*st, it->second.hash};
        }
    }

    debug("stat hash store: miss for '%s' (%s)", physPath.string(), depTypeName(type));
    return LookupResult{*st, std::nullopt};
}

void StatHashStore::storeHash(
    const std::filesystem::path & physPath, DepType type,
    const Blake3Hash & hash, std::optional<PosixStat> st)
{
    if (!st) {
        st = maybeLstat(physPath);
        if (!st)
            return;
    }
    auto fingerprint = FileFingerprint::fromStat(*st);
    auto key = Key{physPath.string(), type};
    auto value = Value{.stat = fingerprint, .hash = hash};

    cache[key] = value;
    dirtyEntries[key] = value;
}

void StatHashStore::clear()
{
    cache.clear();
}

void StatHashStore::load(Map entries)
{
    for (auto & [k, e] : entries)
        cache[k] = std::move(e);
    debug("stat hash store: loaded %d entries from TraceStore", entries.size());
}

void StatHashStore::forEachDirty(
    std::function<void(const Key &, const Value &)> callback)
{
    for (auto & [k, e] : dirtyEntries)
        callback(k, e);
    dirtyEntries.clear();
}

// ── Stat-cached dep hash functions ──────────────────────────────────

Blake3Hash StatHashStore::depHashFile(const SourcePath & path)
{
    if (auto physPath = path.getPhysicalPath()) {
        if (auto result = lookupHash(*physPath, DepType::Content)) {
            if (result->hash)
                return *result->hash;
            auto hash = depHash(path.readFile());
            storeHash(*physPath, DepType::Content, hash, result->stat);
            return hash;
        }
    }
    return depHash(path.readFile());
}

Blake3Hash StatHashStore::depHashPathCached(const SourcePath & path)
{
    if (auto physPath = path.getPhysicalPath()) {
        if (auto result = lookupHash(*physPath, DepType::NARContent)) {
            if (result->hash)
                return *result->hash;
            auto hash = depHashPath(path);
            storeHash(*physPath, DepType::NARContent, hash, result->stat);
            return hash;
        }
    }
    return depHashPath(path);
}

Blake3Hash StatHashStore::depHashDirListingCached(const SourcePath & path, const SourceAccessor::DirEntries & entries)
{
    if (auto physPath = path.getPhysicalPath()) {
        if (auto result = lookupHash(*physPath, DepType::Directory)) {
            if (result->hash)
                return *result->hash;
            auto hash = depHashDirListing(entries);
            storeHash(*physPath, DepType::Directory, hash, result->stat);
            return hash;
        }
    }
    return depHashDirListing(entries);
}

} // namespace nix
