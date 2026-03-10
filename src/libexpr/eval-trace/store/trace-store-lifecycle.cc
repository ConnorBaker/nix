/// trace-store-lifecycle.cc — TraceStore construction, destruction, bulk load, flush
///
/// Split from trace-store.cc (Phase 4b). Contains:
///   - SQL schema definition
///   - TraceStore constructor (DB init, statement prep, bulk load)
///   - TraceStore destructor (flush, vocab/stat-hash commit)
///   - clearSessionCaches, bulkLoadAllLocked, bulkLoadAll, flush

#include "nix/expr/eval-trace/store/trace-store.hh"
#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/eval-trace/store/stat-hash-store.hh"
#include "nix/expr/eval-trace/counters.hh"
#include "nix/store/globals.hh"

#include "nix/util/users.hh"
#include "nix/util/util.hh"

#include "trace-serialize.hh"

#include <filesystem>

namespace nix::eval_trace {

// ── Schema ───────────────────────────────────────────────────────────

static const char * schema = R"sql(

    CREATE TABLE IF NOT EXISTS Strings (
        id    INTEGER PRIMARY KEY,
        value TEXT NOT NULL UNIQUE
    ) STRICT;

    CREATE TABLE IF NOT EXISTS Results (
        id      INTEGER PRIMARY KEY,
        type    INTEGER NOT NULL,
        value   TEXT,
        context TEXT,
        hash    BLOB NOT NULL UNIQUE
    ) STRICT;

    -- Content-addressed dep key sets. Traces with the same dep structure
    -- (same types + sources + keys, different hash values) share a single
    -- DepKeySets row. struct_hash is the BLAKE3 of the structural signature
    -- (type + source + key per dep).
    CREATE TABLE IF NOT EXISTS DepKeySets (
        id          INTEGER PRIMARY KEY,
        struct_hash BLOB NOT NULL UNIQUE,
        keys_blob   BLOB NOT NULL
    ) STRICT;

    -- Each trace references a shared DepKeySets row (dep structure) and stores
    -- its own values_blob (hash values in positional order matching keys_blob).
    CREATE TABLE IF NOT EXISTS Traces (
        id              INTEGER PRIMARY KEY,
        trace_hash      BLOB NOT NULL UNIQUE,
        dep_key_set_id  INTEGER NOT NULL REFERENCES DepKeySets(id),
        values_blob     BLOB NOT NULL
    ) STRICT;

    CREATE INDEX IF NOT EXISTS idx_traces_dep_key_set ON Traces(dep_key_set_id);

    CREATE TABLE IF NOT EXISTS CurrentTraces (
        context_hash  INTEGER NOT NULL,
        attr_path_id  INTEGER NOT NULL,
        trace_id    INTEGER NOT NULL,
        result_id     INTEGER NOT NULL,
        PRIMARY KEY (context_hash, attr_path_id)
    ) WITHOUT ROWID, STRICT;

    CREATE TABLE IF NOT EXISTS TraceHistory (
        context_hash  INTEGER NOT NULL,
        attr_path_id  INTEGER NOT NULL,
        trace_id    INTEGER NOT NULL,
        result_id     INTEGER NOT NULL,
        PRIMARY KEY (context_hash, attr_path_id, trace_id)
    ) WITHOUT ROWID, STRICT;

    -- Normalized dir-set definitions for aggregated DirSet deps.
    -- Each row stores the dirs JSON array once, keyed by content hash.
    -- DirSet dep keys reference this table by ds_hash instead of
    -- embedding the full dirs array (~44 KB) in every dep key string.
    CREATE TABLE IF NOT EXISTS DirSets (
        ds_hash TEXT PRIMARY KEY,
        dirs    TEXT NOT NULL
    ) STRICT;

)sql";

// ── Constructor / Destructor ─────────────────────────────────────────

TraceStore::TraceStore(SymbolTable & symbols, InterningPools & pools, AttrVocabStore & vocab, int64_t contextHash)
    : symbols(symbols)
    , pools(pools)
    , vocab(vocab)
    , contextHash(contextHash)
{
    auto initStart = timerStart();
    auto state = std::make_unique<Sync<State>>();
    auto st(state->lock());

    auto cacheDir = std::filesystem::path(getCacheDir());
    createDirs(cacheDir);

    auto dbPath = cacheDir / "eval-trace-v4.sqlite";

    st->db = SQLite(dbPath, {.useWAL = settings.useSQLiteWAL});
    st->db.exec("pragma page_size = 65536");            // 64KB pages for large BLOB I/O (MUST be before isCache)
    st->db.isCache();
    st->db.exec("pragma cache_size = -16000");           // 16MB page cache
    st->db.exec("pragma mmap_size = 268435456");         // 256MB mmap
    st->db.exec("pragma temp_store = memory");
    st->db.exec("pragma journal_size_limit = 2097152");  // 2MB WAL limit

    st->db.exec(schema);

    // ATTACH vocab and stat-hash databases for atomic cross-DB commits.
    // A single BEGIN/COMMIT spans all attached databases.
    {
        // SQLite ATTACH doesn't support parameter binding, so we quote
        // the path by doubling any embedded single quotes.
        auto quotePath = [](std::string s) {
            std::string out;
            out.reserve(s.size());
            for (char c : s) {
                out += c;
                if (c == '\'') out += '\'';
            }
            return out;
        };
        auto & statStore = StatHashStore::instance();
        statStore.ensureSchema();

        auto vocabPath = quotePath(vocab.getDbPath().string());
        auto statPath = quotePath(statStore.getDbPath().string());

        st->db.exec("ATTACH DATABASE '" + vocabPath + "' AS vocab");
        st->db.exec("ATTACH DATABASE '" + statPath + "' AS stat_cache");

        // Set WAL + cache pragmas on attached DBs
        if (settings.useSQLiteWAL) {
            st->db.exec("PRAGMA vocab.journal_mode = wal");
            st->db.exec("PRAGMA stat_cache.journal_mode = wal");
        }
        st->db.exec("PRAGMA vocab.synchronous = off");
        st->db.exec("PRAGMA stat_cache.synchronous = off");

        // Both schemas are already created by their respective constructors'
        // one-shot connections (AttrVocabStore, StatHashStore).
    }

    // Strings — bulk load + explicit-ID insert (no UPSERT)
    st->getAllStrings.create(st->db,
        "SELECT id, value FROM Strings");
    st->insertStringWithId.create(st->db,
        "INSERT OR IGNORE INTO Strings(id, value) VALUES (?, ?)");

    // Results — bulk load + explicit-ID insert
    st->getAllResults.create(st->db,
        "SELECT id, type, value, context, hash FROM Results");
    st->insertResultWithId.create(st->db,
        "INSERT OR IGNORE INTO Results(id, type, value, context, hash) VALUES (?, ?, ?, ?, ?)");

    st->getResult.create(st->db,
        "SELECT type, value, context FROM Results WHERE id = ?");

    // DepKeySets — bulk load + explicit-ID insert
    st->getAllDepKeySets.create(st->db,
        "SELECT id, struct_hash FROM DepKeySets");
    st->insertDepKeySetWithId.create(st->db,
        "INSERT OR IGNORE INTO DepKeySets(id, struct_hash, keys_blob) VALUES (?, ?, ?)");

    st->getDepKeySet.create(st->db,
        "SELECT struct_hash, keys_blob FROM DepKeySets WHERE id = ?");

    // Traces — bulk load + explicit-ID insert
    st->getAllTraces.create(st->db,
        "SELECT id, trace_hash, dep_key_set_id FROM Traces");
    st->insertTraceWithId.create(st->db,
        "INSERT OR IGNORE INTO Traces(id, trace_hash, dep_key_set_id, values_blob) VALUES (?, ?, ?, ?)");

    st->lookupTraceByFullHash.create(st->db,
        "SELECT id FROM Traces WHERE trace_hash = ?");

    // getTraceInfo: returns trace_hash, struct_hash, dep_key_set_id, keys_blob, values_blob
    // via JOIN with DepKeySets. Used by loadFullTrace, getTraceStructHash, getCurrentTraceHash.
    st->getTraceInfo.create(st->db,
        "SELECT t.trace_hash, dk.struct_hash, dk.id, dk.keys_blob, t.values_blob "
        "FROM Traces t JOIN DepKeySets dk ON t.dep_key_set_id = dk.id WHERE t.id = ?");

    // CurrentTraces (JOIN with Results to get result fields)
    st->lookupAttr.create(st->db,
        "SELECT a.trace_id, a.result_id, r.type, r.value, r.context "
        "FROM CurrentTraces a JOIN Results r ON a.result_id = r.id "
        "WHERE a.context_hash = ? AND a.attr_path_id = ?");

    st->upsertAttr.create(st->db,
        "INSERT INTO CurrentTraces(context_hash, attr_path_id, trace_id, result_id) "
        "VALUES (?, ?, ?, ?) "
        "ON CONFLICT(context_hash, attr_path_id) DO UPDATE SET "
        "trace_id = excluded.trace_id, result_id = excluded.result_id");

    // History
    st->insertHistory.create(st->db,
        "INSERT OR IGNORE INTO TraceHistory(context_hash, attr_path_id, trace_id, result_id) "
        "VALUES (?, ?, ?, ?)");

    st->lookupHistoryByTrace.create(st->db,
        "SELECT result_id FROM TraceHistory "
        "WHERE context_hash = ? AND attr_path_id = ? AND trace_id = ?");

    // scanHistoryForAttr: JOIN through Traces→DepKeySets→Results to pre-load
    // all data needed for recovery in a single query. Columns:
    //   0=dk.id, 1=dk.struct_hash, 2=h.trace_id, 3=h.result_id,
    //   4=t.trace_hash, 5=r.type, 6=r.value, 7=r.context
    // The trace_hash enables in-memory candidate matching (no per-group DB lookup).
    // The result data eliminates getResult calls in acceptRecoveredTrace.
    st->scanHistoryForAttr.create(st->db,
        "SELECT dk.id, dk.struct_hash, h.trace_id, h.result_id, "
        "       t.trace_hash, r.type, r.value, r.context "
        "FROM TraceHistory h "
        "JOIN Traces t ON h.trace_id = t.id "
        "JOIN DepKeySets dk ON t.dep_key_set_id = dk.id "
        "JOIN Results r ON h.result_id = r.id "
        "WHERE h.context_hash = ? AND h.attr_path_id = ?");

    // DirSets (normalized dir-set definitions)
    st->insertDirSet.create(st->db,
        "INSERT OR IGNORE INTO DirSets(ds_hash, dirs) VALUES (?, ?)");
    st->getAllDirSets.create(st->db,
        "SELECT ds_hash, dirs FROM DirSets");

    // Vocab (on ATTACH'd vocab.* schema)
    st->insertVocabName.create(st->db,
        "INSERT OR IGNORE INTO vocab.AttrNames(id, name) VALUES (?, ?)");
    st->insertVocabPath.create(st->db,
        "INSERT OR IGNORE INTO vocab.AttrPaths(id, parent, child) VALUES (?, ?, ?)");

    // StatHashCache (on ATTACH'd stat_cache.* schema)
    st->upsertStatHash.create(st->db,
        "INSERT INTO stat_cache.StatHashCache (path, dep_type, dev, ino, mtime_sec, mtime_nsec, size, hash) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT (path, dep_type) DO UPDATE SET "
        "dev = excluded.dev, ino = excluded.ino, "
        "mtime_sec = excluded.mtime_sec, mtime_nsec = excluded.mtime_nsec, "
        "size = excluded.size, hash = excluded.hash");

    st->txn = std::make_unique<SQLiteTxn>(st->db);

    // Bulk-load StatHashCache entries into in-memory singleton
    {
        SQLiteStmt queryAllStatHash;
        queryAllStatHash.create(st->db,
            "SELECT path, dep_type, dev, ino, mtime_sec, mtime_nsec, size, hash "
            "FROM stat_cache.StatHashCache");

        StatHashStore::Map entries;
        auto use(queryAllStatHash.use());
        while (use.next()) {
            auto [hashBlob, hashLen] = use.getBlob(7);
            if (hashLen != 32) continue;
            entries.emplace(
                StatHashStore::Key{use.getStr(0), static_cast<DepType>(use.getInt(1))},
                StatHashStore::Value{
                    .stat = {
                        .ino = static_cast<ino_t>(use.getInt(3)),
                        .mtime_sec = static_cast<time_t>(use.getInt(4)),
                        .mtime_nsec = use.getInt(5),
                        .size = static_cast<off_t>(use.getInt(6)),
                        .dev = static_cast<dev_t>(use.getInt(2)),
                    },
                    .hash = Blake3Hash::fromBlob(hashBlob, hashLen),
                });
        }
        StatHashStore::instance().load(std::move(entries));
    }

    bulkLoadAllLocked(*st);

    _state = std::move(state);

    nrDbInitTimeUs += elapsedUs(initStart);
}

TraceStore::~TraceStore()
{
    auto closeStart = timerStart();
    // Flush all pending in-memory writes to SQLite
    try {
        flush();
    } catch (...) {
        ignoreExceptionExceptInterrupt();
    }
    // Flush vocab + stat-hash entries and commit — all within a single
    // lock acquisition. Vocab is flushed here (not in flush()) to avoid
    // holding a RESERVED lock on vocab.sqlite across the session, which
    // would cause SQLITE_BUSY when multiple TraceStores ATTACH the same
    // vocab DB.
    try {
        auto st(_state->lock());
        vocab.flushTo(st->insertVocabName, st->insertVocabPath);
        for (auto & [key, v] : StatHashStore::instance().takeDirty()) {
            st->upsertStatHash.use()
                (key.path)
                (static_cast<int64_t>(std::to_underlying(key.depType)))
                (static_cast<int64_t>(v.stat.dev))
                (static_cast<int64_t>(v.stat.ino))
                (v.stat.mtime_sec)
                (v.stat.mtime_nsec)
                (static_cast<int64_t>(v.stat.size))
                (v.hash.data(), v.hash.size())
                .exec();
        }
        if (st->txn)
            st->txn->commit();
    } catch (...) {
        ignoreExceptionExceptInterrupt();
    }
    nrDbCloseTimeUs += elapsedUs(closeStart);
}

// ── Helpers ──────────────────────────────────────────────────────────

void TraceStore::clearSessionCaches()
{
    verifiedTraceIds.clear();
    resultByHash.clear();
    depKeySetByStructHash.clear();
    traceByTraceHash.clear();
    traceDataCache.clear();
    traceRowCache.clear();
    depKeySetCache.clear();
    pools.strings.clear();
    pools.dirSets.clear();
    flushedStringHighWaterMark = 0;
    nextResultId = ResultId();
    nextDepKeySetId = DepKeySetId();
    nextTraceId = TraceId();
    pendingResults.clear();
    pendingDepKeySets.clear();
    pendingTraces.clear();
    currentDepHashes.clear();

    // Repopulate in-memory maps from DB so the store remains usable
    bulkLoadAll();
}

// ── Bulk load / flush ────────────────────────────────────────────────

void TraceStore::bulkLoadAllLocked(State & st)
{
    {
        auto use(st.getAllStrings.use());
        while (use.next()) {
            auto id = static_cast<uint32_t>(use.getInt(0));
            auto value = use.getStr(1);
            pools.strings.bulkLoad(id, value);
            if (id > flushedStringHighWaterMark)
                flushedStringHighWaterMark = id;
        }
    }
    {
        auto use(st.getAllResults.use());
        while (use.next()) {
            auto id = ResultId(static_cast<uint32_t>(use.getInt(0)));
            auto [hashBlob, hashSize] = use.getBlob(4);
            if (hashBlob && hashSize == 32) {
                resultByHash[ResultHash::fromBlob(hashBlob, hashSize)] = id;
            }
            if (id > nextResultId) nextResultId = id;
        }
    }
    {
        auto use(st.getAllDepKeySets.use());
        while (use.next()) {
            auto id = DepKeySetId(static_cast<uint32_t>(use.getInt(0)));
            auto [hashBlob, hashSize] = use.getBlob(1);
            if (hashBlob && hashSize == 32) {
                depKeySetByStructHash[StructHash::fromBlob(hashBlob, hashSize)] = id;
            }
            if (id > nextDepKeySetId) nextDepKeySetId = id;
        }
    }
    {
        auto use(st.getAllTraces.use());
        while (use.next()) {
            auto id = TraceId(static_cast<uint32_t>(use.getInt(0)));
            auto [hashBlob, hashSize] = use.getBlob(1);
            if (hashBlob && hashSize == 32) {
                traceByTraceHash[TraceHash::fromBlob(hashBlob, hashSize)] = id;
            }
            if (id > nextTraceId) nextTraceId = id;
        }
    }
    {
        auto use(st.getAllDirSets.use());
        while (use.next())
            pools.dirSets.emplace(std::string(use.getStr(0)), std::string(use.getStr(1)));
    }
}

void TraceStore::bulkLoadAll()
{
    auto st(_state->lock());
    bulkLoadAllLocked(*st);
}

void TraceStore::flush()
{
    auto st(_state->lock());

    // Note: vocab entries are NOT flushed here. They're flushed in the
    // destructor just before commit, ensuring atomicity (vocab + trace
    // committed together) without holding a RESERVED lock on vocab.sqlite
    // across the session. This prevents SQLITE_BUSY when multiple TraceStores
    // (one per context hash) ATTACH the same vocab DB.

    // Flush in dependency order: Strings → DirSets → Results → DepKeySets → Traces

    // Flush new strings: those with ID > flushedStringHighWaterMark.
    for (uint32_t i = flushedStringHighWaterMark + 1; i < pools.strings.nextId(); i++) {
        auto sv = pools.strings.resolveRaw(i);
        st->insertStringWithId.use()(static_cast<int64_t>(i))(sv).exec();
    }
    flushedStringHighWaterMark = pools.strings.nextId() > 0
        ? pools.strings.nextId() - 1
        : 0;

    // Flush DirSet definitions (INSERT OR IGNORE deduplicates; ~2 entries typical)
    for (auto & [dsHash, dirsJson] : pools.dirSets)
        st->insertDirSet.use()(dsHash)(dirsJson).exec();

    for (auto & r : pendingResults) {
        auto use(st->insertResultWithId.use());
        use(static_cast<int64_t>(r.id.value));
        use(static_cast<int64_t>(r.type));
        use(r.value);
        use(r.context);
        bindTypedHash(use, r.hash);
        use.exec();
    }
    pendingResults.clear();

    for (auto & dks : pendingDepKeySets) {
        auto use(st->insertDepKeySetWithId.use());
        use(static_cast<int64_t>(dks.id.value));
        bindTypedHash(use, dks.structHash);
        bindBlobVec(use, dks.keysBlob);
        use.exec();
    }
    pendingDepKeySets.clear();

    for (auto & t : pendingTraces) {
        auto use(st->insertTraceWithId.use());
        use(static_cast<int64_t>(t.id.value));
        bindTypedHash(use, t.traceHash);
        use(static_cast<int64_t>(t.depKeySetId.value));
        bindBlobVec(use, t.valuesBlob);
        use.exec();
    }
    pendingTraces.clear();
}

} // namespace nix::eval_trace
