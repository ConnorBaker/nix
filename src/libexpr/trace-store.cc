#include "nix/expr/trace-store.hh"
#include "nix/expr/dependency-tracker.hh"
#include "nix/expr/trace-cache.hh"
#include "nix/expr/trace-hash.hh"
#include "nix/expr/eval.hh"
#include "nix/store/globals.hh"

#include "expr-config-private.hh"
#include "nix/util/users.hh"
#include "nix/util/util.hh"
#include "nix/util/hash.hh"
#include "nix/util/compression.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/source-accessor.hh"
#include "nix/fetchers/fetchers.hh"

#include <nlohmann/json.hpp>
#include <toml.hpp>

#include <algorithm>
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

// ── Structured content navigation helpers ────────────────────────────

// Thread-local DOM caches: avoid re-parsing the same file when multiple
// StructuredContent deps reference it. Cleared at verifyTrace entry.
static thread_local std::unordered_map<std::string, nlohmann::json> jsonDomCache;
static thread_local std::unordered_map<std::string, toml::value> tomlDomCache;
static thread_local std::unordered_map<std::string, SourceAccessor::DirEntries> dirListingCache;

static void clearDomCaches()
{
    jsonDomCache.clear();
    tomlDomCache.clear();
    dirListingCache.clear();
}

/**
 * Parse a data path like "nodes.nixpkgs.locked.rev" or "items.[0].name"
 * into segments. '.' separates object keys, '[N]' denotes array indices.
 * Array indices may appear at the start ("[0].name") or after a dot ("items.[0]").
 *
 * Known limitation: object keys containing '.' or '[' will be misinterpreted.
 * This is conservative — verification fails, causing re-evaluation, never stale results.
 */
/**
 * Parse a data path into segments. Supports three segment types:
 *   - Array index: [N]
 *   - Quoted key: "key" (with \" and \\ escape sequences, unescaped on parse)
 *   - Bare key: chars until '.', '[', or end
 * Segments are separated by '.'. Returns empty vector on malformed input.
 */
static std::vector<std::string> parseDataPath(const std::string & path)
{
    std::vector<std::string> segments;
    if (path.empty()) return segments;

    size_t pos = 0;
    while (pos < path.size()) {
        if (path[pos] == '[') {
            // Array index: find closing ']'
            auto end = path.find(']', pos);
            if (end == std::string::npos) return {}; // malformed
            segments.push_back(path.substr(pos, end - pos + 1));
            pos = end + 1;
            if (pos < path.size() && path[pos] == '.') pos++;
        } else if (path[pos] == '"') {
            // Quoted key: unescape backslash-escaped chars
            std::string segment;
            pos++; // skip opening quote
            while (pos < path.size() && path[pos] != '"') {
                if (path[pos] == '\\' && pos + 1 < path.size()) {
                    pos++; // skip backslash, take next char literally
                }
                segment += path[pos++];
            }
            if (pos >= path.size()) return {}; // malformed: no closing quote
            pos++; // skip closing quote
            if (pos < path.size() && path[pos] == '.') pos++;
            segments.push_back(std::move(segment));
        } else {
            // Bare key: until '.', '[', or end
            size_t start = pos;
            while (pos < path.size() && path[pos] != '.' && path[pos] != '[')
                pos++;
            segments.push_back(path.substr(start, pos - start));
            if (pos < path.size() && path[pos] == '.') pos++;
        }
    }
    return segments;
}

/**
 * Navigate a JSON DOM to a data path. Returns nullptr if path is invalid.
 */
static const nlohmann::json * navigateJson(const nlohmann::json & root, const std::string & dataPath)
{
    auto segments = parseDataPath(dataPath);
    const nlohmann::json * node = &root;
    for (auto & seg : segments) {
        if (seg.front() == '[') {
            // Array index
            if (!node->is_array()) return nullptr;
            auto idxStr = seg.substr(1, seg.size() - 2);
            size_t idx = std::stoull(idxStr);
            if (idx >= node->size()) return nullptr;
            node = &(*node)[idx];
        } else {
            // Object key
            if (!node->is_object()) return nullptr;
            auto it = node->find(seg);
            if (it == node->end()) return nullptr;
            node = &*it;
        }
    }
    return node;
}

/**
 * Navigate a TOML DOM to a data path. Returns nullptr if path is invalid.
 */
static const toml::value * navigateToml(const toml::value & root, const std::string & dataPath)
{
    auto segments = parseDataPath(dataPath);
    const toml::value * node = &root;
    for (auto & seg : segments) {
        if (seg.front() == '[') {
            if (!node->is_array()) return nullptr;
            auto idxStr = seg.substr(1, seg.size() - 2);
            size_t idx = std::stoull(idxStr);
            auto & arr = toml::get<std::vector<toml::value>>(*node);
            if (idx >= arr.size()) return nullptr;
            node = &arr[idx];
        } else {
            if (!node->is_table()) return nullptr;
            auto & table = toml::get<toml::table>(*node);
            auto it = table.find(seg);
            if (it == table.end()) return nullptr;
            node = &it->second;
        }
    }
    return node;
}

/**
 * Canonical string form of a TOML scalar value for hashing.
 * Must match TomlDataNode::canonicalValue() in fromTOML.cc.
 */
static std::string tomlCanonical(const toml::value & v)
{
    std::ostringstream ss;
    ss << v;
    return ss.str();
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
    case DepType::StructuredContent: {
        // Key format: "filepath\tf:datapath" (tab separator)
        auto sep = dep.key.find('\t');
        if (sep == std::string::npos || sep + 2 >= dep.key.size())
            return std::nullopt;
        char format = dep.key[sep + 1];
        if (dep.key[sep + 2] != ':')
            return std::nullopt;
        std::string filePath = dep.key.substr(0, sep);
        std::string dataPath = dep.key.substr(sep + 3);

        // Check for shape suffix (#len or #keys)
        std::string shapeSuffix;
        if (dataPath.size() >= 4 && dataPath.compare(dataPath.size() - 4, 4, "#len") == 0) {
            shapeSuffix = "len";
            dataPath.resize(dataPath.size() - 4);
        } else if (dataPath.size() >= 5 && dataPath.compare(dataPath.size() - 5, 5, "#keys") == 0) {
            shapeSuffix = "keys";
            dataPath.resize(dataPath.size() - 5);
        }

        // Construct a synthetic Content dep to resolve the file path
        Dep fileDep{dep.source, filePath, DepHashValue{Blake3Hash{}}, DepType::Content};
        auto path = resolveDepPath(fileDep, inputAccessors);
        if (!path) return std::nullopt;

        try {
            if (format == 'j') {
                // Use DOM cache to avoid re-parsing
                auto cacheKey = dep.source + '\t' + filePath;
                auto cacheIt = jsonDomCache.find(cacheKey);
                if (cacheIt == jsonDomCache.end()) {
                    auto contents = path->readFile();
                    cacheIt = jsonDomCache.emplace(cacheKey, nlohmann::json::parse(contents)).first;
                }
                auto * node = navigateJson(cacheIt->second, dataPath);
                if (!node) return std::nullopt;
                if (shapeSuffix == "len") {
                    if (!node->is_array()) return std::nullopt;
                    return DepHashValue(depHash(std::to_string(node->size())));
                } else if (shapeSuffix == "keys") {
                    if (!node->is_object()) return std::nullopt;
                    std::vector<std::string> keys;
                    for (auto & [k, _] : node->items())
                        keys.push_back(k);
                    std::sort(keys.begin(), keys.end());
                    std::string canonical;
                    for (size_t i = 0; i < keys.size(); i++) {
                        if (i > 0) canonical += '\0';
                        canonical += keys[i];
                    }
                    return DepHashValue(depHash(canonical));
                } else {
                    return DepHashValue(depHash(node->dump()));
                }
            } else if (format == 't') {
                auto cacheKey = dep.source + '\t' + filePath;
                auto cacheIt = tomlDomCache.find(cacheKey);
                if (cacheIt == tomlDomCache.end()) {
                    auto contents = path->readFile();
                    std::istringstream stream(std::move(contents));
                    cacheIt = tomlDomCache.emplace(cacheKey, toml::parse(
                        stream, "verifyTrace"
#if HAVE_TOML11_4
                        , toml::spec::v(1, 0, 0)
#endif
                    )).first;
                }
                auto * node = navigateToml(cacheIt->second, dataPath);
                if (!node) return std::nullopt;
                if (shapeSuffix == "len") {
                    if (!node->is_array()) return std::nullopt;
                    return DepHashValue(depHash(std::to_string(toml::get<std::vector<toml::value>>(*node).size())));
                } else if (shapeSuffix == "keys") {
                    if (!node->is_table()) return std::nullopt;
                    auto & table = toml::get<toml::table>(*node);
                    std::vector<std::string> keys;
                    for (auto & [k, _] : table)
                        keys.push_back(k);
                    std::sort(keys.begin(), keys.end());
                    std::string canonical;
                    for (size_t i = 0; i < keys.size(); i++) {
                        if (i > 0) canonical += '\0';
                        canonical += keys[i];
                    }
                    return DepHashValue(depHash(canonical));
                } else {
                    return DepHashValue(depHash(tomlCanonical(*node)));
                }
            } else if (format == 'd') {
                // Directory structural dep: re-read listing, look up entry
                auto cacheKey = dep.source + '\t' + filePath;
                auto cacheIt = dirListingCache.find(cacheKey);
                if (cacheIt == dirListingCache.end()) {
                    auto dirEntries = path->readDirectory();
                    cacheIt = dirListingCache.emplace(cacheKey, std::move(dirEntries)).first;
                }
                auto & entries = cacheIt->second;

                if (shapeSuffix == "len") {
                    return DepHashValue(depHash(std::to_string(entries.size())));
                } else if (shapeSuffix == "keys") {
                    // std::map is already sorted by key
                    std::string canonical;
                    bool first = true;
                    for (auto & [k, _] : entries) {
                        if (!first) canonical += '\0';
                        canonical += k;
                        first = false;
                    }
                    return DepHashValue(depHash(canonical));
                } else {
                    auto segments = parseDataPath(dataPath);
                    if (segments.size() != 1) return std::nullopt;
                    auto it = entries.find(segments[0]);
                    if (it == entries.end()) return std::nullopt;
                    return DepHashValue(depHash(dirEntryTypeString(it->second)));
                }
            }
            return std::nullopt;
        } catch (...) {
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

    // Compress with zstd level 1 (fast, good ratio for structured binary data)
    if (!blob.empty()) {
        auto compressed = nix::compress(
            CompressionAlgo::zstd,
            {reinterpret_cast<const char *>(blob.data()), blob.size()},
            false, 1);
        return {compressed.begin(), compressed.end()};
    }
    return blob;
}

std::vector<TraceStore::InternedDep> TraceStore::deserializeInternedDeps(
    const void * blob, size_t size)
{
    if (size == 0)
        return {};

    // Decompress zstd-compressed deps_blob
    auto decompressed = nix::decompress("zstd",
        {static_cast<const char *>(blob), size});

    std::vector<InternedDep> deps;
    const uint8_t * p = reinterpret_cast<const uint8_t *>(decompressed.data());
    const uint8_t * end = p + decompressed.size();

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

    CREATE TABLE IF NOT EXISTS DepsSets (
        id        INTEGER PRIMARY KEY,
        deps_hash BLOB NOT NULL UNIQUE,
        deps_blob BLOB NOT NULL
    ) STRICT;

    CREATE TABLE IF NOT EXISTS Traces (
        id           INTEGER PRIMARY KEY,
        trace_hash   BLOB NOT NULL UNIQUE,
        struct_hash  BLOB NOT NULL,
        deps_set_id  INTEGER NOT NULL REFERENCES DepsSets(id)
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

    auto dbPath = cacheDir / "eval-trace-v2.sqlite";

    st->db = SQLite(dbPath, {.useWAL = settings.useSQLiteWAL});
    st->db.exec("pragma page_size = 65536");            // 64KB pages for large BLOB I/O (MUST be before isCache)
    st->db.isCache();
    st->db.exec("pragma cache_size = -16000");           // 16MB page cache
    st->db.exec("pragma mmap_size = 268435456");         // 256MB mmap
    st->db.exec("pragma temp_store = memory");
    st->db.exec("pragma journal_size_limit = 2097152");  // 2MB WAL limit

    st->db.exec(schema);

    // Strings interning (UPSERT RETURNING: 1 statement instead of INSERT + SELECT)
    st->upsertString.create(st->db,
        "INSERT INTO Strings(value) VALUES (?) "
        "ON CONFLICT(value) DO UPDATE SET value = excluded.value "
        "RETURNING id");

    st->getAllStrings.create(st->db,
        "SELECT id, value FROM Strings");

    // AttrPaths interning (UPSERT RETURNING)
    st->upsertAttrPath.create(st->db,
        "INSERT INTO AttrPaths(path) VALUES (?) "
        "ON CONFLICT(path) DO UPDATE SET path = excluded.path "
        "RETURNING id");

    st->lookupAttrPathId.create(st->db,
        "SELECT id FROM AttrPaths WHERE path = ?");

    // Results dedup (UPSERT RETURNING)
    st->upsertResult.create(st->db,
        "INSERT INTO Results(type, value, context, hash) VALUES (?, ?, ?, ?) "
        "ON CONFLICT(hash) DO UPDATE SET type = excluded.type "
        "RETURNING id");

    st->getResult.create(st->db,
        "SELECT type, value, context FROM Results WHERE id = ?");

    // DepsSets (content-addressed dep storage, UPSERT RETURNING)
    st->upsertDepsSet.create(st->db,
        "INSERT INTO DepsSets(deps_hash, deps_blob) VALUES (?, ?) "
        "ON CONFLICT(deps_hash) DO UPDATE SET deps_hash = excluded.deps_hash "
        "RETURNING id");

    // Traces (references DepsSets via deps_set_id FK, UPSERT RETURNING)
    st->upsertTrace.create(st->db,
        "INSERT INTO Traces(trace_hash, struct_hash, deps_set_id) "
        "VALUES (?, ?, ?) "
        "ON CONFLICT(trace_hash) DO UPDATE SET trace_hash = excluded.trace_hash "
        "RETURNING id");

    st->lookupTraceByFullHash.create(st->db,
        "SELECT id FROM Traces WHERE trace_hash = ?");

    st->getTraceInfo.create(st->db,
        "SELECT t.trace_hash, t.struct_hash, d.deps_blob "
        "FROM Traces t JOIN DepsSets d ON t.deps_set_id = d.id WHERE t.id = ?");

    st->lookupTraceByStructHash.create(st->db,
        "SELECT id FROM Traces WHERE struct_hash = ? LIMIT 1");

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
        "SELECT t.struct_hash, h.trace_id, h.result_id "
        "FROM TraceHistory h JOIN Traces t ON h.trace_id = t.id "
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
        auto st(_state->lock());
        if (st->txn)
            st->txn->commit();
    } catch (...) {
        ignoreExceptionExceptInterrupt();
    }
    nrDbCloseTimeUs += elapsedUs(closeStart);
}

// ── Helpers ──────────────────────────────────────────────────────────

// Reverse of buildAttrPath: convert null-byte-separated storage encoding
// to dot-separated display format for debug messages.
static std::string displayAttrPath(std::string_view attrPath)
{
    if (attrPath.empty())
        return "«root»";
    std::string result;
    result.reserve(attrPath.size());
    for (char c : attrPath)
        result.push_back(c == '\0' ? '.' : c);
    return result;
}

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
    traceStructHashCache.clear();
    stringTable.clear();
    stringTableLoaded = false;
    currentDepHashes.clear();
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
    auto use(st->upsertString.use()(s));
    if (!use.next())
        throw Error("failed to intern string '%s'", s);
    StringId id = use.getInt(0);
    internedStrings[key] = id;
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
    auto use(st->upsertAttrPath.use()
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
    auto use(st->upsertResult.use());
    use(static_cast<int64_t>(type));
    use(value);
    use(context);
    bindRawHash(use, resultHash);
    if (!use.next())
        throw Error("failed to intern result");
    return use.getInt(0);
}

// ── Trace storage (BSàlC trace store) ───────────────────────────────


Hash TraceStore::getTraceStructHash(TraceId traceId)
{
    auto cacheIt = traceStructHashCache.find(traceId);
    if (cacheIt != traceStructHashCache.end())
        return cacheIt->second;

    auto st(_state->lock());
    auto use(st->getTraceInfo.use()(traceId));
    if (!use.next())
        throw Error("trace %d not found", traceId);
    auto [blobData, blobSize] = use.getBlob(1);
    auto h = readRawHash(blobData, blobSize);
    traceStructHashCache.insert_or_assign(traceId, h);
    return h;
}

std::vector<Dep> TraceStore::loadFullTrace(TraceId traceId)
{
    auto cacheIt = traceCache.find(traceId);
    if (cacheIt != traceCache.end())
        return cacheIt->second;

    auto loadStart = timerStart();
    nrLoadTraces++;

    // Single DB read via JOIN — no chain walk
    std::vector<uint8_t> blobCopy;
    {
        auto st(_state->lock());
        auto use(st->getTraceInfo.use()(traceId));
        if (!use.next())
            return {};
        // Column 2 = deps_blob (from DepsSets via JOIN)
        auto [data, size] = use.getBlob(2);
        if (!data || size == 0) {
            traceCache[traceId] = {};
            nrLoadTraceTimeUs += elapsedUs(loadStart);
            return {};
        }
        auto * p = static_cast<const uint8_t *>(data);
        blobCopy.assign(p, p + size);
    }

    auto interned = deserializeInternedDeps(blobCopy.data(), blobCopy.size());
    auto result = resolveDeps(interned);

    traceCache[traceId] = result;
    nrLoadTraceTimeUs += elapsedUs(loadStart);
    return result;
}

int64_t TraceStore::getOrCreateDepsSet(
    const std::vector<Dep> & fullDeps,
    const Hash & depsHash)
{
    auto interned = internDeps(fullDeps);
    auto blob = serializeDeps(interned);

    auto st(_state->lock());
    auto use(st->upsertDepsSet.use());
    bindRawHash(use, depsHash);
    bindBlobVec(use, blob);
    if (!use.next())
        throw Error("failed to get or create deps set");
    return use.getInt(0);
}

TraceId TraceStore::getOrCreateTrace(
    const Hash & traceHash,
    const Hash & structHash,
    int64_t depsSetId)
{
    auto st(_state->lock());
    auto use(st->upsertTrace.use());
    bindRawHash(use, traceHash);
    bindRawHash(use, structHash);
    use(depsSetId);
    if (!use.next())
        throw Error("failed to get or create trace");
    return use.getInt(0);
}

// ── Trace verification (BSàlC VT check) ─────────────────────────────

bool TraceStore::verifyTrace(
    TraceId traceId,
    const std::unordered_map<std::string, SourcePath> & inputAccessors,
    EvalState & state,
    bool earlyExit)
{
    if (verifiedTraceIds.count(traceId))
        return true;

    auto vtStart = timerStart();
    clearDomCaches();

    // Load the FULL trace (base chain already resolved)
    auto fullDeps = loadFullTrace(traceId);

    // Two-level verification: Content and Directory failures can be overridden
    // by passing StructuredContent deps that cover the same file/directory.
    // This enables fine-grained invalidation for fromJSON(readFile f) and
    // readDir patterns.
    //
    // Key-set safety: the override only activates when StructuredContent deps
    // exist in this trace. If code only iterates keys (mapAttrs, attrNames)
    // without forcing leaf values, no StructuredContent deps are recorded and
    // the coarse dep alone controls invalidation — any change triggers
    // re-evaluation. This is correct because the key set is part of the cached
    // result's structure at this trace level. StructuredContent deps only appear
    // in child traces (when specific leaf values are forced), where the cached
    // result is the leaf value and key-set changes are irrelevant.
    //
    // First pass: verify non-structural deps, defer StructuredContent deps,
    // track failed coarse (Content/Directory) deps by their file key (source + key).

    bool hasNonContentFailure = false;
    bool hasContentFailure = false;
    bool hasStructuralDeps = false;

    // Track failed coarse dep file/dir keys: "source\tkey"
    std::unordered_set<std::string> failedContentFiles;
    // Deferred structural deps
    std::vector<const Dep *> structuralDeps;

    for (auto & dep : fullDeps) {
        nrDepsChecked++;

        if (dep.type == DepType::CurrentTime || dep.type == DepType::Exec) {
            hasNonContentFailure = true;
            if (earlyExit) break;
            continue;
        }

        if (dep.type == DepType::StructuredContent) {
            hasStructuralDeps = true;
            structuralDeps.push_back(&dep);
            continue;
        }

        // ParentContext: verify the parent's current trace hash matches
        if (dep.type == DepType::ParentContext) {
            auto parentTraceHash = getCurrentTraceHash(dep.key);
            if (parentTraceHash) {
                auto * expected = std::get_if<Blake3Hash>(&dep.expectedHash);
                if (expected && std::memcmp(expected->bytes.data(), parentTraceHash->hash, 32) == 0) {
                    continue; // parent trace unchanged → dep passes
                }
            }
            // Parent trace changed or missing → non-content failure
            nrVerificationsFailed++;
            hasNonContentFailure = true;
            if (earlyExit) break;
            continue;
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
            if (dep.type == DepType::Content || dep.type == DepType::Directory) {
                hasContentFailure = true;
                failedContentFiles.insert(dep.source + '\t' + dep.key);
            } else {
                hasNonContentFailure = true;
                if (earlyExit) break;
            }
        }
    }

    bool allValid;

    if (hasNonContentFailure) {
        // Non-coarse, non-structural failure → trace invalid (no override possible)
        allValid = false;
    } else if (!hasContentFailure) {
        // No failures at all (structural deps haven't been checked yet, but
        // they can only strengthen the result — if coarse deps passed, structural
        // deps would also pass since the file/dir hasn't changed)
        allValid = true;
    } else if (hasContentFailure && hasStructuralDeps) {
        // Coarse failure(s) (Content/Directory) exist AND structural deps are present.
        // Check if all structural deps covering failed files/dirs still pass.
        allValid = true;

        // Build set of files covered by structural deps
        std::unordered_set<std::string> structuralCoveredFiles;
        for (auto * dep : structuralDeps) {
            auto sep = dep->key.find('\t');
            if (sep != std::string::npos)
                structuralCoveredFiles.insert(dep->source + '\t' + dep->key.substr(0, sep));
        }

        // Check that all failed coarse deps (Content/Directory) are covered by structural deps
        for (auto & failedFile : failedContentFiles) {
            if (structuralCoveredFiles.find(failedFile) == structuralCoveredFiles.end()) {
                allValid = false;
                break;
            }
        }

        // If covered, verify all structural deps pass
        if (allValid) {
            for (auto * dep : structuralDeps) {
                nrDepsChecked++;
                DepKey dk(*dep);
                auto cacheIt = currentDepHashes.find(dk);
                std::optional<DepHashValue> current;

                if (cacheIt != currentDepHashes.end()) {
                    current = cacheIt->second;
                } else {
                    current = computeCurrentHash(state, *dep, inputAccessors);
                    currentDepHashes[dk] = current;
                }

                if (!current || *current != dep->expectedHash) {
                    nrVerificationsFailed++;
                    allValid = false;
                    if (earlyExit) break;
                }
            }
        }
    } else {
        // Coarse failure with no structural deps → invalid (backward compat)
        allValid = false;
    }

    if (allValid) {
        verifiedTraceIds.insert(traceId);
    }
    nrVerifyTraceTimeUs += elapsedUs(vtStart);
    clearDomCaches();
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

std::optional<Hash> TraceStore::getCurrentTraceHash(std::string_view attrPath)
{
    // ParentContext dep keys use \t as separator (Strings table is TEXT,
    // truncates at \0), but lookupTraceRow needs \0-separated AttrPaths.
    std::string path(attrPath);
    std::replace(path.begin(), path.end(), '\t', '\0');
    auto row = lookupTraceRow(path);
    if (!row) return std::nullopt;

    // Return trace_hash (captures dep structure + hashes), not result hash.
    // Result hash for attrsets only captures attribute names, not values —
    // it wouldn't detect changes to attribute values within an attrset.
    auto st(_state->lock());
    auto use(st->getTraceInfo.use()(row->traceId));
    if (!use.next()) return std::nullopt;
    auto [hashData, hashSize] = use.getBlob(0);  // column 0 = trace_hash
    return readRawHash(hashData, hashSize);
}

// ── Record path (BSàlC constructive trace recording) ─────────────────

TraceStore::RecordResult TraceStore::record(
    std::string_view attrPath,
    const CachedResult & value,
    const std::vector<Dep> & allDeps,
    bool isRoot)
{
    auto recordStart = timerStart();
    nrRecords++;

    // 1. Sort+dedup deps (ParentContext deps are now stored, not filtered)
    auto sortedDeps = sortAndDedupDeps(allDeps);

    // 3. Compute deps_hash = trace_hash (own deps only, no Merkle chaining)
    auto depsHash = computeTraceHashFromSorted(sortedDeps);

    // 4. Compute struct_hash (for structural variant recovery)
    auto structHash = computeTraceStructHashFromSorted(sortedDeps);

    // 5. Get or create deps set (content-addressed by deps_hash)
    auto depsSetId = getOrCreateDepsSet(sortedDeps, depsHash);

    // 6. Encode CachedResult and intern result
    auto [type, val, ctx] = encodeCachedResult(value);
    auto resultHash = computeResultHash(type, val, ctx);
    ResultId resultId = doInternResult(type, val, ctx, resultHash);

    // 7. Intern attr path
    AttrPathId attrPathId = doInternAttrPath(attrPath);

    // 8. Get or create trace
    TraceId traceId = getOrCreateTrace(depsHash, structHash, depsSetId);

    // 9. Upsert Attrs + insert History
    {
        auto st(_state->lock());
        st->upsertAttr.use()
            (contextHash)(attrPathId)(traceId)(resultId).exec();
        st->insertHistory.use()
            (contextHash)(attrPathId)(traceId)(resultId).exec();
    }

    // 10. Session caches
    bool hasVolatile = std::any_of(allDeps.begin(), allDeps.end(),
        [](auto & d) { return d.type == DepType::CurrentTime || d.type == DepType::Exec; });
    if (!hasVolatile)
        verifiedTraceIds.insert(traceId);

    traceStructHashCache.insert_or_assign(traceId, structHash);
    traceCache[traceId] = sortedDeps;

    nrRecordTimeUs += elapsedUs(recordStart);
    return RecordResult{traceId};
}

// ── Verify path (BSàlC verifying trace) ──────────────────────────────

std::optional<TraceStore::VerifyResult> TraceStore::verify(
    std::string_view attrPath,
    const std::unordered_map<std::string, SourcePath> & inputAccessors,
    EvalState & state)
{
    auto verifyStart = timerStart();

    // 1. Lookup attribute
    auto row = lookupTraceRow(attrPath);
    if (!row) {
        nrVerifyTimeUs += elapsedUs(verifyStart);
        return std::nullopt;
    }

    nrTraceVerifications++;

    // 2. Verify trace (early-exit mode: stop on first mismatch for speed;
    //    recovery() will compute remaining hashes lazily if needed)
    if (verifyTrace(row->traceId, inputAccessors, state, /*earlyExit=*/true)) {
        nrVerificationsPassed++;
        nrVerifyTimeUs += elapsedUs(verifyStart);
        return VerifyResult{decodeCachedResult(*row), row->traceId};
    }

    // 3. Verification failed → constructive recovery (uses currentDepHashes)
    debug("verify: trace validation failed for '%s', attempting constructive recovery", displayAttrPath(attrPath));
    auto result = recovery(row->traceId, attrPath, inputAccessors, state);
    nrVerifyTimeUs += elapsedUs(verifyStart);
    return result;
}

// ── Recovery (BSàlC constructive trace recovery) ─────────────────────
//    Two-phase: direct hash recovery + structural variant recovery

std::optional<TraceStore::VerifyResult> TraceStore::recovery(
    TraceId oldTraceId,
    std::string_view attrPath,
    const std::unordered_map<std::string, SourcePath> & inputAccessors,
    EvalState & state)
{
    auto recoveryStart = timerStart();
    nrRecoveryAttempts++;

    // Load old trace's full deps
    auto oldDeps = loadFullTrace(oldTraceId);

    // Check for volatile deps → immediate abort
    for (auto & dep : oldDeps) {
        if (dep.type == DepType::CurrentTime || dep.type == DepType::Exec) {
            debug("recovery: aborting for '%s' -- contains volatile dep", displayAttrPath(attrPath));
            nrRecoveryFailures++;
            nrRecoveryTimeUs += elapsedUs(recoveryStart);
            return std::nullopt;
        }
    }

    // Recompute current hashes for old dep keys, using currentDepHashes
    std::vector<Dep> currentDeps;
    bool allComputable = true;
    for (auto & dep : oldDeps) {
        // ParentContext: compute current parent trace hash directly
        if (dep.type == DepType::ParentContext) {
            auto parentTraceHash = getCurrentTraceHash(dep.key);
            if (!parentTraceHash) { allComputable = false; break; }
            Blake3Hash b3;
            std::memcpy(b3.bytes.data(), parentTraceHash->hash, 32);
            currentDeps.push_back({dep.source, dep.key, DepHashValue(b3), dep.type});
            continue;
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
            allComputable = false;
            break;
        }
        currentDeps.push_back({dep.source, dep.key, *current, dep.type});
    }

    debug("recovery: recomputed %d/%d dep hashes for '%s'",
          currentDeps.size(), oldDeps.size(), displayAttrPath(attrPath));

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
        debug("recovery: attr path not interned for '%s'", displayAttrPath(attrPath));
        nrRecoveryFailures++;
        nrRecoveryTimeUs += elapsedUs(recoveryStart);
        return std::nullopt;
    }

    std::unordered_set<TraceId> triedTraceIds;

    // Accept a candidate trace found by trace_hash lookup. The hash match
    // proves all deps match current state (probability of false match: 2^-256),
    // so no dep-by-dep verifyTrace call is needed.
    auto acceptRecoveredTrace = [&](TraceId candidateTraceId) -> std::optional<VerifyResult> {
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

        // Update CurrentTraces to point to recovered trace + result
        {
            auto st(_state->lock());
            st->upsertAttr.use()
                (contextHash)(*attrPathId)(candidateTraceId)(resultId).exec();
        }

        // Hash match guarantees all deps match current state
        verifiedTraceIds.insert(candidateTraceId);
        return VerifyResult{decodeCachedResult(recRow), candidateTraceId};
    };

    // === Direct hash recovery (BSàlC CT) ===
    // Recompute trace_hash from current dep hashes. O(1) lookup.
    if (allComputable) {
        auto directHashStart = timerStart();
        auto sortedCurrentDeps = sortAndDedupDeps(currentDeps);
        auto newFullHash = computeTraceHashFromSorted(sortedCurrentDeps);

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
            if (auto r = acceptRecoveredTrace(*newTraceId)) {
                debug("recovery: direct hash recovery succeeded for '%s'", displayAttrPath(attrPath));
                nrRecoveryDirectHashHits++;
                nrRecoveryDirectHashTimeUs += elapsedUs(directHashStart);
                nrRecoveryTimeUs += elapsedUs(recoveryStart);
                return r;
            }
        }
        nrRecoveryDirectHashTimeUs += elapsedUs(directHashStart);
    }

    // If direct hash recovery tried the old trace's dep structure (allComputable),
    // save its struct_hash so we can skip identical structures in variant scan.
    std::optional<Hash> directHashStructHash;
    if (allComputable) {
        directHashStructHash = getTraceStructHash(oldTraceId);
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
          displayAttrPath(attrPath), historyEntries.size());

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
        // Skip structural variants identical to the one direct hash recovery already tried
        if (directHashStructHash && structHash == *directHashStructHash)
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

            // ParentContext: compute current parent trace hash directly
            if (dep.type == DepType::ParentContext) {
                auto parentTraceHash = getCurrentTraceHash(dep.key);
                if (!parentTraceHash) { repComputable = false; break; }
                Blake3Hash b3;
                std::memcpy(b3.bytes.data(), parentTraceHash->hash, 32);
                repCurrentDeps.push_back({dep.source, dep.key, DepHashValue(b3), dep.type});
                continue;
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

        auto sortedRepDeps = sortAndDedupDeps(repCurrentDeps);
        auto candidateFullHash = computeTraceHashFromSorted(sortedRepDeps);

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
            if (auto r = acceptRecoveredTrace(*candidateTraceId)) {
                debug("recovery: structural variant recovery succeeded for '%s'", displayAttrPath(attrPath));
                nrRecoveryStructVariantHits++;
                nrRecoveryStructVariantTimeUs += elapsedUs(structVariantStart);
                nrRecoveryTimeUs += elapsedUs(recoveryStart);
                return r;
            }
        }
    }
    nrRecoveryStructVariantTimeUs += elapsedUs(structVariantStart);

    debug("recovery: all strategies failed for '%s'", displayAttrPath(attrPath));
    nrRecoveryFailures++;
    nrRecoveryTimeUs += elapsedUs(recoveryStart);
    return std::nullopt;
}

} // namespace nix::eval_trace
