/// sqlite-trace-storage-lifecycle.cc — construction, destruction,
/// bulk load, flush for the SQLite-backed trace storage.
///
/// Split from sqlite-trace-storage.cc. Contains:
///   - SQL schema definition
///   - constructor (DB init, statement prep, bulk load)
///   - destructor (flush, vocab commit)
///   - bulkLoadAllLocked, bulkLoadAll, flush

#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/store/verification-session.hh"
#include "nix/expr/eval-trace/store/session-identity.hh"
#include "nix/expr/eval-trace/canonical-hash.hh"
#include "nix/expr/eval-trace/hash-spec.hh"
#include "../fiber/blocking-scope.hh"
#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/eval-trace/counters.hh"
#include "nix/store/globals.hh"

#include "nix/util/users.hh"
#include "nix/util/util.hh"

#include "trace-serialize.hh"

#include <boost/unordered/unordered_flat_map.hpp>

#include <cassert>
#include <cstring>
#include <filesystem>

namespace nix::eval_trace {

// ── `withExclusiveAccess` re-entrancy detection ──────────────────────
//
// Detects the "re-entered `withExclusiveAccess` on the same store from
// the same thread" class of bugs in debug builds. Release builds skip
// the detector entirely — the non-recursive `std::mutex` deadlocks on
// re-entry, which is what would happen anyway, so the thread_local map
// probe would just be wasted work on the hot path
// (~2-3 withExclusiveAccess calls per verify × 700 verifies per bench
// run = thousands of no-op map lookups). The detector exists to give a
// useful assertion message in debug builds BEFORE the deadlock fires.
//
// Implementation: `thread_local` map from SqliteTraceStorage * to a counter.
// Each Enter increments; Exit decrements. The invariant violation is
// that Enter sees a non-zero existing count — that means the current
// thread is already inside a `withExclusiveAccess` on this store.
// The counter (rather than a set) is necessary because Guard::~Guard
// always runs regardless of whether the Enter found a conflict, so
// nesting bugs would otherwise leave the set in a corrupt state.

// Re-entrancy detection defined on TraceStorage (base class) — keys on
// base-class pointer so all concrete backends share one detector and
// one thread_local map.
#ifndef NDEBUG
namespace {
/// Which storage (if any) this thread currently holds exclusive
/// access to.  Single slot — the one-storage-per-thread-under-
/// `withExclusiveAccess` invariant holds across all current
/// production code paths and tests (`TraceCacheFixture` drives a
/// single `TraceSession` per test; `TraceSessionActivationScope`
/// nests the *activation*, not the storage pointer).
///
/// Previously this was a `boost::unordered_flat_map<TraceStorage *,
/// uint32_t>` with ~thousand-per-bench-run lookups + increments.
/// The map supported "multiple storages held concurrently by one
/// thread", but no production or test site exercises that shape;
/// under the invariant above the map was perpetually size 0 or 1.
thread_local TraceStorageBase * heldStorage = nullptr;
} // namespace

void TraceStorageBase::reentrancyCheckEnter()
{
    if (heldStorage == this) {
        // Re-entering `withExclusiveAccess` on the same storage is a
        // call-graph bug: internal callers should reach the shared
        // code via the private helpers (no mutex re-acquisition).
        // The assert fires before the non-recursive mutex deadlocks.
        assert(!"re-entrant withExclusiveAccess on same TraceStorage");
    }
    if (heldStorage != nullptr) {
        // Two concurrent storages held by one thread is not a
        // shape production exercises.  If some future path needs
        // it, revert to the hash-map form above.
        assert(!"thread holds exclusive access to multiple TraceStorages");
    }
    heldStorage = this;
}

void TraceStorageBase::reentrancyCheckExit() noexcept
{
    if (heldStorage == this)
        heldStorage = nullptr;
}
#else
void TraceStorageBase::reentrancyCheckEnter() {}
void TraceStorageBase::reentrancyCheckExit() noexcept {}
#endif

// Epoch constants are defined in trace-store.hh (kSchemaEpoch, kProviderEpoch).

// ── Schema ───────────────────────────────────────────────────────────

namespace {

struct __attribute__((packed)) DirSetBlobEntry {
    uint32_t sourceId;
    uint32_t filePathId;
};
static_assert(sizeof(DirSetBlobEntry) == 8);

std::vector<uint8_t> serializeDirSet(const DirSetDefinition & dirs)
{
    std::vector<uint8_t> blob;
    blob.reserve(dirs.size() * sizeof(DirSetBlobEntry));
    for (auto & entry : dirs) {
        DirSetBlobEntry raw{
            .sourceId = entry.sourceId.value,
            .filePathId = entry.filePathId.value,
        };
        auto * bytes = reinterpret_cast<const uint8_t *>(&raw);
        blob.insert(blob.end(), bytes, bytes + sizeof(raw));
    }
    return blob;
}

DirSetDefinition deserializeDirSet(const void * blob, size_t size)
{
    DirSetDefinition dirs;
    auto * current = static_cast<const uint8_t *>(blob);
    auto * end = current + size;
    while (current + sizeof(DirSetBlobEntry) <= end) {
        DirSetBlobEntry raw{};
        std::memcpy(&raw, current, sizeof(raw));
        current += sizeof(raw);
        dirs.push_back(DirSetOrigin{
            .sourceId = DepSourceId(raw.sourceId),
            .filePathId = FilePathId(raw.filePathId),
        });
    }
    return dirs;
}

} // namespace

// SemanticSessionKey::fromSerialized, SessionConfig::buildSemanticSessionKey,
// and SessionConfig::forTest moved to store/session-identity.cc.

static const char * schema = R"sql(

    CREATE TABLE IF NOT EXISTS Strings (
        id    INTEGER PRIMARY KEY,
        value BLOB NOT NULL
    ) STRICT;
    -- Non-unique index: dedup is enforced by the in-memory StringInternTable.
    -- The previous UNIQUE constraint was redundant and slowed bulk inserts
    -- (removed per P-SK in epoch v25).
    CREATE INDEX IF NOT EXISTS Strings_value_idx ON Strings(value);

    CREATE TABLE IF NOT EXISTS DataPaths (
        id          INTEGER PRIMARY KEY,
        parent_id   INTEGER NOT NULL,
        component   TEXT NOT NULL,
        array_index INTEGER NOT NULL
    ) STRICT;

    -- encoding column dropped per P-SK (epoch v25): the encoding version
    -- is global (kSemanticResultEncodingVersion) and bumps with the epoch,
    -- so storing it per-row is redundant.
    CREATE TABLE IF NOT EXISTS Results (
        id      INTEGER PRIMARY KEY,
        type    INTEGER NOT NULL,
        payload TEXT,
        aux_context TEXT,
        hash    BLOB NOT NULL UNIQUE
    ) STRICT;

    -- Content-addressed exact dep key sets. Traces with the same serialized
    -- dep keys (including non-TraceHash-contributing guard keys) share a
    -- single DepKeySets row.
    CREATE TABLE IF NOT EXISTS DepKeySets (
        id          INTEGER PRIMARY KEY,
        key_set_hash BLOB NOT NULL UNIQUE,
        keys_blob   BLOB NOT NULL
    ) STRICT;

    -- Each trace references a shared DepKeySets row (dep structure) and stores
    -- its own values_blob (hash values in positional order matching keys_blob).
    -- trace_hash is the canonical recovery/result hash and is intentionally
    -- not unique; full_hash is the storage identity over all keys and values.
    CREATE TABLE IF NOT EXISTS Traces (
        id              INTEGER PRIMARY KEY,
        trace_hash      BLOB NOT NULL,
        full_hash       BLOB NOT NULL UNIQUE,
        dep_key_set_id  INTEGER NOT NULL REFERENCES DepKeySets(id),
        values_blob     BLOB NOT NULL
    ) STRICT;

    CREATE INDEX IF NOT EXISTS Traces_trace_hash_idx ON Traces(trace_hash);

    CREATE INDEX IF NOT EXISTS idx_traces_dep_key_set ON Traces(dep_key_set_id);

    CREATE TABLE IF NOT EXISTS Sessions (
        session_key BLOB NOT NULL,
        attr_path_id  INTEGER NOT NULL,
        trace_id    INTEGER NOT NULL,
        result_id     INTEGER NOT NULL,
        node_stamp    INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (session_key, attr_path_id)
    ) WITHOUT ROWID, STRICT;

    CREATE TABLE IF NOT EXISTS History (
        recovery_key       BLOB NOT NULL,
        attr_path_id       INTEGER NOT NULL,
        trace_id           INTEGER NOT NULL,
        result_id          INTEGER NOT NULL,
        git_identity_hash  BLOB,
        PRIMARY KEY (recovery_key, attr_path_id, trace_id)
    ) WITHOUT ROWID, STRICT;

    CREATE INDEX IF NOT EXISTS idx_history_git_identity_ordered
        ON History(recovery_key, attr_path_id, git_identity_hash, trace_id DESC)
        WHERE git_identity_hash IS NOT NULL;

    CREATE TABLE IF NOT EXISTS SessionRuntimeRoots (
        session_key BLOB NOT NULL,
        source_id BLOB NOT NULL,
        fetch_identity BLOB NOT NULL,
        nar_hash BLOB NOT NULL,
        store_path BLOB NOT NULL,
        PRIMARY KEY (session_key, source_id)
    ) WITHOUT ROWID, STRICT;

    -- Normalized dir-set definitions for aggregated DirSet deps.
    -- Each row stores the dirs JSON array once, keyed by content hash.
    -- DirSet dep keys reference this table by ds_hash instead of
    -- embedding the full dirs array (~44 KB) in every dep key string.
    CREATE TABLE IF NOT EXISTS DirSets (
        ds_hash TEXT PRIMARY KEY,
        dirs    BLOB NOT NULL
    ) STRICT;

)sql";

// ── Constructor / Destructor ─────────────────────────────────────────

SqliteTraceStorage::SqliteTraceStorage(
    SymbolTable & symbols,
    InterningPools & pools,
    AttrVocabStore & vocab,
    SemanticSessionKey initialSessionKey)
    : TraceStorage(std::move(initialSessionKey))
    , VocabAwareHasher(pools, vocab)
    , symbols(symbols)
    , pools(pools)
    , vocab(vocab)
{
    auto initStart = timerStart();
    auto state = std::make_unique<State>();
    gdp::Certifier<BlockingTag>::withProof([&](const auto & constructorBs) {
    auto & st = *state;

    auto cacheDir = std::filesystem::path(getCacheDir());
    createDirs(cacheDir);

    // DB filename encodes the semantic cache epoch and hash backend.
    // Epoch-bumping renames the file, orphaning pre-bump DBs (users
    // pay one cold-eval cost).  isCache() does NOT auto-recreate on
    // schema mismatch — a new filename ensures a fresh DB is created
    // on actual schema upgrades.
    auto dbPath = cacheDir / (
        "eval-trace-v" + std::to_string(kSchemaEpoch) + "-"
        + std::string(evalTraceHashAlgorithmSlug(getEvalTraceHashAlgorithm()))
        + ".sqlite");

    st.db = SQLite(dbPath, {.useWAL = settings.useSQLiteWAL});
    st.db.exec("pragma page_size = 65536");            // 64KB pages for large BLOB I/O (MUST be before isCache)
    st.db.isCache();
    st.db.exec("pragma cache_size = -16000");           // 16MB page cache
    st.db.exec("pragma mmap_size = 268435456");         // 256MB mmap
    st.db.exec("pragma temp_store = memory");
    st.db.exec("pragma journal_size_limit = 2097152");  // 2MB WAL limit

    st.db.exec(schema);

    // ATTACH vocab database for atomic cross-DB commits.
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

        auto vocabPath = quotePath(vocab.getDbPath().string());

        st.db.exec("ATTACH DATABASE '" + vocabPath + "' AS vocab");

        // Set WAL + cache pragmas on attached DB
        if (settings.useSQLiteWAL) {
            st.db.exec("PRAGMA vocab.journal_mode = wal");
        }
        st.db.exec("PRAGMA vocab.synchronous = off");

        // The schema is already created by AttrVocabStore's constructor.
    }

    // Strings — bulk load + explicit-ID insert (no UPSERT)
    st.getAllStrings.create(st.db,
        "SELECT id, value FROM Strings ORDER BY id");
    st.insertStringWithId.create(st.db,
        "INSERT OR IGNORE INTO Strings(id, value) VALUES (?, ?)");

    // DataPaths — bulk load + explicit-ID insert
    st.getAllDataPaths.create(st.db,
        "SELECT id, parent_id, component, array_index FROM DataPaths WHERE id > 0 ORDER BY id");
    st.insertDataPathWithId.create(st.db,
        "INSERT OR IGNORE INTO DataPaths(id, parent_id, component, array_index) VALUES (?, ?, ?, ?)");

    // Results — bulk load + explicit-ID insert.  encoding column dropped
    // per P-SK (epoch v25); kSemanticResultEncodingVersion is global.
    st.getAllResults.create(st.db,
        "SELECT id, type, payload, aux_context, hash FROM Results");
    st.insertResultWithId.create(st.db,
        "INSERT OR IGNORE INTO Results(id, type, payload, aux_context, hash) VALUES (?, ?, ?, ?, ?)");

    st.getResult.create(st.db,
        "SELECT type, payload, aux_context FROM Results WHERE id = ?");

    // DepKeySets — bulk load + explicit-ID insert
    st.getAllDepKeySets.create(st.db,
        "SELECT id, key_set_hash FROM DepKeySets");
    st.insertDepKeySetWithId.create(st.db,
        "INSERT OR IGNORE INTO DepKeySets(id, key_set_hash, keys_blob) VALUES (?, ?, ?)");

    st.getDepKeySet.create(st.db,
        "SELECT key_set_hash, keys_blob FROM DepKeySets WHERE id = ?");

    // Traces — bulk load + explicit-ID insert
    st.getAllTraces.create(st.db,
        "SELECT id, trace_hash, full_hash, dep_key_set_id FROM Traces");
    st.insertTraceWithId.create(st.db,
        "INSERT OR IGNORE INTO Traces(id, trace_hash, full_hash, dep_key_set_id, values_blob) VALUES (?, ?, ?, ?, ?)");

    st.lookupTraceByFullHash.create(st.db,
        "SELECT id FROM Traces WHERE full_hash = ?");

    // getTraceInfo: returns trace_hash, key_set_hash, dep_key_set_id, keys_blob, values_blob
    // via JOIN with DepKeySets. Used by loadFullTrace and getCurrentTraceHash.
    st.getTraceInfo.create(st.db,
        "SELECT t.trace_hash, dk.key_set_hash, dk.id, dk.keys_blob, t.values_blob "
        "FROM Traces t JOIN DepKeySets dk ON t.dep_key_set_id = dk.id WHERE t.id = ?");

    // Sessions (lightweight — no Results JOIN — renamed from CurrentTraces)
    st.lookupAttr.create(st.db,
        "SELECT trace_id, result_id, node_stamp "
        "FROM Sessions "
        "WHERE session_key = ? AND attr_path_id = ?");

    st.upsertAttr.create(st.db,
        "INSERT INTO Sessions(session_key, attr_path_id, trace_id, result_id, node_stamp) "
        "VALUES (?, ?, ?, ?, ?) "
        "ON CONFLICT(session_key, attr_path_id) DO UPDATE SET "
        "trace_id = excluded.trace_id, result_id = excluded.result_id, node_stamp = excluded.node_stamp");

    // History (renamed from TraceHistory)
    st.insertHistory.create(st.db,
        "INSERT OR IGNORE INTO History(recovery_key, attr_path_id, trace_id, result_id, git_identity_hash) "
        "VALUES (?, ?, ?, ?, ?)");

    // Lightweight history bootstrap: preserve the old scanHistory join's
    // corruption behavior by requiring the referenced Traces row to exist,
    // without loading dep-key metadata just to pick the newest trace.
    st.lookupLatestHistoryForAttr.create(st.db,
        "SELECT h.trace_id, h.result_id "
        "FROM History h "
        "JOIN Traces t ON h.trace_id = t.id "
        "WHERE h.recovery_key = ? AND h.attr_path_id = ? "
        "ORDER BY h.trace_id DESC LIMIT 1");

    // Direct-hash recovery: once the current canonical trace hash is known,
    // fetch only historical rows with that hash. Guard validation still runs
    // before any candidate result is served.
    st.lookupHistoryByTraceHash.create(st.db,
        "SELECT t.dep_key_set_id, h.trace_id, h.result_id "
        "FROM History h "
        "JOIN Traces t ON h.trace_id = t.id "
        "WHERE h.recovery_key = ? AND h.attr_path_id = ? AND t.trace_hash = ? "
        "ORDER BY h.trace_id DESC");

    // scanHistoryForAttr: JOIN through Traces to load candidate metadata for
    // structural-variant recovery. Results payloads and DepKeySets rows are
    // deliberately not joined: recovery only needs one payload for the accepted
    // candidate, and keysets are loaded lazily per attempted dep_key_set_id.
    // Columns: 0=t.dep_key_set_id, 1=h.trace_id, 2=h.result_id, 3=t.trace_hash.
    // The trace_hash enables in-memory candidate matching (no per-group DB lookup).
    st.scanHistoryForAttr.create(st.db,
        "SELECT t.dep_key_set_id, h.trace_id, h.result_id, t.trace_hash "
        "FROM History h "
        "JOIN Traces t ON h.trace_id = t.id "
        "WHERE h.recovery_key = ? AND h.attr_path_id = ? "
        "ORDER BY h.trace_id DESC");

    // GitIdentity-indexed recovery: look up historical trace/result ids by
    // GitIdentity dep hash value for cross-commit recovery without structural
    // variant scanning. Results are ordered newest-first; verification still
    // validates implicit structural guards before accepting a candidate, so
    // callers may continue scanning if the newest row is stale.
    st.lookupHistoryByGitIdentity.create(st.db,
        "SELECT t.dep_key_set_id, h.trace_id, h.result_id "
        "FROM History h "
        "JOIN Traces t ON h.trace_id = t.id "
        "WHERE h.recovery_key = ? AND h.attr_path_id = ? AND h.git_identity_hash = ? "
        "ORDER BY h.trace_id DESC");

    // DirSets (normalized dir-set definitions)
    st.insertDirSet.create(st.db,
        "INSERT OR IGNORE INTO DirSets(ds_hash, dirs) VALUES (?, ?)");
    st.getAllDirSets.create(st.db,
        "SELECT ds_hash, dirs FROM DirSets");

    // SessionRuntimeRoots (session metadata for runtime-fetched inputs)
    st.insertRuntimeRoot.create(st.db,
        "INSERT OR REPLACE INTO SessionRuntimeRoots(session_key, source_id, fetch_identity, nar_hash, store_path) "
        "VALUES (?, ?, ?, ?, ?)");
    st.loadRuntimeRoots.create(st.db,
        "SELECT source_id, fetch_identity, nar_hash, store_path FROM SessionRuntimeRoots "
        "WHERE session_key = ?");

    // Vocab (on ATTACH'd vocab.* schema)
    st.insertVocabName.create(st.db,
        "INSERT OR IGNORE INTO vocab.AttrNames(id, name) VALUES (?, ?)");
    st.insertVocabPath.create(st.db,
        "INSERT OR IGNORE INTO vocab.AttrPaths(id, parent, child) VALUES (?, ?, ?)");

    {
        SQLiteStmt getMaxNodeStamp;
        getMaxNodeStamp.create(st.db,
            "SELECT COALESCE(MAX(node_stamp), 0) FROM Sessions");
        auto use(getMaxNodeStamp.use());
        if (use.next())
            raiseNextNodeStamp(static_cast<uint32_t>(use.getInt(0)) + 1);
    }

    bulkLoadAllLocked(st);
    }); // withProof

    _state = std::move(state);

    nrDbInitTimeUs += elapsedUs(initStart);
}

SqliteTraceStorage::~SqliteTraceStorage()
{
    auto closeStart = timerStart();
    // Flush all pending in-memory writes to SQLite
    try {
        flushExclusive();
    } catch (...) {
        ignoreExceptionExceptInterrupt();
    }
    // Flush vocab entries.
    // checkpoint() independently persists vocab to attr-vocab.sqlite first
    // (crash-safe). flushTo() then catches any residual entries within the
    // ATTACH'd transaction for exact consistency on clean shutdown.
    try {
        vocab.checkpoint();
        gdp::Certifier<BlockingTag>::withProof([&](const auto & dtorCommitBs) {
            auto & st = *_state;
            SQLiteTxn txn(st.db);
            vocab.flushTo(st.insertVocabName, st.insertVocabPath);
            txn.commit();
        });
    } catch (...) {
        ignoreExceptionExceptInterrupt();
    }
    nrDbCloseTimeUs += elapsedUs(closeStart);
}

// ── Helpers ──────────────────────────────────────────────────────────

void SqliteTraceStorage::flushExclusive()
{
    gdp::Certifier<BlockingTag>::withProof([&](const auto & bs) {
        withExclusiveAccess(bs, [&](const auto & ea) {
            flush(ea);
        });
    });
}

std::optional<SqliteTraceStorage::EvalInfoRecord> SqliteTraceStorage::queryEvalInfoExclusive(
    AttrPathId pathId, bool allowHistoryFallback)
{
    std::optional<EvalInfoRecord> result;
    gdp::Certifier<BlockingTag>::withProof([&](const auto & bs) {
        withExclusiveAccess(bs, [&](const auto & ea) {
            auto source = EvalInfoRecord::Source::Session;
            auto node = lookupCurrentNode(bs, pathId);
            if (!node && allowHistoryFallback) {
                node = lookupLatestHistoryForAttr(ea, pathId);
                if (node)
                    source = EvalInfoRecord::Source::History;
            }
            if (!node)
                return;

            auto deps = loadFullTrace(ea, node->traceId);
            auto * header = ensureTraceHeader(bs, node->traceId);
            if (!header)
                return;
            auto value = decodeCachedResult(bs, node->resultId);

            result.emplace(EvalInfoRecord{
                .traceId = node->traceId,
                .resultId = node->resultId,
                .traceHash = header->traceHash,
                .keySetHash = header->keySetHash,
                .depKeySetId = header->depKeySetId,
                .value = std::move(value),
                .deps = std::move(deps),
                .source = source,
            });
        });
    });
    return result;
}

void SqliteTraceStorage::setSessionConfig(SessionConfig config)
{
    // SQLite backend binds the process-global hash algorithm; the
    // DB filename already encodes it, so the algorithm a session
    // hashes under matches whatever the store was opened with.
    auto algorithm = getEvalTraceHashAlgorithm();
    storeSessionConfig(std::move(config), algorithm);
    auto semanticKeyHex = currentSemanticSessionKey().toHex();
    auto recoveryKeyHex = currentStableRecoveryKey().value.toHex();
    debug("eval-trace/store: setSessionConfig key digest=%s recoveryKey=%s",
        semanticKeyHex, recoveryKeyHex);
}

// ── Bulk load / flush ────────────────────────────────────────────────

void SqliteTraceStorage::bulkLoadAllLocked(State & st)
{
    {
        auto use(st.getAllStrings.use());
        while (use.next()) {
            auto id = static_cast<uint32_t>(use.getInt(0));
            auto [valueBlob, valueSize] = use.getBlob(1);
            if (!valueBlob && valueSize != 0)
                throw Error("Strings.value row %d was null", id);
            pools.bulkLoadString(
                id,
                std::string(
                    reinterpret_cast<const char *>(valueBlob ? valueBlob : ""),
                    valueSize));
            if (id > flushedStringHighWaterMark)
                flushedStringHighWaterMark = id;
        }
    }
    {
        auto use(st.getAllDataPaths.use());
        while (use.next()) {
            auto id = static_cast<uint32_t>(use.getInt(0));
            auto parentId = static_cast<uint32_t>(use.getInt(1));
            auto component = use.getStr(2);
            auto arrayIndex = static_cast<int32_t>(use.getInt(3));
            pools.dataPathPool.bulkLoad(id, parentId, std::string(component), arrayIndex);
            if (id > flushedDataPathHighWaterMark)
                flushedDataPathHighWaterMark = id;
        }
    }
    {
        auto use(st.getAllResults.use());
        while (use.next()) {
            auto id = ResultId(static_cast<uint32_t>(use.getInt(0)));
            // Columns: 0=id, 1=type, 2=payload, 3=aux_context, 4=hash
            // (encoding column removed per P-SK in epoch v25).
            auto [hashBlob, hashSize] = use.getBlob(4);
            if (hashBlob && hashSize == kEvalTraceDigestSize) {
                resultByHash[evalTraceHashFromBlob<ResultHash>(hashBlob, hashSize)] = id;
            }
            if (id > nextResultId) nextResultId = id;
        }
    }
    {
        auto use(st.getAllDepKeySets.use());
        while (use.next()) {
            auto id = DepKeySetId(static_cast<uint32_t>(use.getInt(0)));
            auto [hashBlob, hashSize] = use.getBlob(1);
            if (hashBlob && hashSize == kEvalTraceDigestSize) {
                depKeySetByHash[evalTraceHashFromBlob<DepKeySetHash>(hashBlob, hashSize)] = id;
            }
            if (id > nextDepKeySetId) nextDepKeySetId = id;
        }
    }
    {
        auto use(st.getAllTraces.use());
        while (use.next()) {
            auto id = TraceId(static_cast<uint32_t>(use.getInt(0)));
            auto [hashBlob, hashSize] = use.getBlob(2);
            if (hashBlob && hashSize == kEvalTraceDigestSize) {
                traceByFullHash[evalTraceHashFromBlob<FullTraceHash>(hashBlob, hashSize)] = id;
            }
            if (id > nextTraceId) nextTraceId = id;
        }
    }
    {
        auto use(st.getAllDirSets.use());
        while (use.next()) {
            auto [blob, size] = use.getBlob(1);
            pools.dirSets.emplace(std::string(use.getStr(0)), deserializeDirSet(blob, size));
        }
    }
}

void SqliteTraceStorage::flush(const ExclusiveTraceStorageAccess & ea)
{
    auto flushStart = timerStart();

    // The enclosing `withExclusiveAccess` holds `storeMutex_`, which
    // serializes all `*_state` access.  The previously-redundant inner
    // `BlockingSync<State>` mutex was removed per P30 (every caller now
    // routes through `withExclusiveAccess`, including orchestrator
    // coroBlock paths).

    // Flush vocab to attr-vocab.sqlite via a brief independent connection
    // BEFORE writing trace entities. This ensures vocab entries are durable
    // before any traces referencing them can be committed. Cross-DB ATTACH'd
    // transactions are not atomic in WAL mode — without this, a crash could
    // leave the main DB with traces referencing vocab IDs that were never
    // committed to attr-vocab.sqlite. The independent connection's transaction
    // is brief (just the new entries since last checkpoint), so SQLITE_BUSY
    // contention with other SqliteTraceStorage instances is minimal.
    vocab.checkpoint();

    auto & st = *_state;
    SQLiteTxn txn(st.db);

    // Flush in dependency order: Strings → DataPaths → DirSets → Results → DepKeySets → Traces

    // Flush new strings: those with ID > flushedStringHighWaterMark.
    for (uint32_t i = flushedStringHighWaterMark + 1; i < pools.nextStringId(); i++) {
        auto sv = pools.resolveRawString(i);
        auto use = st.insertStringWithId.use();
        use(static_cast<int64_t>(i));
        bindBlob(use, std::string_view(sv.data(), sv.size()));
        use.exec();
    }
    flushedStringHighWaterMark = pools.nextStringId() > 0
        ? pools.nextStringId() - 1
        : 0;

    for (uint32_t i = flushedDataPathHighWaterMark + 1; i < pools.dataPathPool.nextId(); i++) {
        auto & node = pools.dataPathPool.nodes[i];
        st.insertDataPathWithId.use()
            (static_cast<int64_t>(i))
            (static_cast<int64_t>(node.parentId))
            (node.component)
            (static_cast<int64_t>(node.arrayIndex))
            .exec();
    }
    flushedDataPathHighWaterMark = pools.dataPathPool.nextId() > 0
        ? pools.dataPathPool.nextId() - 1
        : 0;

    // Flush DirSet definitions (INSERT OR IGNORE deduplicates; ~2 entries typical)
    for (auto & [dsHash, dirs] : pools.dirSets) {
        auto blob = serializeDirSet(dirs);
        auto use(st.insertDirSet.use());
        use(dsHash);
        bindBlob(use, blob);
        use.exec();
    }

    for (auto & r : pendingResults) {
        auto use(st.insertResultWithId.use());
        use(static_cast<int64_t>(r.id.value));
        use(static_cast<int64_t>(r.payload.type));
        // encoding column dropped per P-SK in epoch v25.
        use(r.payload.payload);
        use(r.payload.auxContext);
        bindTaggedEvalTraceHash(use, r.hash);
        use.exec();
    }
    pendingResults.clear();

    for (auto & dks : pendingDepKeySets) {
        auto use(st.insertDepKeySetWithId.use());
        use(static_cast<int64_t>(dks.id.value));
        bindTaggedEvalTraceHash(use, dks.keySetHash);
        bindBlob(use, dks.keysBlob);
        use.exec();
    }
    pendingDepKeySets.clear();

    for (auto & t : pendingTraces) {
        auto use(st.insertTraceWithId.use());
        use(static_cast<int64_t>(t.id.value));
        bindTaggedEvalTraceHash(use, t.traceHash);
        bindTaggedEvalTraceHash(use, t.fullHash);
        use(static_cast<int64_t>(t.depKeySetId.value));
        bindBlob(use, t.valuesBlob);
        use.exec();
    }
    pendingTraces.clear();

    txn.commit();
    nrRecordFlushUs += elapsedUs(flushStart);
}

} // namespace nix::eval_trace
