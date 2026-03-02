#include "nix/expr/eval-trace/store/trace-store.hh"
#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/store/stat-hash-store.hh"
#include "nix/expr/eval-trace/cache/trace-cache.hh"
#include "nix/expr/eval-trace/deps/hash.hh"
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

#include "eval-trace/store/interned-hash.hh"

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
    const TraceStore::ResolvedDep & dep, const boost::unordered_flat_map<std::string, SourcePath> & inputAccessors)
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

// Stack-local DOM caches: avoid re-parsing the same file when multiple
// StructuredContent deps reference it within a single verifyTrace() call.
// Each verifyTrace() invocation (including recursive ParentContext calls)
// gets its own scope, fixing the reentrancy bug where recursive calls
// would corrupt the outer invocation's DOM state via shared thread-locals.
struct VerificationScope {
    std::unordered_map<std::string, nlohmann::json> jsonDomCache;
    std::unordered_map<std::string, toml::value> tomlDomCache;
    std::unordered_map<std::string, SourceAccessor::DirEntries> dirListingCache;
};

/**
 * Navigate a JSON DOM using a JSON path array. Returns nullptr if path is invalid.
 * Path components: strings for object keys, numbers for array indices.
 */
static const nlohmann::json * navigateJson(const nlohmann::json & root, const nlohmann::json & pathArray)
{
    const nlohmann::json * node = &root;
    for (auto & component : pathArray) {
        if (component.is_number()) {
            if (!node->is_array()) return nullptr;
            auto idx = component.get<size_t>();
            if (idx >= node->size()) return nullptr;
            node = &(*node)[idx];
        } else {
            if (!node->is_object()) return nullptr;
            auto key = component.get<std::string>();
            auto it = node->find(key);
            if (it == node->end()) return nullptr;
            node = &*it;
        }
    }
    return node;
}

/**
 * Navigate a TOML DOM using a JSON path array. Returns nullptr if path is invalid.
 */
static const toml::value * navigateToml(const toml::value & root, const nlohmann::json & pathArray)
{
    const toml::value * node = &root;
    for (auto & component : pathArray) {
        if (component.is_number()) {
            if (!node->is_array()) return nullptr;
            auto idx = component.get<size_t>();
            auto & arr = toml::get<std::vector<toml::value>>(*node);
            if (idx >= arr.size()) return nullptr;
            node = &arr[idx];
        } else {
            if (!node->is_table()) return nullptr;
            auto key = component.get<std::string>();
            auto & table = toml::get<toml::table>(*node);
            auto it = table.find(key);
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
    EvalState & state, const TraceStore::ResolvedDep & dep,
    const boost::unordered_flat_map<std::string, SourcePath> & inputAccessors,
    VerificationScope & scope,
    const boost::unordered_flat_map<std::string, std::string> & dirSets)
{
    switch (dep.type) {
    case DepType::Content:
    case DepType::RawContent: {
        auto path = resolveDepPath(dep, inputAccessors);
        if (!path) return std::nullopt;
        try {
            return DepHashValue(StatHashStore::instance().depHashFile(*path));
        } catch (std::exception &) {
            return std::nullopt;
        }
    }
    case DepType::NARContent: {
        auto path = resolveDepPath(dep, inputAccessors);
        if (!path) return std::nullopt;
        try {
            return DepHashValue(StatHashStore::instance().depHashPathCached(*path));
        } catch (std::exception &) {
            return std::nullopt;
        }
    }
    case DepType::Directory: {
        auto path = resolveDepPath(dep, inputAccessors);
        if (!path) return std::nullopt;
        try {
            return DepHashValue(StatHashStore::instance().depHashDirListingCached(*path, path->readDirectory()));
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
        // Key format: JSON object {"f":"path","t":"j","p":[...],"s":"keys","h":"key"}
        // Or aggregated DirSet: {"ds":"<hex>","h":"keyName","t":"d"} (dirs in DirSets table)
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(dep.key);
        } catch (...) {
            return std::nullopt;
        }

        // Aggregated DirSet dep: iterate all directories, check each for the key
        if (j.contains("ds")) {
            auto dsHash = j.value("ds", "");
            auto hasKeyName = j.value("h", "");
            if (hasKeyName.empty()) return std::nullopt;

            auto it = dirSets.find(dsHash);
            if (it == dirSets.end()) return std::nullopt;
            auto dirs = nlohmann::json::parse(it->second);
            for (auto & dir : dirs) {
                if (!dir.is_array() || dir.size() != 2) continue;
                auto source = dir[0].get<std::string>();
                auto filePath = dir[1].get<std::string>();
                TraceStore::ResolvedDep fileDep{source, filePath, DepHashValue{Blake3Hash{}}, DepType::Directory};
                auto path = resolveDepPath(fileDep, inputAccessors);
                if (!path) continue;
                try {
                    auto cacheKey = source + '\t' + filePath;
                    auto cacheIt = scope.dirListingCache.find(cacheKey);
                    if (cacheIt == scope.dirListingCache.end())
                        cacheIt = scope.dirListingCache.emplace(cacheKey, path->readDirectory()).first;
                    if (cacheIt->second.count(hasKeyName))
                        return DepHashValue(depHash("1")); // key found in this dir
                } catch (...) {
                    continue;
                }
            }
            return DepHashValue(depHash("0")); // key absent in all dirs
        }

        auto filePath = j.value("f", "");
        auto formatStr = j.value("t", "");
        if (formatStr.empty()) return std::nullopt;
        auto format = parseStructuredFormat(formatStr[0]);
        if (!format) return std::nullopt;
        auto pathArray = j.value("p", nlohmann::json::array());
        auto hasKeyName = j.value("h", "");
        bool isHasKey = !hasKeyName.empty();
        auto parseShapeName = [](const std::string & name) -> ShapeSuffix {
            if (name == "len") return ShapeSuffix::Len;
            if (name == "keys") return ShapeSuffix::Keys;
            if (name == "type") return ShapeSuffix::Type;
            return ShapeSuffix::None;
        };
        auto shape = parseShapeName(j.value("s", ""));

        // Construct a synthetic Content dep to resolve the file path
        TraceStore::ResolvedDep fileDep{dep.source, filePath, DepHashValue{Blake3Hash{}}, DepType::Content};
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
                auto cacheIt = scope.jsonDomCache.find(cacheKey);
                if (cacheIt == scope.jsonDomCache.end()) {
                    auto contents = path->readFile();
                    cacheIt = scope.jsonDomCache.emplace(cacheKey, nlohmann::json::parse(contents)).first;
                }
                auto * node = navigateJson(cacheIt->second, pathArray);
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
                auto cacheIt = scope.tomlDomCache.find(cacheKey);
                if (cacheIt == scope.tomlDomCache.end()) {
                    auto contents = path->readFile();
                    std::istringstream stream(std::move(contents));
                    cacheIt = scope.tomlDomCache.emplace(cacheKey, toml::parse(
                        stream, "verifyTrace"
#if HAVE_TOML11_4
                        , toml::spec::v(1, 0, 0)
#endif
                    )).first;
                }
                auto * node = navigateToml(cacheIt->second, pathArray);
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
                auto cacheIt = scope.dirListingCache.find(cacheKey);
                if (cacheIt == scope.dirListingCache.end()) {
                    auto dirEntries = path->readDirectory();
                    cacheIt = scope.dirListingCache.emplace(cacheKey, std::move(dirEntries)).first;
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
                    // Directory scalar: pathArray should have exactly one string component
                    if (pathArray.size() != 1 || !pathArray[0].is_string()) return std::nullopt;
                    auto it = entries.find(pathArray[0].get<std::string>());
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
    case DepType::StorePathExistence: {
        try {
            auto storePath = state.store->parseStorePath(dep.key);
            return DepHashValue(state.store->isValidPath(storePath)
                ? std::string("valid") : std::string("missing"));
        } catch (std::exception &) {
            return DepHashValue(std::string("missing"));
        }
    }
    case DepType::CurrentTime:
    case DepType::Exec:
    case DepType::ParentContext:
    case DepType::EndSentinel_:
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
        DepKeyBlobEntry entry{std::to_underlying(key.type), key.sourceId.value, key.keyId.value};
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
            StringId(entry.sourceId),
            StringId(entry.keyId)
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

std::string_view TraceStore::resolveString(StringId id) const
{
    return pools.strings.resolve(id);
}

TraceStore::ResolvedDep TraceStore::resolveDep(const InternedDep & idep)
{
    if (idep.key.type == DepType::ParentContext) {
        // keyId holds an AttrPathId.value, not a StringId. Resolve to
        // dot-separated display path for the ResolvedDep string representation.
        auto pathId = AttrPathId(idep.key.keyId.value);
        return ResolvedDep{"", vocab.displayPath(pathId), idep.hash, DepType::ParentContext};
    }
    return ResolvedDep{
        std::string(resolveString(idep.key.sourceId)),
        std::string(resolveString(idep.key.keyId)),
        idep.hash,
        idep.key.type};
}

std::vector<TraceStore::InternedDep> TraceStore::internDeps(const std::vector<Dep> & deps)
{
    // DepSourceId, DepKeyId, and StringId all share the same StringInternTable
    // index space. Conversion is a raw value copy — no string lookup needed.
    // SAFETY: Only valid within the same pool generation. If pools.strings is
    // cleared between recording and calling internDeps(), the IDs become invalid.
    std::vector<InternedDep> interned;
    interned.reserve(deps.size());

    for (auto & dep : deps)
        interned.push_back(InternedDep{
            {dep.type, StringId(dep.sourceId.value), StringId(dep.keyId.value)},
            dep.expectedHash});

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

    // Bulk-load all interned entities into in-memory maps.
    // Done here (not via bulkLoadAll()) because st already holds the lock.
    {
        auto use(st->getAllStrings.use());
        while (use.next()) {
            auto id = static_cast<uint32_t>(use.getInt(0));
            auto value = use.getStr(1);
            pools.strings.bulkLoad(id, value);
            if (id > flushedStringHighWaterMark)
                flushedStringHighWaterMark = id;
        }
    }
    {
        auto use(st->getAllResults.use());
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
        auto use(st->getAllDepKeySets.use());
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
        auto use(st->getAllTraces.use());
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
        auto use(st->getAllDirSets.use());
        while (use.next())
            pools.dirSets.emplace(std::string(use.getStr(0)), std::string(use.getStr(1)));
    }

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

void TraceStore::bulkLoadAll()
{
    auto st(_state->lock());

    // Load Strings into the shared StringInternTable
    {
        auto use(st->getAllStrings.use());
        while (use.next()) {
            auto id = static_cast<uint32_t>(use.getInt(0));
            auto value = use.getStr(1);
            pools.strings.bulkLoad(id, value);
            if (id > flushedStringHighWaterMark)
                flushedStringHighWaterMark = id;
        }
    }

    // Load Results: populate hash → id dedup map
    {
        auto use(st->getAllResults.use());
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

    // Load DepKeySets: populate struct_hash → id dedup map
    {
        auto use(st->getAllDepKeySets.use());
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

    // Load Traces: populate trace_hash → id dedup map
    {
        auto use(st->getAllTraces.use());
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

    // Load DirSets: populate dsHash → dirs JSON cache
    {
        auto use(st->getAllDirSets.use());
        while (use.next())
            pools.dirSets.emplace(std::string(use.getStr(0)), std::string(use.getStr(1)));
    }
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
            // Encode origins into the context field as JSON when present.
            std::string ctx;
            if (!a.origins.empty()) {
                nlohmann::json originsJson;
                nlohmann::json origArr = nlohmann::json::array();
                for (auto & orig : a.origins) {
                    origArr.push_back({
                        {"s", orig.depSource},
                        {"f", orig.depKey},
                        {"p", orig.dataPath},  // already a JSON array string
                        {"t", std::string(1, orig.format)},
                    });
                }
                originsJson["origins"] = std::move(origArr);
                originsJson["indices"] = a.originIndices;
                ctx = originsJson.dump();
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
        // Decode origins from JSON context field when present.
        if (!row.context.empty()) {
            auto ctx = nlohmann::json::parse(row.context);
            for (auto & origJson : ctx["origins"]) {
                attrs_t::Origin orig;
                orig.depSource = origJson["s"].get<std::string>();
                orig.depKey = origJson["f"].get<std::string>();
                orig.dataPath = origJson["p"].get<std::string>();
                orig.format = origJson["t"].get<std::string>()[0];
                result.origins.push_back(std::move(orig));
            }
            for (auto & idx : ctx["indices"])
                result.originIndices.push_back(idx.get<int8_t>());
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

std::vector<TraceStore::InternedDep> TraceStore::loadFullTrace(TraceId traceId)
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

    // Zip keys + values into InternedDep vector (no string resolution)
    std::vector<InternedDep> result;
    result.reserve(keys.size());
    for (size_t i = 0; i < std::min(keys.size(), values.size()); ++i)
        result.push_back({keys[i], std::move(values[i])});

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
    if (verifiedTraceIds.count(traceId))
        return true;

    auto vtStart = timerStart();
    VerificationScope scope;

    // Load the full trace (single DB read via JOIN) — now vector<InternedDep>
    auto fullDeps = loadFullTrace(traceId);


    bool hasNonContentFailure = false;
    bool hasContentFailure = false;
    bool hasStructuralDeps = false;
    bool hasImplicitShapeDeps = false;

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
            hasImplicitShapeDeps = true;
            implicitShapeDepIndices.push_back(i);
            continue;
        }

        if (depKind(idep.key.type) == DepKind::Structural) {
            hasStructuralDeps = true;
            structuralDepIndices.push_back(i);
            continue;
        }

        if (depKind(idep.key.type) == DepKind::ParentContext) {
            auto parentPathId = AttrPathId(idep.key.keyId.value);
            auto parentRow = lookupTraceRow(parentPathId);
            if (parentRow && verifyTrace(parentRow->traceId, inputAccessors, state)) {
                auto parentTraceHash = getCurrentTraceHash(parentPathId);
                if (parentTraceHash) {
                    auto * expected = std::get_if<Blake3Hash>(&idep.hash);
                    if (expected && std::memcmp(expected->bytes.data(), parentTraceHash->hash, 32) == 0) {
                        continue;
                    }
                }
            }
            nrVerificationsFailed++;
            hasNonContentFailure = true;
            continue;
        }

        // Cache lookup: integer key, zero allocation
        auto cacheIt = currentDepHashes.find(idep.key);
        std::optional<DepHashValue> current;
        if (cacheIt != currentDepHashes.end()) {
            current = cacheIt->second;
        } else {
            auto dep = resolveDep(idep);
            current = computeCurrentHash(state, dep, inputAccessors, scope, pools.dirSets);
            currentDepHashes[idep.key] = current;
        }

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
            auto cacheIt = currentDepHashes.find(idep.key);
            std::optional<DepHashValue> current;
            if (cacheIt != currentDepHashes.end()) {
                current = cacheIt->second;
            } else {
                auto dep = resolveDep(idep);
                current = computeCurrentHash(state, dep, inputAccessors, scope, pools.dirSets);
                currentDepHashes[idep.key] = current;
            }
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
        // Build InternedDep directly — no internDeps round-trip needed
        std::vector<InternedDep> currentInterned;
        currentInterned.reserve(fullDeps.size());
        for (auto & idep : fullDeps) {
            if (depKind(idep.key.type) == DepKind::ParentContext) {
                auto parentPathId = AttrPathId(idep.key.keyId.value);
                auto parentHash = getCurrentTraceHash(parentPathId);
                if (parentHash) {
                    Blake3Hash b3;
                    std::memcpy(b3.bytes.data(), parentHash->hash, 32);
                    currentInterned.push_back({idep.key, DepHashValue(b3)});
                } else {
                    currentInterned.push_back(idep);
                }
            } else {
                auto cacheIt = currentDepHashes.find(idep.key);
                if (cacheIt != currentDepHashes.end() && cacheIt->second) {
                    currentInterned.push_back({idep.key, *cacheIt->second});
                } else {
                    currentInterned.push_back(idep);
                }
            }
        }
        sortAndDedupInterned(currentInterned);
        auto feedKey = [this](HashSink & s, DepType type, StringId id) {
            if (depKind(type) == DepKind::ParentContext)
                vocab.hashPath(s, AttrPathId(id.value));
            else
                s(resolveString(id));
        };
        auto newTraceHash = computeTraceHashFromInterned(currentInterned, feedKey);
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
    auto feedKey = [this](HashSink & s, DepType type, StringId id) {
        if (depKind(type) == DepKind::ParentContext)
            vocab.hashPath(s, AttrPathId(id.value));
        else
            s(resolveString(id));
    };

    // 4. Compute trace_hash (BLAKE3 of full sorted deps including hashes)
    auto traceHash = computeTraceHashFromInterned(interned, feedKey);

    // 5. Compute struct_hash (dep types + sources + keys, without hash values)
    auto structHash = computeStructHashFromInterned(interned, feedKey);

    // 6. Split into keys + values
    std::vector<InternedDepKey> keys;
    keys.reserve(interned.size());
    for (auto & d : interned)
        keys.push_back(d.key);

    auto keysBlob = serializeKeys(keys);
    auto valuesBlob = serializeValues(interned);

    // 7. Get or create dep key set (content-addressed by struct_hash)
    auto depKeySetId = getOrCreateDepKeySet(structHash, keysBlob);

    // 8. Encode CachedResult and intern result
    auto [type, val, ctx] = encodeCachedResult(value);
    auto resultHash = computeResultHash(type, val, ctx);
    ResultId resultId = doInternResult(type, val, ctx, resultHash);

    // 9. Get or create trace (keyed by trace_hash, stores dep_key_set_id + values_blob)
    TraceId traceId = getOrCreateTrace(traceHash, depKeySetId, valuesBlob);

    // 10. Flush pending entities to DB (IDs must exist before FK references).
    // flush() also flushes vocab entries via the ATTACH'd connection.
    flush();

    // 11. Upsert Attrs + insert History
    {
        auto st(_state->lock());
        st->upsertAttr.use()
            (contextHash)(static_cast<int64_t>(pathId.value))(static_cast<int64_t>(traceId.value))(static_cast<int64_t>(resultId.value)).exec();
        st->insertHistory.use()
            (contextHash)(static_cast<int64_t>(pathId.value))(static_cast<int64_t>(traceId.value))(static_cast<int64_t>(resultId.value)).exec();
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
        data.deps = interned;  // already vector<InternedDep> from sort/dedup
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
    debug("verify: trace validation failed for '%s', attempting constructive recovery", vocab.displayPath(pathId));
    auto result = recovery(row->traceId, pathId, inputAccessors, state);
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
    auto recoveryStart = timerStart();
    nrRecoveryAttempts++;
    VerificationScope scope;

    // Load old trace's full deps — now vector<InternedDep>
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

    // Build InternedDep directly — no round-trip through Dep + internDeps
    std::vector<InternedDep> currentInterned;
    bool allComputable = true;
    for (auto & idep : oldDeps) {
        if (depKind(idep.key.type) == DepKind::ParentContext) {
            auto parentPathId = AttrPathId(idep.key.keyId.value);
            auto parentRow = lookupTraceRow(parentPathId);
            if (!parentRow || !verifyTrace(parentRow->traceId, inputAccessors, state)) {
                allComputable = false; break;
            }
            auto parentTraceHash = getCurrentTraceHash(parentPathId);
            if (!parentTraceHash) { allComputable = false; break; }
            Blake3Hash b3;
            std::memcpy(b3.bytes.data(), parentTraceHash->hash, 32);
            currentInterned.push_back({idep.key, DepHashValue(b3)});
            continue;
        }

        auto cacheIt = currentDepHashes.find(idep.key);
        std::optional<DepHashValue> current;
        if (cacheIt != currentDepHashes.end()) {
            current = cacheIt->second;
        } else {
            auto dep = resolveDep(idep);
            current = computeCurrentHash(state, dep, inputAccessors, scope, pools.dirSets);
            currentDepHashes[idep.key] = current;
        }

        if (!current) {
            allComputable = false;
            break;
        }
        currentInterned.push_back({idep.key, *current});
    }

    debug("recovery: recomputed %d/%d dep hashes for '%s'",
          currentInterned.size(), oldDeps.size(), vocab.displayPath(pathId));

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

    // String lookup for interned hash computation
    auto feedKey = [this](HashSink & s, DepType type, StringId id) {
        if (depKind(type) == DepKind::ParentContext)
            vocab.hashPath(s, AttrPathId(id.value));
        else
            s(resolveString(id));
    };

    // === Direct hash recovery (BSàlC CT) ===
    if (allComputable) {
        auto directHashStart = timerStart();
        sortAndDedupInterned(currentInterned);
        auto newFullHash = computeTraceHashFromInterned(currentInterned, feedKey);

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

        // Build InternedDep directly using InternedDepKey
        std::vector<InternedDep> repInterned;
        bool repComputable = true;
        for (auto & key : repKeys) {
            if (isVolatile(key.type)) {
                repComputable = false;
                break;
            }

            if (depKind(key.type) == DepKind::ParentContext) {
                auto parentPathId = AttrPathId(key.keyId.value);
                auto parentRow = lookupTraceRow(parentPathId);
                if (!parentRow || !verifyTrace(parentRow->traceId, inputAccessors, state)) {
                    repComputable = false; break;
                }
                auto parentTraceHash = getCurrentTraceHash(parentPathId);
                if (!parentTraceHash) { repComputable = false; break; }
                Blake3Hash b3;
                std::memcpy(b3.bytes.data(), parentTraceHash->hash, 32);
                repInterned.push_back({key, DepHashValue(b3)});
                continue;
            }

            // Direct InternedDepKey lookup — zero allocation
            auto cacheIt = currentDepHashes.find(key);
            std::optional<DepHashValue> current;
            if (cacheIt != currentDepHashes.end()) {
                current = cacheIt->second;
            } else {
                // Resolve strings only on cache miss
                ResolvedDep syntheticDep{
                    std::string(resolveString(key.sourceId)),
                    std::string(resolveString(key.keyId)),
                    DepHashValue{Blake3Hash{}}, key.type};
                current = computeCurrentHash(state, syntheticDep, inputAccessors, scope, pools.dirSets);
                currentDepHashes[key] = current;
            }

            if (!current) {
                repComputable = false;
                break;
            }
            repInterned.push_back({key, *current});
        }
        if (!repComputable)
            continue;

        sortAndDedupInterned(repInterned);
        auto candidateFullHash = computeTraceHashFromInterned(repInterned, feedKey);

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
