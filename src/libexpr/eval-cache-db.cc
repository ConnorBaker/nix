#include "nix/expr/eval-cache-db.hh"
#include "nix/expr/eval-result-serialise.hh"
#include "nix/expr/eval.hh"
#include "nix/store/globals.hh"
#include "nix/util/users.hh"
#include "nix/util/util.hh"
#include "nix/util/hash.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/source-accessor.hh"
#include "nix/fetchers/fetchers.hh"

#include <filesystem>

namespace nix::eval_cache {

// ── Dep validation helpers (ported from eval-cache-store.cc) ─────────

static std::optional<SourcePath> resolveDepPath(
    const Dep & dep, const std::map<std::string, SourcePath> & inputAccessors)
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
    const std::map<std::string, SourcePath> & inputAccessors)
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

// ── Dep type naming ──────────────────────────────────────────────────

std::string depTypeString(DepType type)
{
    switch (type) {
    case DepType::Content:       return "content";
    case DepType::Directory:     return "directory";
    case DepType::Existence:     return "existence";
    case DepType::EnvVar:        return "envvar";
    case DepType::CurrentTime:   return "current-time";
    case DepType::System:        return "system";
    case DepType::UnhashedFetch: return "unhashed-fetch";
    case DepType::ParentContext: return "parent-context";
    case DepType::CopiedPath:    return "copied-path";
    case DepType::Exec:          return "exec";
    case DepType::NARContent:    return "nar-content";
    }
    return "unknown";
}

// ── Schema ───────────────────────────────────────────────────────────

static const char * schema = R"sql(

    CREATE TABLE IF NOT EXISTS Attributes (
        attr_id      INTEGER PRIMARY KEY,
        context_hash INTEGER NOT NULL,
        attr_path    BLOB NOT NULL,
        parent_id    INTEGER REFERENCES Attributes(attr_id),
        parent_epoch INTEGER,
        epoch        INTEGER NOT NULL DEFAULT 0,
        type         INTEGER NOT NULL,
        value        TEXT,
        context      TEXT,
        dep_set_id   INTEGER REFERENCES DepSets(set_id),
        UNIQUE(context_hash, attr_path)
    ) STRICT;

    CREATE TABLE IF NOT EXISTS DepSets (
        set_id       INTEGER PRIMARY KEY,
        content_hash BLOB NOT NULL UNIQUE,
        struct_hash  BLOB NOT NULL
    ) STRICT;

    CREATE TABLE IF NOT EXISTS DepSetEntries (
        set_id       INTEGER NOT NULL REFERENCES DepSets(set_id),
        dep_type     INTEGER NOT NULL,
        source       TEXT NOT NULL,
        key          TEXT NOT NULL,
        hash_value   BLOB NOT NULL,
        PRIMARY KEY (set_id, dep_type, source, key)
    ) WITHOUT ROWID, STRICT;

    CREATE TABLE IF NOT EXISTS DepHashRecovery (
        context_hash INTEGER NOT NULL,
        attr_path    BLOB NOT NULL,
        dep_hash     BLOB NOT NULL,
        dep_set_id   INTEGER NOT NULL REFERENCES DepSets(set_id),
        type         INTEGER NOT NULL,
        value        TEXT,
        context      TEXT,
        PRIMARY KEY (context_hash, attr_path, dep_hash)
    ) WITHOUT ROWID, STRICT;

    CREATE TABLE IF NOT EXISTS DepStructGroups (
        context_hash INTEGER NOT NULL,
        attr_path    BLOB NOT NULL,
        struct_hash  BLOB NOT NULL,
        dep_set_id   INTEGER NOT NULL REFERENCES DepSets(set_id),
        PRIMARY KEY (context_hash, attr_path, struct_hash)
    ) WITHOUT ROWID, STRICT;

)sql";

// ── Constructor / Destructor ─────────────────────────────────────────

EvalCacheDb::EvalCacheDb(SymbolTable & symbols, int64_t contextHash)
    : symbols(symbols)
    , contextHash(contextHash)
{
    auto state = std::make_unique<Sync<State>>();
    auto st(state->lock());

    auto cacheDir = std::filesystem::path(getCacheDir());
    createDirs(cacheDir);

    auto dbPath = cacheDir / "eval-cache-v1.sqlite";

    st->db = SQLite(dbPath, {.useWAL = settings.useSQLiteWAL, .noMutex = true});
    st->db.isCache();
    st->db.exec("pragma cache_size = -4000");          // 4MB page cache
    st->db.exec("pragma mmap_size = 30000000");        // 30MB mmap
    st->db.exec("pragma temp_store = memory");
    st->db.exec("pragma journal_size_limit = 2097152"); // 2MB WAL limit
    st->db.exec(schema);

    // Attributes
    st->upsertAttr.create(st->db,
        "INSERT INTO Attributes (context_hash, attr_path, parent_id, parent_epoch, epoch, type, value, context, dep_set_id) "
        "VALUES (?, ?, ?, ?, 0, ?, ?, ?, ?) "
        "ON CONFLICT (context_hash, attr_path) DO UPDATE SET "
        "parent_id = COALESCE(excluded.parent_id, Attributes.parent_id), "
        "parent_epoch = COALESCE(excluded.parent_epoch, Attributes.parent_epoch), "
        "epoch = Attributes.epoch + 1, "
        "type = excluded.type, value = excluded.value, context = excluded.context, "
        "dep_set_id = excluded.dep_set_id");

    st->lookupAttr.create(st->db,
        "SELECT attr_id, parent_id, parent_epoch, type, value, context, dep_set_id "
        "FROM Attributes WHERE context_hash = ? AND attr_path = ?");

    st->getAttrDepSetId.create(st->db,
        "SELECT dep_set_id FROM Attributes WHERE attr_id = ?");

    st->getAttrParentId.create(st->db,
        "SELECT parent_id FROM Attributes WHERE attr_id = ?");

    st->getAttrEpoch.create(st->db,
        "SELECT epoch FROM Attributes WHERE attr_id = ?");

    st->getAttrResult.create(st->db,
        "SELECT type, value, context FROM Attributes WHERE attr_id = ?");

    st->getAttrValidationInfo.create(st->db,
        "SELECT dep_set_id, parent_id, parent_epoch FROM Attributes WHERE attr_id = ?");

    // DepSets
    st->insertDepSet.create(st->db,
        "INSERT OR IGNORE INTO DepSets (content_hash, struct_hash) VALUES (?, ?)");

    st->lookupDepSet.create(st->db,
        "SELECT set_id FROM DepSets WHERE content_hash = ?");

    // DepSetEntries
    st->insertDepEntry.create(st->db,
        "INSERT OR IGNORE INTO DepSetEntries (set_id, dep_type, source, key, hash_value) "
        "VALUES (?, ?, ?, ?, ?)");

    st->getDepEntries.create(st->db,
        "SELECT dep_type, source, key, hash_value FROM DepSetEntries WHERE set_id = ?");

    // DepHashRecovery
    st->upsertRecovery.create(st->db,
        "INSERT INTO DepHashRecovery (context_hash, attr_path, dep_hash, dep_set_id, type, value, context) "
        "VALUES (?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT (context_hash, attr_path, dep_hash) DO UPDATE SET "
        "dep_set_id = excluded.dep_set_id, type = excluded.type, "
        "value = excluded.value, context = excluded.context");

    st->lookupRecovery.create(st->db,
        "SELECT dep_set_id, type, value, context FROM DepHashRecovery "
        "WHERE context_hash = ? AND attr_path = ? AND dep_hash = ?");

    // DepStructGroups
    st->upsertStruct.create(st->db,
        "INSERT INTO DepStructGroups (context_hash, attr_path, struct_hash, dep_set_id) "
        "VALUES (?, ?, ?, ?) "
        "ON CONFLICT (context_hash, attr_path, struct_hash) DO UPDATE SET "
        "dep_set_id = excluded.dep_set_id");

    st->scanStructGroups.create(st->db,
        "SELECT dep_set_id FROM DepStructGroups "
        "WHERE context_hash = ? AND attr_path = ?");

    st->getDepSetContentHash.create(st->db,
        "SELECT content_hash FROM DepSets WHERE set_id = ?");

    st->txn = std::make_unique<SQLiteTxn>(st->db);

    _state = std::move(state);
}

EvalCacheDb::~EvalCacheDb()
{
    try {
        auto st(_state->lock());
        if (st->txn)
            st->txn->commit();
    } catch (...) {
        ignoreExceptionExceptInterrupt();
    }
}

// ── Helpers ──────────────────────────────────────────────────────────

std::string EvalCacheDb::buildAttrPath(const std::vector<std::string> & components)
{
    std::string path;
    for (size_t i = 0; i < components.size(); i++) {
        if (i > 0) path.push_back('\0');
        path.append(components[i]);
    }
    return path;
}

void EvalCacheDb::clearSessionCaches()
{
    validatedAttrIds.clear();
    validatedDepSetIds.clear();
    depSetCache.clear();
}

// ── AttrValue SQL encoding/decoding ──────────────────────────────────

std::tuple<int, std::string, std::string> EvalCacheDb::encodeAttrValue(const AttrValue & value)
{
    return std::visit(overloaded{
        [&](const std::vector<Symbol> & attrs) -> std::tuple<int, std::string, std::string> {
            std::string val;
            bool first = true;
            for (auto & sym : attrs) {
                if (!first) val.push_back('\t');
                val.append(std::string(symbols[sym]));
                first = false;
            }
            return {AttrType::FullAttrs, std::move(val), ""};
        },
        [&](const string_t & s) -> std::tuple<int, std::string, std::string> {
            std::string ctx;
            bool first = true;
            for (auto & elem : s.second) {
                if (!first) ctx.push_back(' ');
                ctx.append(elem.to_string());
                first = false;
            }
            return {AttrType::String, s.first, std::move(ctx)};
        },
        [&](const placeholder_t &) -> std::tuple<int, std::string, std::string> {
            return {AttrType::Placeholder, "", ""};
        },
        [&](const missing_t &) -> std::tuple<int, std::string, std::string> {
            return {AttrType::Missing, "", ""};
        },
        [&](const misc_t &) -> std::tuple<int, std::string, std::string> {
            return {AttrType::Misc, "", ""};
        },
        [&](const failed_t &) -> std::tuple<int, std::string, std::string> {
            return {AttrType::Failed, "", ""};
        },
        [&](bool b) -> std::tuple<int, std::string, std::string> {
            return {AttrType::Bool, b ? "1" : "0", ""};
        },
        [&](const int_t & i) -> std::tuple<int, std::string, std::string> {
            return {AttrType::Int, std::to_string(i.x.value), ""};
        },
        [&](const std::vector<std::string> & l) -> std::tuple<int, std::string, std::string> {
            std::string val;
            bool first = true;
            for (auto & s : l) {
                if (!first) val.push_back('\t');
                val.append(s);
                first = false;
            }
            return {AttrType::ListOfStrings, std::move(val), ""};
        },
        [&](const path_t & p) -> std::tuple<int, std::string, std::string> {
            return {AttrType::Path, p.path, ""};
        },
        [&](const null_t &) -> std::tuple<int, std::string, std::string> {
            return {AttrType::Null, "", ""};
        },
        [&](const float_t & f) -> std::tuple<int, std::string, std::string> {
            return {AttrType::Float, std::to_string(f.x), ""};
        },
        [&](const list_t & lt) -> std::tuple<int, std::string, std::string> {
            return {AttrType::List, std::to_string(lt.size), ""};
        },
    }, value);
}

AttrValue EvalCacheDb::decodeAttrValue(const AttrRow & row)
{
    auto type = static_cast<AttrType>(row.type);
    switch (type) {
    case AttrType::FullAttrs: {
        std::vector<Symbol> attrs;
        if (!row.value.empty()) {
            for (auto & name : tokenizeString<std::vector<std::string>>(row.value, "\t"))
                attrs.push_back(symbols.create(name));
        }
        return attrs;
    }
    case AttrType::String: {
        NixStringContext context;
        if (!row.context.empty()) {
            for (auto & elem : tokenizeString<std::vector<std::string>>(row.context, " "))
                context.insert(NixStringContextElem::parse(elem));
        }
        return string_t{row.value, std::move(context)};
    }
    case AttrType::Bool:
        return row.value != "0";
    case AttrType::Int:
        return int_t{NixInt{row.value.empty() ? 0L : std::stol(row.value)}};
    case AttrType::ListOfStrings:
        return row.value.empty()
            ? std::vector<std::string>{}
            : tokenizeString<std::vector<std::string>>(row.value, "\t");
    case AttrType::Path:
        return path_t{row.value};
    case AttrType::Null:
        return null_t{};
    case AttrType::Float:
        return float_t{row.value.empty() ? 0.0 : std::stod(row.value)};
    case AttrType::List:
        return list_t{row.value.empty() ? (size_t)0 : std::stoull(row.value)};
    case AttrType::Missing:
        return missing_t{};
    case AttrType::Misc:
        return misc_t{};
    case AttrType::Failed:
        return failed_t{};
    case AttrType::Placeholder:
        return placeholder_t{};
    default:
        throw Error("unexpected type %d in eval cache", row.type);
    }
}

// ── Dep hash BLOB helpers ────────────────────────────────────────────

static Dep readDepEntry(SQLiteStmt::Use & use)
{
    auto depType = static_cast<DepType>(use.getInt(0));
    auto source = use.getStr(1);
    auto key = use.getStr(2);

    auto [blobData, blobSize] = use.getBlob(3);
    DepHashValue hashVal;
    if (isBlake3Dep(depType) && blobSize == 64) {
        // Hex-encoded Blake3 hash stored as BLOB
        auto hexStr = std::string(static_cast<const char *>(blobData), blobSize);
        Blake3Hash h;
        for (size_t i = 0; i < 32; i++) {
            auto hi = std::stoul(hexStr.substr(i * 2, 2), nullptr, 16);
            h.bytes[i] = static_cast<uint8_t>(hi);
        }
        hashVal = h;
    } else if (isBlake3Dep(depType) && blobSize == 32) {
        // Raw 32-byte Blake3 hash
        hashVal = Blake3Hash::fromBlob(blobData, blobSize);
    } else {
        // String hash (store paths for CopiedPath/UnhashedFetch, etc.)
        hashVal = std::string(static_cast<const char *>(blobData), blobSize);
    }

    return Dep{std::move(source), std::move(key), std::move(hashVal), depType};
}

static void bindDepHashValue(SQLiteStmt::Use & use, const DepHashValue & v)
{
    auto [data, size] = blobData(v);
    use(reinterpret_cast<const unsigned char *>(data), size);
}

// ── DB operations ────────────────────────────────────────────────────

std::optional<EvalCacheDb::AttrRow> EvalCacheDb::lookupAttrRow(std::string_view attrPath)
{
    auto st(_state->lock());

    auto use(st->lookupAttr.use()
        (contextHash)
        (reinterpret_cast<const unsigned char *>(attrPath.data()), attrPath.size()));
    if (!use.next())
        return std::nullopt;

    AttrRow row;
    row.attrId = static_cast<AttrId>(use.getInt(0));
    row.parentId = use.isNull(1) ? std::nullopt : std::optional<AttrId>(static_cast<AttrId>(use.getInt(1)));
    row.parentEpoch = use.isNull(2) ? std::nullopt : std::optional<int64_t>(use.getInt(2));
    row.type = static_cast<int>(use.getInt(3));
    row.value = use.isNull(4) ? "" : use.getStr(4);
    row.context = use.isNull(5) ? "" : use.getStr(5);
    row.depSetId = use.isNull(6) ? std::nullopt : std::optional<int64_t>(use.getInt(6));
    return row;
}

std::optional<AttrId> EvalCacheDb::lookupAttr(std::string_view attrPath)
{
    auto row = lookupAttrRow(attrPath);
    if (!row) return std::nullopt;
    return row->attrId;
}

std::vector<Dep> EvalCacheDb::loadDepSetEntries(int64_t depSetId)
{
    auto it = depSetCache.find(depSetId);
    if (it != depSetCache.end())
        return it->second;

    auto st(_state->lock());
    auto use(st->getDepEntries.use()(depSetId));

    std::vector<Dep> deps;
    while (use.next())
        deps.push_back(readDepEntry(use));

    depSetCache.emplace(depSetId, deps);
    return deps;
}

std::vector<Dep> EvalCacheDb::loadDepsForAttr(AttrId attrId)
{
    int64_t depSetId;
    {
        auto st(_state->lock());
        auto use(st->getAttrDepSetId.use()(static_cast<int64_t>(attrId)));
        if (!use.next() || use.isNull(0))
            return {};
        depSetId = use.getInt(0);
    }
    // Lock released — loadDepSetEntries acquires its own lock
    return loadDepSetEntries(depSetId);
}

std::optional<Hash> EvalCacheDb::getDepContentHashForAttr(AttrId attrId)
{
    auto st(_state->lock());

    // Get dep_set_id from Attributes
    int64_t depSetId;
    {
        auto use(st->getAttrDepSetId.use()(static_cast<int64_t>(attrId)));
        if (!use.next() || use.isNull(0))
            return std::nullopt;
        depSetId = use.getInt(0);
    }

    // Get content_hash from DepSets
    {
        auto use(st->getDepSetContentHash.use()(depSetId));
        if (!use.next())
            return std::nullopt;
        auto [blobData, blobSize] = use.getBlob(0);
        auto hexStr = std::string(static_cast<const char *>(blobData), blobSize);
        return Hash::parseAny(hexStr, HashAlgorithm::SHA256);
    }
}

// Compute a hash of the parent's stored result (type, value, context).
// This is content-based and reproducible: same parent value → same hash.
static Hash computeValueHash(int type, std::string_view value, std::string_view context)
{
    HashSink sink(HashAlgorithm::SHA256);
    sink(std::string_view("T", 1));
    auto typeStr = std::to_string(type);
    sink(typeStr);
    sink(std::string_view("V", 1));
    sink(value);
    sink(std::string_view("C", 1));
    sink(context);
    return sink.finish().hash;
}

Hash EvalCacheDb::computeIdentityHash(AttrId attrId)
{
    HashSink sink(HashAlgorithm::SHA256);

    // Include value hash
    {
        auto st(_state->lock());
        auto use(st->getAttrResult.use()(static_cast<int64_t>(attrId)));
        if (use.next()) {
            auto type = static_cast<int>(use.getInt(0));
            auto val = use.isNull(1) ? "" : use.getStr(1);
            auto ctx = use.isNull(2) ? "" : use.getStr(2);
            auto valHash = computeValueHash(type, val, ctx);
            auto hex = valHash.to_string(HashFormat::Base16, false);
            sink(std::string_view("V", 1));
            sink(hex);
        }
    }

    // Include dep content hash
    auto depHash = getDepContentHashForAttr(attrId);
    if (depHash) {
        auto hex = depHash->to_string(HashFormat::Base16, false);
        sink(std::string_view("D", 1));
        sink(hex);
    }

    // Include parent's identity hash (recursive)
    std::optional<AttrId> parentId;
    {
        auto st(_state->lock());
        auto use(st->getAttrParentId.use()(static_cast<int64_t>(attrId)));
        if (use.next() && !use.isNull(0))
            parentId = static_cast<AttrId>(use.getInt(0));
    }

    if (parentId) {
        auto parentIdentity = computeIdentityHash(*parentId);
        auto hex = parentIdentity.to_string(HashFormat::Base16, false);
        sink(std::string_view("P", 1));
        sink(hex);
    }

    return sink.finish().hash;
}

int64_t EvalCacheDb::getOrCreateDepSet(
    const std::vector<Dep> & sortedDeps,
    const Hash & contentHash,
    const Hash & structHash)
{
    auto st(_state->lock());

    // Try to find existing dep set with same content hash
    auto contentHex = contentHash.to_string(HashFormat::Base16, false);
    auto structHex = structHash.to_string(HashFormat::Base16, false);

    {
        auto use(st->lookupDepSet.use()
            (reinterpret_cast<const unsigned char *>(contentHex.data()), contentHex.size()));
        if (use.next())
            return use.getInt(0);
    }

    // Insert new dep set
    st->insertDepSet.use()
        (reinterpret_cast<const unsigned char *>(contentHex.data()), contentHex.size())
        (reinterpret_cast<const unsigned char *>(structHex.data()), structHex.size())
        .exec();

    // Get the set_id (INSERT OR IGNORE may not set last_insert_rowid)
    int64_t setId;
    {
        auto use(st->lookupDepSet.use()
            (reinterpret_cast<const unsigned char *>(contentHex.data()), contentHex.size()));
        if (!use.next())
            throw Error("failed to retrieve dep set after insert");
        setId = use.getInt(0);
    }

    // Insert dep entries
    for (auto & dep : sortedDeps) {
        auto use(st->insertDepEntry.use()
            (setId)
            (static_cast<int64_t>(dep.type))
            (dep.source)
            (dep.key));
        bindDepHashValue(use, dep.expectedHash);
        use.exec();
    }

    return setId;
}

// ── Validate ─────────────────────────────────────────────────────────

bool EvalCacheDb::validateDepSet(
    int64_t depSetId,
    const std::map<std::string, SourcePath> & inputAccessors,
    EvalState & state)
{
    if (validatedDepSetIds.count(depSetId))
        return true;

    auto deps = loadDepSetEntries(depSetId);

    for (auto & dep : deps) {
        nrDepsChecked++;
        if (dep.type == DepType::CurrentTime || dep.type == DepType::Exec)
            return false;
        auto current = computeCurrentHash(state, dep, inputAccessors);
        if (!current || *current != dep.expectedHash) {
            nrDepValidationsFailed++;
            return false;
        }
    }

    validatedDepSetIds.insert(depSetId);
    return true;
}

bool EvalCacheDb::validateAttr(
    AttrId attrId,
    const std::map<std::string, SourcePath> & inputAccessors,
    EvalState & state)
{
    if (validatedAttrIds.count(attrId))
        return true;

    nrDepValidations++;

    // Get dep_set_id, parent_id, and parent_epoch in one query
    std::optional<int64_t> depSetId;
    std::optional<AttrId> parentId;
    std::optional<int64_t> storedParentEpoch;
    {
        auto st(_state->lock());
        auto use(st->getAttrValidationInfo.use()(static_cast<int64_t>(attrId)));
        if (!use.next())
            return false;
        depSetId = use.isNull(0) ? std::nullopt : std::optional<int64_t>(use.getInt(0));
        parentId = use.isNull(1) ? std::nullopt : std::optional<AttrId>(static_cast<AttrId>(use.getInt(1)));
        storedParentEpoch = use.isNull(2) ? std::nullopt : std::optional<int64_t>(use.getInt(2));
    }

    // Validate dep set
    if (!depSetId) {
        // No dep set → always invalid (empty dep sets should still have a row)
        nrDepValidationsFailed++;
        return false;
    }

    if (!validateDepSet(*depSetId, inputAccessors, state)) {
        nrDepValidationsFailed++;
        return false;
    }

    // Recursively validate parent
    if (parentId) {
        if (!validateAttr(*parentId, inputAccessors, state)) {
            debug("attr validation failed: parent invalid for attr_id=%d", attrId);
            nrDepValidationsFailed++;
            return false;
        }

        // Check that parent's current epoch matches what was stored when
        // this child was cold-stored. If the parent was cold-stored again
        // (bumping its epoch), this child's result may be stale — even if
        // the parent's deps are unchanged, its VALUE may have changed.
        if (storedParentEpoch) {
            int64_t parentCurrentEpoch = -1;
            {
                auto st(_state->lock());
                auto use(st->getAttrEpoch.use()(static_cast<int64_t>(*parentId)));
                if (use.next() && !use.isNull(0))
                    parentCurrentEpoch = use.getInt(0);
            }
            if (parentCurrentEpoch != *storedParentEpoch) {
                debug("attr validation failed: parent epoch changed for attr_id=%d "
                      "(stored=%d, current=%d)", attrId, *storedParentEpoch, parentCurrentEpoch);
                nrDepValidationsFailed++;
                return false;
            }
        }
    }

    validatedAttrIds.insert(attrId);
    nrDepValidationsPassed++;
    return true;
}

// ── Cold Store ───────────────────────────────────────────────────────

AttrId EvalCacheDb::coldStore(
    std::string_view attrPath,
    const AttrValue & value,
    const std::vector<Dep> & directDeps,
    std::optional<AttrId> parentAttrId,
    bool isRoot)
{
    // 1. Filter deps: remove ParentContext
    std::vector<Dep> storedDeps;
    for (auto & dep : directDeps) {
        if (dep.type == DepType::ParentContext) continue;
        storedDeps.push_back(dep);
    }

    // 2. Sort+dedup ONCE
    auto sortedDeps = sortAndDedupDeps(storedDeps);

    // 3. Compute hashes
    auto contentHash = computeDepContentHashFromSorted(sortedDeps);
    auto structHash = computeDepStructHashFromSorted(sortedDeps);

    // 4. Get or create dep set
    auto depSetId = getOrCreateDepSet(sortedDeps, contentHash, structHash);

    // 5. Encode AttrValue
    auto [type, val, ctx] = encodeAttrValue(value);

    // 6. Look up parent's current epoch (for parent_epoch column)
    std::optional<int64_t> parentEpochVal;
    if (parentAttrId) {
        auto st(_state->lock());
        auto use(st->getAttrEpoch.use()(static_cast<int64_t>(*parentAttrId)));
        if (use.next() && !use.isNull(0))
            parentEpochVal = use.getInt(0);
    }

    // 7. Upsert attribute (preserves attr_id via ON CONFLICT DO UPDATE)
    AttrId attrId;
    {
        auto st(_state->lock());
        auto use = st->upsertAttr.use();
        use(contextHash);
        use(reinterpret_cast<const unsigned char *>(attrPath.data()), attrPath.size());
        use(parentAttrId ? static_cast<int64_t>(*parentAttrId) : (int64_t)0, parentAttrId.has_value());
        use(parentEpochVal ? *parentEpochVal : (int64_t)0, parentEpochVal.has_value());
        use(static_cast<int64_t>(type));
        use(val);
        use(ctx);
        use(depSetId);
        use.exec();

        // Get the attr_id (upsert returns last_insert_rowid on insert, but on update
        // we need to look it up)
        auto lookup(st->lookupAttr.use()
            (contextHash)
            (reinterpret_cast<const unsigned char *>(attrPath.data()), attrPath.size()));
        if (!lookup.next())
            throw Error("failed to retrieve attribute after upsert");
        attrId = static_cast<AttrId>(lookup.getInt(0));
    }

    // 7. Recovery index writes

    // Compute parent identity hash outside lock (it acquires its own lock internally)
    std::optional<Hash> phase2Hash;
    if (parentAttrId) {
        auto parentIdentity = computeIdentityHash(*parentAttrId);
        phase2Hash = computeDepContentHashWithParentFromSorted(sortedDeps, parentIdentity);
    }

    {
        auto st(_state->lock());
        auto attrPathPtr = reinterpret_cast<const unsigned char *>(attrPath.data());
        auto attrPathLen = attrPath.size();

        // Phase 1 key: dep content hash only
        auto depHashHex = contentHash.to_string(HashFormat::Base16, false);
        st->upsertRecovery.use()
            (contextHash)
            (attrPathPtr, attrPathLen)
            (reinterpret_cast<const unsigned char *>(depHashHex.data()), depHashHex.size())
            (depSetId)
            (static_cast<int64_t>(type))
            (val)
            (ctx)
            .exec();

        // Phase 2 key: dep content hash with parent Merkle identity
        // Uses parent's identity hash (Merkle hash of value + deps + ancestors).
        // This captures the entire ancestor chain, so any change at any level
        // (value, deps, or ancestor content) produces a different Phase 2 key.
        // Unlike epoch (which only increments), identity hash is reproducible:
        // reverting to a previous state produces the same Phase 2 key.
        if (phase2Hash) {
            auto pHashHex = phase2Hash->to_string(HashFormat::Base16, false);
            st->upsertRecovery.use()
                (contextHash)
                (attrPathPtr, attrPathLen)
                (reinterpret_cast<const unsigned char *>(pHashHex.data()), pHashHex.size())
                (depSetId)
                (static_cast<int64_t>(type))
                (val)
                (ctx)
                .exec();
        }

        // Phase 3: struct group
        auto structHashHex = structHash.to_string(HashFormat::Base16, false);
        st->upsertStruct.use()
            (contextHash)
            (attrPathPtr, attrPathLen)
            (reinterpret_cast<const unsigned char *>(structHashHex.data()), structHashHex.size())
            (depSetId)
            .exec();
    }

    // 8. Session cache (skip volatile deps)
    bool hasVolatile = std::any_of(directDeps.begin(), directDeps.end(),
        [](auto & d) { return d.type == DepType::CurrentTime || d.type == DepType::Exec; });
    if (!hasVolatile) {
        validatedAttrIds.insert(attrId);
        validatedDepSetIds.insert(depSetId);
    }

    return attrId;
}

// ── Warm Path ────────────────────────────────────────────────────────

std::optional<EvalCacheDb::WarmResult> EvalCacheDb::warmPath(
    std::string_view attrPath,
    const std::map<std::string, SourcePath> & inputAccessors,
    EvalState & state,
    std::optional<AttrId> parentAttrIdHint)
{
    // 1. Lookup attribute
    auto row = lookupAttrRow(attrPath);
    if (!row)
        return std::nullopt;

    // 2. Validate deps + parent chain
    if (!validateAttr(row->attrId, inputAccessors, state)) {
        debug("warm path: validation failed for '%s', attempting recovery", std::string(attrPath));
        return recovery(row->attrId, attrPath, inputAccessors, state, parentAttrIdHint);
    }

    // 3. Decode and return
    return WarmResult{decodeAttrValue(*row), row->attrId};
}

// ── Recovery helpers ──────────────────────────────────────────────────

std::optional<EvalCacheDb::WarmResult> EvalCacheDb::tryCandidate(
    const Hash & depHash,
    std::string_view attrPath,
    const std::map<std::string, SourcePath> & inputAccessors,
    EvalState & state,
    std::set<Hash> & tried,
    std::optional<AttrId> parentAttrIdHint)
{
    if (tried.count(depHash))
        return std::nullopt;
    tried.insert(depHash);

    auto depHashHex = depHash.to_string(HashFormat::Base16, false);

    RecoveryResult rec;
    {
        auto st(_state->lock());
        auto use(st->lookupRecovery.use()
            (contextHash)
            (reinterpret_cast<const unsigned char *>(attrPath.data()), attrPath.size())
            (reinterpret_cast<const unsigned char *>(depHashHex.data()), depHashHex.size()));
        if (!use.next())
            return std::nullopt;
        rec.depSetId = use.getInt(0);
        rec.type = static_cast<int>(use.getInt(1));
        rec.value = use.isNull(2) ? "" : use.getStr(2);
        rec.context = use.isNull(3) ? "" : use.getStr(3);
    }

    // Validate the recovered dep set
    // Validate the recovered dep set
    if (!validateDepSet(rec.depSetId, inputAccessors, state))
        return std::nullopt;

    // Look up parent's current epoch if parent hint provided
    std::optional<int64_t> parentEpochVal;
    if (parentAttrIdHint) {
        auto st(_state->lock());
        auto use(st->getAttrEpoch.use()(static_cast<int64_t>(*parentAttrIdHint)));
        if (use.next() && !use.isNull(0))
            parentEpochVal = use.getInt(0);
    }

    // Update the main Attributes entry to point to recovered dep set + result
    {
        auto st(_state->lock());
        auto upsert = st->upsertAttr.use();
        upsert(contextHash);
        upsert(reinterpret_cast<const unsigned char *>(attrPath.data()), attrPath.size());
        // parent_id: use hint if provided, otherwise NULL (COALESCE preserves existing)
        upsert(parentAttrIdHint ? static_cast<int64_t>(*parentAttrIdHint) : (int64_t)0, parentAttrIdHint.has_value());
        // parent_epoch: use current parent's epoch if available
        upsert(parentEpochVal ? *parentEpochVal : (int64_t)0, parentEpochVal.has_value());
        upsert(static_cast<int64_t>(rec.type));
        upsert(rec.value);
        upsert(rec.context);
        upsert(rec.depSetId);
        upsert.exec();

        // Get the attr_id
        auto use(st->lookupAttr.use()
            (contextHash)
            (reinterpret_cast<const unsigned char *>(attrPath.data()), attrPath.size()));
        if (!use.next())
            return std::nullopt;
        auto attrId = static_cast<AttrId>(use.getInt(0));

        AttrRow recRow;
        recRow.attrId = attrId;
        recRow.type = rec.type;
        recRow.value = rec.value;
        recRow.context = rec.context;
        recRow.depSetId = rec.depSetId;

        validatedAttrIds.insert(attrId);
        return WarmResult{decodeAttrValue(recRow), attrId};
    }
}

// ── Recovery (three-phase) ───────────────────────────────────────────

std::optional<EvalCacheDb::WarmResult> EvalCacheDb::recovery(
    AttrId oldAttrId,
    std::string_view attrPath,
    const std::map<std::string, SourcePath> & inputAccessors,
    EvalState & state,
    std::optional<AttrId> parentAttrIdHint)
{
    std::set<Hash> triedCandidates;

    // ── Recompute current dep hashes from old dep keys ───────────
    bool hasVolatile = false;
    std::vector<Dep> newDeps;
    bool allComputable = false;

    // Load old deps from dep_set_id
    std::optional<int64_t> oldDepSetId;
    {
        auto st(_state->lock());
        auto use(st->getAttrDepSetId.use()(static_cast<int64_t>(oldAttrId)));
        if (use.next() && !use.isNull(0))
            oldDepSetId = use.getInt(0);
    }

    if (oldDepSetId) {
        auto oldDeps = loadDepSetEntries(*oldDepSetId);

        debug("recovery: recomputing %d dep hashes for '%s'",
              oldDeps.size(), attrPath);

        allComputable = true;
        for (auto & dep : oldDeps) {
            if (dep.type == DepType::CurrentTime || dep.type == DepType::Exec) {
                hasVolatile = true;
                break;
            }
            auto current = computeCurrentHash(state, dep, inputAccessors);
            if (!current) {
                allComputable = false;
                break;
            }
            newDeps.push_back({dep.source, dep.key, *current, dep.type});
        }

        if (hasVolatile) {
            debug("recovery: aborting for '%s' -- contains volatile dep", attrPath);
            return std::nullopt;
        }

        if (!allComputable) {
            newDeps.clear();
        }
    }

    // ── Phase 2 first (when parent hint available) ───────────────
    // Phase 2 uses parent's Merkle identity hash to discriminate child versions.
    // Must be tried before Phase 1 for children, because Phase 1
    // keys don't include parent identity and may return a stale result
    // from a different parent version.
    if (parentAttrIdHint) {
        auto parentIdentity = computeIdentityHash(*parentAttrIdHint);
        if (allComputable && !newDeps.empty()) {
            auto depHashP = computeDepContentHashWithParent(newDeps, parentIdentity);
            if (auto r = tryCandidate(depHashP, attrPath, inputAccessors, state, triedCandidates, parentAttrIdHint)) {
                debug("recovery: Phase 2 succeeded for '%s' (with deps)", attrPath);
                return r;
            }
        } else {
            auto depHashP = computeDepContentHashWithParent({}, parentIdentity);
            if (auto r = tryCandidate(depHashP, attrPath, inputAccessors, state, triedCandidates, parentAttrIdHint)) {
                debug("recovery: Phase 2 succeeded for '%s' (dep-less)", attrPath);
                return r;
            }
        }
    }

    // ── Phase 1: depHash point lookup (no parent identity) ───────
    // Skip Phase 1 for dep-less children when parent hint is available:
    // hash([]) is the same for ALL dep-less children regardless of parent,
    // so Phase 1 always finds the first-stored version. Phase 2 (with parent
    // identity) is the only way to discriminate. If Phase 2 failed, the
    // correct action is cold eval, not returning an arbitrary old version.
    bool skipPhase1 = parentAttrIdHint && newDeps.empty();
    if (allComputable && !skipPhase1) {
        auto depHash = computeDepContentHash(newDeps);
        if (auto r = tryCandidate(depHash, attrPath, inputAccessors, state, triedCandidates, parentAttrIdHint)) {
            debug("recovery: Phase 1 succeeded for '%s'", attrPath);
            return r;
        }
    }

    // ── Phase 3: struct-group scan ───────────────────────────────
    std::vector<int64_t> groupDepSetIds;
    {
        auto st(_state->lock());
        auto use(st->scanStructGroups.use()
            (contextHash)
            (reinterpret_cast<const unsigned char *>(attrPath.data()), attrPath.size()));
        while (use.next())
            groupDepSetIds.push_back(use.getInt(0));
    }
    debug("recovery: Phase 3 for '%s' -- scanning %d struct groups",
          attrPath, groupDepSetIds.size());

    for (auto groupDepSetId : groupDepSetIds) {
        auto repDeps = loadDepSetEntries(groupDepSetId);

        std::vector<Dep> groupDeps;
        bool allGroupComputable = true;
        for (auto & dep : repDeps) {
            if (dep.type == DepType::CurrentTime || dep.type == DepType::Exec) {
                allGroupComputable = false;
                break;
            }
            auto current = computeCurrentHash(state, dep, inputAccessors);
            if (!current) {
                allGroupComputable = false;
                break;
            }
            groupDeps.push_back({dep.source, dep.key, *current, dep.type});
        }
        if (!allGroupComputable)
            continue;

        // Try without parent (skip for dep-less children with parent hint,
        // same rationale as Phase 1 skip above)
        bool skipGroupPhase1 = parentAttrIdHint && groupDeps.empty();
        if (!skipGroupPhase1) {
            auto groupDepHash = computeDepContentHash(groupDeps);
            if (auto r = tryCandidate(groupDepHash, attrPath, inputAccessors, state, triedCandidates, parentAttrIdHint)) {
                debug("recovery: Phase 3 succeeded for '%s'", attrPath);
                return r;
            }
        }

        // Also try with parent identity (same as Phase 2)
        if (parentAttrIdHint) {
            auto parentIdentity = computeIdentityHash(*parentAttrIdHint);
            auto groupDepHashP = computeDepContentHashWithParent(groupDeps, parentIdentity);
            if (auto r = tryCandidate(groupDepHashP, attrPath, inputAccessors, state, triedCandidates, parentAttrIdHint)) {
                debug("recovery: Phase 3 succeeded for '%s' (with parent)", attrPath);
                return r;
            }
        }
    }

    debug("recovery: all phases failed for '%s'", attrPath);
    return std::nullopt;
}

} // namespace nix::eval_cache
