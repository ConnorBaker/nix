#include "nix/expr/eval-index-db.hh"
#include "nix/store/globals.hh"
#include "nix/util/users.hh"
#include "nix/util/util.hh"

#include <filesystem>

namespace nix::eval_cache {

static const char * schema = R"sql(

    CREATE TABLE IF NOT EXISTS EvalIndex (
        context_hash INTEGER NOT NULL,
        attr_path    BLOB NOT NULL,
        trace_path   TEXT NOT NULL,
        PRIMARY KEY (context_hash, attr_path)
    ) WITHOUT ROWID, STRICT;

    CREATE TABLE IF NOT EXISTS DepHashRecovery (
        context_hash INTEGER NOT NULL,
        attr_path    BLOB NOT NULL,
        dep_hash     BLOB NOT NULL,
        trace_path   TEXT NOT NULL,
        PRIMARY KEY (context_hash, attr_path, dep_hash)
    ) WITHOUT ROWID, STRICT;

    CREATE TABLE IF NOT EXISTS DepStructGroups (
        context_hash INTEGER NOT NULL,
        attr_path    BLOB NOT NULL,
        struct_hash  BLOB NOT NULL,
        trace_path   TEXT NOT NULL,
        PRIMARY KEY (context_hash, attr_path, struct_hash)
    ) WITHOUT ROWID, STRICT;

)sql";

EvalIndexDb::EvalIndexDb()
{
    auto state = std::make_unique<Sync<State>>();
    auto st(state->lock());

    auto indexDir = std::filesystem::path(getCacheDir()) / "nix";
    createDirs(indexDir);

    auto dbPath = indexDir / "eval-index-v2.sqlite";

    st->db = SQLite(dbPath, {.useWAL = settings.useSQLiteWAL, .noMutex = true});
    st->db.isCache();
    st->db.exec(schema);

    st->upsert.create(st->db,
        "INSERT INTO EvalIndex (context_hash, attr_path, trace_path) "
        "VALUES (?, ?, ?) "
        "ON CONFLICT (context_hash, attr_path) DO UPDATE SET "
        "trace_path = excluded.trace_path");

    st->lookup.create(st->db,
        "SELECT trace_path FROM EvalIndex "
        "WHERE context_hash = ? AND attr_path = ?");

    st->insertRecovery.create(st->db,
        "INSERT INTO DepHashRecovery (context_hash, attr_path, dep_hash, trace_path) "
        "VALUES (?, ?, ?, ?) "
        "ON CONFLICT (context_hash, attr_path, dep_hash) DO UPDATE SET "
        "trace_path = excluded.trace_path");

    st->lookupRecovery.create(st->db,
        "SELECT trace_path FROM DepHashRecovery "
        "WHERE context_hash = ? AND attr_path = ? AND dep_hash = ?");

    st->upsertStructGroup.create(st->db,
        "INSERT INTO DepStructGroups (context_hash, attr_path, struct_hash, trace_path) "
        "VALUES (?, ?, ?, ?) "
        "ON CONFLICT (context_hash, attr_path, struct_hash) DO UPDATE SET "
        "trace_path = excluded.trace_path");

    st->scanStructGroups.create(st->db,
        "SELECT trace_path FROM DepStructGroups "
        "WHERE context_hash = ? AND attr_path = ?");

    st->txn = std::make_unique<SQLiteTxn>(st->db);

    _state = std::move(state);
}

EvalIndexDb::~EvalIndexDb()
{
    try {
        auto st(_state->lock());
        if (st->txn)
            st->txn->commit();
    } catch (...) {
        ignoreExceptionExceptInterrupt();
    }
}

std::optional<EvalIndexDb::IndexEntry> EvalIndexDb::lookup(
    int64_t contextHash,
    std::string_view attrPath,
    const StoreDirConfig & store)
{
    auto st(_state->lock());

    auto use(st->lookup.use()
        (contextHash)
        (reinterpret_cast<const unsigned char *>(attrPath.data()), attrPath.size()));
    if (!use.next())
        return std::nullopt;

    auto tracePathStr = use.getStr(0);

    return IndexEntry{
        store.parseStorePath(tracePathStr),
    };
}

void EvalIndexDb::upsert(
    int64_t contextHash,
    std::string_view attrPath,
    const StorePath & tracePath,
    const StoreDirConfig & store)
{
    auto st(_state->lock());

    st->upsert.use()
        (contextHash)
        (reinterpret_cast<const unsigned char *>(attrPath.data()), attrPath.size())
        (store.printStorePath(tracePath))
        .exec();
}

void EvalIndexDb::upsertRecovery(
    int64_t contextHash,
    std::string_view attrPath,
    const Hash & depHash,
    const StorePath & tracePath,
    const StoreDirConfig & store)
{
    auto st(_state->lock());

    auto hashBytes = depHash.to_string(HashFormat::Base16, false);
    st->insertRecovery.use()
        (contextHash)
        (reinterpret_cast<const unsigned char *>(attrPath.data()), attrPath.size())
        (reinterpret_cast<const unsigned char *>(hashBytes.data()), hashBytes.size())
        (store.printStorePath(tracePath))
        .exec();
}

std::optional<StorePath> EvalIndexDb::lookupByDepHash(
    int64_t contextHash,
    std::string_view attrPath,
    const Hash & depHash,
    const StoreDirConfig & store)
{
    auto st(_state->lock());

    auto hashBytes = depHash.to_string(HashFormat::Base16, false);
    auto use(st->lookupRecovery.use()
        (contextHash)
        (reinterpret_cast<const unsigned char *>(attrPath.data()), attrPath.size())
        (reinterpret_cast<const unsigned char *>(hashBytes.data()), hashBytes.size()));
    if (!use.next())
        return std::nullopt;

    return store.parseStorePath(use.getStr(0));
}

void EvalIndexDb::upsertStructGroup(
    int64_t contextHash,
    std::string_view attrPath,
    const Hash & structHash,
    const StorePath & tracePath,
    const StoreDirConfig & store)
{
    auto st(_state->lock());

    auto hashBytes = structHash.to_string(HashFormat::Base16, false);
    st->upsertStructGroup.use()
        (contextHash)
        (reinterpret_cast<const unsigned char *>(attrPath.data()), attrPath.size())
        (reinterpret_cast<const unsigned char *>(hashBytes.data()), hashBytes.size())
        (store.printStorePath(tracePath))
        .exec();
}

std::vector<EvalIndexDb::StructGroup> EvalIndexDb::scanStructGroups(
    int64_t contextHash,
    std::string_view attrPath,
    const StoreDirConfig & store)
{
    auto st(_state->lock());

    auto use(st->scanStructGroups.use()
        (contextHash)
        (reinterpret_cast<const unsigned char *>(attrPath.data()), attrPath.size()));

    std::vector<StructGroup> groups;
    while (use.next()) {
        groups.push_back(StructGroup{
            store.parseStorePath(use.getStr(0)),
        });
    }
    return groups;
}

/**
 * Batch all cold-path index writes under a single lock acquisition.
 *
 * Steps 2 and 3 both use the same insertRecovery prepared statement
 * (same DepHashRecovery table), but with different dep_hash PK values:
 *   - Step 2: computeDepContentHash(deps)             — Phase 1 lookup key
 *   - Step 3: computeDepContentHashWithParent(deps, parent) — Phase 2 lookup key
 * These never conflict because the parent suffix makes Phase 2 hashes
 * distinct from Phase 1 hashes for the same dep set.
 *
 * Step 4 uses ON CONFLICT DO UPDATE, keeping only the latest trace per
 * struct hash. This is intentional: DepStructGroups stores one
 * representative per unique dep structure. Phase 3 recovery uses the
 * representative only to extract dep KEYS (type, source, key), then
 * recomputes current hash values and does a separate depHash point
 * lookup in DepHashRecovery to find the actual matching trace.
 */
void EvalIndexDb::coldStoreIndex(
    int64_t contextHash,
    std::string_view attrPath,
    const StorePath & tracePath,
    const Hash & depHash,
    const std::optional<std::pair<Hash, StorePath>> & parentDepHash,
    const Hash & structHash,
    const StoreDirConfig & store)
{
    auto st(_state->lock());

    auto tracePathStr = store.printStorePath(tracePath);
    auto attrPathPtr = reinterpret_cast<const unsigned char *>(attrPath.data());
    auto attrPathLen = attrPath.size();

    // 1. EvalIndex: main lookup table (context_hash, attr_path) → trace_path.
    //    Points to the latest trace; updated by recovery on success.
    st->upsert.use()
        (contextHash)
        (attrPathPtr, attrPathLen)
        (tracePathStr)
        .exec();

    // 2. DepHashRecovery Phase 1 key: hash of dep values only (no parent).
    //    For dep-less children, this is a constant hash across all commits
    //    and gets clobbered by ON CONFLICT DO UPDATE — Phase 2 handles that.
    auto depHashBytes = depHash.to_string(HashFormat::Base16, false);
    st->insertRecovery.use()
        (contextHash)
        (attrPathPtr, attrPathLen)
        (reinterpret_cast<const unsigned char *>(depHashBytes.data()), depHashBytes.size())
        (tracePathStr)
        .exec();

    // 3. DepHashRecovery Phase 2 key: hash of dep values + parent identity.
    //    Different parents produce different hashes, so dep-less children
    //    from different commits coexist instead of clobbering each other.
    if (parentDepHash) {
        auto pHashBytes = parentDepHash->first.to_string(HashFormat::Base16, false);
        st->insertRecovery.use()
            (contextHash)
            (attrPathPtr, attrPathLen)
            (reinterpret_cast<const unsigned char *>(pHashBytes.data()), pHashBytes.size())
            (tracePathStr)
            .exec();
    }

    // 4. DepStructGroups: one representative per unique dep key structure.
    //    ON CONFLICT keeps latest; the representative's dep KEYS are used
    //    by Phase 3 to recompute current hashes for a depHash point lookup.
    auto structHashBytes = structHash.to_string(HashFormat::Base16, false);
    st->upsertStructGroup.use()
        (contextHash)
        (attrPathPtr, attrPathLen)
        (reinterpret_cast<const unsigned char *>(structHashBytes.data()), structHashBytes.size())
        (tracePathStr)
        .exec();
}

} // namespace nix::eval_cache
