#pragma once

#include "nix/expr/eval-cache.hh"
#include "nix/expr/file-load-tracker.hh"
#include "nix/store/sqlite.hh"
#include "nix/util/sync.hh"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace nix {
class EvalState;
}

namespace nix::eval_cache {

std::string depTypeString(DepType type);

struct EvalCacheDb {
    struct State {
        SQLite db;

        // Strings interning
        SQLiteStmt insertString;
        SQLiteStmt lookupStringId;

        // AttrPaths interning
        SQLiteStmt insertAttrPath;
        SQLiteStmt lookupAttrPathId;

        // Results dedup
        SQLiteStmt insertResult;
        SQLiteStmt lookupResultByHash;
        SQLiteStmt getResult;

        // DepSets (delta-encoded with base_id chain, BLOB storage)
        SQLiteStmt insertDepSet;
        SQLiteStmt lookupDepSetByFullHash;
        SQLiteStmt getDepSetInfo;
        SQLiteStmt lookupDepSetByStructHash;
        SQLiteStmt updateDepSetBlob;
        SQLiteStmt getAllStrings;

        // Attrs (current state)
        SQLiteStmt lookupAttr;
        SQLiteStmt upsertAttr;

        // History (all historical results for recovery)
        SQLiteStmt insertHistory;
        SQLiteStmt lookupHistoryByDepSet;
        SQLiteStmt scanHistoryForAttr;

        std::unique_ptr<SQLiteTxn> txn;
    };
    std::unique_ptr<Sync<State>> _state;
    SymbolTable & symbols;
    int64_t contextHash;

    // Interned dep entry (string IDs instead of strings, for BLOB serialization)
    struct InternedDep {
        DepType type;
        uint32_t sourceId;
        uint32_t keyId;
        DepHashValue hash;
    };

    struct AttrRow {
        int64_t depSetId;
        int64_t resultId;
        int type;
        std::string value;
        std::string context;
    };

    // Session caches
    std::set<int64_t> validatedDepSetIds;
    std::unordered_map<std::string, int64_t> internedStrings;
    std::unordered_map<std::string, int64_t> internedAttrPaths;
    std::map<int64_t, std::vector<Dep>> fullDepSetCache;
    std::map<int64_t, Hash> depSetFullHashCache;
    std::map<int64_t, Hash> depSetStructHashCache;

    // Session string table (reverse: id -> string, for BLOB deserialization)
    std::unordered_map<int64_t, std::string> stringTable;
    bool stringTableLoaded = false;
    void ensureStringTableLoaded();

    // Current hash cache (persists across validation -> recovery within session)
    std::unordered_map<DepKey, std::optional<DepHashValue>, DepKey::Hash> currentHashCache;

    // Dirty dep sets (written this session, for post-write optimization)
    std::set<int64_t> dirtyDepSetIds;

    EvalCacheDb(SymbolTable & symbols, int64_t contextHash);
    ~EvalCacheDb();

    struct WarmResult {
        AttrValue value;
        int64_t depSetId;
    };

    struct ColdStoreResult {
        int64_t depSetId;
    };

    std::optional<WarmResult> warmPath(
        std::string_view attrPath,
        const std::map<std::string, SourcePath> & inputAccessors,
        EvalState & state,
        std::optional<int64_t> parentDepSetIdHint = std::nullopt);

    ColdStoreResult coldStore(
        std::string_view attrPath,
        const AttrValue & value,
        const std::vector<Dep> & allDeps,
        std::optional<int64_t> parentDepSetId,
        bool isRoot);

    std::optional<WarmResult> recovery(
        int64_t oldDepSetId,
        std::string_view attrPath,
        const std::map<std::string, SourcePath> & inputAccessors,
        EvalState & state,
        std::optional<int64_t> parentDepSetIdHint = std::nullopt);

    std::vector<Dep> loadFullDepSet(int64_t depSetId);
    bool attrExists(std::string_view attrPath);
    void clearSessionCaches();
    static std::string buildAttrPath(const std::vector<std::string> & components);

    void optimizeDepSets();

    // BLOB serialization for dep entries
    static std::vector<uint8_t> serializeDeps(const std::vector<InternedDep> & deps);
    static std::vector<InternedDep> deserializeInternedDeps(const void * blob, size_t size);
    std::vector<Dep> resolveDeps(const std::vector<InternedDep> & interned);
    std::vector<InternedDep> internDeps(const std::vector<Dep> & deps);

    bool validateDepSet(
        int64_t depSetId,
        const std::map<std::string, SourcePath> & inputAccessors,
        EvalState & state);

private:
    std::optional<AttrRow> lookupAttrRow(std::string_view attrPath);

    int64_t doInternString(std::string_view s);
    int64_t doInternAttrPath(std::string_view path);
    int64_t doInternResult(int type, const std::string & value,
                           const std::string & context, const Hash & resultHash);

    int64_t getOrCreateDepSet(
        const std::vector<Dep> & fullDeps,
        const std::vector<InternedDep> & deltaDeps,
        const Hash & fullHash,
        const Hash & structHash,
        std::optional<int64_t> baseId);

    std::vector<Dep> loadDepSetDelta(int64_t depSetId);

    static std::vector<Dep> computeDelta(
        const std::vector<Dep> & fullDeps,
        const std::vector<Dep> & baseDeps);

    AttrValue decodeAttrValue(const AttrRow & row);
    std::tuple<int, std::string, std::string> encodeAttrValue(const AttrValue & value);

    Hash getDepSetFullHash(int64_t depSetId);
    std::optional<int64_t> getDepSetBaseId(int64_t depSetId);
};

} // namespace nix::eval_cache
