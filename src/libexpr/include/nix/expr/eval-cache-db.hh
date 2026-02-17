#pragma once

#include "nix/expr/eval-cache.hh"
#include "nix/expr/file-load-tracker.hh"
#include "nix/store/sqlite.hh"
#include "nix/util/sync.hh"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace nix {
class EvalState;
}

namespace nix::eval_cache {

std::string depTypeString(DepType type);

struct EvalCacheDb {
    struct State {
        SQLite db;
        SQLiteStmt upsertAttr;
        SQLiteStmt lookupAttr;
        SQLiteStmt getAttrDepSetId;
        SQLiteStmt getAttrParentId;
        SQLiteStmt getAttrEpoch;
        SQLiteStmt getAttrResult;
        SQLiteStmt getAttrValidationInfo;
        SQLiteStmt insertDepSet;
        SQLiteStmt lookupDepSet;
        SQLiteStmt insertDepEntry;
        SQLiteStmt getDepEntries;
        SQLiteStmt upsertRecovery;
        SQLiteStmt lookupRecovery;
        SQLiteStmt upsertStruct;
        SQLiteStmt scanStructGroups;
        SQLiteStmt getDepSetContentHash;
        std::unique_ptr<SQLiteTxn> txn;
    };
    std::unique_ptr<Sync<State>> _state;
    SymbolTable & symbols;
    int64_t contextHash;

    // Session caches
    std::set<AttrId> validatedAttrIds;
    std::set<int64_t> validatedDepSetIds;
    std::map<int64_t, std::vector<Dep>> depSetCache;

    EvalCacheDb(SymbolTable & symbols, int64_t contextHash);
    ~EvalCacheDb();

    struct WarmResult {
        AttrValue value;
        AttrId attrId;
    };

    std::optional<WarmResult> warmPath(
        std::string_view attrPath,
        const std::map<std::string, SourcePath> & inputAccessors,
        EvalState & state,
        std::optional<AttrId> parentAttrIdHint = std::nullopt);

    AttrId coldStore(
        std::string_view attrPath,
        const AttrValue & value,
        const std::vector<Dep> & directDeps,
        std::optional<AttrId> parentAttrId,
        bool isRoot);

    std::optional<WarmResult> recovery(
        AttrId oldAttrId,
        std::string_view attrPath,
        const std::map<std::string, SourcePath> & inputAccessors,
        EvalState & state,
        std::optional<AttrId> parentAttrIdHint = std::nullopt);

    std::vector<Dep> loadDepsForAttr(AttrId attrId);
    bool validateAttr(
        AttrId attrId,
        const std::map<std::string, SourcePath> & inputAccessors,
        EvalState & state);
    std::optional<AttrId> lookupAttr(std::string_view attrPath);
    void clearSessionCaches();
    static std::string buildAttrPath(const std::vector<std::string> & components);

private:
    struct AttrRow {
        AttrId attrId;
        std::optional<AttrId> parentId;
        std::optional<int64_t> parentEpoch;
        int type;
        std::string value;
        std::string context;
        std::optional<int64_t> depSetId;
    };

    std::optional<AttrRow> lookupAttrRow(std::string_view attrPath);

    int64_t getOrCreateDepSet(
        const std::vector<Dep> & sortedDeps,
        const Hash & contentHash,
        const Hash & structHash);

    bool validateDepSet(
        int64_t depSetId,
        const std::map<std::string, SourcePath> & inputAccessors,
        EvalState & state);

    std::vector<Dep> loadDepSetEntries(int64_t depSetId);

    AttrValue decodeAttrValue(const AttrRow & row);
    std::tuple<int, std::string, std::string> encodeAttrValue(const AttrValue & value);

    struct RecoveryResult {
        int64_t depSetId;
        int type;
        std::string value;
        std::string context;
    };

    std::optional<Hash> getDepContentHashForAttr(AttrId attrId);
    Hash computeIdentityHash(AttrId attrId);

    std::optional<WarmResult> tryCandidate(
        const Hash & depHash,
        std::string_view attrPath,
        const std::map<std::string, SourcePath> & inputAccessors,
        EvalState & state,
        std::set<Hash> & tried,
        std::optional<AttrId> parentAttrIdHint = std::nullopt);
};

} // namespace nix::eval_cache
