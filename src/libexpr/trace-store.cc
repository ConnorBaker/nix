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
        if (seg.empty()) continue; // skip empty segments from malformed paths (e.g., "a..b")
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
        if (seg.empty()) continue; // skip empty segments from malformed paths
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
    case DepType::Content:
    case DepType::RawContent: {
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
    case DepType::ImplicitShape:
        // ImplicitShape uses the same key format and hash computation as
        // StructuredContent. Verified only for failed sources without SC deps.
        [[fallthrough]];
    case DepType::StructuredContent: {
        // Key format: "filepath\tf:datapath[#suffix]" (tab separator)
        auto sep = dep.key.find('\t');
        if (sep == std::string::npos || sep + 2 >= dep.key.size())
            return std::nullopt;
        auto format = parseStructuredFormat(dep.key[sep + 1]);
        if (!format || dep.key[sep + 2] != ':')
            return std::nullopt;
        std::string filePath = dep.key.substr(0, sep);
        auto [dataPath, shape] = parseShapeSuffix(dep.key.substr(sep + 3));

        // Check for #has:key suffix (finer-grained than #keys, records per-key existence).
        // This runs only when parseShapeSuffix returns None — no collision with #len/#keys/#type.
        // Real keys containing '#' are escaped by escapeDataPathKey (quoted with "..."),
        // so "#has:" can only appear bare at a segment boundary, never inside a quoted key.
        // The key name after "#has:" may itself be escaped (e.g., "#has:\"key.name\"")
        // and must be unescaped before use.
        std::string hasKeyName;
        bool isHasKey = false;
        if (shape == ShapeSuffix::None) {
            auto hasPos = dataPath.find("#has:");
            if (hasPos != std::string::npos) {
                isHasKey = true;
                hasKeyName = unescapeDataPathKey(dataPath.substr(hasPos + 5));
                dataPath = dataPath.substr(0, hasPos);
            }
        }

        // Construct a synthetic Content dep to resolve the file path
        Dep fileDep{dep.source, filePath, DepHashValue{Blake3Hash{}}, DepType::Content};
        auto path = resolveDepPath(fileDep, inputAccessors);
        if (!path) return std::nullopt;

        try {
            // Helper: compute hash for sorted key set (shared across formats)
            auto hashSortedKeys = [](std::vector<std::string> keys) -> DepHashValue {
                std::sort(keys.begin(), keys.end());
                std::string canonical;
                for (size_t i = 0; i < keys.size(); i++) {
                    if (i > 0) canonical += '\0';
                    canonical += keys[i];
                }
                return DepHashValue(depHash(canonical));
            };

            switch (*format) {
            case StructuredFormat::Json: {
                // Use DOM cache to avoid re-parsing
                auto cacheKey = dep.source + '\t' + filePath;
                auto cacheIt = jsonDomCache.find(cacheKey);
                if (cacheIt == jsonDomCache.end()) {
                    auto contents = path->readFile();
                    cacheIt = jsonDomCache.emplace(cacheKey, nlohmann::json::parse(contents)).first;
                }
                auto * node = navigateJson(cacheIt->second, dataPath);
                if (!node) return std::nullopt;
                switch (shape) {
                case ShapeSuffix::Len:
                    if (!node->is_array()) return std::nullopt;
                    return DepHashValue(depHash(std::to_string(node->size())));
                case ShapeSuffix::Keys: {
                    if (!node->is_object()) return std::nullopt;
                    std::vector<std::string> keys;
                    for (auto & [k, _] : node->items())
                        keys.push_back(k);
                    return hashSortedKeys(std::move(keys));
                }
                case ShapeSuffix::Type:
                    if (node->is_object()) return DepHashValue(depHash("object"));
                    if (node->is_array()) return DepHashValue(depHash("array"));
                    return std::nullopt; // scalar — type changed from container
                case ShapeSuffix::None:
                    if (isHasKey) {
                        if (!node->is_object()) return std::nullopt;
                        return DepHashValue(depHash(node->contains(hasKeyName) ? "1" : "0"));
                    }
                    return DepHashValue(depHash(node->dump()));
                }
                break;
            }
            case StructuredFormat::Toml: {
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
                switch (shape) {
                case ShapeSuffix::Len:
                    if (!node->is_array()) return std::nullopt;
                    return DepHashValue(depHash(std::to_string(toml::get<std::vector<toml::value>>(*node).size())));
                case ShapeSuffix::Keys: {
                    if (!node->is_table()) return std::nullopt;
                    auto & table = toml::get<toml::table>(*node);
                    std::vector<std::string> keys;
                    for (auto & [k, _] : table)
                        keys.push_back(k);
                    return hashSortedKeys(std::move(keys));
                }
                case ShapeSuffix::Type:
                    if (node->is_table()) return DepHashValue(depHash("object"));
                    if (node->is_array()) return DepHashValue(depHash("array"));
                    return std::nullopt;
                case ShapeSuffix::None:
                    if (isHasKey) {
                        if (!node->is_table()) return std::nullopt;
                        auto & table = toml::get<toml::table>(*node);
                        return DepHashValue(depHash(table.count(hasKeyName) ? "1" : "0"));
                    }
                    return DepHashValue(depHash(tomlCanonical(*node)));
                }
                break;
            }
            case StructuredFormat::Directory: {
                // Directory structural dep: re-read listing, look up entry
                auto cacheKey = dep.source + '\t' + filePath;
                auto cacheIt = dirListingCache.find(cacheKey);
                if (cacheIt == dirListingCache.end()) {
                    auto dirEntries = path->readDirectory();
                    cacheIt = dirListingCache.emplace(cacheKey, std::move(dirEntries)).first;
                }
                auto & entries = cacheIt->second;

                switch (shape) {
                case ShapeSuffix::Len:
                    return DepHashValue(depHash(std::to_string(entries.size())));
                case ShapeSuffix::Keys: {
                    // std::map is already sorted by key
                    std::string canonical;
                    bool first = true;
                    for (auto & [k, _] : entries) {
                        if (!first) canonical += '\0';
                        canonical += k;
                        first = false;
                    }
                    return DepHashValue(depHash(canonical));
                }
                case ShapeSuffix::Type:
                    // Directories are always "object" (key→type mapping)
                    return DepHashValue(depHash("object"));
                case ShapeSuffix::None: {
                    if (isHasKey) {
                        return DepHashValue(depHash(entries.count(hasKeyName) ? "1" : "0"));
                    }
                    auto segments = parseDataPath(dataPath);
                    if (segments.size() != 1) return std::nullopt;
                    auto it = entries.find(segments[0]);
                    if (it == entries.end()) return std::nullopt;
                    return DepHashValue(depHash(dirEntryTypeString(it->second)));
                }
                }
                break;
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

// ── BLOB serialization for dep key sets ──────────────────────────────
//
// keys_blob: packed 9-byte entries (type[1] + sourceId[4] + keyId[4]),
// zstd compressed. Stored in DepKeySets table, shared across traces with
// the same dep structure (same struct_hash).
// Note: uses native byte order for uint32_t fields. The database is a
// local cache (safe to delete), so cross-endianness portability is not required.

struct __attribute__((packed)) DepKeyBlobEntry {
    uint8_t type;
    uint32_t sourceId;
    uint32_t keyId;
};
static_assert(sizeof(DepKeyBlobEntry) == 9);

std::vector<uint8_t> TraceStore::serializeKeys(const std::vector<InternedDepKey> & keys)
{
    std::vector<uint8_t> blob;
    blob.reserve(keys.size() * sizeof(DepKeyBlobEntry));

    for (auto & key : keys) {
        DepKeyBlobEntry entry{std::to_underlying(key.type), key.sourceId, key.keyId};
        auto * raw = reinterpret_cast<const uint8_t *>(&entry);
        blob.insert(blob.end(), raw, raw + sizeof(entry));
    }

    if (!blob.empty()) {
        auto compressed = nix::compress(
            CompressionAlgo::zstd,
            {reinterpret_cast<const char *>(blob.data()), blob.size()},
            false, 1);
        return {compressed.begin(), compressed.end()};
    }
    return blob;
}

std::vector<TraceStore::InternedDepKey> TraceStore::deserializeKeys(
    const void * blob, size_t size)
{
    if (size == 0)
        return {};

    auto decompressed = nix::decompress("zstd",
        {static_cast<const char *>(blob), size});

    std::vector<InternedDepKey> keys;
    const uint8_t * p = reinterpret_cast<const uint8_t *>(decompressed.data());
    const uint8_t * end = p + decompressed.size();

    while (p + sizeof(DepKeyBlobEntry) <= end) {
        DepKeyBlobEntry entry;
        std::memcpy(&entry, p, sizeof(entry));
        p += sizeof(entry);

        keys.push_back({
            static_cast<DepType>(entry.type),
            entry.sourceId,
            entry.keyId
        });
    }

    return keys;
}

// ── BLOB serialization for dep hash values ───────────────────────────
//
// values_blob: per-entry hashLen[1] + hashData[hashLen], zstd compressed.
// Stored in Traces table. Entries are positionally matched with keys_blob
// in the corresponding DepKeySets row.

std::vector<uint8_t> TraceStore::serializeValues(const std::vector<InternedDep> & deps)
{
    std::vector<uint8_t> blob;
    blob.reserve(deps.size() * 33);  // BLAKE3: 1 + 32 bytes typical

    for (auto & dep : deps) {
        auto [hashData, hashSize] = blobData(dep.hash);
        assert(hashSize <= 255 && "dep hash value exceeds single-byte length prefix");
        blob.push_back(static_cast<uint8_t>(hashSize));
        blob.insert(blob.end(), hashData, hashData + hashSize);
    }

    if (!blob.empty()) {
        auto compressed = nix::compress(
            CompressionAlgo::zstd,
            {reinterpret_cast<const char *>(blob.data()), blob.size()},
            false, 1);
        return {compressed.begin(), compressed.end()};
    }
    return blob;
}

std::vector<DepHashValue> TraceStore::deserializeValues(
    const void * blob, size_t size, const std::vector<InternedDepKey> & keys)
{
    if (size == 0)
        return {};

    auto decompressed = nix::decompress("zstd",
        {static_cast<const char *>(blob), size});

    std::vector<DepHashValue> values;
    const uint8_t * p = reinterpret_cast<const uint8_t *>(decompressed.data());
    const uint8_t * end = p + decompressed.size();
    size_t idx = 0;

    while (p < end && idx < keys.size()) {
        uint8_t hashLen = *p++;
        if (p + hashLen > end) break;

        // Use the dep type from the corresponding key to determine whether
        // this is a Blake3Hash or a variable-length string. Without this,
        // a 32-byte store path string would be incorrectly decoded as Blake3.
        if (isBlake3Dep(keys[idx].type) && hashLen == 32)
            values.push_back(Blake3Hash::fromBlob(p, 32));
        else
            values.push_back(std::string(reinterpret_cast<const char *>(p), hashLen));
        p += hashLen;
        idx++;
    }

    // Warn on key/value count mismatch (could indicate DB corruption)
    if (values.size() != keys.size())
        warn("deserializeValues: got %d values but expected %d (keys count)",
             values.size(), keys.size());

    return values;
}

// ── Dep key/value resolution ─────────────────────────────────────────

std::vector<Dep> TraceStore::resolveDeps(
    const std::vector<InternedDepKey> & keys,
    const std::vector<DepHashValue> & values)
{
    ensureStringTableLoaded();
    std::vector<Dep> deps;
    auto count = std::min(keys.size(), values.size());
    deps.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        auto & k = keys[i];
        auto sourceIt = stringTable.find(static_cast<StringId>(k.sourceId));
        auto keyIt = stringTable.find(static_cast<StringId>(k.keyId));
        deps.push_back(Dep{
            sourceIt != stringTable.end() ? sourceIt->second : "",
            keyIt != stringTable.end() ? keyIt->second : "",
            values[i],
            k.type
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

    // DepKeySets (content-addressed dep key storage, UPSERT RETURNING)
    st->upsertDepKeySet.create(st->db,
        "INSERT INTO DepKeySets(struct_hash, keys_blob) VALUES (?, ?) "
        "ON CONFLICT(struct_hash) DO UPDATE SET struct_hash = excluded.struct_hash "
        "RETURNING id");

    st->getDepKeySet.create(st->db,
        "SELECT struct_hash, keys_blob FROM DepKeySets WHERE id = ?");

    // Traces (references DepKeySets via dep_key_set_id FK, UPSERT RETURNING)
    st->upsertTrace.create(st->db,
        "INSERT INTO Traces(trace_hash, dep_key_set_id, values_blob) "
        "VALUES (?, ?, ?) "
        "ON CONFLICT(trace_hash) DO UPDATE SET trace_hash = excluded.trace_hash "
        "RETURNING id");

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
    traceDataCache.clear();
    traceRowCache.clear();
    depKeySetCache.clear();
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
        [&](const attrs_t & a) -> std::tuple<ResultKind, std::string, std::string> {
            std::string val;
            bool first = true;
            for (auto & sym : a.names) {
                if (!first) val.push_back('\t');
                val.append(std::string(symbols[sym]));
                first = false;
            }
            // Encode origins into the context field when present.
            // Format: "N\t" + N groups of "depSource\tdepKey\tdataPath\tformat"
            //         + "\n" + space-separated per-attr indices
            std::string ctx;
            if (!a.origins.empty()) {
                ctx += std::to_string(a.origins.size());
                for (auto & orig : a.origins) {
                    ctx += '\t';
                    ctx += orig.depSource;
                    ctx += '\t';
                    ctx += orig.depKey;
                    ctx += '\t';
                    ctx += orig.dataPath;
                    ctx += '\t';
                    ctx += orig.format;
                }
                ctx += '\n';
                for (size_t i = 0; i < a.originIndices.size(); i++) {
                    if (i > 0) ctx += ' ';
                    ctx += std::to_string(static_cast<int>(a.originIndices[i]));
                }
            }
            return {ResultKind::FullAttrs, std::move(val), std::move(ctx)};
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
        attrs_t result;
        if (!row.value.empty()) {
            for (auto & name : tokenizeString<std::vector<std::string>>(row.value, "\t"))
                result.names.push_back(symbols.create(name));
        }
        // Decode origins from the context field when present.
        if (!row.context.empty()) {
            auto nlPos = row.context.find('\n');
            if (nlPos != std::string::npos) {
                // Parse origin count and origins from before \n
                auto header = std::string_view(row.context).substr(0, nlPos);
                auto firstTab = header.find('\t');
                size_t nOrigins = 0;
                if (firstTab != std::string_view::npos) {
                    nOrigins = std::stoull(std::string(header.substr(0, firstTab)));
                }
                // Parse origin groups: depSource\tdepKey\tdataPath\tformat
                size_t pos = firstTab + 1;
                for (size_t i = 0; i < nOrigins; i++) {
                    attrs_t::Origin orig;
                    // depSource
                    auto tab1 = header.find('\t', pos);
                    orig.depSource = std::string(header.substr(pos, tab1 - pos));
                    pos = tab1 + 1;
                    // depKey
                    auto tab2 = header.find('\t', pos);
                    orig.depKey = std::string(header.substr(pos, tab2 - pos));
                    pos = tab2 + 1;
                    // dataPath
                    auto tab3 = header.find('\t', pos);
                    orig.dataPath = std::string(header.substr(pos, tab3 - pos));
                    pos = tab3 + 1;
                    // format (single char)
                    orig.format = header[pos];
                    pos += 1;
                    if (pos < header.size() && header[pos] == '\t')
                        pos++; // skip separator before next origin
                    result.origins.push_back(std::move(orig));
                }
                // Parse per-attr indices from after \n
                auto indicesStr = row.context.substr(nlPos + 1);
                if (!indicesStr.empty()) {
                    for (auto & s : tokenizeString<std::vector<std::string>>(indicesStr, " "))
                        result.originIndices.push_back(static_cast<int8_t>(std::stoi(s)));
                }
            }
        }
        return result;
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


TraceStore::CachedTraceData * TraceStore::ensureTraceHashes(TraceId traceId)
{
    auto it = traceDataCache.find(traceId);
    if (it != traceDataCache.end())
        return &it->second;

    auto st(_state->lock());
    // getTraceInfo columns: 0=trace_hash, 1=struct_hash, 2=dep_key_set_id, 3=keys_blob, 4=values_blob
    auto use(st->getTraceInfo.use()(traceId));
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
        throw Error("trace %d not found", traceId);
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
    DepKeySetId depKeySetId = 0;
    Hash traceHash(HashAlgorithm::BLAKE3), structHash(HashAlgorithm::BLAKE3);
    {
        auto st(_state->lock());
        auto use(st->getTraceInfo.use()(traceId));
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
        depKeySetId = use.getInt(2);

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
    auto result = resolveDeps(keys, values);

    // Also populate the dep key set cache for potential recovery use
    depKeySetCache.insert_or_assign(depKeySetId, keys);

    data.deps = result;
    nrLoadTraceTimeUs += elapsedUs(loadStart);
    return result;
}

std::vector<TraceStore::InternedDepKey> TraceStore::loadKeySet(DepKeySetId depKeySetId)
{
    // Session cache: avoid re-decompressing keys_blob for shared key sets
    auto cacheIt = depKeySetCache.find(depKeySetId);
    if (cacheIt != depKeySetCache.end())
        return cacheIt->second;

    auto st(_state->lock());
    auto use(st->getDepKeySet.use()(depKeySetId));
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

DepKeySetId TraceStore::getOrCreateDepKeySet(
    const Hash & structHash,
    const std::vector<uint8_t> & keysBlob)
{
    auto st(_state->lock());
    auto use(st->upsertDepKeySet.use());
    bindRawHash(use, structHash);
    bindBlobVec(use, keysBlob);
    if (!use.next())
        throw Error("failed to get or create dep key set");
    return use.getInt(0);
}

TraceId TraceStore::getOrCreateTrace(
    const Hash & traceHash,
    DepKeySetId depKeySetId,
    const std::vector<uint8_t> & valuesBlob)
{
    auto st(_state->lock());
    auto use(st->upsertTrace.use());
    bindRawHash(use, traceHash);
    use(depKeySetId);
    bindBlobVec(use, valuesBlob);
    if (!use.next())
        throw Error("failed to get or create trace");
    return use.getInt(0);
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
    clearDomCaches();

    // Load the full trace (single DB read via JOIN)
    auto fullDeps = loadFullTrace(traceId);

    // Two-level verification: Content and Directory failures can be overridden
    // by passing StructuredContent deps that cover the same file/directory.
    // This enables fine-grained invalidation for fromJSON(readFile f) and
    // readDir patterns.
    //
    // ImplicitShape fallback: for failed sources with NO StructuredContent deps
    // (e.g., an operand of // whose values were never accessed), ImplicitShape
    // deps (creation-time #keys/#len) provide conservative verification. This
    // enables structural recovery for multi-provenance // without losing
    // single-provenance precision. For sources WITH SC deps, ImplicitShape is
    // skipped (SC provides finer-grained verification that tolerates key
    // additions irrelevant to the accessed values).
    //
    // First pass: verify non-structural deps, defer StructuredContent and
    // ImplicitShape deps, track failed coarse (Content/Directory) deps.

    bool hasNonContentFailure = false;
    bool hasContentFailure = false;
    bool hasStructuralDeps = false;
    bool hasImplicitShapeDeps = false;

    // Track failed coarse dep file/dir keys: "source\tkey"
    std::unordered_set<std::string> failedContentFiles;
    // Deferred structural deps (StructuredContent)
    std::vector<const Dep *> structuralDeps;
    // Deferred implicit shape deps (ImplicitShape — creation-time #keys/#len)
    std::vector<const Dep *> implicitShapeDeps;

    for (auto & dep : fullDeps) {
        nrDepsChecked++;

        if (isVolatile(dep.type)) {
            hasNonContentFailure = true;
            continue;
        }

        // ImplicitShape deps (creation-time #keys/#len) are deferred.
        // They serve as conservative structural fingerprints. During normal
        // verification they never block; they're only verified as a fallback
        // for failed sources that have no StructuredContent deps (enables
        // structural recovery for multi-provenance // without losing
        // precision for single-provenance //).
        if (depKind(dep.type) == DepKind::ImplicitStructural) {
            hasImplicitShapeDeps = true;
            implicitShapeDeps.push_back(&dep);
            continue;
        }

        if (depKind(dep.type) == DepKind::Structural) {
            hasStructuralDeps = true;
            structuralDeps.push_back(&dep);
            continue;
        }

        // ParentContext: verify the parent's trace hash matches AND parent is valid.
        // Recursive verification ensures stale parents don't make children appear valid.
        // verifiedTraceIds cache prevents infinite loops and ensures O(N) total work.
        if (depKind(dep.type) == DepKind::ParentContext) {
            auto parentTraceHash = getCurrentTraceHash(dep.key);
            if (parentTraceHash) {
                auto * expected = std::get_if<Blake3Hash>(&dep.expectedHash);
                if (expected && std::memcmp(expected->bytes.data(), parentTraceHash->hash, 32) == 0) {
                    // Hash matches. Now recursively verify the parent trace is valid.
                    std::string path(dep.key);
                    std::replace(path.begin(), path.end(), '\t', '\0');
                    auto parentRow = lookupTraceRow(path);
                    if (parentRow && verifyTrace(parentRow->traceId, inputAccessors, state)) {
                        continue; // parent trace unchanged AND valid → dep passes
                    }
                }
            }
            // Parent trace changed, missing, or invalid → non-content failure
            nrVerificationsFailed++;
            hasNonContentFailure = true;
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
            if (isContentOverrideable(dep.type)) {
                hasContentFailure = true;
                failedContentFiles.insert(dep.source + '\t' + dep.key);
            } else {
                hasNonContentFailure = true;
            }
        }
    }

    bool allValid;

    if (hasNonContentFailure) {
        // Non-coarse, non-structural failure → trace invalid (no override possible)
        allValid = false;
    } else if (!hasContentFailure) {
        // No coarse failures. Structural deps for files WITH a passing Content/Directory
        // dep in this trace don't need checking (file unchanged → SC deps pass too).
        // However, "standalone" SC deps — SC deps for files WITHOUT a Content/Directory
        // dep in this trace — must be verified directly. These arise from cross-trace
        // dep separation: a child trace inherits ExprTracedData thunks from the parent's
        // result but has no Content dep for the file (the parent does).
        allValid = true;
        if (hasStructuralDeps) {
            // Build set of files covered by passing Content/Directory deps in this trace
            std::unordered_set<std::string> coveredFiles;
            for (auto & dep : fullDeps) {
                if (isContentOverrideable(dep.type))
                    coveredFiles.insert(dep.source + '\t' + dep.key);
            }

            for (auto * dep : structuralDeps) {
                auto sep = dep->key.find('\t');
                if (sep == std::string::npos) continue;
                auto fileKey = dep->source + '\t' + dep->key.substr(0, sep);
                if (coveredFiles.count(fileKey)) continue; // File has passing coarse dep

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
                    break;
                }
            }
        }
    } else if (hasContentFailure && (hasStructuralDeps || hasImplicitShapeDeps)) {
        // Coarse failure(s) exist AND structural/implicit-shape deps are present.
        //
        // Two-tier coverage: StructuredContent (SC) deps provide fine-grained
        // verification. ImplicitShape deps provide fallback coverage for sources
        // that have no SC deps (e.g., an operand of // whose values were never
        // accessed). This enables structural recovery for multi-provenance //
        // without losing single-provenance precision.
        //
        // Rule: for each failed source, if SC deps cover it, verify SC deps
        // (ImplicitShape skipped — SC is finer-grained). If only ImplicitShape
        // covers it, verify ImplicitShape (conservative but sound: key set
        // unchanged means the source's structural contribution is unchanged).
        allValid = true;

        // Build coverage sets: which files are covered by SC vs ImplicitShape
        std::unordered_set<std::string> structuralCoveredFiles;
        for (auto * dep : structuralDeps) {
            auto sep = dep->key.find('\t');
            if (sep != std::string::npos)
                structuralCoveredFiles.insert(dep->source + '\t' + dep->key.substr(0, sep));
        }

        std::unordered_set<std::string> implicitCoveredFiles;
        for (auto * dep : implicitShapeDeps) {
            auto sep = dep->key.find('\t');
            if (sep != std::string::npos)
                implicitCoveredFiles.insert(dep->source + '\t' + dep->key.substr(0, sep));
        }

        // Check that all failed coarse deps are covered by EITHER SC or ImplicitShape
        for (auto & failedFile : failedContentFiles) {
            if (!structuralCoveredFiles.count(failedFile)
                && !implicitCoveredFiles.count(failedFile)) {
                allValid = false;
                break;
            }
        }

        // Verify all SC deps (as before — fine-grained verification)
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
                }
            }
        }

        // Verify ImplicitShape deps ONLY for failed files NOT covered by SC.
        // If a file has SC deps, those provide finer verification and
        // ImplicitShape would be over-conservative (e.g., key additions
        // that don't affect accessed values would spuriously fail).
        if (allValid) {
            for (auto * dep : implicitShapeDeps) {
                auto sep = dep->key.find('\t');
                if (sep == std::string::npos) continue;
                auto fileKey = dep->source + '\t' + dep->key.substr(0, sep);
                if (structuralCoveredFiles.count(fileKey)) continue;
                if (!failedContentFiles.count(fileKey)) continue;

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
                }
            }
        }
    } else {
        // Coarse failure with no structural or implicit-shape deps → invalid
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
    auto pathKey = std::string(attrPath);

    // Check traceRowCache first (populated on previous lookups, invalidated on
    // CurrentTraces changes in acceptRecoveredTrace and record).
    auto rowCacheIt = traceRowCache.find(pathKey);
    if (rowCacheIt != traceRowCache.end())
        return rowCacheIt->second;

    // Resolve attr_path_id (lookup only, no insert)
    auto apCacheIt = internedAttrPaths.find(pathKey);

    auto st(_state->lock());

    AttrPathId attrPathId;
    if (apCacheIt != internedAttrPaths.end()) {
        attrPathId = apCacheIt->second;
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

    traceRowCache[pathKey] = row;
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
    auto row = lookupTraceRow(path);  // hits traceRowCache after first call
    if (!row) return std::nullopt;

    // Return trace_hash (captures dep structure + hashes), not result hash.
    // Result hash for attrsets only captures attribute names, not values —
    // it wouldn't detect changes to attribute values within an attrset.
    auto * data = ensureTraceHashes(row->traceId);  // hits traceDataCache after first call
    if (!data) return std::nullopt;
    return data->traceHash;
}

// ── Interned dep sort + hash (integer key comparison) ────────────────
//
// Sort on (type:1, sourceId:4, keyId:4) = 9 bytes of integers instead of
// ~100 bytes of strings. Used by record() and recovery() for canonical ordering.
// NOTE: produces a DIFFERENT sort order than sortAndDedupDeps (integer ID order
// vs string lexicographic). Trace hashes are NOT compatible — one-time cache
// invalidation of existing traces on first use.

static void sortAndDedupInterned(std::vector<TraceStore::InternedDep> & deps)
{
    std::sort(deps.begin(), deps.end(),
        [](const TraceStore::InternedDep & a, const TraceStore::InternedDep & b) {
            if (auto cmp = a.type <=> b.type; cmp != 0) return cmp < 0;
            if (a.sourceId != b.sourceId) return a.sourceId < b.sourceId;
            return a.keyId < b.keyId;
        });
    deps.erase(std::unique(deps.begin(), deps.end(),
        [](const TraceStore::InternedDep & a, const TraceStore::InternedDep & b) {
            return a.type == b.type && a.sourceId == b.sourceId && a.keyId == b.keyId;
        }), deps.end());
}

using StringLookup = std::function<std::string_view(uint32_t)>;

static void feedInternedDepToSink(
    HashSink & sink,
    const TraceStore::InternedDep & dep,
    bool includeHash,
    const StringLookup & lookupString)
{
    auto typeStr = std::to_string(static_cast<int>(dep.type));
    sink(std::string_view("T", 1));
    sink(typeStr);
    sink(std::string_view("S", 1));
    sink(lookupString(dep.sourceId));
    sink(std::string_view("K", 1));
    sink(lookupString(dep.keyId));
    if (includeHash) {
        sink(std::string_view("H", 1));
        hashDepValue(sink, dep.hash);
    }
}

static Hash computeTraceHashFromInterned(
    const std::vector<TraceStore::InternedDep> & sorted,
    const StringLookup & lookupString)
{
    HashSink sink(HashAlgorithm::BLAKE3);
    for (auto & dep : sorted)
        feedInternedDepToSink(sink, dep, true, lookupString);
    return sink.finish().hash;
}

static Hash computeStructHashFromInterned(
    const std::vector<TraceStore::InternedDep> & sorted,
    const StringLookup & lookupString)
{
    HashSink sink(HashAlgorithm::BLAKE3);
    for (auto & dep : sorted)
        feedInternedDepToSink(sink, dep, false, lookupString);
    return sink.finish().hash;
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

    // 1. Intern strings FIRST (getting stable SQLite row IDs)
    auto interned = internDeps(allDeps);

    // 2. Sort+dedup on integer keys (type:1, sourceId:4, keyId:4 = 9 bytes)
    //    Much faster than string-based sort (~100 bytes per comparison).
    //    NOTE: Sort order changes from string-lexicographic to integer-ID-based.
    //    Trace hashes change → one-time cache invalidation of existing traces.
    sortAndDedupInterned(interned);

    // 3. String lookup for hash computation
    auto lookupString = [this](uint32_t id) -> std::string_view {
        auto it = stringTable.find(static_cast<StringId>(id));
        if (it != stringTable.end()) return it->second;
        return "";
    };

    // 4. Compute trace_hash (BLAKE3 of full sorted deps including hashes)
    auto traceHash = computeTraceHashFromInterned(interned, lookupString);

    // 5. Compute struct_hash (dep types + sources + keys, without hash values)
    auto structHash = computeStructHashFromInterned(interned, lookupString);

    // 6. Split into keys + values
    std::vector<InternedDepKey> keys;
    keys.reserve(interned.size());
    for (auto & d : interned)
        keys.push_back({d.type, d.sourceId, d.keyId});

    auto keysBlob = serializeKeys(keys);
    auto valuesBlob = serializeValues(interned);

    // 7. Get or create dep key set (content-addressed by struct_hash)
    auto depKeySetId = getOrCreateDepKeySet(structHash, keysBlob);

    // 8. Encode CachedResult and intern result
    auto [type, val, ctx] = encodeCachedResult(value);
    auto resultHash = computeResultHash(type, val, ctx);
    ResultId resultId = doInternResult(type, val, ctx, resultHash);

    // 9. Intern attr path
    AttrPathId attrPathId = doInternAttrPath(attrPath);

    // 10. Get or create trace (keyed by trace_hash, stores dep_key_set_id + values_blob)
    TraceId traceId = getOrCreateTrace(traceHash, depKeySetId, valuesBlob);

    // 11. Upsert Attrs + insert History
    {
        auto st(_state->lock());
        st->upsertAttr.use()
            (contextHash)(attrPathId)(traceId)(resultId).exec();
        st->insertHistory.use()
            (contextHash)(attrPathId)(traceId)(resultId).exec();
    }

    // 12. Session caches
    bool hasVolatile = std::any_of(allDeps.begin(), allDeps.end(),
        [](auto & d) { return isVolatile(d.type); });
    if (!hasVolatile)
        verifiedTraceIds.insert(traceId);

    {
        auto & data = traceDataCache[traceId];
        data.traceHash = traceHash;
        data.structHash = structHash;
        // Reconstruct sorted Dep vector from interned for cache compatibility
        data.deps = resolveDeps(keys,
            [&]() {
                std::vector<DepHashValue> vals;
                vals.reserve(interned.size());
                for (auto & d : interned) vals.push_back(d.hash);
                return vals;
            }());
        assert(data.hashesPopulated() && "recording trace with placeholder (all-zero) hashes");
    }
    depKeySetCache.insert_or_assign(depKeySetId, keys);

    // Update traceRowCache so subsequent lookupTraceRow/getCurrentTraceHash
    // calls for this attr path don't go to DB.
    traceRowCache[std::string(attrPath)] = TraceRow{traceId, resultId, type, val, ctx};

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

    // 2. Verify trace — compute ALL dep hashes upfront.
    //    With StatHashCache warm, computing all hashes is cheap (~2ms for ~4K deps:
    //    just lstat() + L1 cache lookup per file dep). Computing all hashes upfront
    //    ensures recovery can reuse them immediately via currentDepHashes.
    if (verifyTrace(row->traceId, inputAccessors, state)) {
        nrVerificationsPassed++;
        nrVerifyTimeUs += elapsedUs(verifyStart);
        return VerifyResult{decodeCachedResult(*row), row->traceId};
    }

    // 3. Verification failed → constructive recovery.
    //    All dep hashes are pre-computed in currentDepHashes (from step 2).
    //    Direct hash recovery is O(1): sort+hash the pre-computed values, lookup.
    //    No additional hash computation needed unless structural variant recovery
    //    encounters a trace with different dep keys (rare).
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
        if (isVolatile(dep.type)) {
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
        // ParentContext: compute current parent trace hash AND recursively
        // verify the parent trace is valid. Without recursive verification,
        // recovery accepts stale traces when the parent's deps have changed
        // but the parent's trace hash hasn't been updated yet in the DB.
        if (depKind(dep.type) == DepKind::ParentContext) {
            auto parentTraceHash = getCurrentTraceHash(dep.key);
            if (!parentTraceHash) { allComputable = false; break; }
            std::string path(dep.key);
            std::replace(path.begin(), path.end(), '\t', '\0');
            auto parentRow = lookupTraceRow(path);
            if (!parentRow || !verifyTrace(parentRow->traceId, inputAccessors, state)) {
                allComputable = false; break;
            }
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

    // === Pre-load: scan history with widened query ===
    // Single DB query pre-loads trace_hash + result data for ALL history entries.
    // This enables in-memory candidate matching for both directHash and structural
    // variant recovery, eliminating per-group lookupTraceByFullHash DB calls and
    // per-candidate getResult DB calls.
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
        // scanHistoryForAttr columns: 0=dk.id, 1=dk.struct_hash, 2=h.trace_id,
        //   3=h.result_id, 4=t.trace_hash, 5=r.type, 6=r.value, 7=r.context
        auto use(st->scanHistoryForAttr.use()(contextHash)(*attrPathId));
        while (use.next()) {
            auto [shData, shSize] = use.getBlob(1);
            auto [thData, thSize] = use.getBlob(4);
            historyEntries.emplace_back(
                use.getInt(0),
                readRawHash(shData, shSize),
                use.getInt(2),
                use.getInt(3),
                readRawHash(thData, thSize),
                static_cast<ResultKind>(use.getInt(5)),
                use.isNull(6) ? "" : use.getStr(6),
                use.isNull(7) ? "" : use.getStr(7)
            );
        }
    }

    // Build in-memory trace_hash → entry index lookup.
    // Key: raw 32-byte hash as std::string (efficient hashing, no custom hasher needed).
    // trace_hash is UNIQUE in the Traces table, so each hash maps to at most one entry.
    // When duplicates exist in history (same trace_id appearing multiple times), the
    // first entry wins via emplace — all duplicates have the same result data.
    std::unordered_map<std::string, size_t> traceHashToEntry;
    for (size_t i = 0; i < historyEntries.size(); i++) {
        auto & e = historyEntries[i];
        std::string hashKey(reinterpret_cast<const char *>(e.traceHash.hash), e.traceHash.hashSize);
        traceHashToEntry.emplace(hashKey, i);
    }

    // Look up a candidate trace_hash in the pre-loaded in-memory map.
    // Returns null if no history entry has this hash (the trace either doesn't
    // exist or exists for a different attr path — either way, not recoverable).
    auto lookupCandidate = [&](const Hash & candidateHash) -> const HistoryEntry * {
        std::string hashKey(reinterpret_cast<const char *>(candidateHash.hash), candidateHash.hashSize);
        auto it = traceHashToEntry.find(hashKey);
        if (it == traceHashToEntry.end()) return nullptr;
        return &historyEntries[it->second];
    };

    // Accept a candidate found via in-memory trace_hash matching. The hash match
    // proves all deps match current state (collision probability: 2^-256),
    // so no dep-by-dep verifyTrace call is needed.
    // Result data is pre-loaded from the widened scan — only upsertAttr needs DB.
    auto acceptRecoveredTrace = [&](const HistoryEntry & entry) -> std::optional<VerifyResult> {
        if (triedTraceIds.count(entry.traceId))
            return std::nullopt;
        triedTraceIds.insert(entry.traceId);

        // Update CurrentTraces to point to recovered trace + result (only remaining DB call)
        {
            auto st(_state->lock());
            st->upsertAttr.use()
                (contextHash)(*attrPathId)(entry.traceId)(entry.resultId).exec();
        }

        // Update traceRowCache so subsequent lookupTraceRow/getCurrentTraceHash
        // calls for this attr path reflect the recovered state.
        TraceRow newRow{entry.traceId, entry.resultId, entry.type, entry.value, entry.context};
        traceRowCache[pathKey] = newRow;

        // Hash match guarantees all deps match current state
        verifiedTraceIds.insert(entry.traceId);
        return VerifyResult{decodeCachedResult(newRow), entry.traceId};
    };

    // String lookup for interned hash computation (used by both recovery paths)
    auto lookupString = [this](uint32_t id) -> std::string_view {
        auto it = stringTable.find(static_cast<StringId>(id));
        if (it != stringTable.end()) return it->second;
        return "";
    };

    // === Direct hash recovery (BSàlC CT) ===
    // Recompute trace_hash from current dep hashes. In-memory lookup.
    if (allComputable) {
        auto directHashStart = timerStart();
        auto interned = internDeps(currentDeps);
        sortAndDedupInterned(interned);
        auto newFullHash = computeTraceHashFromInterned(interned, lookupString);

        if (auto * entry = lookupCandidate(newFullHash)) {
            if (auto r = acceptRecoveredTrace(*entry)) {
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
    // different dep structures across evaluations. Groups history entries by
    // dep_key_set_id, loads key set once per group, recomputes current hashes,
    // and matches against in-memory trace_hash map. O(V) where V = number of
    // distinct dep structures. Zero values_blob decompression needed.
    auto structVariantStart = timerStart();

    debug("recovery: structural variant scan for '%s' -- scanning %d history entries",
          displayAttrPath(attrPath), historyEntries.size());

    // Group by dep_key_set_id (integer), pick one representative per group.
    // Using dep_key_set_id instead of struct_hash for grouping is more efficient
    // (integer comparison vs 32-byte hash comparison).
    std::unordered_map<DepKeySetId, TraceId> structGroups;
    std::unordered_map<DepKeySetId, Hash> groupStructHashes;
    for (auto & e : historyEntries) {
        if (triedTraceIds.count(e.traceId))
            continue;
        structGroups.emplace(e.depKeySetId, e.traceId);
        groupStructHashes.emplace(e.depKeySetId, e.structHash);
    }

    for (auto & [depKeySetId, repTraceId] : structGroups) {
        if (triedTraceIds.count(repTraceId))
            continue;
        // Skip structural variants identical to the one direct hash recovery already tried
        auto structHashIt = groupStructHashes.find(depKeySetId);
        if (directHashStructHash && structHashIt != groupStructHashes.end()
            && structHashIt->second == *directHashStructHash)
            continue;

        // Load only the dep key set (no values_blob decompression).
        // Session-cached: subsequent traces with the same dep structure hit the cache.
        auto repKeys = loadKeySet(depKeySetId);
        ensureStringTableLoaded();

        // Resolve keys to Dep objects and look up current hashes
        std::vector<Dep> repCurrentDeps;
        bool repComputable = true;
        for (auto & key : repKeys) {
            auto type = key.type;

            if (isVolatile(type)) {
                repComputable = false;
                break;
            }

            // Resolve string IDs to strings
            auto sourceIt = stringTable.find(static_cast<StringId>(key.sourceId));
            auto keyIt = stringTable.find(static_cast<StringId>(key.keyId));
            std::string source = sourceIt != stringTable.end() ? sourceIt->second : "";
            std::string depKey = keyIt != stringTable.end() ? keyIt->second : "";

            // ParentContext: compute current parent trace hash AND recursively
            // verify the parent trace is valid (same invariant as verifyTrace).
            if (depKind(type) == DepKind::ParentContext) {
                auto parentTraceHash = getCurrentTraceHash(depKey);
                if (!parentTraceHash) { repComputable = false; break; }
                std::string path(depKey);
                std::replace(path.begin(), path.end(), '\t', '\0');
                auto parentRow = lookupTraceRow(path);
                if (!parentRow || !verifyTrace(parentRow->traceId, inputAccessors, state)) {
                    repComputable = false; break;
                }
                Blake3Hash b3;
                std::memcpy(b3.bytes.data(), parentTraceHash->hash, 32);
                repCurrentDeps.push_back({source, depKey, DepHashValue(b3), type});
                continue;
            }

            // Look up pre-computed hash from currentDepHashes (populated by verify step)
            DepKey dk(type, source, depKey);
            auto cacheIt = currentDepHashes.find(dk);
            std::optional<DepHashValue> current;

            if (cacheIt != currentDepHashes.end()) {
                current = cacheIt->second;
            } else {
                // Dep not in cache — must be a new key not seen in old trace.
                // Construct a synthetic Dep for hash computation.
                Dep syntheticDep{source, depKey, DepHashValue{Blake3Hash{}}, type};
                current = computeCurrentHash(state, syntheticDep, inputAccessors);
                currentDepHashes[dk] = current;
            }

            if (!current) {
                repComputable = false;
                break;
            }
            repCurrentDeps.push_back({source, depKey, *current, type});
        }
        if (!repComputable)
            continue;

        auto repInterned = internDeps(repCurrentDeps);
        sortAndDedupInterned(repInterned);
        auto candidateFullHash = computeTraceHashFromInterned(repInterned, lookupString);

        // In-memory lookup instead of per-group DB query
        if (auto * entry = lookupCandidate(candidateFullHash)) {
            if (auto r = acceptRecoveredTrace(*entry)) {
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
