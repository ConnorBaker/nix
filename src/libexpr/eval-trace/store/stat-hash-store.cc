#include "nix/expr/eval-trace/store/stat-hash-store.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/store/globals.hh"
#include "nix/store/sqlite.hh"
#include "nix/util/file-system.hh"
#include "nix/util/logging.hh"
#include "nix/util/source-path.hh"
#include "nix/util/users.hh"
#include "nix/util/util.hh"

#include <filesystem>

namespace nix::eval_trace {

// ── Schema ──────────────────────────────────────────────────────────

static const char * statHashSchema = R"sql(
    CREATE TABLE IF NOT EXISTS StatHashCache (
        path       TEXT NOT NULL,
        dep_type   INTEGER NOT NULL,
        dev        INTEGER NOT NULL,
        ino        INTEGER NOT NULL,
        mtime_sec  INTEGER NOT NULL,
        mtime_nsec INTEGER NOT NULL,
        size       INTEGER NOT NULL,
        hash       BLOB NOT NULL,
        PRIMARY KEY (path, dep_type)
    ) STRICT;
)sql";

// ── Singleton + DB lifecycle ────────────────────────────────────────

StatHashStore & StatHashStore::instance()
{
    static StatHashStore store;
    return store;
}

std::filesystem::path StatHashStore::getDbPath() const
{
    return std::filesystem::path(getCacheDir()) / "stat-hash-cache.sqlite";
}

void StatHashStore::ensureSchema()
{
    auto path = getDbPath();
    createDirs(path.parent_path());

    SQLite db(path, {.useWAL = settings.useSQLiteWAL});
    db.isCache();
    db.exec(statHashSchema);
}

// ── Cache operations ────────────────────────────────────────────────

std::optional<StatHashStore::LookupResult> StatHashStore::lookupHash(const std::filesystem::path & physPath, DepType type)
{
    auto st = maybeLstat(physPath);
    if (!st)
        return std::nullopt;

    auto fingerprint = FileFingerprint::fromStat(*st);
    auto key = Key{physPath.string(), type};

    auto it = cache.find(key);
    if (it != cache.end() && it->second.stat == fingerprint) {
        debug("stat hash store: hit for '%s' (%s)", physPath.string(), depTypeName(type));
        return LookupResult{*st, it->second.hash};
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
    debug("stat hash store: loading %d entries", entries.size());
    cache.merge(entries);
}

StatHashStore::Map StatHashStore::takeDirty()
{
    return std::exchange(dirtyEntries, {});
}

// ── Stat-cached dep hash functions ──────────────────────────────────

Blake3Hash StatHashStore::cachedHash(
    const SourcePath & path, DepType depType, auto && computeHash)
{
    if (auto physPath = path.getPhysicalPath()) {
        if (auto result = lookupHash(*physPath, depType)) {
            if (result->hash)
                return *result->hash;
            auto hash = computeHash();
            storeHash(*physPath, depType, hash, result->stat);
            return hash;
        }
    }
    return computeHash();
}

Blake3Hash StatHashStore::depHashFile(const SourcePath & path)
{
    return cachedHash(path, DepType::Content, [&] { return depHash(path.readFile()); });
}

Blake3Hash StatHashStore::depHashPathCached(const SourcePath & path)
{
    return cachedHash(path, DepType::NARContent, [&] { return depHashPath(path); });
}

Blake3Hash StatHashStore::depHashDirListingCached(const SourcePath & path, const SourceAccessor::DirEntries & entries)
{
    return cachedHash(path, DepType::Directory, [&] { return depHashDirListing(entries); });
}

} // namespace nix::eval_trace
