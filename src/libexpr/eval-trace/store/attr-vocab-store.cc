#include "nix/expr/eval-trace/store/attr-vocab-store.hh"
#include "nix/util/hash.hh"
#include "nix/util/users.hh"
#include "nix/util/util.hh"
#include "nix/store/globals.hh"

#include <cassert>
#include <filesystem>

namespace nix::eval_trace {

// ── Schema ───────────────────────────────────────────────────────────

static const char * vocabSchema = R"sql(
    CREATE TABLE IF NOT EXISTS AttrNames (
        id   INTEGER PRIMARY KEY,
        name TEXT NOT NULL UNIQUE
    ) STRICT;

    CREATE TABLE IF NOT EXISTS AttrPaths (
        id     INTEGER PRIMARY KEY,
        parent INTEGER NOT NULL,
        child  INTEGER NOT NULL,
        UNIQUE(parent, child)
    ) STRICT;

    -- Root sentinels (id=0)
    INSERT OR IGNORE INTO AttrNames(id, name) VALUES (0, '');
    INSERT OR IGNORE INTO AttrPaths(id, parent, child) VALUES (0, 0, 0);
)sql";

// ── Helpers ──────────────────────────────────────────────────────────

void AttrVocabStore::seedRootSentinels()
{
    nameTable.bulkLoad(0, "");
    paths.push_back({AttrPathId(0), AttrNameId(0)});  // paths[0] = root
    pathByKey[packKey(AttrPathId(0), AttrNameId(0))] = AttrPathId(0);
    nameToSymbol.push_back(symbols.create(""));
    symbolToName[symbols.create("").getId()] = AttrNameId(0);
}

// ── Constructor ──────────────────────────────────────────────────────

AttrVocabStore::AttrVocabStore(SymbolTable & symbols)
    : symbols(symbols)
    , dbPath(std::filesystem::path(getCacheDir()) / "attr-vocab.sqlite")
{
    seedRootSentinels();

    // One-shot: open DB, create schema, load data, close.
    // The persistent connection is managed by TraceStore via ATTACH.
    auto cacheDir = dbPath.parent_path();
    createDirs(cacheDir);

    SQLite db(dbPath, {.useWAL = settings.useSQLiteWAL});
    db.isCache();
    db.exec(vocabSchema);
    loadFrom(db);
    // db closes automatically when it goes out of scope
}

// ── Intern ───────────────────────────────────────────────────────────

AttrNameId AttrVocabStore::internName(std::string_view name)
{
    return nameTable.intern<AttrNameId>(name);
}

AttrNameId AttrVocabStore::internName(Symbol sym)
{
    auto symId = sym.getId();
    auto it = symbolToName.find(symId);
    if (it != symbolToName.end())
        return it->second;

    auto nameId = nameTable.intern<AttrNameId>(symbols[sym]);
    // Grow nameToSymbol vector if needed.
    if (nameId.value >= nameToSymbol.size())
        nameToSymbol.resize(nameId.value + 1);
    nameToSymbol[nameId.value] = sym;
    symbolToName[symId] = nameId;
    return nameId;
}

AttrPathId AttrVocabStore::internPath(AttrPathId parent, AttrNameId child)
{
    auto key = packKey(parent, child);
    auto it = pathByKey.find(key);
    if (it != pathByKey.end())
        return it->second;

    auto id = AttrPathId(static_cast<uint32_t>(paths.size()));
    paths.push_back({parent, child});
    pathByKey[key] = id;
    return id;
}

AttrPathId AttrVocabStore::extendPath(AttrPathId parent, Symbol child)
{
    return internPath(parent, internName(child));
}

// ── Resolution ───────────────────────────────────────────────────────

std::string_view AttrVocabStore::resolveName(AttrNameId id) const
{
    return nameTable.resolve(id);
}

AttrNameId AttrVocabStore::childName(AttrPathId id) const
{
    assert(id.value < paths.size());
    return paths[id.value].child;
}

AttrPathId AttrVocabStore::parentPath(AttrPathId id) const
{
    assert(id.value < paths.size());
    return paths[id.value].parent;
}

Symbol AttrVocabStore::childSymbol(AttrPathId id) const
{
    auto nameId = childName(id);
    assert(nameId.value < nameToSymbol.size());
    return nameToSymbol[nameId.value];
}

std::string AttrVocabStore::displayPath(AttrPathId id) const
{
    if (id == rootPath())
        return "\xC2\xABroot\xC2\xBB"; // «root» in UTF-8

    std::vector<std::string_view> components;
    auto cur = id;
    while (cur != rootPath()) {
        components.push_back(resolveName(childName(cur)));
        cur = parentPath(cur);
    }
    std::string result;
    for (auto it = components.rbegin(); it != components.rend(); ++it) {
        if (!result.empty()) result += '.';
        result += *it;
    }
    return result;
}

void AttrVocabStore::hashPath(HashSink & sink, AttrPathId pathId) const
{
    // Collect components (bottom-up walk).
    std::vector<AttrNameId> components;
    auto cur = pathId;
    while (cur != rootPath()) {
        components.push_back(childName(cur));
        cur = parentPath(cur);
    }
    // Feed in path order (reversed) with length-prefix framing.
    for (auto it = components.rbegin(); it != components.rend(); ++it) {
        auto name = resolveName(*it);
        uint32_t len = static_cast<uint32_t>(name.size());
        sink({reinterpret_cast<const char *>(&len), sizeof(len)});
        sink(name);
    }
}

// ── Lookup without interning ─────────────────────────────────────────

std::optional<AttrNameId> AttrVocabStore::lookupName(std::string_view name) const
{
    return nameTable.find<AttrNameId>(name);
}

std::optional<AttrPathId> AttrVocabStore::lookupPath(AttrPathId parent, AttrNameId child) const
{
    auto key = packKey(parent, child);
    auto it = pathByKey.find(key);
    if (it != pathByKey.end())
        return it->second;
    return std::nullopt;
}

// ── DB lifecycle ─────────────────────────────────────────────────────

void AttrVocabStore::flushTo(SQLiteStmt & insertName, SQLiteStmt & insertPath)
{
    // Flush new names (IDs > maxLoadedNameId).
    for (uint32_t i = maxLoadedNameId + 1; i < nameTable.nextId(); i++) {
        auto sv = nameTable.resolveRaw(i);
        insertName.use()(static_cast<int64_t>(i))(sv).exec();
    }
    if (nameTable.nextId() > 0)
        maxLoadedNameId = nameTable.nextId() - 1;

    // Flush new paths (IDs > maxLoadedPathId).
    for (uint32_t i = maxLoadedPathId + 1; i < static_cast<uint32_t>(paths.size()); i++) {
        auto & entry = paths[i];
        insertPath.use()
            (static_cast<int64_t>(i))
            (static_cast<int64_t>(entry.parent.value))
            (static_cast<int64_t>(entry.child.value))
            .exec();
    }
    if (!paths.empty())
        maxLoadedPathId = static_cast<uint32_t>(paths.size()) - 1;
}

void AttrVocabStore::loadFrom(SQLite & db)
{
    SQLiteStmt getAllNames, getAllPaths;
    getAllNames.create(db, "SELECT id, name FROM AttrNames");
    getAllPaths.create(db, "SELECT id, parent, child FROM AttrPaths");

    {
        auto use(getAllNames.use());
        while (use.next()) {
            auto id = static_cast<uint32_t>(use.getInt(0));
            auto name = use.getStr(1);
            nameTable.bulkLoad(id, name);
            if (id > maxLoadedNameId)
                maxLoadedNameId = id;

            Symbol sym = symbols.create(name);
            if (id >= nameToSymbol.size())
                nameToSymbol.resize(id + 1);
            nameToSymbol[id] = sym;
            symbolToName[sym.getId()] = AttrNameId(id);
        }
    }
    {
        auto use(getAllPaths.use());
        while (use.next()) {
            auto id = static_cast<uint32_t>(use.getInt(0));
            auto parent = AttrPathId(static_cast<uint32_t>(use.getInt(1)));
            auto child = AttrNameId(static_cast<uint32_t>(use.getInt(2)));

            if (id >= paths.size())
                paths.resize(id + 1);
            paths[id] = {parent, child};
            pathByKey[packKey(parent, child)] = AttrPathId(id);

            if (id > maxLoadedPathId)
                maxLoadedPathId = id;
        }
    }
}

} // namespace nix::eval_trace
