#include "nix/expr/stat-hash-cache.hh"
#include "nix/store/sqlite.hh"
#include "nix/util/file-system.hh"
#include "nix/util/logging.hh"
#include "nix/util/sync.hh"
#include "nix/util/users.hh"
#include "nix/util/util.hh"

#include <boost/unordered/concurrent_flat_map.hpp>

namespace nix {

// ── L1 key: (dev, ino, mtime_sec, mtime_nsec, size, depType) ────────

struct StatHashKey
{
    dev_t dev;
    ino_t ino;
    int64_t mtime_sec;
    int64_t mtime_nsec;
    off_t size;
    DepType depType;

    bool operator==(const StatHashKey &) const = default;
};

struct StatHashKeyHash
{
    std::size_t operator()(const StatHashKey & k) const noexcept
    {
        // FNV-1a-style combine
        std::size_t h = 14695981039346656037ULL;
        auto mix = [&](auto v) {
            h ^= static_cast<std::size_t>(v);
            h *= 1099511628211ULL;
        };
        mix(k.dev);
        mix(k.ino);
        mix(k.mtime_sec);
        mix(k.mtime_nsec);
        mix(k.size);
        mix(static_cast<int>(k.depType));
        return h;
    }
};

// ── L2: SQLite state ────────────────────────────────────────────────

struct SQLiteState
{
    SQLite db;
    SQLiteStmt queryHash;
    SQLiteStmt upsertHash;
    SQLiteStmt queryAll;
    std::unique_ptr<SQLiteTxn> txn;  // Wraps all writes in one transaction; destroyed first (reverse order)
};

// ── Impl ────────────────────────────────────────────────────────────

static constexpr size_t L1_MAX_SIZE = 65536;

// ── L2 bulk load entry ───────────────────────────────────────────────

struct L2Entry {
    dev_t dev; ino_t ino;
    int64_t mtime_sec; int64_t mtime_nsec;
    off_t size;
    Blake3Hash hash;
};

// Key for L2 bulk map: path + dep_type
struct L2BulkKey {
    std::string path;
    DepType depType;

    bool operator==(const L2BulkKey &) const = default;
};

struct L2BulkKeyHash {
    std::size_t operator()(const L2BulkKey & k) const noexcept {
        std::size_t h = std::hash<std::string>{}(k.path);
        h ^= std::hash<int>{}(static_cast<int>(k.depType)) * 1099511628211ULL;
        return h;
    }
};

struct StatHashCache::Impl
{
    boost::concurrent_flat_map<StatHashKey, Blake3Hash, StatHashKeyHash> l1;
    Sync<SQLiteState> sqliteState;
    bool sqliteAvailable = false;

    // L2 bulk load cache
    std::unordered_map<L2BulkKey, L2Entry, L2BulkKeyHash> l2Bulk;
    bool l2Loaded = false;

    ~Impl()
    {
        if (sqliteAvailable) {
            try {
                auto state(sqliteState.lock());
                if (state->txn)
                    state->txn->commit();
            } catch (...) {
                ignoreExceptionExceptInterrupt();
            }
        }
    }

    Impl()
    {
        try {
            auto state(sqliteState.lock());

            auto cacheDir = getCacheDir();
            createDirs(cacheDir);

            auto dbPath = (cacheDir / "stat-hash-cache-v2.sqlite").string();
            state->db = SQLite(dbPath, {.useWAL = true});
            state->db.isCache();
            state->db.exec("pragma cache_size = -2000");    // 2MB page cache
            state->db.exec("pragma mmap_size = 10000000");  // 10MB mmap
            state->db.exec("pragma temp_store = memory");
            state->db.exec("pragma journal_size_limit = 2097152");  // 2MB WAL limit

            state->db.exec(R"sql(
                create table if not exists StatHashCache (
                    path       text not null,
                    dep_type   integer not null,
                    dev        integer not null,
                    ino        integer not null,
                    mtime_sec  integer not null,
                    mtime_nsec integer not null,
                    size       integer not null,
                    hash       blob not null,
                    primary key (path, dep_type)
                ) strict
            )sql");

            state->queryHash.create(
                state->db,
                "select hash from StatHashCache "
                "where path = ? and dep_type = ? "
                "and dev = ? and ino = ? and mtime_sec = ? and mtime_nsec = ? and size = ?");

            state->upsertHash.create(
                state->db,
                "insert into StatHashCache (path, dep_type, dev, ino, mtime_sec, mtime_nsec, size, hash) "
                "values (?, ?, ?, ?, ?, ?, ?, ?) "
                "on conflict (path, dep_type) do update set "
                "dev = excluded.dev, ino = excluded.ino, "
                "mtime_sec = excluded.mtime_sec, mtime_nsec = excluded.mtime_nsec, "
                "size = excluded.size, hash = excluded.hash");

            state->queryAll.create(
                state->db,
                "select path, dep_type, dev, ino, mtime_sec, mtime_nsec, size, hash from StatHashCache");

            state->txn = std::make_unique<SQLiteTxn>(state->db.db);

            sqliteAvailable = true;
        } catch (SQLiteError & e) {
            warn("stat hash cache: failed to open database: %s", e.what());
        } catch (std::exception & e) {
            warn("stat hash cache: failed to initialize: %s", e.what());
        }
    }
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

// ── Public API ──────────────────────────────────────────────────────

StatHashCache & StatHashCache::instance()
{
    static StatHashCache cache;
    return cache;
}

StatHashCache::StatHashCache()
    : impl(std::make_unique<Impl>())
{
}

StatHashCache::~StatHashCache() = default;

void StatHashCache::ensureL2Loaded()
{
    if (impl->l2Loaded || !impl->sqliteAvailable)
        return;
    impl->l2Loaded = true;

    try {
        auto state(impl->sqliteState.lock());
        auto query = state->queryAll.use();
        while (query.next()) {
            auto path = query.getStr(0);
            auto depType = static_cast<DepType>(query.getInt(1));
            auto [hashBlob, hashLen] = query.getBlob(7);
            if (hashLen != 32) continue;

            impl->l2Bulk[L2BulkKey{path, depType}] = L2Entry{
                .dev = static_cast<dev_t>(query.getInt(2)),
                .ino = static_cast<ino_t>(query.getInt(3)),
                .mtime_sec = query.getInt(4),
                .mtime_nsec = query.getInt(5),
                .size = static_cast<off_t>(query.getInt(6)),
                .hash = Blake3Hash::fromBlob(hashBlob, hashLen),
            };
        }
        debug("stat hash cache: loaded %d L2 entries", impl->l2Bulk.size());
    } catch (SQLiteError & e) {
        warn("stat hash cache: bulk load failed: %s", e.what());
    }
}

StatHashCache::LookupResult StatHashCache::lookupHash(
    const std::filesystem::path & physPath, DepType type)
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

    // L2: bulk-loaded lookup
    ensureL2Loaded();
    auto pathStr = physPath.string();
    auto l2It = impl->l2Bulk.find(L2BulkKey{pathStr, type});
    if (l2It != impl->l2Bulk.end()) {
        auto & entry = l2It->second;
        // Validate stat metadata
        if (entry.dev == key.dev && entry.ino == key.ino
            && entry.mtime_sec == key.mtime_sec && entry.mtime_nsec == key.mtime_nsec
            && entry.size == key.size)
        {
            // Promote to L1
            if (impl->l1.size() < L1_MAX_SIZE)
                impl->l1.emplace(key, entry.hash);

            debug("stat hash cache: L2 hit for '%s' (%s)", physPath.string(), depTypeName(type));
            return {entry.hash, *st};
        }
    }

    debug("stat hash cache: miss for '%s' (%s)", physPath.string(), depTypeName(type));
    return {std::nullopt, *st};
}

void StatHashCache::storeHash(
    const std::filesystem::path & physPath, DepType type,
    const Blake3Hash & hash, const PosixStat & st)
{
    auto key = makeKey(st, type);

    // L1: store in memory
    if (impl->l1.size() < L1_MAX_SIZE)
        impl->l1.emplace_or_visit(key,
            hash,
            [&](auto & entry) { entry.second = hash; });

    // L2: store in SQLite as BLOB — raw 32-byte BLAKE3 hash
    if (impl->sqliteAvailable) {
        try {
            auto state(impl->sqliteState.lock());
            auto pathStr = physPath.string();
            state->upsertHash.use()
                (pathStr)
                (static_cast<int>(type))
                (static_cast<int64_t>(key.dev))
                (static_cast<int64_t>(key.ino))
                (key.mtime_sec)
                (key.mtime_nsec)
                (static_cast<int64_t>(key.size))
                (hash.data(), hash.size())
                .exec();
        } catch (SQLiteError & e) {
            warn("stat hash cache: store failed: %s", e.what());
        }
    }

    // Update L2 bulk cache
    if (impl->l2Loaded) {
        auto pathStr = physPath.string();
        impl->l2Bulk[L2BulkKey{pathStr, type}] = L2Entry{
            .dev = st.st_dev,
            .ino = st.st_ino,
#ifdef __APPLE__
            .mtime_sec = st.st_mtimespec.tv_sec,
            .mtime_nsec = st.st_mtimespec.tv_nsec,
#else
            .mtime_sec = st.st_mtim.tv_sec,
            .mtime_nsec = st.st_mtim.tv_nsec,
#endif
            .size = st.st_size,
            .hash = hash,
        };
    }
}

void StatHashCache::storeHash(
    const std::filesystem::path & physPath, DepType type,
    const Blake3Hash & hash)
{
    auto st = maybeLstat(physPath);
    if (!st)
        return;
    storeHash(physPath, type, hash, *st);
}

void StatHashCache::clearMemoryCache()
{
    impl->l1.clear();
}

} // namespace nix
