#include "nix/store/tests/temp-local-store.hh"

#include "nix/store/globals.hh"
#include "nix/store/sqlite.hh"
// Needed for template specialisations. This is not good! When we
// overhaul how store configs work, this should be fixed.
#include "nix/util/args.hh"
#include "nix/util/config-impl.hh"
#include "nix/util/abstract-setting-to-json.hh"

namespace nix {

void TempLocalStoreTest::SetUp()
{
    tempStoreDir = canonPath(createTempDir(), /*resolveSymlinks=*/true);
    config = std::make_shared<LocalStoreConfig>(tempStoreDir.path(), StoreConfig::Params{});
    store = std::make_shared<LocalStore>(ref{config});
}

void TempLocalStoreTest::TearDown()
{
    /* The store first: its connection closes (and, under WAL, checkpoints
       into the main file) while the directory is still there; deleting the
       directory first would leave whatever the close writes. */
    store.reset();
    tempStoreDir.deletePath();
}

/* A second connection to the store's database. */
static SQLite openDb(LocalStore & store)
{
    return SQLite(store.dbDir / "db.sqlite", {.mode = SQLiteOpenMode::NoCreate, .useWAL = settings.useSQLiteWAL});
}

void plantSchema10Row(LocalStore & store, const StorePath & path, const Hash & narHash, uint64_t narSize)
{
    /* The row must exist: an update of no row would plant nothing. */
    readHashColumn(store, path);

    auto db = openDb(store);
    SQLiteStmt stmt{db, "update ValidPaths set hash = ?, narSize = ? where path = ?"};
    stmt.use()
        .apply(narHash.to_string(HashFormat::Base16, true))
        .apply((int64_t) narSize, narSize != 0)
        .apply(store.printStorePath(path))
        .exec();
    store.clearPathInfoCache();
}

void plantCaColumn(LocalStore & store, const StorePath & path, std::string_view ca)
{
    readHashColumn(store, path);

    auto db = openDb(store);
    SQLiteStmt stmt{db, "update ValidPaths set ca = ? where path = ?"};
    stmt.use().apply(std::string{ca}).apply(store.printStorePath(path)).exec();
    store.clearPathInfoCache();
}

std::string readHashColumn(LocalStore & store, const StorePath & path)
{
    auto db = openDb(store);
    SQLiteStmt stmt{db, "select hash from ValidPaths where path = ?"};
    auto use(stmt.use().apply(store.printStorePath(path)));
    if (!use.next())
        throw Error("no row for '%s'", store.printStorePath(path));
    return use.getStr(0);
}

} // namespace nix
