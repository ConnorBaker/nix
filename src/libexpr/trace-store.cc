#include "nix/expr/trace-store.hh"
#include "nix/expr/dependency-tracker.hh"
#include "nix/expr/trace-cache.hh"
#include "nix/expr/trace-hash.hh"
#include "nix/expr/eval.hh"
#include "nix/store/globals.hh"
#include "nix/util/users.hh"
#include "nix/util/util.hh"
#include "nix/util/hash.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/source-accessor.hh"
#include "nix/fetchers/fetchers.hh"

#include <chrono>
#include <cstring>
#include <filesystem>
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

// ── Trace verification helpers (BSàlC verifying trace check) ─────────

static std::optional<SourcePath> resolveDepPath(
    const Dep & dep, const std::unordered_map<std::string, SourcePath> & inputAccessors)
{
    if (dep.source == absolutePathDep)
        return SourcePath(getFSSourceAccessor(), CanonPath(dep.key));
    auto it = inputAccessors.find(dep.source);
    if (it != inputAccessors.end())
        return it->second / CanonPath(dep.key);
    if (dep.source.empty())
        return SourcePath(getFSSourceAccessor(), CanonPath(dep.key));
    return std::nullopt;
}

static std::optional<DepHashValue> computeCurrentHash(
    EvalState & state, const Dep & dep,
    const std::unordered_map<std::string, SourcePath> & inputAccessors)
{
    switch (dep.type) {
    case DepType::Content: {
        auto path = resolveDepPath(dep, inputAccessors);
        if (!path) return std::nullopt;
        try {
            return DepHashValue(depHashFile(*path));
        } catch (std::exception &) {
            return std::nullopt;
        }
    }
    case DepType::NARContent: {
        auto path = resolveDepPath(dep, inputAccessors);
        if (!path) return std::nullopt;
        try {
            return DepHashValue(depHashPathCached(*path));
        } catch (std::exception &) {
            return std::nullopt;
        }
    }
    case DepType::Directory: {
        auto path = resolveDepPath(dep, inputAccessors);
        if (!path) return std::nullopt;
        try {
            return DepHashValue(depHashDirListingCached(*path, path->readDirectory()));
        } catch (std::exception &) {
            return std::nullopt;
        }
    }
    case DepType::Existence: {
        auto path = resolveDepPath(dep, inputAccessors);
        if (!path) return std::nullopt;
        auto st = path->maybeLstat();
        return DepHashValue(st
            ? fmt("type:%d", static_cast<int>(st->type))
            : std::string("missing"));
    }
    case DepType::EnvVar:
        return DepHashValue(depHash(getEnv(dep.key).value_or("")));
    case DepType::System:
        return DepHashValue(depHash(state.settings.getCurrentSystem()));
    case DepType::CopiedPath: {
        auto sourcePath = resolveDepPath(dep, inputAccessors);
        if (!sourcePath) return std::nullopt;
        try {
            auto * storePathStr = std::get_if<std::string>(&dep.expectedHash);
            if (!storePathStr) return std::nullopt;
            auto expectedStorePath = state.store->parseStorePath(*storePathStr);
            auto name2 = std::string(expectedStorePath.name());
            auto [storePath, hash] = state.store->computeStorePath(
                name2,
                sourcePath->resolveSymlinks(SymlinkResolution::Ancestors),
                ContentAddressMethod::Raw::NixArchive,
                HashAlgorithm::SHA256, {});
            return DepHashValue(state.store->printStorePath(storePath));
        } catch (std::exception &) {
            return std::nullopt;
        }
    }
    case DepType::UnhashedFetch: {
        try {
            auto input = fetchers::Input::fromURL(state.fetchSettings, dep.key);
            auto [storePath, lockedInput] = input.fetchToStore(
                state.fetchSettings, *state.store);
            return DepHashValue(state.store->printStorePath(storePath));
        } catch (std::exception &) {
            return std::nullopt;
        }
    }
    case DepType::CurrentTime:
    case DepType::Exec:
    case DepType::ParentContext:
        return std::nullopt;
    }
    unreachable();
}

// ── Raw hash helpers ─────────────────────────────────────────────────

static void bindRawHash(SQLiteStmt::Use & use, const Hash & h)
{
    use(reinterpret_cast<const unsigned char *>(h.hash),
        static_cast<size_t>(h.hashSize));
}

static Hash readRawHash(const void * data, size_t size)
{
    if (size != 32)
        throw Error("expected 32-byte BLAKE3 hash blob, got %d bytes", size);
    Hash h(HashAlgorithm::BLAKE3);
    memcpy(h.hash, data, 32);
    return h;
}

static Hash computeResultHash(ResultKind type, std::string_view value, std::string_view context)
{
    HashSink sink(HashAlgorithm::BLAKE3);
    sink(std::string_view("T", 1));
    auto typeStr = std::to_string(std::to_underlying(type));
    sink(typeStr);
    sink(std::string_view("V", 1));
    sink(value);
    sink(std::string_view("C", 1));
    sink(context);
    return sink.finish().hash;
}

// ── BLOB serialization for dep entries ───────────────────────────────
//
// Packed binary format per dep entry:
//   header:    10 bytes (type[1] + source_id[4] + key_id[4] + hash_len[1])
//   hash_data: hash_len bytes

struct __attribute__((packed)) DepBlobHeader {
    uint8_t type;
    uint32_t sourceId;
    uint32_t keyId;
    uint8_t hashLen;
};
static_assert(sizeof(DepBlobHeader) == 10);

std::vector<uint8_t> TraceStore::serializeDeps(const std::vector<InternedDep> & deps)
{
    std::vector<uint8_t> blob;
    blob.reserve(deps.size() * 42);

    for (auto & dep : deps) {
        auto [hashData, hashSize] = blobData(dep.hash);
        DepBlobHeader hdr{std::to_underlying(dep.type), dep.sourceId, dep.keyId,
                          static_cast<uint8_t>(hashSize)};
        auto * raw = reinterpret_cast<const uint8_t *>(&hdr);
        blob.insert(blob.end(), raw, raw + sizeof(hdr));
        blob.insert(blob.end(), hashData, hashData + hashSize);
    }

    return blob;
}

std::vector<TraceStore::InternedDep> TraceStore::deserializeInternedDeps(
    const void * blob, size_t size)
{
    std::vector<InternedDep> deps;
    const uint8_t * p = static_cast<const uint8_t *>(blob);
    const uint8_t * end = p + size;

    while (p + sizeof(DepBlobHeader) <= end) {
        DepBlobHeader hdr;
        std::memcpy(&hdr, p, sizeof(hdr));
        p += sizeof(hdr);

        if (p + hdr.hashLen > end) break;

        InternedDep dep;
        dep.type = static_cast<DepType>(hdr.type);
        dep.sourceId = hdr.sourceId;
        dep.keyId = hdr.keyId;
        if (isBlake3Dep(dep.type) && hdr.hashLen == 32)
            dep.hash = Blake3Hash::fromBlob(p, 32);
        else
            dep.hash = std::string(reinterpret_cast<const char *>(p), hdr.hashLen);
        p += hdr.hashLen;

        deps.push_back(std::move(dep));
    }

    return deps;
}

std::vector<Dep> TraceStore::resolveDeps(const std::vector<InternedDep> & interned)
{
    ensureStringTableLoaded();
    std::vector<Dep> deps;
    deps.reserve(interned.size());

    for (auto & d : interned) {
        auto sourceIt = stringTable.find(static_cast<StringId>(d.sourceId));
        auto keyIt = stringTable.find(static_cast<StringId>(d.keyId));
        deps.push_back(Dep{
            sourceIt != stringTable.end() ? sourceIt->second : "",
            keyIt != stringTable.end() ? keyIt->second : "",
            d.hash,
            d.type
        });
    }

    return deps;
}

std::vector<TraceStore::InternedDep> TraceStore::internDeps(const std::vector<Dep> & deps)
{
    std::vector<InternedDep> interned;
    interned.reserve(deps.size());

    for (auto & dep : deps) {
        interned.push_back({
            dep.type,
            static_cast<uint32_t>(doInternString(dep.source)),
            static_cast<uint32_t>(doInternString(dep.key)),
            dep.expectedHash
        });
    }

    return interned;
}

// ── BLOB binding helper (ensures non-null pointer for empty BLOBs) ───

static void bindBlobVec(SQLiteStmt::Use & use, const std::vector<uint8_t> & blob)
{
    // sqlite3_bind_blob with a null pointer binds NULL, not empty BLOB.
    // Use a sentinel address for empty blobs.
    static const uint8_t empty = 0;
    use(reinterpret_cast<const unsigned char *>(
            blob.empty() ? &empty : blob.data()),
        blob.size());
}

// ── Schema ───────────────────────────────────────────────────────────

static const char * schema = R"sql(

    CREATE TABLE IF NOT EXISTS Strings (
        id    INTEGER PRIMARY KEY,
        value TEXT NOT NULL UNIQUE
    ) STRICT;

    CREATE TABLE IF NOT EXISTS AttrPaths (
        id   INTEGER PRIMARY KEY,
        path BLOB NOT NULL UNIQUE
    ) STRICT;

    CREATE TABLE IF NOT EXISTS Results (
        id      INTEGER PRIMARY KEY,
        type    INTEGER NOT NULL,
        value   TEXT,
        context TEXT,
        hash    BLOB NOT NULL UNIQUE
    ) STRICT;

    CREATE TABLE IF NOT EXISTS Traces (
        id          INTEGER PRIMARY KEY,
        base_trace_id     INTEGER REFERENCES Traces(id),
        trace_hash   BLOB NOT NULL UNIQUE,
        struct_hash BLOB NOT NULL,
        deps_blob   BLOB NOT NULL
    ) STRICT;

    CREATE INDEX IF NOT EXISTS idx_traces_struct ON Traces(struct_hash);

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

// ── Constructor / Destructor ─────────────────────────────────────────

TraceStore::TraceStore(SymbolTable & symbols, int64_t contextHash)
    : symbols(symbols)
    , contextHash(contextHash)
{
    auto initStart = timerStart();
    auto state = std::make_unique<Sync<State>>();
    auto st(state->lock());

    auto cacheDir = std::filesystem::path(getCacheDir());
    createDirs(cacheDir);

    auto dbPath = cacheDir / "eval-trace-v1.sqlite";

    st->db = SQLite(dbPath, {.useWAL = settings.useSQLiteWAL});
    st->db.isCache();
    st->db.exec("pragma cache_size = -4000");          // 4MB page cache
    st->db.exec("pragma mmap_size = 30000000");        // 30MB mmap
    st->db.exec("pragma temp_store = memory");
    st->db.exec("pragma journal_size_limit = 2097152"); // 2MB WAL limit

    st->db.exec(schema);

    // Strings interning
    st->insertString.create(st->db,
        "INSERT OR IGNORE INTO Strings(value) VALUES (?)");

    st->lookupStringId.create(st->db,
        "SELECT id FROM Strings WHERE value = ?");

    st->getAllStrings.create(st->db,
        "SELECT id, value FROM Strings");

    // AttrPaths interning
    st->insertAttrPath.create(st->db,
        "INSERT OR IGNORE INTO AttrPaths(path) VALUES (?)");

    st->lookupAttrPathId.create(st->db,
        "SELECT id FROM AttrPaths WHERE path = ?");

    // Results dedup
    st->insertResult.create(st->db,
        "INSERT OR IGNORE INTO Results(type, value, context, hash) VALUES (?, ?, ?, ?)");

    st->lookupResultByHash.create(st->db,
        "SELECT id FROM Results WHERE hash = ?");

    st->getResult.create(st->db,
        "SELECT type, value, context FROM Results WHERE id = ?");

    // Traces (BLOB storage)
    st->insertTrace.create(st->db,
        "INSERT OR IGNORE INTO Traces(base_trace_id, trace_hash, struct_hash, deps_blob) "
        "VALUES (?, ?, ?, ?)");

    st->lookupTraceByFullHash.create(st->db,
        "SELECT id FROM Traces WHERE trace_hash = ?");

    st->getTraceInfo.create(st->db,
        "SELECT base_trace_id, trace_hash, struct_hash, deps_blob FROM Traces WHERE id = ?");

    st->lookupTraceByStructHash.create(st->db,
        "SELECT id FROM Traces WHERE struct_hash = ? LIMIT 1");

    st->updateTraceBlob.create(st->db,
        "UPDATE Traces SET base_trace_id = ?, deps_blob = ? WHERE id = ?");

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

    st->scanHistoryForAttr.create(st->db,
        "SELECT ds.struct_hash, h.trace_id, h.result_id "
        "FROM TraceHistory h JOIN Traces ds ON h.trace_id = ds.id "
        "WHERE h.context_hash = ? AND h.attr_path_id = ?");

    // StatHashCache
    st->queryAllStatHash.create(st->db,
        "SELECT path, dep_type, dev, ino, mtime_sec, mtime_nsec, size, hash "
        "FROM StatHashCache");

    st->upsertStatHash.create(st->db,
        "INSERT INTO StatHashCache (path, dep_type, dev, ino, mtime_sec, mtime_nsec, size, hash) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT (path, dep_type) DO UPDATE SET "
        "dev = excluded.dev, ino = excluded.ino, "
        "mtime_sec = excluded.mtime_sec, mtime_nsec = excluded.mtime_nsec, "
        "size = excluded.size, hash = excluded.hash");

    st->txn = std::make_unique<SQLiteTxn>(st->db);

    // Bulk-load StatHashCache entries into in-memory singleton
    {
        std::vector<StatHashEntry> entries;
        auto use(st->queryAllStatHash.use());
        while (use.next()) {
            auto [hashBlob, hashLen] = use.getBlob(7);
            if (hashLen != 32) continue;
            entries.push_back(StatHashEntry{
                .path = use.getStr(0),
                .stat = {
                    .dev = static_cast<dev_t>(use.getInt(2)),
                    .ino = static_cast<ino_t>(use.getInt(3)),
                    .mtime_sec = static_cast<time_t>(use.getInt(4)),
                    .mtime_nsec = use.getInt(5),
                    .size = static_cast<off_t>(use.getInt(6)),
                    .depType = static_cast<DepType>(use.getInt(1)),
                },
                .hash = Blake3Hash::fromBlob(hashBlob, hashLen),
            });
        }
        loadStatHashEntries(std::move(entries));
    }

    _state = std::move(state);
    nrDbInitTimeUs += elapsedUs(initStart);
}

TraceStore::~TraceStore()
{
    auto closeStart = timerStart();
    // Flush dirty stat-hash entries back to SQLite
    try {
        auto st(_state->lock());
        forEachDirtyStatHash([&](const StatHashEntry & e) {
            st->upsertStatHash.use()
                (e.path)
                (static_cast<int64_t>(std::to_underlying(e.stat.depType)))
                (static_cast<int64_t>(e.stat.dev))
                (static_cast<int64_t>(e.stat.ino))
                (e.stat.mtime_sec)
                (e.stat.mtime_nsec)
                (static_cast<int64_t>(e.stat.size))
                (e.hash.data(), e.hash.size())
                .exec();
        });
    } catch (...) {
        ignoreExceptionExceptInterrupt();
    }
    try {
        optimizeTraces();
    } catch (...) {
        ignoreExceptionExceptInterrupt();
    }
    try {
        auto st(_state->lock());
        if (st->txn)
            st->txn->commit();
    } catch (...) {
        ignoreExceptionExceptInterrupt();
    }
    nrDbCloseTimeUs += elapsedUs(closeStart);
}

// ── Helpers ──────────────────────────────────────────────────────────

std::string TraceStore::buildAttrPath(const std::vector<std::string> & components)
{
    return concatStringsSep(std::string_view("\0", 1), components);
}

void TraceStore::clearSessionCaches()
{
    verifiedTraceIds.clear();
    internedStrings.clear();
    internedAttrPaths.clear();
    traceCache.clear();
    traceHashCache.clear();
    traceStructHashCache.clear();
    stringTable.clear();
    stringTableLoaded = false;
    currentDepHashes.clear();
    dirtyTraceIds.clear();
}

void TraceStore::ensureStringTableLoaded()
{
    if (stringTableLoaded) return;

    auto st(_state->lock());
    auto use(st->getAllStrings.use());
    while (use.next()) {
        auto id = use.getInt(0);
        auto value = use.getStr(1);
        stringTable[id] = value;
    }
    stringTableLoaded = true;
}

// ── CachedResult SQL encoding/decoding ──────────────────────────────────

std::tuple<ResultKind, std::string, std::string> TraceStore::encodeCachedResult(const CachedResult & value)
{
    return std::visit(overloaded{
        [&](const std::vector<Symbol> & attrs) -> std::tuple<ResultKind, std::string, std::string> {
            std::string val;
            bool first = true;
            for (auto & sym : attrs) {
                if (!first) val.push_back('\t');
                val.append(std::string(symbols[sym]));
                first = false;
            }
            return {ResultKind::FullAttrs, std::move(val), ""};
        },
        [&](const string_t & s) -> std::tuple<ResultKind, std::string, std::string> {
            std::string ctx;
            bool first = true;
            for (auto & elem : s.second) {
                if (!first) ctx.push_back(' ');
                ctx.append(elem.to_string());
                first = false;
            }
            return {ResultKind::String, s.first, std::move(ctx)};
        },
        [&](const placeholder_t &) -> std::tuple<ResultKind, std::string, std::string> {
            return {ResultKind::Placeholder, "", ""};
        },
        [&](const missing_t &) -> std::tuple<ResultKind, std::string, std::string> {
            return {ResultKind::Missing, "", ""};
        },
        [&](const misc_t &) -> std::tuple<ResultKind, std::string, std::string> {
            return {ResultKind::Misc, "", ""};
        },
        [&](const failed_t &) -> std::tuple<ResultKind, std::string, std::string> {
            return {ResultKind::Failed, "", ""};
        },
        [&](bool b) -> std::tuple<ResultKind, std::string, std::string> {
            return {ResultKind::Bool, b ? "1" : "0", ""};
        },
        [&](const int_t & i) -> std::tuple<ResultKind, std::string, std::string> {
            return {ResultKind::Int, std::to_string(i.x.value), ""};
        },
        [&](const std::vector<std::string> & l) -> std::tuple<ResultKind, std::string, std::string> {
            std::string val;
            bool first = true;
            for (auto & s : l) {
                if (!first) val.push_back('\t');
                val.append(s);
                first = false;
            }
            return {ResultKind::ListOfStrings, std::move(val), ""};
        },
        [&](const path_t & p) -> std::tuple<ResultKind, std::string, std::string> {
            return {ResultKind::Path, p.path, ""};
        },
        [&](const null_t &) -> std::tuple<ResultKind, std::string, std::string> {
            return {ResultKind::Null, "", ""};
        },
        [&](const float_t & f) -> std::tuple<ResultKind, std::string, std::string> {
            return {ResultKind::Float, std::to_string(f.x), ""};
        },
        [&](const list_t & lt) -> std::tuple<ResultKind, std::string, std::string> {
            return {ResultKind::List, std::to_string(lt.size), ""};
        },
    }, value);
}

CachedResult TraceStore::decodeCachedResult(const TraceRow & row)
{
    switch (row.type) {
    case ResultKind::FullAttrs: {
        std::vector<Symbol> attrs;
        if (!row.value.empty()) {
            for (auto & name : tokenizeString<std::vector<std::string>>(row.value, "\t"))
                attrs.push_back(symbols.create(name));
        }
        return attrs;
    }
    case ResultKind::String: {
        NixStringContext context;
        if (!row.context.empty()) {
            for (auto & elem : tokenizeString<std::vector<std::string>>(row.context, " "))
                context.insert(NixStringContextElem::parse(elem));
        }
        return string_t{row.value, std::move(context)};
    }
    case ResultKind::Bool:
        return row.value != "0";
    case ResultKind::Int:
        return int_t{NixInt{row.value.empty() ? 0L : std::stol(row.value)}};
    case ResultKind::ListOfStrings:
        return row.value.empty()
            ? std::vector<std::string>{}
            : tokenizeString<std::vector<std::string>>(row.value, "\t");
    case ResultKind::Path:
        return path_t{row.value};
    case ResultKind::Null:
        return null_t{};
    case ResultKind::Float:
        return float_t{row.value.empty() ? 0.0 : std::stod(row.value)};
    case ResultKind::List:
        return list_t{row.value.empty() ? (size_t)0 : std::stoull(row.value)};
    case ResultKind::Missing:
        return missing_t{};
    case ResultKind::Misc:
        return misc_t{};
    case ResultKind::Failed:
        return failed_t{};
    case ResultKind::Placeholder:
        return placeholder_t{};
    default:
        throw Error("unexpected type %d in eval trace", std::to_underlying(row.type));
    }
}

// ── Intern methods ───────────────────────────────────────────────────

StringId TraceStore::doInternString(std::string_view s)
{
    auto key = std::string(s);
    auto it = internedStrings.find(key);
    if (it != internedStrings.end())
        return it->second;

    auto st(_state->lock());
    st->insertString.use()(s).exec();

    auto use(st->lookupStringId.use()(s));
    if (!use.next())
        throw Error("failed to intern string '%s'", s);
    StringId id = use.getInt(0);
    internedStrings[key] = id;
    // Also populate reverse string table
    stringTable[id] = key;
    return id;
}

AttrPathId TraceStore::doInternAttrPath(std::string_view path)
{
    auto key = std::string(path);
    auto it = internedAttrPaths.find(key);
    if (it != internedAttrPaths.end())
        return it->second;

    auto st(_state->lock());
    st->insertAttrPath.use()
        (reinterpret_cast<const unsigned char *>(path.data()), path.size())
        .exec();

    auto use(st->lookupAttrPathId.use()
        (reinterpret_cast<const unsigned char *>(path.data()), path.size()));
    if (!use.next())
        throw Error("failed to intern attr path");
    AttrPathId id = use.getInt(0);
    internedAttrPaths[key] = id;
    return id;
}

ResultId TraceStore::doInternResult(ResultKind type, const std::string & value,
                                     const std::string & context, const Hash & resultHash)
{
    auto st(_state->lock());

    // Try to find existing result by hash
    {
        auto use(st->lookupResultByHash.use());
        bindRawHash(use, resultHash);
        if (use.next())
            return use.getInt(0);
    }

    // Insert new result
    {
        auto use(st->insertResult.use());
        use(static_cast<int64_t>(type));
        use(value);
        use(context);
        bindRawHash(use, resultHash);
        use.exec();
    }

    // Get id
    {
        auto use(st->lookupResultByHash.use());
        bindRawHash(use, resultHash);
        if (!use.next())
            throw Error("failed to intern result");
        return use.getInt(0);
    }
}

// ── Trace storage (BSàlC trace store) ───────────────────────────────

std::optional<TraceId> TraceStore::getTraceBaseId(TraceId traceId)
{
    auto st(_state->lock());
    auto use(st->getTraceInfo.use()(traceId));
    if (!use.next())
        return std::nullopt;
    if (use.isNull(0))
        return std::nullopt;
    return use.getInt(0);
}

Hash TraceStore::getTraceFullHash(TraceId traceId)
{
    auto cacheIt = traceHashCache.find(traceId);
    if (cacheIt != traceHashCache.end())
        return cacheIt->second;

    auto st(_state->lock());
    auto use(st->getTraceInfo.use()(traceId));
    if (!use.next())
        throw Error("trace %d not found", traceId);
    auto [blobData, blobSize] = use.getBlob(1);
    auto h = readRawHash(blobData, blobSize);
    traceHashCache.insert_or_assign(traceId, h);
    return h;
}

std::vector<Dep> TraceStore::loadTraceDelta(TraceId traceId)
{
    // Read deps_blob from Traces and deserialize
    const void * blobPtr = nullptr;
    size_t blobLen = 0;

    {
        auto st(_state->lock());
        auto use(st->getTraceInfo.use()(traceId));
        if (!use.next())
            return {};

        // Column 3 = deps_blob
        auto [data, size] = use.getBlob(3);
        if (!data || size == 0)
            return {};

        // Copy blob data since the SQLite statement will be reset
        blobLen = size;
        auto * copy = new uint8_t[size];
        memcpy(copy, data, size);
        blobPtr = copy;
    }

    auto interned = deserializeInternedDeps(blobPtr, blobLen);
    delete[] static_cast<const uint8_t *>(blobPtr);

    return resolveDeps(interned);
}

std::vector<Dep> TraceStore::loadFullTrace(TraceId traceId)
{
    auto cacheIt = traceCache.find(traceId);
    if (cacheIt != traceCache.end())
        return cacheIt->second;

    auto loadStart = timerStart();
    nrLoadTraces++;

    // Walk up the base_trace_id chain to find nearest cached ancestor or root
    std::vector<TraceId> chainToApply;
    TraceId currentId = traceId;
    std::vector<Dep> baseDeps;

    while (true) {
        auto cached = traceCache.find(currentId);
        if (cached != traceCache.end()) {
            baseDeps = cached->second;
            break;
        }
        chainToApply.push_back(currentId);
        auto baseTraceId = getTraceBaseId(currentId);
        if (!baseTraceId)
            break;
        currentId = *baseTraceId;
    }

    // chainToApply is [traceId, parent, ..., root_or_near_cached]
    // Reverse to apply from root toward traceId
    std::reverse(chainToApply.begin(), chainToApply.end());

    // Build dep map starting from base
    std::unordered_map<DepKey, Dep, DepKey::Hash> depMap;
    for (auto & dep : baseDeps)
        depMap.insert_or_assign(DepKey(dep), dep);

    // Apply deltas from root toward traceId
    for (auto id : chainToApply) {
        auto delta = loadTraceDelta(id);
        for (auto & dep : delta)
            depMap.insert_or_assign(DepKey(dep), dep);
    }

    // Flatten to vector
    std::vector<Dep> result;
    result.reserve(depMap.size());
    for (auto & [_, dep] : depMap)
        result.push_back(dep);

    traceCache[traceId] = result;
    nrLoadTraceTimeUs += elapsedUs(loadStart);
    return result;
}

std::vector<Dep> TraceStore::computeTraceDelta(
    const std::vector<Dep> & fullDeps,
    const std::vector<Dep> & baseDeps)
{
    // Build lookup map for base deps: key -> expectedHash
    std::unordered_map<DepKey, DepHashValue, DepKey::Hash> baseMap;
    for (auto & dep : baseDeps)
        baseMap.insert_or_assign(DepKey(dep), dep.expectedHash);

    std::vector<Dep> delta;
    for (auto & dep : fullDeps) {
        auto it = baseMap.find(DepKey(dep));
        if (it != baseMap.end() && it->second == dep.expectedHash)
            continue; // Same key and same hash → inherited from base
        // New dep or different hash → include in delta
        delta.push_back(dep);
    }
    return delta;
}

TraceId TraceStore::getOrCreateTrace(
    const std::vector<Dep> & fullDeps,
    const std::vector<InternedDep> & deltaDeps,
    const Hash & traceHash,
    const Hash & structHash,
    std::optional<TraceId> baseTraceId)
{
    // Serialize delta deps into BLOB
    auto blob = serializeDeps(deltaDeps);

    // Batch DB operations under one lock
    auto st(_state->lock());

    // Check if trace already exists
    {
        auto use(st->lookupTraceByFullHash.use());
        bindRawHash(use, traceHash);
        if (use.next())
            return use.getInt(0);
    }

    // Insert new trace with BLOB
    {
        auto use(st->insertTrace.use());
        use(baseTraceId ? *baseTraceId : (int64_t)0, baseTraceId.has_value());
        bindRawHash(use, traceHash);
        bindRawHash(use, structHash);
        bindBlobVec(use, blob);
        use.exec();
    }

    // Get the trace ID
    TraceId newTraceId;
    {
        auto use(st->lookupTraceByFullHash.use());
        bindRawHash(use, traceHash);
        if (!use.next())
            throw Error("failed to retrieve trace after insert");
        newTraceId = use.getInt(0);
    }

    // Track as dirty for post-write optimization
    dirtyTraceIds.insert(newTraceId);

    return newTraceId;
}

// ── Trace verification (BSàlC VT check) ─────────────────────────────

bool TraceStore::verifyTrace(
    TraceId traceId,
    const std::unordered_map<std::string, SourcePath> & inputAccessors,
    EvalState & state)
{
    if (verifiedTraceIds.count(traceId))
        return true;

    auto vtStart = timerStart();

    // Load the FULL trace (base chain already resolved)
    auto fullDeps = loadFullTrace(traceId);

    // Batch verification: compute ALL current hashes, cache them,
    // then compare. This ensures all hashes are cached for recovery.
    bool allValid = true;
    for (auto & dep : fullDeps) {
        nrDepsChecked++;

        if (dep.type == DepType::CurrentTime || dep.type == DepType::Exec) {
            allValid = false;
            continue; // still compute other hashes for caching
        }

        DepKey dk(dep);
        auto cacheIt = currentDepHashes.find(dk);
        std::optional<DepHashValue> current;

        if (cacheIt != currentDepHashes.end()) {
            current = cacheIt->second;
        } else {
            current = computeCurrentHash(state, dep, inputAccessors);
            currentDepHashes[dk] = current;
        }

        if (!current || *current != dep.expectedHash) {
            nrVerificationsFailed++;
            allValid = false;
            // Don't short-circuit: continue computing hashes for caching
        }
    }

    if (allValid) {
        verifiedTraceIds.insert(traceId);
    }
    nrVerifyTraceTimeUs += elapsedUs(vtStart);
    return allValid;
}

// ── DB lookups ───────────────────────────────────────────────────────

std::optional<TraceStore::TraceRow> TraceStore::lookupTraceRow(std::string_view attrPath)
{
    // Resolve attr_path_id (lookup only, no insert)
    auto pathKey = std::string(attrPath);
    auto cacheIt = internedAttrPaths.find(pathKey);

    auto st(_state->lock());

    AttrPathId attrPathId;
    if (cacheIt != internedAttrPaths.end()) {
        attrPathId = cacheIt->second;
    } else {
        auto use(st->lookupAttrPathId.use()
            (reinterpret_cast<const unsigned char *>(attrPath.data()), attrPath.size()));
        if (!use.next())
            return std::nullopt;
        attrPathId = use.getInt(0);
        internedAttrPaths[pathKey] = attrPathId;
    }

    auto use(st->lookupAttr.use()(contextHash)(attrPathId));
    if (!use.next())
        return std::nullopt;

    TraceRow row;
    row.traceId = use.getInt(0);
    row.resultId = use.getInt(1);
    row.type = static_cast<ResultKind>(use.getInt(2));
    row.value = use.isNull(3) ? "" : use.getStr(3);
    row.context = use.isNull(4) ? "" : use.getStr(4);
    return row;
}

bool TraceStore::attrExists(std::string_view attrPath)
{
    return lookupTraceRow(attrPath).has_value();
}

// ── Record path (BSàlC constructive trace recording) ─────────────────

TraceStore::RecordResult TraceStore::record(
    std::string_view attrPath,
    const CachedResult & value,
    const std::vector<Dep> & allDeps,
    std::optional<TraceId> parentTraceId,
    bool isRoot)
{
    auto recordStart = timerStart();
    nrRecords++;

    // 1. Filter deps: remove ParentContext
    std::vector<Dep> storedDeps;
    for (auto & dep : allDeps) {
        if (dep.type == DepType::ParentContext) continue;
        storedDeps.push_back(dep);
    }

    // 2. Sort+dedup own deps
    auto sortedDeps = sortAndDedupDeps(storedDeps);

    // 3. Compute full trace (own + inherited from parent chain)
    std::vector<Dep> fullDeps;
    if (parentTraceId) {
        auto parentFullDeps = loadFullTrace(*parentTraceId);
        std::unordered_map<DepKey, Dep, DepKey::Hash> depMap;
        for (auto & dep : parentFullDeps)
            depMap.insert_or_assign(DepKey(dep), dep);
        for (auto & dep : sortedDeps)
            depMap.insert_or_assign(DepKey(dep), dep);
        fullDeps.reserve(depMap.size());
        for (auto & [_, dep] : depMap)
            fullDeps.push_back(dep);
        fullDeps = sortAndDedupDeps(fullDeps);
    } else {
        fullDeps = sortedDeps;
    }

    // 4. Compute trace_hash from full trace (includes parent context)
    Hash traceHash(HashAlgorithm::BLAKE3);
    if (parentTraceId) {
        auto parentFullHash = getTraceFullHash(*parentTraceId);
        traceHash = computeTraceHashWithParentFromSorted(fullDeps, parentFullHash);
    } else {
        traceHash = computeTraceHashFromSorted(fullDeps);
    }

    // 5. Compute struct_hash from FULL deps (for delta encoding + structural variant recovery)
    auto structHash = computeTraceStructHashFromSorted(fullDeps);

    // 6. Find struct-hash-based base for delta encoding
    //    base_trace_id is for storage compression only — decoupled from parentTraceId
    std::optional<TraceId> baseTraceId;
    {
        auto st(_state->lock());
        auto use(st->lookupTraceByStructHash.use());
        bindRawHash(use, structHash);
        if (use.next())
            baseTraceId = use.getInt(0);
    }

    // 7. Compute delta from struct-hash base (not parent)
    std::vector<Dep> deltaDeps;
    if (baseTraceId) {
        auto baseDeps = loadFullTrace(*baseTraceId);
        deltaDeps = computeTraceDelta(fullDeps, baseDeps);
    } else {
        deltaDeps = fullDeps;
    }

    // 8. Intern delta dep strings and serialize
    auto internedDelta = internDeps(deltaDeps);

    // 9. Encode CachedResult and intern result
    auto [type, val, ctx] = encodeCachedResult(value);
    auto resultHash = computeResultHash(type, val, ctx);
    ResultId resultId = doInternResult(type, val, ctx, resultHash);

    // 10. Intern attr path
    AttrPathId attrPathId = doInternAttrPath(attrPath);

    // 11. Get or create trace (with BLOB)
    TraceId traceId = getOrCreateTrace(fullDeps, internedDelta, traceHash, structHash, baseTraceId);

    // 12. Upsert Attrs + insert History
    {
        auto st(_state->lock());
        st->upsertAttr.use()
            (contextHash)(attrPathId)(traceId)(resultId).exec();
        st->insertHistory.use()
            (contextHash)(attrPathId)(traceId)(resultId).exec();
    }

    // 13. Session caches
    bool hasVolatile = std::any_of(allDeps.begin(), allDeps.end(),
        [](auto & d) { return d.type == DepType::CurrentTime || d.type == DepType::Exec; });
    if (!hasVolatile)
        verifiedTraceIds.insert(traceId);

    traceHashCache.insert_or_assign(traceId, traceHash);
    traceStructHashCache.insert_or_assign(traceId, structHash);
    traceCache[traceId] = fullDeps;

    nrRecordTimeUs += elapsedUs(recordStart);
    return RecordResult{traceId};
}

// ── Verify path (BSàlC verifying trace) ──────────────────────────────

std::optional<TraceStore::VerifyResult> TraceStore::verify(
    std::string_view attrPath,
    const std::unordered_map<std::string, SourcePath> & inputAccessors,
    EvalState & state,
    std::optional<TraceId> parentTraceIdHint)
{
    auto verifyStart = timerStart();

    // 1. Lookup attribute
    auto row = lookupTraceRow(attrPath);
    if (!row) {
        nrVerifyTimeUs += elapsedUs(verifyStart);
        return std::nullopt;
    }

    nrTraceVerifications++;

    // 2. Verify trace (batch mode: computes all hashes, caches them)
    if (verifyTrace(row->traceId, inputAccessors, state)) {
        nrVerificationsPassed++;
        nrVerifyTimeUs += elapsedUs(verifyStart);
        return VerifyResult{decodeCachedResult(*row), row->traceId};
    }

    // 3. Verification failed → constructive recovery (uses currentDepHashes)
    debug("verify: trace validation failed for '%s', attempting constructive recovery", std::string(attrPath));
    auto result = recovery(row->traceId, attrPath, inputAccessors, state, parentTraceIdHint);
    nrVerifyTimeUs += elapsedUs(verifyStart);
    return result;
}

// ── Recovery (BSàlC constructive trace recovery) ─────────────────────
//    Two-phase: direct hash recovery + structural variant recovery

std::optional<TraceStore::VerifyResult> TraceStore::recovery(
    TraceId oldTraceId,
    std::string_view attrPath,
    const std::unordered_map<std::string, SourcePath> & inputAccessors,
    EvalState & state,
    std::optional<TraceId> parentTraceIdHint)
{
    auto recoveryStart = timerStart();
    nrRecoveryAttempts++;

    // Load old trace's full deps
    auto oldDeps = loadFullTrace(oldTraceId);

    // Check for volatile deps → immediate abort
    for (auto & dep : oldDeps) {
        if (dep.type == DepType::CurrentTime || dep.type == DepType::Exec) {
            debug("recovery: aborting for '%s' -- contains volatile dep", attrPath);
            nrRecoveryFailures++;
            nrRecoveryTimeUs += elapsedUs(recoveryStart);
            return std::nullopt;
        }
    }

    // Recompute current hashes for old dep keys, using currentDepHashes
    std::vector<Dep> currentDeps;
    bool allComputable = true;
    for (auto & dep : oldDeps) {
        DepKey dk(dep);
        auto cacheIt = currentDepHashes.find(dk);
        std::optional<DepHashValue> current;

        if (cacheIt != currentDepHashes.end()) {
            current = cacheIt->second;
        } else {
            current = computeCurrentHash(state, dep, inputAccessors);
            currentDepHashes[dk] = current;
        }

        if (!current) {
            allComputable = false;
            break;
        }
        currentDeps.push_back({dep.source, dep.key, *current, dep.type});
    }

    debug("recovery: recomputed %d/%d dep hashes for '%s'",
          currentDeps.size(), oldDeps.size(), attrPath);

    // Resolve attr_path_id for DB lookups
    auto pathKey = std::string(attrPath);
    std::optional<AttrPathId> attrPathId;
    {
        auto cacheIt = internedAttrPaths.find(pathKey);
        if (cacheIt != internedAttrPaths.end()) {
            attrPathId = cacheIt->second;
        } else {
            auto st(_state->lock());
            auto use(st->lookupAttrPathId.use()
                (reinterpret_cast<const unsigned char *>(attrPath.data()), attrPath.size()));
            if (use.next()) {
                attrPathId = use.getInt(0);
                internedAttrPaths[pathKey] = *attrPathId;
            }
        }
    }
    if (!attrPathId) {
        debug("recovery: attr path not interned for '%s'", attrPath);
        nrRecoveryFailures++;
        nrRecoveryTimeUs += elapsedUs(recoveryStart);
        return std::nullopt;
    }

    std::unordered_set<TraceId> triedTraceIds;

    // Helper: try a candidate trace ID for recovery
    auto tryCandidate = [&](TraceId candidateTraceId) -> std::optional<VerifyResult> {
        if (triedTraceIds.count(candidateTraceId))
            return std::nullopt;
        triedTraceIds.insert(candidateTraceId);

        // Look up in History
        ResultId resultId;
        {
            auto st(_state->lock());
            auto use(st->lookupHistoryByTrace.use()
                (contextHash)(*attrPathId)(candidateTraceId));
            if (!use.next())
                return std::nullopt;
            resultId = use.getInt(0);
        }

        // Verify the candidate trace
        if (!verifyTrace(candidateTraceId, inputAccessors, state))
            return std::nullopt;

        // Get result
        TraceRow recRow;
        {
            auto st(_state->lock());
            auto use(st->getResult.use()(resultId));
            if (!use.next())
                return std::nullopt;
            recRow.traceId = candidateTraceId;
            recRow.resultId = resultId;
            recRow.type = static_cast<ResultKind>(use.getInt(0));
            recRow.value = use.isNull(1) ? "" : use.getStr(1);
            recRow.context = use.isNull(2) ? "" : use.getStr(2);
        }

        // Update Attrs entry to point to recovered trace + result
        {
            auto st(_state->lock());
            st->upsertAttr.use()
                (contextHash)(*attrPathId)(candidateTraceId)(resultId).exec();
        }

        verifiedTraceIds.insert(candidateTraceId);
        return VerifyResult{decodeCachedResult(recRow), candidateTraceId};
    };

    // === Direct hash recovery (BSàlC CT, with Salsa-style parent Merkle chaining) ===
    // Recompute trace_hash from current dep hashes. When parentTraceIdHint is
    // available, the parent's trace_hash is mixed in (Merkle chaining),
    // disambiguating child traces across different parent versions. O(1) lookup.
    if (allComputable) {
        auto directHashStart = timerStart();
        auto sortedCurrentDeps = sortAndDedupDeps(currentDeps);
        Hash newFullHash(HashAlgorithm::BLAKE3);
        if (parentTraceIdHint) {
            auto parentFullHash = getTraceFullHash(*parentTraceIdHint);
            newFullHash = computeTraceHashWithParentFromSorted(sortedCurrentDeps, parentFullHash);
        } else {
            newFullHash = computeTraceHashFromSorted(sortedCurrentDeps);
        }

        // Look up Traces by trace_hash
        std::optional<TraceId> newTraceId;
        {
            auto st(_state->lock());
            auto use(st->lookupTraceByFullHash.use());
            bindRawHash(use, newFullHash);
            if (use.next())
                newTraceId = use.getInt(0);
        }

        if (newTraceId) {
            if (auto r = tryCandidate(*newTraceId)) {
                debug("recovery: direct hash recovery succeeded for '%s'", attrPath);
                nrRecoveryDirectHashHits++;
                nrRecoveryDirectHashTimeUs += elapsedUs(directHashStart);
                nrRecoveryTimeUs += elapsedUs(recoveryStart);
                return r;
            }
        }
        nrRecoveryDirectHashTimeUs += elapsedUs(directHashStart);
    }

    // === Structural variant recovery (novel extension beyond BSàlC) ===
    // Handles dynamic dep instability (Shake-style): the same attribute can have
    // different dep structures across evaluations. Scans TraceHistory for entries
    // with the same attr, groups by struct_hash (dep types + sources + keys,
    // ignoring hash values), recomputes current hashes per group, retries direct
    // hash lookup. O(V) where V = number of distinct dep structures.
    auto structVariantStart = timerStart();
    struct HistoryEntry {
        Hash structHash;
        TraceId traceId;
        ResultId resultId;
    };
    std::vector<HistoryEntry> historyEntries;
    {
        auto st(_state->lock());
        auto use(st->scanHistoryForAttr.use()(contextHash)(*attrPathId));
        while (use.next()) {
            auto [blobData, blobSize] = use.getBlob(0);
            historyEntries.push_back(HistoryEntry{
                readRawHash(blobData, blobSize),
                use.getInt(1),
                use.getInt(2)
            });
        }
    }

    debug("recovery: structural variant scan for '%s' -- scanning %d history entries",
          attrPath, historyEntries.size());

    // Group by struct_hash, pick one representative per group
    std::unordered_map<Hash, TraceId> structGroups; // struct_hash -> representative trace_id
    for (auto & e : historyEntries) {
        if (triedTraceIds.count(e.traceId))
            continue;
        structGroups.emplace(e.structHash, e.traceId);
    }

    for (auto & [structHash, repTraceId] : structGroups) {
        if (triedTraceIds.count(repTraceId))
            continue;

        // Load full deps for this representative
        auto repDeps = loadFullTrace(repTraceId);

        // Recompute current hashes for this trace's keys, using cache
        std::vector<Dep> repCurrentDeps;
        bool repComputable = true;
        for (auto & dep : repDeps) {
            if (dep.type == DepType::CurrentTime || dep.type == DepType::Exec) {
                repComputable = false;
                break;
            }

            DepKey dk(dep);
            auto cacheIt = currentDepHashes.find(dk);
            std::optional<DepHashValue> current;

            if (cacheIt != currentDepHashes.end()) {
                current = cacheIt->second;
            } else {
                current = computeCurrentHash(state, dep, inputAccessors);
                currentDepHashes[dk] = current;
            }

            if (!current) {
                repComputable = false;
                break;
            }
            repCurrentDeps.push_back({dep.source, dep.key, *current, dep.type});
        }
        if (!repComputable)
            continue;

        // Compute candidate trace_hash
        auto sortedRepDeps = sortAndDedupDeps(repCurrentDeps);
        Hash candidateFullHash(HashAlgorithm::BLAKE3);
        if (parentTraceIdHint) {
            auto parentFullHash = getTraceFullHash(*parentTraceIdHint);
            candidateFullHash = computeTraceHashWithParentFromSorted(sortedRepDeps, parentFullHash);
        } else {
            candidateFullHash = computeTraceHashFromSorted(sortedRepDeps);
        }

        // Look up Traces by candidate trace_hash
        std::optional<TraceId> candidateTraceId;
        {
            auto st(_state->lock());
            auto use(st->lookupTraceByFullHash.use());
            bindRawHash(use, candidateFullHash);
            if (use.next())
                candidateTraceId = use.getInt(0);
        }

        if (candidateTraceId) {
            if (auto r = tryCandidate(*candidateTraceId)) {
                debug("recovery: structural variant recovery succeeded for '%s'", attrPath);
                nrRecoveryStructVariantHits++;
                nrRecoveryStructVariantTimeUs += elapsedUs(structVariantStart);
                nrRecoveryTimeUs += elapsedUs(recoveryStart);
                return r;
            }
        }
    }
    nrRecoveryStructVariantTimeUs += elapsedUs(structVariantStart);

    debug("recovery: all strategies failed for '%s'", attrPath);
    nrRecoveryFailures++;
    nrRecoveryTimeUs += elapsedUs(recoveryStart);
    return std::nullopt;
}

// ── Post-write optimization ──────────────────────────────────────────

void TraceStore::optimizeTraces()
{
    if (dirtyTraceIds.empty())
        return;

    // Group dirty traces by struct_hash.
    // All lock-acquiring work (DB reads, string interning) is done OUTSIDE
    // lock scopes to avoid deadlock. The lock is only held for UPDATE writes.
    std::unordered_map<Hash, std::vector<TraceId>> structGroups;
    for (auto traceId : dirtyTraceIds) {
        // Get struct_hash from session cache or DB
        Hash structHash(HashAlgorithm::BLAKE3);
        auto cacheIt = traceStructHashCache.find(traceId);
        if (cacheIt != traceStructHashCache.end()) {
            structHash = cacheIt->second;
        } else {
            auto st(_state->lock());
            auto use(st->getTraceInfo.use()(traceId));
            if (!use.next()) continue;
            auto [data, size] = use.getBlob(2); // struct_hash is column 2
            structHash = readRawHash(data, size);
        }
        structGroups[structHash].push_back(traceId);
    }

    for (auto & [structHash, group] : structGroups) {
        if (group.size() <= 1) continue;

        // Pick the trace with the most full deps as canonical base
        TraceId baseTraceId = group[0];
        size_t maxSize = 0;
        for (auto traceId : group) {
            auto it = traceCache.find(traceId);
            size_t sz = (it != traceCache.end()) ? it->second.size() : 0;
            if (sz > maxSize) {
                maxSize = sz;
                baseTraceId = traceId;
            }
        }

        // Load base deps and pre-compute all blobs OUTSIDE lock scope.
        // This avoids deadlock: loadFullTrace, internDeps, and resolveDeps
        // all acquire _state->lock() internally.
        auto baseDeps = loadFullTrace(baseTraceId);

        // Pre-compute the base blob
        auto internedBase = internDeps(baseDeps);
        auto baseBlob = serializeDeps(internedBase);

        // Pre-compute all member blobs
        struct MemberUpdate {
            TraceId traceId;
            bool isBase;
            std::vector<uint8_t> blob;
        };
        std::vector<MemberUpdate> updates;
        updates.reserve(group.size());

        for (auto traceId : group) {
            if (traceId == baseTraceId) {
                updates.push_back({traceId, true, baseBlob});
            } else {
                auto myDeps = loadFullTrace(traceId);
                auto delta = computeTraceDelta(myDeps, baseDeps);
                auto internedDelta = internDeps(delta);
                auto blob = serializeDeps(internedDelta);
                updates.push_back({traceId, false, std::move(blob)});
            }
        }

        // Now write all updates under a single lock acquisition
        auto st(_state->lock());
        for (auto & upd : updates) {
            auto use(st->updateTraceBlob.use());
            if (upd.isBase) {
                use((int64_t)0, false); // NULL base_trace_id
            } else {
                use(baseTraceId);
            }
            bindBlobVec(use, upd.blob);
            use(upd.traceId);
            use.exec();
        }
    }

    dirtyTraceIds.clear();
}

} // namespace nix::eval_trace
