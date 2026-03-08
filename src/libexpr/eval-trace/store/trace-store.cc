#include "nix/expr/eval-trace/store/trace-store.hh"
#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/eval-trace/store/stat-hash-store.hh"
#include "nix/expr/eval-trace/counters.hh"
#include "nix/expr/eval-trace/deps/hash.hh"
#include "nix/expr/eval.hh"
#include "nix/store/globals.hh"

#include "nix/util/users.hh"
#include "nix/util/util.hh"
#include "nix/util/hash.hh"

#include "trace-serialize.hh"
#include "trace-verify-deps.hh"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <tuple>
#include <unordered_set>

namespace nix::eval_trace {

// ── Timing helpers (no-op when NIX_SHOW_STATS is unset) ──────────────

static auto timerStart()
{
    return Counter::enabled ? std::chrono::steady_clock::now()
                            : std::chrono::steady_clock::time_point{};
}

static uint64_t elapsedUs(std::chrono::steady_clock::time_point start)
{
    if (!Counter::enabled) return 0;
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
}

// ── Dep key/value resolution ─────────────────────────────────────────

TraceStore::ResolvedDep TraceStore::resolveDep(const Dep & dep)
{
    if (dep.key.type == DepType::ParentContext) {
        auto pathId = AttrPathId(dep.key.keyId.value);
        return ResolvedDep{"", vocab.displayPath(pathId), dep.hash, DepType::ParentContext};
    }
    return ResolvedDep{
        std::string(pools.resolve(dep.key.sourceId)),
        std::string(pools.resolve(dep.key.keyId)),
        dep.hash,
        dep.key.type};
}

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
                auto h = readRawHash(hashBlob, hashSize);
                resultByHash[h] = id;
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
                auto h = readRawHash(hashBlob, hashSize);
                depKeySetByStructHash[h] = id;
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
                auto h = readRawHash(hashBlob, hashSize);
                traceByTraceHash[h] = id;
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
        bindRawHash(use, r.hash);
        use.exec();
    }
    pendingResults.clear();

    for (auto & dks : pendingDepKeySets) {
        auto use(st->insertDepKeySetWithId.use());
        use(static_cast<int64_t>(dks.id.value));
        bindRawHash(use, dks.structHash);
        bindBlobVec(use, dks.keysBlob);
        use.exec();
    }
    pendingDepKeySets.clear();

    for (auto & t : pendingTraces) {
        auto use(st->insertTraceWithId.use());
        use(static_cast<int64_t>(t.id.value));
        bindRawHash(use, t.traceHash);
        use(static_cast<int64_t>(t.depKeySetId.value));
        bindBlobVec(use, t.valuesBlob);
        use.exec();
    }
    pendingTraces.clear();
}

// ── Intern methods ───────────────────────────────────────────────────

ResultId TraceStore::doInternResult(ResultKind type, const std::string & value,
                                     const std::string & context, const Hash & resultHash)
{
    auto it = resultByHash.find(resultHash);
    if (it != resultByHash.end())
        return it->second;

    auto id = ResultId(++nextResultId.value);
    resultByHash[resultHash] = id;
    pendingResults.push_back({id, type, value, context, resultHash});
    return id;
}

// ── Trace storage (BSàlC trace store) ───────────────────────────────


TraceStore::CachedTraceData * TraceStore::ensureTraceHashes(TraceId traceId)
{
    auto it = traceDataCache.find(traceId);
    if (it != traceDataCache.end())
        return &it->second;

    auto st(_state->lock());
    // getTraceInfo columns: 0=trace_hash, 1=struct_hash, 2=dep_key_set_id, 3=keys_blob, 4=values_blob
    auto use(st->getTraceInfo.use()(static_cast<int64_t>(traceId.value)));
    if (!use.next())
        return nullptr;

    CachedTraceData data;
    auto [thData, thSize] = use.getBlob(0);
    data.traceHash = readRawHash(thData, thSize);
    auto [shData, shSize] = use.getBlob(1);
    data.structHash = readRawHash(shData, shSize);
    // deps left as nullopt (populated lazily by loadFullTrace)

    // Sanity: DB should never contain all-zero hashes (placeholder sentinel).
    assert(data.hashesPopulated() && "deserialized trace has placeholder (all-zero) hashes");

    auto [insertIt, _] = traceDataCache.emplace(traceId, std::move(data));
    return &insertIt->second;
}

Hash TraceStore::getTraceStructHash(TraceId traceId)
{
    auto * data = ensureTraceHashes(traceId);
    if (!data)
        throw Error("trace %d not found", traceId.value);
    return data->structHash;
}

std::vector<Dep> TraceStore::loadFullTrace(TraceId traceId)
{
    // Check if deps already cached in unified traceDataCache
    auto it = traceDataCache.find(traceId);
    if (it != traceDataCache.end() && it->second.deps)
        return *it->second.deps;

    auto loadStart = timerStart();
    nrLoadTraces++;

    // Single DB read via JOIN — keys from DepKeySets, values from Traces
    // getTraceInfo columns: 0=trace_hash, 1=struct_hash, 2=dep_key_set_id, 3=keys_blob, 4=values_blob
    std::vector<uint8_t> keysBlobCopy, valuesBlobCopy;
    DepKeySetId depKeySetId;
    Hash traceHash(HashAlgorithm::BLAKE3), structHash(HashAlgorithm::BLAKE3);
    {
        auto st(_state->lock());
        auto use(st->getTraceInfo.use()(static_cast<int64_t>(traceId.value)));
        if (!use.next()) {
            // Trace not found in DB — don't create a cache entry with
            // placeholder hashes, as ensureTraceHashes would incorrectly
            // return them as valid.
            nrLoadTraceTimeUs += elapsedUs(loadStart);
            return {};
        }

        // Opportunistically populate hash fields from the same query
        auto [thData, thSize] = use.getBlob(0);
        traceHash = readRawHash(thData, thSize);
        auto [shData, shSize] = use.getBlob(1);
        structHash = readRawHash(shData, shSize);
        depKeySetId = DepKeySetId(static_cast<uint32_t>(use.getInt(2)));

        auto [keysData, keysSize] = use.getBlob(3);
        if (keysData && keysSize > 0) {
            auto * p = static_cast<const uint8_t *>(keysData);
            keysBlobCopy.assign(p, p + keysSize);
        }

        auto [valsData, valsSize] = use.getBlob(4);
        if (valsData && valsSize > 0) {
            auto * p = static_cast<const uint8_t *>(valsData);
            valuesBlobCopy.assign(p, p + valsSize);
        }
    }

    // Update hash fields in cache (may already exist from ensureTraceHashes)
    auto & data = traceDataCache[traceId];
    data.traceHash = traceHash;
    data.structHash = structHash;
    assert(data.hashesPopulated() && "DB returned placeholder (all-zero) hashes in loadFullTrace");

    if (keysBlobCopy.empty()) {
        data.deps = {};
        nrLoadTraceTimeUs += elapsedUs(loadStart);
        return {};
    }

    // Deserialize keys first (needed to type-dispatch values), then values
    auto keys = deserializeKeys(keysBlobCopy.data(), keysBlobCopy.size());
    auto values = deserializeValues(valuesBlobCopy.data(), valuesBlobCopy.size(), keys);

    // Zip keys + values into Dep vector (no string resolution)
    std::vector<Dep> result;
    result.reserve(keys.size());
    for (size_t i = 0; i < std::min(keys.size(), values.size()); ++i)
        result.push_back({keys[i], std::move(values[i])});

    // Also populate the dep key set cache for potential recovery use
    depKeySetCache.insert_or_assign(depKeySetId, keys);

    data.deps = result;
    nrLoadTraceTimeUs += elapsedUs(loadStart);
    return result;
}

std::vector<Dep::Key> TraceStore::loadKeySet(DepKeySetId depKeySetId)
{
    // Session cache: avoid re-decompressing keys_blob for shared key sets
    auto cacheIt = depKeySetCache.find(depKeySetId);
    if (cacheIt != depKeySetCache.end())
        return cacheIt->second;

    auto st(_state->lock());
    auto use(st->getDepKeySet.use()(static_cast<int64_t>(depKeySetId.value)));
    if (!use.next())
        return {};

    // Column 0 = struct_hash (not needed here), column 1 = keys_blob
    auto [keysData, keysSize] = use.getBlob(1);
    if (!keysData || keysSize == 0)
        return {};

    std::vector<uint8_t> blobCopy(
        static_cast<const uint8_t *>(keysData),
        static_cast<const uint8_t *>(keysData) + keysSize);

    auto keys = deserializeKeys(blobCopy.data(), blobCopy.size());
    depKeySetCache.insert_or_assign(depKeySetId, keys);
    return keys;
}

void TraceStore::feedKey(HashSink & s, DepType type, uint32_t idValue) const
{
    if (depKind(type) == DepKind::ParentContext)
        vocab.hashPath(s, AttrPathId(idValue));
    else
        s(pools.resolve(DepKeyId(idValue)));
}

// ── Dedup helpers (A1–A4) ────────────────────────────────────────────

Hash TraceStore::computeSortedTraceHash(std::vector<Dep> & deps) const
{
    std::sort(deps.begin(), deps.end());
    deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
    auto feeder = [this](HashSink & s, DepType type, uint32_t idValue) {
        feedKey(s, type, idValue);
    };
    return computeTraceHashFromSorted(deps, feeder);
}

std::optional<DepHashValue> TraceStore::resolveCurrentDepHash(
    const Dep & dep,
    const boost::unordered_flat_map<std::string, SourcePath> & inputAccessors,
    EvalState & state, VerificationScope & scope)
{
    auto cacheIt = currentDepHashes.find(dep.key);
    if (cacheIt != currentDepHashes.end())
        return cacheIt->second;
    auto resolved = resolveDep(dep);
    auto current = computeCurrentHash(state, resolved, inputAccessors, scope, pools.dirSets);
    currentDepHashes[dep.key] = current;
    return current;
}

std::optional<Blake3Hash> TraceStore::resolveParentContextHash(
    const Dep::Key & key,
    const boost::unordered_flat_map<std::string, SourcePath> & inputAccessors,
    EvalState & state,
    VerificationScope & scope)
{
    auto parentPathId = AttrPathId(key.keyId.value);
    auto parentRow = lookupTraceRow(parentPathId);
    if (!parentRow || !verifyTrace(parentRow->traceId, inputAccessors, state, scope))
        return std::nullopt;
    auto parentTraceHash = getCurrentTraceHash(parentPathId);
    if (!parentTraceHash) return std::nullopt;
    Blake3Hash b3;
    std::memcpy(b3.bytes.data(), parentTraceHash->hash, 32);
    return b3;
}

DepKeySetId TraceStore::getOrCreateDepKeySet(
    const Hash & structHash,
    const std::vector<uint8_t> & keysBlob)
{
    auto it = depKeySetByStructHash.find(structHash);
    if (it != depKeySetByStructHash.end())
        return it->second;

    auto id = DepKeySetId(++nextDepKeySetId.value);
    depKeySetByStructHash[structHash] = id;
    pendingDepKeySets.push_back({id, structHash, keysBlob});
    return id;
}

TraceId TraceStore::getOrCreateTrace(
    const Hash & traceHash,
    DepKeySetId depKeySetId,
    const std::vector<uint8_t> & valuesBlob)
{
    auto it = traceByTraceHash.find(traceHash);
    if (it != traceByTraceHash.end())
        return it->second;

    auto id = TraceId(++nextTraceId.value);
    traceByTraceHash[traceHash] = id;
    pendingTraces.push_back({id, traceHash, depKeySetId, valuesBlob});
    return id;
}

// ── Trace verification (BSàlC VT check) ─────────────────────────────

/**
 * File identity for coverage set lookups: (source, filePath).
 * Content/Directory deps use dep.key directly as the file path.
 * StructuredContent/ImplicitShape deps extract "f" from their JSON key.
 */
struct FileIdentity {
    std::string source;
    std::string filePath;

    bool operator==(const FileIdentity &) const = default;

    struct Hash {
        size_t operator()(const FileIdentity & fi) const noexcept {
            return hashValues(fi.source, fi.filePath);
        }
    };
};

static FileIdentity scFileIdentity(const TraceStore::ResolvedDep & dep) {
    auto j = nlohmann::json::parse(dep.key);
    if (j.contains("ds"))
        return {dep.source, "ds:" + j["ds"].get<std::string>()};
    return {dep.source, j["f"].get<std::string>()};
}

static FileIdentity contentFileIdentity(const TraceStore::ResolvedDep & dep) {
    return {dep.source, dep.key};
}

/**
 * Classification of trace verification outcome. Replaces the ad-hoc boolean
 * combination (allValid, hasContentFailure, hasImplicitShapeOnlyOverride).
 * -Wswitch ensures every consumer handles all cases.
 */
enum class VerifyOutcome {
    /** All deps match current state. No hash recomputation needed. */
    Valid,
    /** Content dep(s) failed but StructuredContent deps cover all failures.
     *  Value-aware: accessed scalars verified. No hash recomputation needed. */
    ValidViaStructuralOverride,
    /** Content dep(s) failed, covered by ImplicitShape-only (no SC coverage).
     *  Value-blind: key set unchanged but values may differ. Requires
     *  trace_hash recomputation so ParentContext deps detect potential change. */
    ValidViaImplicitShapeOverride,
    /** Unrecoverable verification failure. */
    Invalid,
};

bool TraceStore::verifyTrace(
    TraceId traceId,
    const boost::unordered_flat_map<std::string, SourcePath> & inputAccessors,
    EvalState & state)
{
    auto scopeOwner = createVerificationScope();
    return verifyTrace(traceId, inputAccessors, state, *scopeOwner);
}

bool TraceStore::verifyTrace(
    TraceId traceId,
    const boost::unordered_flat_map<std::string, SourcePath> & inputAccessors,
    EvalState & state,
    VerificationScope & scope)
{
    if (verifiedTraceIds.count(traceId))
        return true;

    auto vtStart = timerStart();

    // Load the full trace (single DB read via JOIN) — vector<Dep>
    auto fullDeps = loadFullTrace(traceId);


    bool hasNonContentFailure = false;
    bool hasContentFailure = false;
    bool hasStructuralDeps = false;
    bool hasImplicitShapeDeps = false;
    bool gitIdentityMatched = false;

    std::unordered_set<FileIdentity, FileIdentity::Hash> failedContentFiles;
    // Deferred structural/implicit deps stored as indices into fullDeps
    std::vector<size_t> structuralDepIndices;
    std::vector<size_t> implicitShapeDepIndices;

    for (size_t i = 0; i < fullDeps.size(); ++i) {
        auto & idep = fullDeps[i];
        nrDepsChecked++;

        if (isVolatile(idep.key.type)) {
            hasNonContentFailure = true;
            continue;
        }

        if (depKind(idep.key.type) == DepKind::ImplicitStructural) {
            if (idep.key.type == DepType::GitIdentity) {
                // Eagerly check GitIdentity for fast-path (don't defer).
                // Does NOT set hasImplicitShapeDeps — GitIdentity is not a
                // shape dep and should not trigger Pass 2 shape logic.
                auto current = resolveCurrentDepHash(idep, inputAccessors, state, scope);
                if (current && *current == idep.hash)
                    gitIdentityMatched = true;
            } else {
                hasImplicitShapeDeps = true;
                implicitShapeDepIndices.push_back(i);
            }
            continue;
        }

        if (depKind(idep.key.type) == DepKind::Structural) {
            hasStructuralDeps = true;
            structuralDepIndices.push_back(i);
            continue;
        }

        if (depKind(idep.key.type) == DepKind::ParentContext) {
            auto parentHash = resolveParentContextHash(idep.key, inputAccessors, state, scope);
            if (parentHash) {
                auto * expected = std::get_if<Blake3Hash>(&idep.hash);
                if (expected && std::memcmp(expected->bytes.data(), parentHash->bytes.data(), 32) == 0)
                    continue;
            }
            nrVerificationsFailed++;
            hasNonContentFailure = true;
            continue;
        }

        auto current = resolveCurrentDepHash(idep, inputAccessors, state, scope);

        if (!current || *current != idep.hash) {
            nrVerificationsFailed++;
            if (isContentOverrideable(idep.key.type)) {
                hasContentFailure = true;
                auto dep = resolveDep(idep);
                failedContentFiles.insert(contentFileIdentity(dep));
            } else {
                hasNonContentFailure = true;
            }
        }
    }

    // ── GitIdentity fast-path: skip Pass 2 if git identity matches,
    //    all other deps passed, and no structural/implicit deps need checking.
    if (gitIdentityMatched && !hasNonContentFailure && !hasContentFailure
        && structuralDepIndices.empty() && implicitShapeDepIndices.empty()) {
        verifiedTraceIds.insert(traceId);
        nrVerifyTraceTimeUs += elapsedUs(vtStart);
        return true;
    }

    // ── Pass 2: Resolve overrides and determine outcome ─────────────

    // Helper: verify a set of structural/implicit deps (by index) against current hashes.
    auto verifyDeps = [&](const std::vector<size_t> & indices,
                          const std::unordered_set<FileIdentity, FileIdentity::Hash> * skipFiles = nullptr,
                          const std::unordered_set<FileIdentity, FileIdentity::Hash> * onlyFiles = nullptr) -> bool {
        for (auto idx : indices) {
            auto & idep = fullDeps[idx];
            if (skipFiles || onlyFiles) {
                auto dep = resolveDep(idep);
                auto fileKey = scFileIdentity(dep);
                if (skipFiles && skipFiles->count(fileKey)) continue;
                if (onlyFiles && !onlyFiles->count(fileKey)) continue;
            }
            nrDepsChecked++;
            auto current = resolveCurrentDepHash(idep, inputAccessors, state, scope);
            if (!current || *current != idep.hash) {
                nrVerificationsFailed++;
                return false;
            }
        }
        return true;
    };

    VerifyOutcome outcome;

    if (hasNonContentFailure) {
        outcome = VerifyOutcome::Invalid;
    } else if (!hasContentFailure) {
        bool standalonePassed = true;
        if (hasStructuralDeps || hasImplicitShapeDeps) {
            std::unordered_set<FileIdentity, FileIdentity::Hash> coveredFiles;
            for (auto & idep : fullDeps) {
                if (isContentOverrideable(idep.key.type)) {
                    auto dep = resolveDep(idep);
                    coveredFiles.insert(contentFileIdentity(dep));
                }
            }
            standalonePassed = verifyDeps(structuralDepIndices, &coveredFiles)
                            && verifyDeps(implicitShapeDepIndices, &coveredFiles);
        }
        outcome = standalonePassed ? VerifyOutcome::Valid : VerifyOutcome::Invalid;
    } else if (hasContentFailure && (hasStructuralDeps || hasImplicitShapeDeps)) {
        std::unordered_set<FileIdentity, FileIdentity::Hash> structuralCoveredFiles;
        for (auto idx : structuralDepIndices) {
            auto dep = resolveDep(fullDeps[idx]);
            auto fi = scFileIdentity(dep);
            structuralCoveredFiles.insert(fi);
            if (fi.filePath.starts_with("ds:")) {
                try {
                    auto j = nlohmann::json::parse(dep.key);
                    auto dsHash = j.value("ds", "");
                    auto it = pools.dirSets.find(dsHash);
                    if (it == pools.dirSets.end()) continue;
                    auto dirs = nlohmann::json::parse(it->second);
                    for (auto & dir : dirs) {
                        if (dir.is_array() && dir.size() == 2)
                            structuralCoveredFiles.insert({dir[0].get<std::string>(), dir[1].get<std::string>()});
                    }
                } catch (...) {}
            }
        }

        std::unordered_set<FileIdentity, FileIdentity::Hash> implicitCoveredFiles;
        for (auto idx : implicitShapeDepIndices) {
            auto dep = resolveDep(fullDeps[idx]);
            implicitCoveredFiles.insert(scFileIdentity(dep));
        }

        bool allCovered = true;
        bool hasImplicitOnly = false;
        for (auto & failedFile : failedContentFiles) {
            if (!structuralCoveredFiles.count(failedFile)
                && !implicitCoveredFiles.count(failedFile)) {
                allCovered = false;
                break;
            }
            if (!structuralCoveredFiles.count(failedFile)
                && implicitCoveredFiles.count(failedFile)) {
                hasImplicitOnly = true;
            }
        }

        if (!allCovered) {
            outcome = VerifyOutcome::Invalid;
        } else if (!verifyDeps(structuralDepIndices)) {
            outcome = VerifyOutcome::Invalid;
        } else if (!verifyDeps(implicitShapeDepIndices, &structuralCoveredFiles, &failedContentFiles)) {
            outcome = VerifyOutcome::Invalid;
        } else if (hasImplicitOnly) {
            outcome = VerifyOutcome::ValidViaImplicitShapeOverride;
        } else {
            outcome = VerifyOutcome::ValidViaStructuralOverride;
        }
    } else {
        outcome = VerifyOutcome::Invalid;
    }

    // ── Apply outcome ────────────────────────────────────────────────

    switch (outcome) {
    case VerifyOutcome::Valid:
    case VerifyOutcome::ValidViaStructuralOverride:
        verifiedTraceIds.insert(traceId);
        break;

    case VerifyOutcome::ValidViaImplicitShapeOverride: {
        // Build Dep directly — no round-trip needed
        std::vector<Dep> currentDeps;
        currentDeps.reserve(fullDeps.size());
        for (auto & idep : fullDeps) {
            if (depKind(idep.key.type) == DepKind::ParentContext) {
                auto b3 = resolveParentContextHash(idep.key, inputAccessors, state, scope);
                currentDeps.push_back(b3 ? Dep{idep.key, DepHashValue(*b3)} : idep);
            } else {
                auto cacheIt = currentDepHashes.find(idep.key);
                currentDeps.push_back(cacheIt != currentDepHashes.end() && cacheIt->second
                    ? Dep{idep.key, *cacheIt->second} : idep);
            }
        }
        auto newTraceHash = computeSortedTraceHash(currentDeps);
        auto * data = ensureTraceHashes(traceId);
        if (data) {
            data->traceHash = newTraceHash;
        }
        verifiedTraceIds.insert(traceId);
        break;
    }

    case VerifyOutcome::Invalid:
        break;
    }

    nrVerifyTraceTimeUs += elapsedUs(vtStart);
    return outcome != VerifyOutcome::Invalid;
}

// ── DB lookups ───────────────────────────────────────────────────────

std::optional<TraceStore::TraceRow> TraceStore::lookupTraceRow(AttrPathId pathId)
{
    // Check traceRowCache first (populated on previous lookups, invalidated on
    // CurrentTraces changes in acceptRecoveredTrace and record).
    auto rowCacheIt = traceRowCache.find(pathId);
    if (rowCacheIt != traceRowCache.end())
        return rowCacheIt->second;

    auto st(_state->lock());

    auto use(st->lookupAttr.use()(contextHash)(static_cast<int64_t>(pathId.value)));
    if (!use.next())
        return std::nullopt;

    TraceRow row;
    row.traceId = TraceId(static_cast<uint32_t>(use.getInt(0)));
    row.resultId = ResultId(static_cast<uint32_t>(use.getInt(1)));
    row.type = static_cast<ResultKind>(use.getInt(2));
    row.value = use.isNull(3) ? "" : use.getStr(3);
    row.context = use.isNull(4) ? "" : use.getStr(4);

    traceRowCache[pathId] = row;
    return row;
}

bool TraceStore::attrExists(AttrPathId pathId)
{
    return lookupTraceRow(pathId).has_value();
}

std::optional<Hash> TraceStore::getCurrentTraceHash(AttrPathId pathId)
{
    auto row = lookupTraceRow(pathId);  // hits traceRowCache after first call
    if (!row) return std::nullopt;

    // Return trace_hash (captures dep structure + hashes), not result hash.
    // Result hash for attrsets only captures attribute names, not values —
    // it wouldn't detect changes to attribute values within an attrset.
    auto * data = ensureTraceHashes(row->traceId);  // hits traceDataCache after first call
    if (!data) return std::nullopt;
    return data->traceHash;
}

// ── Record path (BSàlC constructive trace recording) ─────────────────

TraceStore::RecordResult TraceStore::record(
    AttrPathId pathId,
    const CachedResult & value,
    const std::vector<Dep> & allDeps)
{
    auto recordStart = timerStart();
    nrRecords++;

    // 1. Sort+dedup deps by key (type, sourceId, keyId)
    auto sorted = sortAndDedupDeps(allDeps);

    // 2. Compute trace_hash and struct_hash
    auto feedKeyFn = [this](HashSink & s, DepType type, uint32_t idValue) {
        feedKey(s, type, idValue);
    };
    auto traceHash = computeTraceHashFromSorted(sorted, feedKeyFn);
    auto structHash = computeTraceStructHashFromSorted(sorted, feedKeyFn);

    // 5. Split into keys + values
    std::vector<Dep::Key> keys;
    keys.reserve(sorted.size());
    for (auto & d : sorted)
        keys.push_back(d.key);

    auto keysBlob = serializeKeys(keys);
    auto valuesBlob = serializeValues(sorted);

    // 6. Get or create dep key set (content-addressed by struct_hash)
    auto depKeySetId = getOrCreateDepKeySet(structHash, keysBlob);

    // 7. Encode CachedResult and intern result
    auto [type, val, ctx] = encodeCachedResult(value);
    auto resultHash = computeResultHash(type, val, ctx);
    ResultId resultId = doInternResult(type, val, ctx, resultHash);

    // 8. Get or create trace (keyed by trace_hash, stores dep_key_set_id + values_blob)
    TraceId traceId = getOrCreateTrace(traceHash, depKeySetId, valuesBlob);

    // 9. Flush pending entities to DB (IDs must exist before FK references).
    // flush() also flushes vocab entries via the ATTACH'd connection.
    flush();

    // 10. Upsert Attrs + insert History
    {
        auto st(_state->lock());
        st->upsertAttr.use()
            (contextHash)(static_cast<int64_t>(pathId.value))(static_cast<int64_t>(traceId.value))(static_cast<int64_t>(resultId.value)).exec();
        st->insertHistory.use()
            (contextHash)(static_cast<int64_t>(pathId.value))(static_cast<int64_t>(traceId.value))(static_cast<int64_t>(resultId.value)).exec();
    }

    // 11. Session caches
    bool hasVolatile = std::any_of(allDeps.begin(), allDeps.end(),
        [](auto & d) { return isVolatile(d.key.type); });
    if (!hasVolatile)
        verifiedTraceIds.insert(traceId);

    {
        auto & data = traceDataCache[traceId];
        data.traceHash = traceHash;
        data.structHash = structHash;
        data.deps = sorted;  // already vector<Dep> from sort/dedup
        assert(data.hashesPopulated() && "recording trace with placeholder (all-zero) hashes");
    }
    depKeySetCache.insert_or_assign(depKeySetId, keys);

    // Update traceRowCache so subsequent lookupTraceRow/getCurrentTraceHash
    // calls for this attr path don't go to DB.
    traceRowCache[pathId] = TraceRow{traceId, resultId, type, val, ctx};

    nrRecordTimeUs += elapsedUs(recordStart);
    return RecordResult{traceId};
}

// ── Verify path (BSàlC verifying trace) ──────────────────────────────

std::optional<TraceStore::VerifyResult> TraceStore::verify(
    AttrPathId pathId,
    const boost::unordered_flat_map<std::string, SourcePath> & inputAccessors,
    EvalState & state)
{
    auto verifyStart = timerStart();

    // 1. Lookup attribute
    auto row = lookupTraceRow(pathId);
    if (!row) {
        nrVerifyTimeUs += elapsedUs(verifyStart);
        return std::nullopt;
    }

    nrTraceVerifications++;

    // Shared scope: DOM caches (JSON, TOML, dir listing, Nix AST) persist
    // across verifyTrace → recovery, avoiding redundant file parsing.
    auto scopeOwner = createVerificationScope();
    auto & scope = *scopeOwner;

    // 2. Verify trace — compute ALL dep hashes upfront.
    //    With StatHashCache warm, computing all hashes is cheap (~2ms for ~4K deps:
    //    just lstat() + L1 cache lookup per file dep). Computing all hashes upfront
    //    ensures recovery can reuse them immediately via currentDepHashes.
    if (verifyTrace(row->traceId, inputAccessors, state, scope)) {
        nrVerificationsPassed++;
        nrVerifyTimeUs += elapsedUs(verifyStart);
        return VerifyResult{decodeCachedResult(*row), row->traceId};
    }

    // 3. Verification failed → constructive recovery.
    //    All dep hashes are pre-computed in currentDepHashes (from step 2).
    //    Direct hash recovery is O(1): sort+hash the pre-computed values, lookup.
    //    No additional hash computation needed unless structural variant recovery
    //    encounters a trace with different dep keys (rare).
    debug("verify: trace validation failed for '%s', attempting constructive recovery", vocab.displayPath(pathId));
    auto result = recovery(row->traceId, pathId, inputAccessors, state, scope);
    nrVerifyTimeUs += elapsedUs(verifyStart);
    return result;
}

// ── Recovery (BSàlC constructive trace recovery) ─────────────────────
//    Two-phase: direct hash recovery + structural variant recovery

std::optional<TraceStore::VerifyResult> TraceStore::recovery(
    TraceId oldTraceId,
    AttrPathId pathId,
    const boost::unordered_flat_map<std::string, SourcePath> & inputAccessors,
    EvalState & state)
{
    auto scopeOwner = createVerificationScope();
    return recovery(oldTraceId, pathId, inputAccessors, state, *scopeOwner);
}

std::optional<TraceStore::VerifyResult> TraceStore::recovery(
    TraceId oldTraceId,
    AttrPathId pathId,
    const boost::unordered_flat_map<std::string, SourcePath> & inputAccessors,
    EvalState & state,
    VerificationScope & scope)
{
    auto recoveryStart = timerStart();
    nrRecoveryAttempts++;

    // Load old trace's full deps — now vector<Dep>
    auto oldDeps = loadFullTrace(oldTraceId);

    // Check for volatile deps → immediate abort
    for (auto & idep : oldDeps) {
        if (isVolatile(idep.key.type)) {
            debug("recovery: aborting for '%s' -- contains volatile dep", vocab.displayPath(pathId));
            nrRecoveryFailures++;
            nrRecoveryTimeUs += elapsedUs(recoveryStart);
            return std::nullopt;
        }
    }

    // Build Dep directly with current hash values
    std::vector<Dep> currentDeps;
    bool allComputable = true;
    for (auto & idep : oldDeps) {
        if (depKind(idep.key.type) == DepKind::ParentContext) {
            auto parentHash = resolveParentContextHash(idep.key, inputAccessors, state, scope);
            if (!parentHash) { allComputable = false; break; }
            currentDeps.push_back({idep.key, DepHashValue(*parentHash)});
            continue;
        }

        auto current = resolveCurrentDepHash(idep, inputAccessors, state, scope);

        if (!current) {
            allComputable = false;
            break;
        }
        currentDeps.push_back({idep.key, *current});
    }

    debug("recovery: recomputed %d/%d dep hashes for '%s'",
          currentDeps.size(), oldDeps.size(), vocab.displayPath(pathId));

    boost::unordered_flat_set<TraceId, TraceId::Hash> triedTraceIds;

    // === Pre-load: scan history with widened query ===
    struct HistoryEntry {
        DepKeySetId depKeySetId;
        Hash structHash;
        TraceId traceId;
        ResultId resultId;
        Hash traceHash;
        ResultKind type;
        std::string value;
        std::string context;

        HistoryEntry(DepKeySetId dksId, Hash sh, TraceId tid, ResultId rid,
                     Hash th, ResultKind t, std::string v, std::string c)
            : depKeySetId(dksId), structHash(std::move(sh)), traceId(tid), resultId(rid),
              traceHash(std::move(th)), type(t), value(std::move(v)), context(std::move(c)) {}
    };
    std::vector<HistoryEntry> historyEntries;
    {
        auto st(_state->lock());
        auto use(st->scanHistoryForAttr.use()(contextHash)(static_cast<int64_t>(pathId.value)));
        while (use.next()) {
            auto [shData, shSize] = use.getBlob(1);
            auto [thData, thSize] = use.getBlob(4);
            historyEntries.emplace_back(
                DepKeySetId(static_cast<uint32_t>(use.getInt(0))),
                readRawHash(shData, shSize),
                TraceId(static_cast<uint32_t>(use.getInt(2))),
                ResultId(static_cast<uint32_t>(use.getInt(3))),
                readRawHash(thData, thSize),
                static_cast<ResultKind>(use.getInt(5)),
                use.isNull(6) ? "" : use.getStr(6),
                use.isNull(7) ? "" : use.getStr(7)
            );
        }
    }

    // Build in-memory trace_hash → entry index lookup.
    // Uses Hash key directly (no string construction needed).
    boost::unordered_flat_map<Hash, size_t, HashKeyHash> traceHashToEntry;
    for (size_t i = 0; i < historyEntries.size(); i++)
        traceHashToEntry.emplace(historyEntries[i].traceHash, i);

    auto lookupCandidate = [&](const Hash & candidateHash) -> const HistoryEntry * {
        auto it = traceHashToEntry.find(candidateHash);
        if (it == traceHashToEntry.end()) return nullptr;
        return &historyEntries[it->second];
    };

    auto acceptRecoveredTrace = [&](const HistoryEntry & entry) -> std::optional<VerifyResult> {
        if (triedTraceIds.count(entry.traceId))
            return std::nullopt;
        triedTraceIds.insert(entry.traceId);

        {
            auto st(_state->lock());
            st->upsertAttr.use()
                (contextHash)(static_cast<int64_t>(pathId.value))(static_cast<int64_t>(entry.traceId.value))(static_cast<int64_t>(entry.resultId.value)).exec();
        }

        TraceRow newRow{entry.traceId, entry.resultId, entry.type, entry.value, entry.context};
        traceRowCache[pathId] = newRow;

        verifiedTraceIds.insert(entry.traceId);
        return VerifyResult{decodeCachedResult(newRow), entry.traceId};
    };

    // === Direct hash recovery (BSàlC CT) ===
    if (allComputable) {
        auto directHashStart = timerStart();
        auto newFullHash = computeSortedTraceHash(currentDeps);

        if (auto * entry = lookupCandidate(newFullHash)) {
            if (auto r = acceptRecoveredTrace(*entry)) {
                debug("recovery: direct hash recovery succeeded for '%s'", vocab.displayPath(pathId));
                nrRecoveryDirectHashHits++;
                nrRecoveryDirectHashTimeUs += elapsedUs(directHashStart);
                nrRecoveryTimeUs += elapsedUs(recoveryStart);
                return r;
            }
        }
        nrRecoveryDirectHashTimeUs += elapsedUs(directHashStart);
    }

    std::optional<Hash> directHashStructHash;
    if (allComputable) {
        directHashStructHash = getTraceStructHash(oldTraceId);
    }

    // === Structural variant recovery ===
    auto structVariantStart = timerStart();

    debug("recovery: structural variant scan for '%s' -- scanning %d history entries",
          vocab.displayPath(pathId), historyEntries.size());

    boost::unordered_flat_map<DepKeySetId, TraceId, DepKeySetId::Hash> structGroups;
    boost::unordered_flat_map<DepKeySetId, Hash, DepKeySetId::Hash> groupStructHashes;
    for (auto & e : historyEntries) {
        if (triedTraceIds.count(e.traceId))
            continue;
        structGroups.emplace(e.depKeySetId, e.traceId);
        groupStructHashes.emplace(e.depKeySetId, e.structHash);
    }

    for (auto & [depKeySetId, repTraceId] : structGroups) {
        if (triedTraceIds.count(repTraceId))
            continue;
        auto structHashIt = groupStructHashes.find(depKeySetId);
        if (directHashStructHash && structHashIt != groupStructHashes.end()
            && structHashIt->second == *directHashStructHash)
            continue;

        auto repKeys = loadKeySet(depKeySetId);

        // Build Dep directly using Dep::Key
        std::vector<Dep> repDeps;
        bool repComputable = true;
        for (auto & key : repKeys) {
            if (isVolatile(key.type)) {
                repComputable = false;
                break;
            }

            if (depKind(key.type) == DepKind::ParentContext) {
                auto parentHash = resolveParentContextHash(key, inputAccessors, state, scope);
                if (!parentHash) { repComputable = false; break; }
                repDeps.push_back({key, DepHashValue(*parentHash)});
                continue;
            }

            auto current = resolveCurrentDepHash(
                Dep{key, DepHashValue{Blake3Hash{}}}, inputAccessors, state, scope);

            if (!current) {
                repComputable = false;
                break;
            }
            repDeps.push_back({key, *current});
        }
        if (!repComputable)
            continue;

        auto candidateFullHash = computeSortedTraceHash(repDeps);

        if (auto * entry = lookupCandidate(candidateFullHash)) {
            if (auto r = acceptRecoveredTrace(*entry)) {
                debug("recovery: structural variant recovery succeeded for '%s'", vocab.displayPath(pathId));
                nrRecoveryStructVariantHits++;
                nrRecoveryStructVariantTimeUs += elapsedUs(structVariantStart);
                nrRecoveryTimeUs += elapsedUs(recoveryStart);
                return r;
            }
        }
    }
    nrRecoveryStructVariantTimeUs += elapsedUs(structVariantStart);

    debug("recovery: all strategies failed for '%s'", vocab.displayPath(pathId));
    nrRecoveryFailures++;
    nrRecoveryTimeUs += elapsedUs(recoveryStart);
    return std::nullopt;
}

} // namespace nix::eval_trace
