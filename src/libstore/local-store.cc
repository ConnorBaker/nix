#include "nix/store/local-store.hh"
#include "nix/store/build.hh"
#include "nix/store/globals.hh"
#include "nix/store/path-references.hh"
#include "nix/util/archive.hh"
#include "nix/util/object-hash.hh"
#include "nix/util/object-hash-sink.hh"
#include "nix/store/pathlocks.hh"
#include "nix/store/worker-protocol.hh"
#include "nix/store/derivations.hh"
#include "nix/store/realisation.hh"
#include "nix/store/references.hh"
#include "nix/util/callback.hh"
#include "nix/util/topo-sort.hh"
#include "nix/util/compression.hh"
#include "nix/util/signals.hh"
#include "nix/store/posix-fs-canonicalise.hh"
#include "nix/util/source-accessor.hh"
#include "nix/store/keys.hh"
#include "nix/util/users.hh"
#include "nix/store/store-registration.hh"

#include <algorithm>
#include <cstring>

#include <memory>
#include <new>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <utime.h>
#include <fcntl.h>
#include <stdio.h>
#include <time.h>
#include <variant>

#ifndef _WIN32
#  include <grp.h>
#endif

#ifdef __linux__
#  include "nix/util/linux-namespaces.hh"
#endif

#ifdef __CYGWIN__
#  include <windows.h>
#endif

#include <sqlite3.h>

#include <nlohmann/json.hpp>

#include "nix/util/strings.hh"

#include "store-config-private.hh"

namespace nix {

void LocalStoreConfig::anchor() {}

void LocalBuildStoreConfig::anchor() {}

void LocalStore::anchor() {}

void GcStore::anchor() {}

LocalStoreConfig::LocalStoreConfig(const std::filesystem::path & path, const Params & params)
    : StoreConfig(params, FilePathType::Native)
    , LocalFSStoreConfig(path, params)
{
}

std::string LocalStoreConfig::doc()
{
    return
#include "local-store.md"
        ;
}

const LocalSettings & LocalBuildStoreConfig::getLocalSettings() const

{
    return settings.getLocalSettings();
}

std::filesystem::path LocalBuildStoreConfig::getBuildDir() const
{
    auto & bd = getLocalSettings().buildDir.get();
    return bd.has_value()               ? *bd
           : buildDir.get().has_value() ? *buildDir.get()
                                        : AbsolutePath{stateDir.get() / "builds"};
}

ref<Store> LocalStore::Config::openStore() const
{
    return make_ref<LocalStore>(ref{shared_from_this()});
}

bool LocalStoreConfig::getDefaultRequireSigs()
{
    return settings.requireSigs;
}

/* The object store's index keys the blob by its identifier's 32 bytes, not by
   the hexadecimal of them: half the key, so half the b-tree pages and half the
   comparison work at a real store's size (`05` section 6). */
static Hash blobIdOf(std::string_view bytes)
{
    Hash id(GitObjectStore::hashAlgo);
    if (bytes.size() != id.hashSize)
        throw Error("blob index row holds a %d-byte identifier, not %d", bytes.size(), id.hashSize);
    memcpy(id.hash, bytes.data(), id.hashSize);
    return id;
}

struct LocalStore::State::Stmts
{
    /* Some precompiled SQLite statements. */
    SQLiteStmt RegisterValidPath;
    SQLiteStmt UpdatePathInfo;
    SQLiteStmt AddReference;
    SQLiteStmt QueryPathInfo;
    SQLiteStmt QueryValidPathId;
    SQLiteStmt QueryReferences;
    SQLiteStmt QueryReferrers;
    SQLiteStmt InvalidatePath;
    SQLiteStmt AddDerivationOutput;
    SQLiteStmt RegisterRealisedOutput;
    SQLiteStmt UpdateRealisedOutput;
    SQLiteStmt DeleteRealisedOutputByName;
    SQLiteStmt QueryValidDerivers;
    SQLiteStmt QueryDerivationOutputs;
    SQLiteStmt QueryRealisedOutput;
    SQLiteStmt QueryPathFromHashPart;
    SQLiteStmt QueryValidPaths;
    SQLiteStmt QueryValidObjectHashes;
    /* The blob index (`06-holder-index.md`). */
    SQLiteStmt LookupBlob;
    SQLiteStmt InsertBlob;
    SQLiteStmt DeleteBlob;
    SQLiteStmt QueryBlobInodes;
    SQLiteStmt QueryBlobCollisions;
    SQLiteStmt QueryHoldIngestions;
    SQLiteStmt DeletePathBlobs;
    SQLiteStmt DeletePathIngestions;
    SQLiteStmt NewIngestion;
    SQLiteStmt SetIngestionPath;
    SQLiteStmt QueryStaleIngestions;
    SQLiteStmt DeleteBlobsOf;
    SQLiteStmt DeleteIngestion;
};

static constexpr auto lookupBlobSql =
    "select b.ino, b.dev, b.size, i.path, b.name, b.ingestion from Blobs b"
    " join Ingestions i on i.id = b.ingestion join ValidPaths v on v.path = i.path"
    " where b.id = ? and b.exec = ? and (b.dev, b.ino, b.ingestion) > (?, ?, ?);";

LocalStore::LocalStore(ref<const Config> config)
    : Store{*config}
    , LocalFSStore{*config}
    , config{config}
    , _state(make_ref<Sync<State>>())
    , dbDir(config->stateDir.get() / "db")
    , objects(
          config->realStoreDir.get() / ".objects",
          config->getLocalSettings().fsyncStorePaths.get(),
          config->stateDir.get() / "object-gc.lock")
    , reservedPath(dbDir / "reserved")
    , schemaPath(dbDir / "schema")
    , tempRootsDir(config->stateDir.get() / "temproots")
{
    auto state(_state->lock());
    state->stmts = std::make_unique<State::Stmts>();

    /* Create missing state directories if they don't already exist. */
    createDirs(config->realStoreDir.get());
    if (config->readOnly) {
        experimentalFeatureSettings.require(Xp::ReadOnlyLocalStore);
    } else {
        makeStoreWritable();
        /* The object store is the store's representation: every path added
           is entered, so its directories exist from the start. */
        objects.createDirectories();
    }
    auto profilesDir = config->stateDir.get() / "profiles";
    createDirs(profilesDir);
    createDirs(tempRootsDir);
    createDirs(dbDir);
    auto gcRootsDir = config->stateDir.get() / "gcroots";
    const auto & localSettings = config->getLocalSettings();
    const auto & gcSettings = localSettings.getGCSettings();
    createDirs(gcRootsDir);

    for (auto & perUserDir : {profilesDir / "per-user", gcRootsDir / "per-user"}) {
        createDirs(perUserDir);
        if (!config->readOnly) {
            // Skip chmod call if the directory already has the correct permissions (0755).
            // This is to avoid failing when the executing user lacks permissions to change the directory's permissions
            // even if it would be no-op.
            chmodIfNeeded(perUserDir, 0755, S_IRWXU | S_IRWXG | S_IRWXO);
        }
    }

#ifndef _WIN32
    /* Optionally, create directories and set permissions for a
       multi-user install. */
    if (isRootUser() && localSettings.buildUsersGroup != "") {
        mode_t perm = 01775;

        struct group * gr = getgrnam(localSettings.buildUsersGroup.get().c_str());
        if (!gr)
            printError(
                "warning: the group '%1%' specified in 'build-users-group' does not exist",
                localSettings.buildUsersGroup);
        else if (!config->readOnly) {
            auto st = stat(config->realStoreDir.get());

            if (st.st_uid != 0 || st.st_gid != gr->gr_gid || (st.st_mode & ~S_IFMT) != perm) {
                chown(config->realStoreDir.get(), 0, gr->gr_gid);
                chmod(config->realStoreDir.get(), perm);
            }
        }
    }
#endif

    /* Ensure that the store and its parents are not symlinks. */
    if (!localSettings.allowSymlinkedStore) {
        std::filesystem::path path = config->realStoreDir.get();
        std::filesystem::path root = path.root_path();
        while (path != root) {
            if (std::filesystem::is_symlink(path))
                throw Error(
                    "the path %1% is a symlink; "
                    "this is not allowed for the Nix store and its parent directories",
                    PathFmt(path));
            path = path.parent_path();
        }
    }

    /* We can't open a SQLite database if the disk is full.  Since
       this prevents the garbage collector from running when it's most
       needed, we reserve some dummy space that we can free just
       before doing a garbage collection. */
    try {
        auto st = maybeStat(reservedPath);
        if (!st || st->st_size != gcSettings.reservedSize) {
            AutoCloseFD fd = toDescriptor(open(
                reservedPath.string().c_str(),
                O_WRONLY | O_CREAT
#ifndef _WIN32
                    | O_CLOEXEC
#endif
                ,
                0600));
            int res = -1;
#if HAVE_POSIX_FALLOCATE
            res = posix_fallocate(fd.get(), 0, gcSettings.reservedSize);
#endif
            if (res != 0) {
                writeFull(fd.get(), std::string(gcSettings.reservedSize, 'X'));
                [[gnu::unused]] auto res2 =

#ifdef _WIN32
                    SetEndOfFile(fd.get())
#else
                    ftruncate(fd.get(), gcSettings.reservedSize)
#endif
                    ;
            }
        }
    } catch (SystemError & e) { /* don't care about errors */
    }

    /* Acquire the big fat lock in shared mode to make sure that no
       schema upgrade is in progress. */
    if (!config->readOnly) {
        auto globalLockPath = dbDir / "big-lock";
        try {
            globalLock = openLockFile(globalLockPath, true);
        } catch (SystemError & e) {
            if (e.is(std::errc::permission_denied) || e.is(std::errc::operation_not_permitted)) {
                e.addTrace(
                    {},
                    "This command may have been run as non-root in a single-user Nix installation,\n"
                    "or the Nix daemon may have crashed.");
            }
            throw;
        }
    }

    if (!config->readOnly && !lockFile(globalLock.get(), ltRead, false)) {
        printInfo("waiting for the big Nix store lock...");
        lockFile(globalLock.get(), ltRead, true);
    }

    /* Check the current database schema and if necessary do an
       upgrade.  */
    int curSchema = getSchema();
    /* A schema-10 store is readable as it is (`nixSchemaVersion`). */
    const int oldestReadOnlySchema = 10;
    if (config->readOnly && curSchema < oldestReadOnlySchema) {
        debug("current schema version: %d", curSchema);
        debug("supported schema version: %d", nixSchemaVersion);
        throw Error(
            curSchema == 0 ? "database does not exist, and cannot be created in read-only mode"
                           : "database schema needs migrating, but this cannot be done in read-only mode");
    }

    auto acquireWriteLock = [&]() {
        if (!lockFile(globalLock.get(), ltWrite, false)) {
            printInfo("waiting for exclusive access to the Nix store...");
            // We have acquired a shared lock; release it to prevent deadlocks.
            // This can happen if someone else is trying to promote their read
            // lock into a write lock.
            lockFile(globalLock.get(), ltNone, false);
            lockFile(globalLock.get(), ltWrite, true);
        }
    };

    if (curSchema > nixSchemaVersion)
        throw Error("current Nix store schema is version %1%, but I only support %2%", curSchema, nixSchemaVersion);

    else if (curSchema == 0) { /* new store */
        curSchema = nixSchemaVersion;
        openDB(*state, true);
        writeFile(schemaPath, fmt("%1%", curSchema), 0666, FsSync::Yes);
    }

    else if (curSchema < nixSchemaVersion && !config->readOnly) {
        if (curSchema < 5)
            throw Error(
                "Your Nix store has a database in Berkeley DB format,\n"
                "which is no longer supported. To convert to the new format,\n"
                "please upgrade Nix to version 0.12 first.");

        if (curSchema < 6)
            throw Error(
                "Your Nix store has a database in flat file format,\n"
                "which is no longer supported. To convert to the new format,\n"
                "please upgrade Nix to version 1.11 first.");

        acquireWriteLock();

        /* Get the schema version again, because another process may
           have performed the upgrade already. */
        curSchema = getSchema();

        openDB(*state, false);

        /* Legacy database schema migrations. Don't bump 'schema' for
           new migrations; instead, add a migration to
           upgradeDBSchema().  Schema 11 is the exception, and runs no
           statement here (`nixSchemaVersion`). */

        if (curSchema < 8) {
            SQLiteTxn txn(state->db);
            state->db.exec("alter table ValidPaths add column ultimate integer");
            state->db.exec("alter table ValidPaths add column sigs text");
            txn.commit();
        }

        if (curSchema < 9) {
            SQLiteTxn txn(state->db);
            state->db.exec("drop table FailedPaths");
            txn.commit();
        }

        if (curSchema < 10) {
            SQLiteTxn txn(state->db);
            state->db.exec("alter table ValidPaths add column ca text");
            txn.commit();
        }

        writeFile(schemaPath, fmt("%1%", nixSchemaVersion), 0666, FsSync::Yes);

        // Downgrade to a read lock and hold to prevent other processes from
        // upgrading the schema while we're using the store
        lockFile(globalLock.get(), ltRead, true);
    }

    else
        openDB(*state, false);

    if (!config->readOnly && upgradeDBSchema(*state, true)) {
        acquireWriteLock();
        upgradeDBSchema(*state, false);
        // Downgrade to a read lock and hold to prevent other processes from
        // upgrading the schema while we're using the store
        lockFile(globalLock.get(), ltRead, true);
    }

    /* Prepare SQL statements. */
    state->stmts->RegisterValidPath.create(
        state->db,
        "insert into ValidPaths (path, hash, registrationTime, deriver, narSize, ultimate, sigs, ca) values (?, ?, ?, ?, ?, ?, ?, ?);");
    state->stmts->UpdatePathInfo.create(
        state->db, "update ValidPaths set narSize = ?, hash = ?, ultimate = ?, sigs = ?, ca = ? where path = ?;");
    state->stmts->AddReference.create(state->db, "insert or replace into Refs (referrer, reference) values (?, ?);");
    state->stmts->QueryPathInfo.create(
        state->db,
        "select id, hash, registrationTime, deriver, narSize, ultimate, sigs, ca from ValidPaths where path = ?;");
    /* Validity and reference lookup need only the row id, which the unique
       path index already contains; the full description lives in the table. */
    state->stmts->QueryValidPathId.create(state->db, "select id from ValidPaths where path = ?;");
    state->stmts->QueryReferences.create(
        state->db, "select path from Refs join ValidPaths on reference = id where referrer = ?;");
    state->stmts->QueryReferrers.create(
        state->db,
        "select path from Refs join ValidPaths on referrer = id where reference = (select id from ValidPaths where path = ?);");
    state->stmts->InvalidatePath.create(state->db, "delete from ValidPaths where path = ?;");
    state->stmts->AddDerivationOutput.create(
        state->db, "insert or replace into DerivationOutputs (drv, id, path) values (?, ?, ?);");
    state->stmts->QueryValidDerivers.create(
        state->db, "select v.id, v.path from DerivationOutputs d join ValidPaths v on d.drv = v.id where d.path = ?;");
    state->stmts->QueryDerivationOutputs.create(state->db, "select id, path from DerivationOutputs where drv = ?;");
    // Use "path >= ?" with limit 1 rather than "path like '?%'" to
    // ensure efficient lookup.
    state->stmts->QueryPathFromHashPart.create(state->db, "select path from ValidPaths where path >= ? limit 1;");
    state->stmts->QueryValidPaths.create(state->db, "select path from ValidPaths");
    state->stmts->QueryValidObjectHashes.create(state->db, "select hash from ValidPaths");
    state->stmts->LookupBlob.create(state->db, lookupBlobSql);
    state->stmts->InsertBlob.create(
        state->db,
        "insert or replace into Blobs (id, exec, ino, dev, ingestion, name, size) values (?, ?, ?, ?, ?, ?, ?);");
    state->stmts->DeleteBlob.create(
        state->db,
        "delete from Blobs where id = ? and exec = ? and ingestion = ? and ino = ? and dev = ? and size = ? and name = ?;");
    /* Only the first registered candidate is canonical to the optimiser's
       inode shortcut. Other candidates must reach place() to coalesce. */
    state->stmts->QueryBlobInodes.create(
        state->db,
        "select b.ino, b.id from Blobs b join Ingestions i on i.id = b.ingestion "
        "join ValidPaths v on v.path = i.path where not exists ("
        "select 1 from Blobs earlier join Ingestions ei on ei.id = earlier.ingestion "
        "join ValidPaths ev on ev.path = ei.path "
        "where earlier.id = b.id and earlier.exec = b.exec"
        " and (earlier.dev, earlier.ino, earlier.ingestion) < (b.dev, b.ino, b.ingestion));");
    state->stmts->QueryBlobCollisions.create(
        state->db,
        "select own.id, own.exec, own.ino, own.dev, own.size, own.name,"
        " donor.ino, donor.dev, donor.size, i.path, donor.name, donor.ingestion"
        " from Blobs own join Blobs donor on donor.id = own.id and donor.exec = own.exec"
        " join Ingestions i on i.id = donor.ingestion join ValidPaths v on v.path = i.path"
        " where own.ingestion = ? and donor.ingestion != own.ingestion"
        " and exists (select 1 from Blobs other join Ingestions oi on oi.id = other.ingestion"
        " join ValidPaths ov on ov.path = oi.path where other.id = own.id and other.exec = own.exec"
        " and (other.ino != own.ino or other.dev != own.dev))"
        " order by own.id, own.exec, donor.dev, donor.ino, donor.ingestion;");
    state->stmts->QueryHoldIngestions.create(
        state->db, "select id from Ingestions where path = ? and hold = ? order by id desc limit 1;");
    state->stmts->DeletePathBlobs.create(
        state->db, "delete from Blobs where ingestion in (select id from Ingestions where path = ?);");
    state->stmts->DeletePathIngestions.create(state->db, "delete from Ingestions where path = ?;");
    state->stmts->NewIngestion.create(state->db, "insert into Ingestions (path, hold) values (?, ?);");
    state->stmts->SetIngestionPath.create(state->db, "update Ingestions set path = ? where id = ?;");
    /* The sweep's stale-row rule (schema.sql): a row whose ingestion never
       got its path, or whose path is no longer valid, names no holder. */
    /* An ingestion still in flight -- no path yet, its hold directory still
       under `tmp/` -- is not stale: its rows must survive a collection running
       beside it as its objects do (`08` K16).  The sweep has already removed
       the holds of dead ingestions before this runs, so a hold that is still
       there is a live one.  `?` is the object store's `tmp` directory. */
    state->stmts->QueryStaleIngestions.create(
        state->db, "select id, hold from Ingestions where path is null or path not in (select path from ValidPaths);");
    state->stmts->DeleteBlobsOf.create(state->db, "delete from Blobs where ingestion = ?;");
    state->stmts->DeleteIngestion.create(state->db, "delete from Ingestions where id = ?;");
    if (experimentalFeatureSettings.isEnabled(Xp::CaDerivations)) {
        state->stmts->RegisterRealisedOutput.create(
            state->db,
            R"(
                insert into BuildTraceV3 (drvPath, outputName, outputPath, signatures)
                values (?, ?, ?, ?)
                ;
            )");
        state->stmts->UpdateRealisedOutput.create(
            state->db,
            R"(
                update BuildTraceV3
                    set signatures = ?
                where
                    drvPath = ? and
                    outputName = ?
                ;
            )");
        state->stmts->DeleteRealisedOutputByName.create(
            state->db,
            R"(
                delete from BuildTraceV3
                where
                    drvPath = ? and
                    outputName = ?
                ;
            )");
        state->stmts->QueryRealisedOutput.create(
            state->db,
            R"(
                select id, outputPath, signatures from BuildTraceV3
                    where drvPath = ? and outputName = ?
                    ;
            )");
    }

    /* The object store's index of
       blob to holder.  Installed here, after the statements exist; a bare
       `GitObjectStore` (a test's) has none and misses every lookup. */
    /* The ablation, test-only (`local-store.hh`, `BlobRows`). */
    if (auto v = getEnv("_NIX_TEST_HOLDER_NO_ROWS"); v && *v == "1")
        blobRows = BlobRows::NoRows;
    blobIndex.lookup = [this](const Hash & id, bool executable, GitObjectStore::Holder::Cursor cursor)
        -> std::optional<GitObjectStore::Holder> {
        return retrySQLite<std::optional<GitObjectStore::Holder>>([&]() -> std::optional<GitObjectStore::Holder> {
            auto state(_state->lock());
            auto use(state->stmts->LookupBlob.use());
            /* The identifier is a 32-byte blob, not its hexadecimal: half
               the key bytes, so half the b-tree pages and half the
               comparisons (`05` section 6, the scale tables). */
            use.apply(id.hash, id.hashSize)
                .apply(int64_t(executable ? 1 : 0))
                .apply(std::get<0>(cursor))
                .apply(std::get<1>(cursor))
                .apply(static_cast<int64_t>(std::get<2>(cursor)));
            std::optional<GitObjectStore::Holder> holder;
            std::tuple<int64_t, int64_t, int64_t> minimum;
            /* Select the same physical minimum without opening SQLite's
               temporary sorter on every miss. Compare signed database
               coordinates before converting to platform inode types. */
            while (use.next()) {
                auto candidate = std::tuple{use.getInt(1), use.getInt(0), use.getInt(5)};
                if (holder && candidate >= minimum)
                    continue;
                minimum = candidate;
                holder = GitObjectStore::Holder{
                    .ino = static_cast<ino_t>(std::get<1>(candidate)),
                    .dev = static_cast<dev_t>(std::get<0>(candidate)),
                    .size = static_cast<uint64_t>(use.getInt(2)),
                    /* The column holds the *logical* store path; the holder is
                       a file on disk under the real store directory. */
                    .file = (getRealStoreDir() / std::filesystem::path(use.getStr(3)).filename()).string(),
                    .ingestion = static_cast<uint64_t>(std::get<2>(candidate)),
                };
                auto name = use.getStr(4);
                if (!name.empty()) {
                    holder->file += '/';
                    holder->file += name;
                }
            }
            return holder;
        });
    };
    blobIndex.forget = [this](const Hash & id, bool executable, const GitObjectStore::Holder & holder) {
        if (this->config->readOnly)
            return;
        /* Recover the recorded name from the physical holder path, dropping
           the store directory and its first component (the store path). */
        auto relative = std::filesystem::path(holder.file).lexically_relative(getRealStoreDir());
        std::filesystem::path name;
        for (auto i = ++relative.begin(); i != relative.end(); ++i)
            name /= *i;
        retrySQLite<void>([&]() {
            auto state(_state->lock());
            auto use(state->stmts->DeleteBlob.use());
            use.apply(id.hash, id.hashSize)
                .apply(int64_t(executable ? 1 : 0))
                .apply(static_cast<int64_t>(holder.ingestion))
                .apply(static_cast<int64_t>(holder.ino))
                .apply(static_cast<int64_t>(holder.dev))
                .apply(static_cast<int64_t>(holder.size))
                .apply(name.string())
                .exec();
        });
    };
    blobIndex.inodes = [this](boost::unordered_flat_map<ino_t, Hash> & inodes) {
        retrySQLite<void>([&]() {
            auto state(_state->lock());
            auto use(state->stmts->QueryBlobInodes.use());
            while (use.next())
                inodes.insert_or_assign(static_cast<ino_t>(use.getInt(0)), blobIdOf(use.getBlobStr(1)));
        });
    };
    objects.index = &blobIndex;
}

void LocalStore::beginIngestion(const GitObjectStore::Hold & hold, const std::filesystem::path & root)
{
    if (config->readOnly || blobRows == BlobRows::NoRows)
        return;
    hold.setRoot(root);
    /* During copying an ingestion needs a database identity only when
       rows flush. Small copies stage their rows in the final transaction.
       Completion also names an empty projection, so replacing a path with
       no eligible regular files retires its previous candidate rows. */
    auto holdName = hold.directory().filename().string();
    auto ingestion = std::make_shared<GitObjectStore::Hold::Ingestion>();
    /* The copy's workers hand their batches here as they fill, so the index
       write overlaps the copy as the object store's links used to (`05`
       section 6, the twenty-trial table). */
    hold.setSink([this, ingestion, holdName](std::vector<GitObjectStore::Hold::Row> && batch) {
        writeBlobRows(*ingestion, holdName, batch);
    });
    hold.ingestion = std::move(ingestion);
}

void LocalStore::openBlobWriter()
{
    if (blobWriterDb)
        return;
    /* The same database file, a second connection: WAL admits one writer
       beside its readers, so the index's writes and the copy's probes stop
       excluding each other (`local-store.hh`, `blobWriterDb`).  `synchronous`
       and the journal are the store's own; the connection is this process's
       and is closed with the store. */
    blobWriterDb = std::make_unique<SQLite>(
        dbDir / "db.sqlite", SQLiteSettings{.mode = SQLiteOpenMode::NoCreate, .useWAL = settings.useSQLiteWAL});
    /* The index is a cache -- every row is derivable from the store's own
       paths, and `nix-store --optimise` rebuilds it -- so its writes need not
       be synchronous. Keep the store's journal mode: `isCache()` would
       force WAL even when useSQLiteWAL is false. */
    blobWriterDb->exec("pragma synchronous = off");
    blobWriterInsert = std::make_unique<SQLiteStmt>();
    blobWriterInsert->create(
        *blobWriterDb,
        "insert or replace into Blobs (id, exec, ino, dev, ingestion, name, size) values (?, ?, ?, ?, ?, ?, ?);");
    blobWriterNewIngestion = std::make_unique<SQLiteStmt>();
    blobWriterNewIngestion->create(*blobWriterDb, "insert into Ingestions (path, hold) values (?, ?);");
}

void LocalStore::writeBlobRows(
    GitObjectStore::Hold::Ingestion & ingestion,
    const std::string & holdName,
    std::vector<GitObjectStore::Hold::Row> & batch)
{
    if (batch.empty())
        return;
    /* In key order: a b-tree takes sorted inserts far better than random
       ones, and an identifier is a hash (`05` section 6, the memo's rows). */
    std::sort(batch.begin(), batch.end(), [](const auto & a, const auto & b) {
        auto c = memcmp(a.id.hash, b.id.hash, a.id.hashSize);
        return c != 0 ? c < 0 : a.executable < b.executable;
    });
    auto lock = std::lock_guard{blobWriterMutex};
    openBlobWriter();
    retrySQLite<void>([&]() {
        SQLiteTxn txn(*blobWriterDb);
        auto id = ingestion.id;
        if (!id) {
            auto use(blobWriterNewIngestion->use());
            use.apply(ingestion.path.value_or(""), ingestion.path.has_value()).apply(holdName).exec();
            id = static_cast<uint64_t>(sqlite3_last_insert_rowid(*blobWriterDb));
        }
        for (auto & row : batch) {
            auto use(blobWriterInsert->use());
            use.apply(row.id.hash, row.id.hashSize)
                .apply(int64_t(row.executable ? 1 : 0))
                .apply(static_cast<int64_t>(row.ino))
                .apply(static_cast<int64_t>(row.dev))
                .apply(static_cast<int64_t>(*id))
                .apply(row.name)
                .apply(static_cast<int64_t>(row.size))
                .exec();
        }
        txn.commit();
        ingestion.id = id;
    });
}

void LocalStore::insertBlobRows(State & state, uint64_t ingestion, const std::vector<GitObjectStore::Hold::Row> & rows)
{
    for (auto & row : rows)
        state.stmts->InsertBlob.use()
            .apply(row.id.hash, row.id.hashSize)
            .apply(int64_t(row.executable ? 1 : 0))
            .apply(static_cast<int64_t>(row.ino))
            .apply(static_cast<int64_t>(row.dev))
            .apply(static_cast<int64_t>(ingestion))
            .apply(row.name)
            .apply(static_cast<int64_t>(row.size))
            .exec();
}

void LocalStore::coalesceBlobRows(
    State & state,
    const StorePath & path,
    const GitObjectStore::Hold & hold,
    uint64_t ingestion,
    std::vector<GitObjectStore::Hold::Row> & rows)
{
    /* The caller has already written and keeps this ingestion unnamed.
       Therefore every donor is published, and GC cannot invalidate one
       between this query and pinning its file. No collision means no walk. */
    std::vector<BlobCoalescing> collisions;
    {
        auto use(state.stmts->QueryBlobCollisions.use());
        use.apply(static_cast<int64_t>(ingestion));
        while (use.next()) {
            auto id = blobIdOf(use.getBlobStr(0));
            bool executable = use.getInt(1) != 0;
            if (collisions.empty() || collisions.back().row.id != id || collisions.back().row.executable != executable)
                collisions.push_back({
                    .row =
                        {id,
                         executable,
                         static_cast<ino_t>(use.getInt(2)),
                         static_cast<dev_t>(use.getInt(3)),
                         static_cast<uint64_t>(use.getInt(4)),
                         use.getStr(5)},
                    .candidates = {},
                });
            auto file = getRealStoreDir() / std::filesystem::path(use.getStr(9)).filename();
            auto name = use.getStr(10);
            if (!name.empty())
                file /= name;
            collisions.back().candidates.push_back({
                .ino = static_cast<ino_t>(use.getInt(6)),
                .dev = static_cast<dev_t>(use.getInt(7)),
                .size = static_cast<uint64_t>(use.getInt(8)),
                .file = file.string(),
                .ingestion = static_cast<uint64_t>(use.getInt(11)),
            });
        }
    }
    if (collisions.empty())
        return;
    auto result = coalesceBlobs(toRealPath(path), hold, collisions);
    auto & representatives = result.representatives;
    auto & rejected = result.rejected;
    /* SQL rollback cannot undo hard links. Retain the selected physical
       representatives in the caller's existing pending rows before SQL.
       A key declined for a link limit/full directory is not a new donor. */
    auto keyLess = [](const auto & a, const auto & b) {
        auto c = memcmp(a.id.hash, b.id.hash, a.id.hashSize);
        return c != 0 ? c < 0 : a.executable < b.executable;
    };
    std::sort(representatives.begin(), representatives.end(), keyLess);
    auto represented = [&](const auto & row) {
        return std::binary_search(representatives.begin(), representatives.end(), row, keyLess);
    };
    std::erase_if(rows, [&](const auto & row) {
        auto collision =
            std::lower_bound(collisions.begin(), collisions.end(), row, [&](const auto & c, const auto & r) {
                return keyLess(c.row, r);
            });
        return collision != collisions.end() && !keyLess(row, collision->row);
    });
    rows.insert(rows.end(), representatives.begin(), representatives.end());
    /* Preserve the physical facts first, then finish their durability
       before any SQL can retry. A retry may already see the winning inode
       and therefore make no further filesystem changes. */
    if (result.modified) {
        if (config->getLocalSettings().fsyncStorePaths) {
            auto realPath = toRealPath(path);
            recursiveSync(realPath);
            syncParent(realPath);
        }
        if (config->getLocalSettings().syncBeforeRegistering)
            sync();
    }
    for (auto & collision : collisions)
        if (!represented(collision.row)) {
            auto & row = collision.row;
            state.stmts->DeleteBlob.use()
                .apply(row.id.hash, row.id.hashSize)
                .apply(int64_t(row.executable ? 1 : 0))
                .apply(static_cast<int64_t>(ingestion))
                .apply(static_cast<int64_t>(row.ino))
                .apply(static_cast<int64_t>(row.dev))
                .apply(static_cast<int64_t>(row.size))
                .apply(row.name)
                .exec();
        }
    for (auto & [row, holder] : rejected) {
        auto relative = std::filesystem::path(holder.file).lexically_relative(getRealStoreDir());
        std::filesystem::path name;
        for (auto i = ++relative.begin(); i != relative.end(); ++i)
            name /= *i;
        state.stmts->DeleteBlob.use()
            .apply(row.id.hash, row.id.hashSize)
            .apply(int64_t(row.executable ? 1 : 0))
            .apply(static_cast<int64_t>(holder.ingestion))
            .apply(static_cast<int64_t>(holder.ino))
            .apply(static_cast<int64_t>(holder.dev))
            .apply(static_cast<int64_t>(holder.size))
            .apply(name.string())
            .exec();
    }
    insertBlobRows(state, ingestion, representatives);
}

void LocalStore::endIngestion(const GitObjectStore::Hold & hold, const StorePath & path)
{
    if (config->readOnly || blobRows == BlobRows::NoRows || !hold.ingestion)
        return;
    /* Join the background writer while rows are still unnamed. An already
       valid repair/optimisation path must not expose unreconciled batches. */
    auto ingestion = hold.ingestion;
    auto rows = hold.takeRows();
    auto logical = printStorePath(path);
    retrySQLite<void>([&]() {
        auto state(_state->lock());
        SQLiteTxn txn(state->db);
        auto id = ingestion->id;
        if (!id) {
            state->stmts->NewIngestion.use().apply("", false).apply(hold.directory().filename().string()).exec();
            id = static_cast<uint64_t>(sqlite3_last_insert_rowid(state->db));
        } else
            state->stmts->SetIngestionPath.use().apply("", false).apply(static_cast<int64_t>(*id)).exec();
        insertBlobRows(*state, *id, rows);
        /* This naming is itself publication for a path already valid.
           Builder outputs are reconciled when their validity is committed. */
        if (isValidPath_(*state, path))
            coalesceBlobRows(*state, path, hold, *id, rows);
        state->stmts->DeletePathBlobs.use().apply(logical).exec();
        state->stmts->DeletePathIngestions.use().apply(logical).exec();
        state->stmts->SetIngestionPath.use().apply(logical).apply(static_cast<int64_t>(*id)).exec();
        txn.commit();
        ingestion->id = id;
        ingestion->path = logical;
    });
}

void LocalStore::recordBlobsAtPath(const GitObjectStore::Hold & hold, const std::filesystem::path & root)
{
    /* Tests only.  A tree written at `root` under the store directory that is
       not a store path: a `ValidPaths` row is made for it under its own name,
       so that the sweep's stale-row rule judges this ingestion as it judges a
       real one, and the ingestion is given that path. */
    hold.flush();
    if (!hold.ingestion)
        return;
    auto logical = std::filesystem::path(config->storeDir) / root.filename();
    auto ingestion = hold.ingestion->id;
    retrySQLite<void>([&]() {
        auto state(_state->lock());
        SQLiteTxn txn(state->db);
        auto id = ingestion;
        if (!id) {
            state->stmts->NewIngestion.use().apply("", false).apply(hold.directory().filename().string()).exec();
            id = static_cast<uint64_t>(sqlite3_last_insert_rowid(state->db));
        } else
            state->stmts->SetIngestionPath.use().apply("", false).apply(static_cast<int64_t>(*id)).exec();
        auto valid = state->stmts->QueryValidPathId.use().apply(logical.string()).next();
        if (!valid) {
            auto use(state->stmts->RegisterValidPath.use());
            use.apply(logical.string())
                .apply("git:sha256:0000000000000000000000000000000000000000000000000000000000000000")
                .apply(int64_t(time(nullptr)))
                .apply("", false)
                .apply(int64_t(0))
                .apply(int64_t(0), false)
                .apply("", false)
                .apply("", false)
                .exec();
        }
        state->stmts->DeletePathBlobs.use().apply(logical.string()).exec();
        state->stmts->DeletePathIngestions.use().apply(logical.string()).exec();
        state->stmts->SetIngestionPath.use().apply(logical.string()).apply(static_cast<int64_t>(*id)).exec();
        txn.commit();
        hold.ingestion->id = id;
    });
}

/* The index's own sweep (schema.sql): a row whose ingestion never got its
   path -- a copy that crashed or failed between its writes and its
   registration -- or whose path is no longer valid, names no holder.  Two
   statements under the GC lock, where the collector already is, and the one
   place the cascade a foreign key would have given is replaced.  `link`
   covers the window until this runs: it finds no file at the holder's name
   and forgets the row. */
void LocalStore::sweepStaleBlobRows()
{
    if (config->readOnly)
        return;
    auto tmp = objects.dir / "tmp";
    retrySQLite<void>([&]() {
        auto state(_state->lock());
        SQLiteTxn txn(state->db);
        std::vector<uint64_t> stale;
        {
            auto use(state->stmts->QueryStaleIngestions.use());
            while (use.next()) {
                auto id = static_cast<uint64_t>(use.getInt(0));
                auto held = use.isNull(1) ? std::string() : use.getStr(1);
                /* In flight: the sweep removed the holds of dead ingestions
                   before this ran, so a hold still there is a live one and
                   its rows are kept, as its objects are (`08` K16). */
                if (!held.empty() && pathExists(tmp / held))
                    continue;
                stale.push_back(id);
            }
        }
        for (auto id : stale) {
            state->stmts->DeleteBlobsOf.use().apply(static_cast<int64_t>(id)).exec();
            state->stmts->DeleteIngestion.use().apply(static_cast<int64_t>(id)).exec();
        }
        txn.commit();
    });
}

uint64_t LocalStore::blobRowCount(bool executable)
{
    return retrySQLite<uint64_t>([&]() -> uint64_t {
        auto state(_state->lock());
        SQLiteStmt stmt;
        stmt.create(state->db, "select count(*) from Blobs where exec = ?;");
        auto use(stmt.use());
        use.apply(int64_t(executable ? 1 : 0));
        if (!use.next())
            return 0;
        return static_cast<uint64_t>(use.getInt(0));
    });
}

AutoCloseFD LocalStore::openGCLock()
{
    auto fnGCLock = config->stateDir.get() / "gc.lock";
    return openLockFile(fnGCLock, /*create=*/true);
}

void LocalStore::deleteStorePath(const std::filesystem::path & path, uint64_t & bytesFreed, bool isKnownPath)
{
    try {
        deletePath(path, bytesFreed);
    } catch (SystemError & e) {
        if (config->ignoreGcDeleteFailure) {
            logWarning(
                {.msg = HintFmt(
                     isKnownPath ? "ignoring failure to remove store path %1%: %2%"
                                 : "ignoring failure to remove garbage in store directory %1%: %2%",
                     PathFmt(path),
                     e.info().msg)});
        } else {
            e.addTrace(
                {},
                isKnownPath ? "While deleting store path %1%" : "While deleting garbage in store directory %1%",
                PathFmt(path));
            throw;
        }
    }
}

LocalStore::~LocalStore()
{
    std::shared_future<void> future;

    {
        auto state(_state->lock());
        if (state->gcRunning)
            future = state->gcFuture;
    }

    if (future.valid()) {
        printInfo("waiting for auto-GC to finish on exit...");
        future.get();
    }

    {
        auto state(_state->lock());
        if (state->gcThread.joinable())
            state->gcThread.join();
    }

    try {
        auto fdTempRoots(_fdTempRoots.lock());
        if (fdTempRoots->fd) {
#ifdef _WIN32
            fdTempRoots->fd.close();
#endif
            /* Keep the POSIX lock until the name is gone, so a collector
               cannot acquire this inode before its owner unlinks it. */
            tryUnlink(fnTempRoots);
            fdTempRoots->fd.close();
        }
    } catch (...) {
        ignoreExceptionInDestructor();
    }
}

std::filesystem::path LocalStoreConfig::getRootsSocketPath() const
{
    return std::filesystem::path(stateDir.get()) / "gc-roots-socket" / "socket";
}

StoreReference LocalStoreConfig::getReference() const
{
    auto params = getQueryParams();
    /* Back-compatibility kludge. Tools like nix-output-monitor expect 'local'
       and can't parse 'local://'. */
    if (params.empty())
        /* TODO: Add the rootDir here as the authority? */
        return {.variant = StoreReference::Local{}};
    return {
        .variant =
            StoreReference::Specified{
                .scheme = *uriSchemes().begin(),
                /* TODO: Add the rootDir here as the authority? */
            },
        .params = std::move(params),
    };
}

bool LocalStoreConfig::getReadOnly() const
{
    return readOnly.get() || StoreConfig::getReadOnly();
}

int LocalStore::getSchema()
{
    int curSchema = 0;
    if (pathExists(schemaPath)) {
        auto s = readFile(schemaPath);
        auto n = string2Int<int>(s);
        if (!n)
            throw Error("%1% is corrupt", PathFmt(schemaPath));
        curSchema = *n;
    }
    return curSchema;
}

void LocalStore::openDB(State & state, bool create)
{
    if (create && config->readOnly) {
        throw Error("cannot create database while in read-only mode");
    }

    if (access(dbDir.string().c_str(), R_OK | (config->readOnly ? 0 : W_OK)))
        throw SysError("Nix database directory %1% is not writable", PathFmt(dbDir));

    /* Open the Nix database. */
    auto & db(state.db);
    auto openMode = config->readOnly ? SQLiteOpenMode::Immutable
                    : create         ? SQLiteOpenMode::Normal
                                     : SQLiteOpenMode::NoCreate;
    /* State owns this connection and all its statements; every access is
       serialized by _state, including callbacks and schema setup. */
    state.db = SQLite(
        dbDir / "db.sqlite", {.mode = openMode, .useWAL = settings.useSQLiteWAL, .externalSynchronization = true});

#ifdef __CYGWIN__
    /* The cygwin version of sqlite3 has a patch which calls
       SetDllDirectory("/usr/bin") on init. It was intended to fix extension
       loading, which we don't use, and the effect of SetDllDirectory is
       inherited by child processes, and causes libraries to be loaded from
       /usr/bin instead of $PATH. This breaks quite a few things (e.g.
       checkPhase on openssh), so we set it back to default behaviour. */
    SetDllDirectoryW(L"");
#endif

    /* !!! check whether sqlite has been built with foreign key
       support */

    /* Whether SQLite should fsync().  "Normal" synchronous mode
       should be safe enough.  If the user asks for it, don't sync at
       all.  This can cause database corruption if the system
       crashes. */
    std::string syncMode = config->getLocalSettings().fsyncMetadata ? "normal" : "off";
    db.exec("pragma synchronous = " + syncMode);

    /* Set the SQLite journal mode.  WAL mode is fastest, so it's the
       default. */
    std::string mode = settings.useSQLiteWAL ? "wal" : "truncate";
    std::string prevMode;
    {
        SQLiteStmt stmt;
        stmt.create(db, "pragma main.journal_mode;");
        if (sqlite3_step(stmt) != SQLITE_ROW)
            SQLiteError::throw_(db, "querying journal mode");
        prevMode = std::string((const char *) sqlite3_column_text(stmt, 0));
    }
    if (prevMode != mode
        && sqlite3_exec(db, ("pragma main.journal_mode = " + mode + ";").c_str(), 0, 0, 0) != SQLITE_OK)
        SQLiteError::throw_(db, "setting journal mode");

    if (mode == "wal") {
        /* persist the WAL files when the db connection is closed. This allows
           for read-only connections without write permissions on the
           containing directory to succeed on a closed db. Setting the
           journal_size_limit to 2^40 bytes results in the WAL files getting
           truncated to 0 on exit and limits the on disk size of the WAL files
           to 2^40 bytes following a checkpoint */
        if (sqlite3_exec(db, "pragma main.journal_size_limit = 1099511627776;", 0, 0, 0) == SQLITE_OK) {
            int enable = 1;
            sqlite3_file_control(db, NULL, SQLITE_FCNTL_PERSIST_WAL, &enable);
        }
    }

    /* Increase the auto-checkpoint interval to 40000 pages.  This
       seems enough to ensure that instantiating the NixOS system
       derivation is done in a single fsync(). */
    if (mode == "wal" && sqlite3_exec(db, "pragma wal_autocheckpoint = 40000;", 0, 0, 0) != SQLITE_OK)
        SQLiteError::throw_(db, "setting autocheckpoint interval");

    /* Initialise the database schema, if necessary. */
    if (create) {
        db.exec({
#embed "schema.sql"
        });
    }
}

bool LocalStore::upgradeDBSchema(State & state, bool dryRun)
{
    bool ret = false;

    {
        SQLiteStmt queryHasSchemaMigrations;
        queryHasSchemaMigrations.create(
            state.db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='SchemaMigrations';");
        auto useQueryHasSchemaMigrations(queryHasSchemaMigrations.use());
        if (!useQueryHasSchemaMigrations.next()) {
            if (dryRun)
                return true;
            else {
                state.db.exec("create table SchemaMigrations (migration text primary key not null);");
                ret = true;
            }
        }
    }

    StringSet schemaMigrations;

    {
        SQLiteStmt querySchemaMigrations;
        querySchemaMigrations.create(state.db, "select migration from SchemaMigrations;");
        auto useQuerySchemaMigrations(querySchemaMigrations.use());
        while (useQuerySchemaMigrations.next())
            schemaMigrations.insert(useQuerySchemaMigrations.getStr(0));
    }

    auto needsMigration = [&](const std::string & migrationName) -> bool {
        return !schemaMigrations.contains(migrationName);
    };

    auto maybeUpgrade = [&](const std::string & migrationName, const std::string & stmt) {
        if (!needsMigration(migrationName))
            return;

        ret = true;
        if (dryRun)
            return;

        debug("executing Nix database schema migration '%s'...", migrationName);

        SQLiteTxn txn(state.db);
        state.db.exec(stmt + fmt(";\ninsert or ignore into SchemaMigrations values('%s')", migrationName));
        txn.commit();

        schemaMigrations.insert(migrationName);
    };

    if (experimentalFeatureSettings.isEnabled(Xp::CaDerivations)) {
        maybeUpgrade(
            "20251017-ca-derivations",
            {
#embed "ca-specific-schema.sql"
            });
    }

    maybeUpgrade("20260309-drop-redundant-indexreferrer", "drop index if exists IndexReferrer");

    /* The blob index (`schema.sql`, 01 section 9.10): where a regular-file
       blob's holder is.  **Additive, so it belongs here and not in a schema
       bump**, which is the convention the comment above states and which
       `nixSchemaVersion` 11 is the stated exception to: 11 was bumped because
       an existing column's *meaning* changed (`ValidPaths.hash` went from the
       NAR hash to the object hash), so an older Nix reading such a store would
       misread it.  Two new tables and an index change no existing column, and
       an older Nix simply does not look at them, so no bump is owed.

       The index is a *cache*: every row in it is derivable from the store's own
       paths, and `nix-store --optimise` rebuilds it by walking them.  So a
       migration that changes its shape may drop it and make it again rather
       than alter it in place -- which is what this one does, under a name of
       its own, because a store made by an earlier build of this work has the
       tables in an older shape and `create table if not exists` would leave
       them so.  Sharing is not lost by it: the paths keep their files, and the
       next ingestion or `nix-store --optimise` records the rows again. */
    maybeUpgrade(
        "20260919-blob-index-holders",
        "drop table if exists Blobs;"
        "\ndrop table if exists Ingestions;"
        "\ncreate table Ingestions ("
        " id integer primary key autoincrement not null,"
        " path text, hold text);"
        "\ncreate table Blobs ("
        " id blob not null, exec integer not null, ino integer not null, dev integer not null,"
        " ingestion integer not null, name text not null, size integer not null,"
        " primary key (id, exec));"
        "\ncreate index IndexBlobsIngestion on Blobs(ingestion)");

    /* Candidate ownership is per ingestion. Preserve existing holders
       while allowing concurrent ingestions to publish independently. */
    maybeUpgrade(
        "20260921-blob-holder-candidates",
        "create table BlobCandidates ("
        " id blob not null, exec integer not null, ino integer not null, dev integer not null,"
        " ingestion integer not null, name text not null, size integer not null,"
        " primary key (id, exec, ingestion));"
        "\ninsert into BlobCandidates select id, exec, ino, dev, ingestion, name, size from Blobs;"
        "\ndrop table Blobs;"
        "\nalter table BlobCandidates rename to Blobs;"
        "\ncreate index IndexBlobsIngestion on Blobs(ingestion)");

    maybeUpgrade(
        "20260921-ingestion-paths", "create index if not exists IndexIngestionsPath on Ingestions(path, hold)");

    return ret;
}

/* To improve purity, users may want to make the Nix store a read-only
   bind mount.  So make the Nix store writable for this process. */
void LocalStore::makeStoreWritable()
{
#ifdef __linux__
    if (!isRootUser())
        return;
    remountReadOnlyWritable(config->realStoreDir.get());
#endif
}

void LocalStore::registerDrvOutput(const Realisation & info, CheckSigsFlag checkSigs)
{
    experimentalFeatureSettings.require(Xp::CaDerivations);
    if (checkSigs == NoCheckSigs || !realisationIsUntrusted(info))
        registerDrvOutputUnchecked(info);
    else
        throw Error(
            "cannot register realisation '%s' because it lacks a signature by a trusted key", info.outPath.to_string());
}

void LocalStore::registerDrvOutputUnchecked(const Realisation & info)
{
    experimentalFeatureSettings.require(Xp::CaDerivations);
    retrySQLite<void>([&]() {
        auto state(_state->lock());
        if (auto oldR = queryRealisation_(*state, info.id)) {
            if (info.isCompatibleWith(*oldR)) {
                auto combinedSignatures = oldR->signatures;
                combinedSignatures.insert(info.signatures.begin(), info.signatures.end());
                state->stmts->UpdateRealisedOutput.use()
                    .apply(concatStringsSep(" ", Signature::toStrings(combinedSignatures)))
                    .apply(info.id.drvPath.to_string())
                    .apply(info.id.outputName)
                    .exec();
            } else {
                throw Error(
                    "Trying to register a realisation of '%s', but we already "
                    "have another one locally.\n"
                    "Local:  %s\n"
                    "Remote: %s",
                    info.id.to_string(),
                    printStorePath(oldR->outPath),
                    printStorePath(info.outPath));
            }
        } else {
            state->stmts->RegisterRealisedOutput.use()
                .apply(info.id.drvPath.to_string())
                .apply(info.id.outputName)
                .apply(printStorePath(info.outPath))
                .apply(concatStringsSep(" ", Signature::toStrings(info.signatures)))
                .exec();
        }
    });
}

void LocalStore::deleteBuildTraces(const std::set<DrvOutput> & keys)
{
    experimentalFeatureSettings.require(Xp::CaDerivations);
    retrySQLite<void>([&]() {
        auto state(_state->lock());
        SQLiteTxn txn(state->db);
        for (const auto & key : keys) {
            state->stmts->DeleteRealisedOutputByName.use().apply(key.drvPath.to_string()).apply(key.outputName).exec();
        }
        txn.commit();
    });
}

void LocalStore::cacheDrvOutputMapping(
    State & state, const uint64_t deriver, const std::string & outputName, const StorePath & output)
{
    retrySQLite<void>([&]() {
        state.stmts->AddDerivationOutput.use().apply(deriver).apply(outputName).apply(printStorePath(output)).exec();
    });
}

uint64_t LocalStore::addValidPath(State & state, const ValidPathInfo & info)
{
    if (info.ca.has_value() && !info.isContentAddressed(*this))
        throw Error(
            "cannot add path '%s' to the Nix store because it claims to be content-addressed but isn't",
            printStorePath(info.path));

    /* The column holds the object hash and nothing else: an info that
       carries only a NAR hash (an old peer's, an old cache's) is
       registered by the ingestion that verified it and computed the
       object hash from its sink, never directly. */
    state.stmts->RegisterValidPath.use()
        .apply(printStorePath(info.path))
        .apply(info.requireObjectHash(*this).render())
        .apply(info.registrationTime == 0 ? time(nullptr) : info.registrationTime)
        .apply(info.deriver ? printStorePath(*info.deriver) : "", (bool) info.deriver)
        .apply(info.narSize, info.narSize != 0)
        .apply(info.ultimate ? 1 : 0, info.ultimate)
        .apply(concatStringsSep(" ", Signature::toStrings(info.sigs)), !info.sigs.empty())
        .apply(renderContentAddress(info.ca), (bool) info.ca)
        .exec();
    uint64_t id = state.db.getLastInsertedRowId();

    /* If this is a derivation, then store the derivation outputs in
       the database.  This is useful for the garbage collector: it can
       efficiently query whether a path is an output of some
       derivation. */
    if (info.path.isDerivation()) {
        auto parsedDrv = readInvalidDerivation(info.path);

        /* Verify that the output paths in the derivation are correct
           (i.e., follow the scheme for computing output paths from
           derivations).  Note that if this throws an error, then the
           DB transaction is rolled back, so the path validity
           registration above is undone. */
        checkInvariants(parsedDrv, *this, info.path);

        for (auto & i : outputsAndOptPaths(parsedDrv, *this)) {
            /* Floating CA derivations have indeterminate output paths until
               they are built, so don't register anything in that case */
            if (i.second.second)
                cacheDrvOutputMapping(state, id, i.first, *i.second.second);
        }
    }

    return id;
}

void LocalStore::queryPathInfoUncached(
    const StorePath & path, Callback<std::shared_ptr<const ValidPathInfo>> callback) noexcept
{
    try {
        auto info = retrySQLite<std::shared_ptr<const ValidPathInfo>>(
            [&]() { return queryPathInfoInternal(*_state->lock(), path); });

        /* Outside the lock: a schema-10 row's migration may walk the path. */
        if (info)
            info = migratedPathInfo(std::move(info));

        callback(std::move(info));

    } catch (...) {
        callback.rethrow();
    }
}

std::shared_ptr<const ValidPathInfo> LocalStore::queryPathInfoUnmigrated(const StorePath & path)
{
    return retrySQLite<std::shared_ptr<const ValidPathInfo>>(
        [&]() { return queryPathInfoInternal(*_state->lock(), path); });
}

void LocalStore::migratePathInfo(const StorePath & path)
{
    auto info = retrySQLite<std::shared_ptr<const ValidPathInfo>>(
        [&]() { return queryPathInfoInternal(*_state->lock(), path); });

    if (!info)
        throw InvalidPath("path '%s' is not valid", printStorePath(path));
    if (info->objectHash)
        return;

    ValidPathInfo row(*info);
    std::optional<std::pair<Hash, Hash>> narHashes;
    switch (walkOldRow(row, &narHashes)) {
    case OldRowWalk::Migrated:
        return;
    case OldRowWalk::Modified:
        throw Error(
            "path '%s' was modified! expected hash '%s', got '%s'; its row is left as it is -- run `nix-store --verify --check-contents --repair`",
            printStorePath(path),
            narHashes->first.to_string(HashFormat::Nix32, true),
            narHashes->second.to_string(HashFormat::Nix32, true));
    case OldRowWalk::FilesMissing:
        throw Error(
            "path '%s' is registered but its files are missing; run `nix-store --verify` to remove the row",
            printStorePath(path));
    }
    unreachable();
}

LocalStore::OldRowWalk LocalStore::walkOldRow(ValidPathInfo & info, std::optional<std::pair<Hash, Hash>> * narHashes)
{
    auto & path = info.path;

    /* One read of the bytes, two hashes: the object hash from the NAR
       stream (law 2 of 01 section 9.10: the same value the accessor walk
       gives) and the NAR hash from a tee of it, under the algorithm the row
       asserts.  A path the collector took meanwhile is reported as
       invalid, the caller's contract. */
    HashSink narSink{info.assertedNarHash ? info.assertedNarHash->algo : HashAlgorithm::SHA256};
    /* Gone under a valid row -- the directory (`requireStoreObjectAccessor`
       finds none) or a file of it (`ENOENT` in the walk) -- is the state
       master's `nix-store --verify` finds and removes; a row that is gone
       is the collector's doing and the caller's `InvalidPath`. */
    auto rowValid = [&] { return isValidPathUncached(path); };
    std::optional<ObjectHashSink::Result> object;
    try {
        auto accessor = requireStoreObjectAccessor(path, /*requireValidPath=*/false);
        auto source = sinkToSource([&](Sink & sink) { accessor->dumpPath(CanonPath::root, sink); });
        TeeSource tee{*source, narSink};
        object = objectHashOfNar(tee);
    } catch (InvalidPath &) {
        if (!rowValid())
            throw;
        return OldRowWalk::FilesMissing;
    } catch (SysError & e) {
        if (!rowValid())
            throw InvalidPath("path '%s' is not valid", printStorePath(path));
        if (e.errNo == ENOENT)
            return OldRowWalk::FilesMissing;
        throw;
    }
    auto narHash = narSink.finish().hash;

    /* The row's assertion checked before anything is written, as master's
       verifier checked it and `--load-db` still does.  (The NAR size need
       not be compared: the hash is over the whole serialisation.) */
    if (info.assertedNarHash && *info.assertedNarHash != narHash) {
        if (narHashes)
            *narHashes = {*info.assertedNarHash, narHash};
        return OldRowWalk::Modified;
    }

    info.objectHash = ObjectHash::of(object->root);
    if (info.narSize == 0)
        info.narSize = object->narSize;
    recordObjectHash(path, *info.objectHash, object->narSize);
    return OldRowWalk::Migrated;
}

std::shared_ptr<const ValidPathInfo> LocalStore::migratedPathInfo(std::shared_ptr<const ValidPathInfo> info)
{
    if (info->objectHash)
        return info;

    /* A schema-10 row (01 section 9.11, "The database"): its NAR hash
       stays as `assertedNarHash`; one walk of the path gives the object
       hash and the NAR size and checks that NAR hash (`walkOldRow`).  No
       root is taken here: the collector itself queries infos under its
       exclusive lock (`keep-derivations`), and a root registered then
       would keep a dead path alive; every other caller holds a root or a
       shared lock already. */
    auto migrated = std::make_shared<ValidPathInfo>(*info);

    switch (walkOldRow(*migrated)) {
    case OldRowWalk::Migrated:
        return migrated;
    case OldRowWalk::Modified:
        /* The row as the database has it (01 section 10, *a schema-10 row's
           migration checks the NAR hash the row asserts*): the
           query answers, as master's did; `contentMismatch` and
           `verifyStore --check-contents` find the modification through the
           NAR hash and `--repair` restores the path, after which the next
           walk migrates the row.  Writing the walk's object hash here
           would bless the modified bytes for every later check. */
        debug(
            "path '%s' does not hash to the NAR hash its row asserts; row left unmigrated", printStorePath(info->path));
        return info;
    case OldRowWalk::FilesMissing:
        /* Likewise: master answered a query of such a row from the
           database, and `nix-store --verify` is what removes the row. */
        debug("path '%s' is registered but its files are missing; row left unmigrated", printStorePath(info->path));
        return info;
    }
    unreachable();
}

bool LocalStore::recordObjectHash(const StorePath & path, const ObjectHash & objectHash, uint64_t narSize)
{
    if (config->readOnly)
        return false;
    return retrySQLite<bool>([&]() {
        auto state(_state->lock());

        SQLiteTxn txn(state->db);

        /* Re-read under the lock: another process may have migrated the
           row, signed it, or deleted the path meanwhile.  Only the hash
           and a missing size are ours to write. */
        auto current = queryPathInfoInternal(*state, path);
        bool written = current && !current->objectHash;
        if (written) {
            ValidPathInfo row(*current);
            row.objectHash = objectHash;
            if (row.narSize == 0)
                row.narSize = narSize;
            updatePathInfo(*state, row);
        }

        txn.commit();
        return written;
    });
}

std::shared_ptr<const ValidPathInfo> LocalStore::queryPathInfoInternal(State & state, const StorePath & path)
{
    /* Get the path info. */
    auto useQueryPathInfo(state.stmts->QueryPathInfo.use().apply(printStorePath(path)));

    if (!useQueryPathInfo.next())
        return std::shared_ptr<ValidPathInfo>();

    auto id = useQueryPathInfo.getInt(0);

    auto info = std::make_shared<ValidPathInfo>(path, UnkeyedValidPathInfo(*this, std::nullopt));

    /* The column: an object hash rendered `git:sha256:<base16>`, or a
       schema-10 row's NAR hash rendered `sha256:<base16>`, told apart by
       prefix *before* the old parser runs -- for the `git` prefix
       `parseHashAlgo` raises `UsageError`, not the `BadHash` caught
       below.  The old branch keeps its error for a row that parses as
       neither. */
    {
        auto hashColumn = useQueryPathInfo.getStr(1);
        if (auto oh = ObjectHash::parse(hashColumn))
            info->objectHash = *oh;
        else {
            try {
                info->assertedNarHash = Hash::parseAnyPrefixed(hashColumn);
            } catch (BadHash & e) {
                throw Error("invalid-path entry for '%s': %s", printStorePath(path), e.what());
            }
        }
    }

    info->registrationTime = useQueryPathInfo.getInt(2);

    auto s = (const char *) sqlite3_column_text(state.stmts->QueryPathInfo, 3);
    if (s)
        info->deriver = parseStorePath(s);

    /* Note that narSize = NULL yields 0. */
    info->narSize = useQueryPathInfo.getInt(4);

    info->ultimate = useQueryPathInfo.getInt(5) == 1;

    s = (const char *) sqlite3_column_text(state.stmts->QueryPathInfo, 6);
    if (s)
        info->sigs = Signature::parseMany(tokenizeString<StringSet>(s, " "));

    s = (const char *) sqlite3_column_text(state.stmts->QueryPathInfo, 7);
    if (s)
        info->ca = ContentAddress::parseOpt(s);

    /* Get the references. */
    auto useQueryReferences(state.stmts->QueryReferences.use().apply(id));

    while (useQueryReferences.next())
        info->references.insert(parseStorePath(useQueryReferences.getStr(0)));

    return info;
}

/* Update path info in the database. */
void LocalStore::updatePathInfo(State & state, const ValidPathInfo & info)
{
    state.stmts->UpdatePathInfo.use()
        .apply(info.narSize, info.narSize != 0)
        .apply(info.requireObjectHash(*this).render())
        .apply(info.ultimate ? 1 : 0, info.ultimate)
        .apply(concatStringsSep(" ", Signature::toStrings(info.sigs)), !info.sigs.empty())
        .apply(renderContentAddress(info.ca), (bool) info.ca)
        .apply(printStorePath(info.path))
        .exec();
}

uint64_t LocalStore::queryValidPathId(State & state, const StorePath & path)
{
    auto use(state.stmts->QueryValidPathId.use().apply(printStorePath(path)));
    if (!use.next())
        throw InvalidPath("path '%s' is not valid", printStorePath(path));
    return use.getInt(0);
}

bool LocalStore::isValidPath_(State & state, const StorePath & path)
{
    return state.stmts->QueryValidPathId.use().apply(printStorePath(path)).next();
}

bool LocalStore::isValidPathUncached(const StorePath & path)
{
    return retrySQLite<bool>([&]() {
        auto state(_state->lock());
        bool valid = isValidPath_(*state, path);
        if (valid && pathInfoCache) {
            /* Publish under the same database lock as invalidation, so a
               completed invalidation cannot be followed by an older hit.
               Preserve any fresh description already in the cache. */
            auto cache(pathInfoCache->lock());
            auto previous = cache->getOrNullptr(path);
            if (!previous || !previous->didExist() || !previous->isKnownNow(settings.getNarInfoDiskCacheSettings()))
                cache->upsert(path, PathInfoCacheValue{.value = PathInfoCacheValue::ValidWithoutInfo{}});
        }
        return valid;
    });
}

StorePathSet LocalStore::queryValidPaths(const StorePathSet & paths, SubstituteFlag maybeSubstitute)
{
    StorePathSet res;
    for (auto & i : paths)
        if (isValidPath(i))
            res.insert(i);
    return res;
}

StorePathSet LocalStore::queryAllValidPaths()
{
    return retrySQLite<StorePathSet>([&]() {
        auto state(_state->lock());
        auto use(state->stmts->QueryValidPaths.use());
        StorePathSet res;
        while (use.next())
            res.insert(parseStorePath(use.getStr(0)));
        return res;
    });
}

bool LocalStore::hasValidPaths()
{
    return retrySQLite<bool>([&] {
        auto state(_state->lock());
        return state->stmts->QueryValidPaths.use().next();
    });
}

uint64_t LocalStore::forEachValidObjectHash(fun<void(const ObjectHash &)> callback)
{
    return retrySQLite<uint64_t>([&]() {
        auto state(_state->lock());
        auto use(state->stmts->QueryValidObjectHashes.use());
        uint64_t unmigrated = 0;
        while (use.next()) {
            /* A row older than schema 11 holds a NAR hash, which `parse`
               refuses; it is counted, not migrated, since the migration
               walks the path and this runs under the database lock. */
            if (auto hash = ObjectHash::parse(use.getStr(0)))
                callback(*hash);
            else
                unmigrated++;
        }
        return unmigrated;
    });
}

void LocalStore::queryReferrers(State & state, const StorePath & path, StorePathSet & referrers)
{
    auto useQueryReferrers(state.stmts->QueryReferrers.use().apply(printStorePath(path)));

    while (useQueryReferrers.next())
        referrers.insert(parseStorePath(useQueryReferrers.getStr(0)));
}

void LocalStore::queryReferrers(const StorePath & path, StorePathSet & referrers)
{
    return retrySQLite<void>([&]() { queryReferrers(*_state->lock(), path, referrers); });
}

StorePathSet LocalStore::queryValidDerivers(const StorePath & path)
{
    return retrySQLite<StorePathSet>([&]() {
        auto state(_state->lock());

        auto useQueryValidDerivers(state->stmts->QueryValidDerivers.use().apply(printStorePath(path)));

        StorePathSet derivers;
        while (useQueryValidDerivers.next())
            derivers.insert(parseStorePath(useQueryValidDerivers.getStr(1)));

        return derivers;
    });
}

std::map<std::string, std::optional<StorePath>>
LocalStore::queryStaticPartialDerivationOutputMap(const StorePath & path)
{
    return retrySQLite<std::map<std::string, std::optional<StorePath>>>([&]() {
        auto state(_state->lock());
        std::map<std::string, std::optional<StorePath>> outputs;
        uint64_t drvId;
        drvId = queryValidPathId(*state, path);
        auto use(state->stmts->QueryDerivationOutputs.use().apply(drvId));
        while (use.next())
            outputs.insert_or_assign(use.getStr(0), parseStorePath(use.getStr(1)));

        return outputs;
    });
}

std::optional<StorePath>
LocalStore::queryStaticPartialDerivationOutput(const StorePath & path, const std::string & outputName)
{
    auto outputs = queryStaticPartialDerivationOutputMap(path);
    auto it = outputs.find(outputName);
    if (it == outputs.end()) {
        /* Only throw if CA derivations is disabled, because then the
           SQL table is complete.

           With CA derivations enabled, derivations without static
           outputs exist, this absence of a row in this table does not
           mean the derivation doesn't have an output necessarily, just
           that that it doesn't have an output with a known output path.
          */
        if (!experimentalFeatureSettings.isEnabled(Xp::CaDerivations))
            throw Error("derivation '%s' does not have an output named '%s'", printStorePath(path), outputName);
        return std::nullopt;
    }
    return it->second;
}

std::optional<StorePath> LocalStore::queryPathFromHashPart(const std::string & hashPart)
{
    if (hashPart.size() != StorePath::HashLen)
        throw Error("invalid hash part");

    std::string prefix = storeDir + "/" + hashPart;

    return retrySQLite<std::optional<StorePath>>([&]() -> std::optional<StorePath> {
        auto state(_state->lock());

        auto useQueryPathFromHashPart(state->stmts->QueryPathFromHashPart.use().apply(prefix));

        if (!useQueryPathFromHashPart.next())
            return {};

        const char * s = (const char *) sqlite3_column_text(state->stmts->QueryPathFromHashPart, 0);
        if (s && prefix.compare(0, prefix.size(), s, prefix.size()) == 0)
            return parseStorePath(s);
        return {};
    });
}

void LocalStore::registerValidPath(const ValidPathInfo & info)
{
    registerValidPaths({{info.path, info}});
}

void LocalStore::registerValidPaths(const ValidPathInfos & infos)
{
    registerValidPaths(infos, nullptr);
}

void LocalStore::registerValidPath(const ValidPathInfo & info, const GitObjectStore::Hold & hold)
{
    registerValidPaths({{info.path, info}}, &hold);
}

void LocalStore::validatePathInfoKeys(const ValidPathInfos & infos)
{
    for (auto & [path, info] : infos)
        if (path != info.path)
            throw Error("cannot register path info for '%s' under '%s'", info.path.to_string(), path.to_string());
}

void LocalStore::registerValidPaths(const ValidPathInfos & infos, const GitObjectStore::Hold * hold)
{
    validatePathInfoKeys(infos);

    /* Finish the ingestion before taking the database lock: a large copy's
       writer may still be committing a batch. The usual single-file object
       has never started that writer and publishes all of its rows here,
       with its path, through the one connection and transaction. */
    auto ingestion = hold ? hold->ingestion : nullptr;

    struct Publication
    {
        StorePath path;
        std::optional<uint64_t> ingestion;
        std::vector<GitObjectStore::Hold::Row> rows;
    };

    std::vector<Publication> publications;
    if (ingestion) {
        auto rows = hold->takeRows();
        if (!ingestion->path) {
            assert(infos.size() == 1);
            publications.push_back({infos.begin()->first, ingestion->id, std::move(rows)});
        } else {
            /* A builder uses one hold for several completed outputs. Their
               individually named ingestions remain private until this
               transaction publishes the corresponding valid paths. */
            assert(rows.empty());
            retrySQLite<void>([&] {
                publications.clear();
                auto state(_state->lock());
                for (auto & [path, _] : infos) {
                    auto use(state->stmts->QueryHoldIngestions.use());
                    use.apply(printStorePath(path)).apply(hold->directory().filename().string());
                    if (use.next())
                        publications.push_back({path, static_cast<uint64_t>(use.getInt(0)), {}});
                }
            });
        }
        for (auto & publication : publications)
            std::sort(publication.rows.begin(), publication.rows.end(), [](const auto & a, const auto & b) {
                auto c = memcmp(a.id.hash, b.id.hash, a.id.hashSize);
                return c != 0 ? c < 0 : a.executable < b.executable;
            });
    }
#ifndef _WIN32
    /* SQLite will fsync by default, but the new valid paths may not
       be fsync-ed.  So some may want to fsync them before registering
       the validity, at the expense of some speed of the path
       registering operation. */
    if (config->getLocalSettings().syncBeforeRegistering)
        sync();
#endif

    return retrySQLite<void>([&]() {
        auto state(_state->lock());

        SQLiteTxn txn(state->db);
        StorePathSet paths;
        StorePathSet updated;
        /* Within this transaction, the finite path/reference domain has
           stable row ids. Retain each lookup through reference insertion. */
        std::map<StorePath, uint64_t> pathIds;
        std::vector<uint64_t> ingestionIds;
        for (auto & publication : publications) {
            auto id = publication.ingestion;
            if (!id) {
                state->stmts->NewIngestion.use().apply("", false).apply(hold->directory().filename().string()).exec();
                id = static_cast<uint64_t>(sqlite3_last_insert_rowid(state->db));
            } else
                state->stmts->SetIngestionPath.use().apply("", false).apply(static_cast<int64_t>(*id)).exec();
            ingestionIds.push_back(*id);
            insertBlobRows(*state, *id, publication.rows);
        }

        for (auto & [_, i] : infos) {
            i.requireObjectHash(*this);
            {
                auto use(state->stmts->QueryValidPathId.use().apply(printStorePath(i.path)));
                if (use.next()) {
                    updated.insert(i.path);
                    pathIds.emplace(i.path, static_cast<uint64_t>(use.getInt(0)));
                }
            }
            paths.insert(i.path);
            for (auto & reference : i.references)
                if (!infos.contains(reference) && !pathIds.contains(reference))
                    pathIds.emplace(reference, queryValidPathId(*state, reference));
        }

        /* Do a topological sort of the paths.  This will throw an
           error if a cycle is detected and roll back the
           transaction.  Cycles can only occur when a derivation
           has multiple outputs. */
        auto topoSortResult = topoSort(paths, [&](const StorePath & path) {
            auto i = infos.find(path);
            return i == infos.end() ? StorePathSet() : i->second.references;
        });

        std::visit(
            overloaded{
                [&](const Cycle<StorePath> & cycle) {
                    throw BuildError(
                        BuildResult::Failure::OutputRejected,
                        "cycle detected in the references of '%s' from '%s'",
                        printStorePath(cycle.path),
                        printStorePath(cycle.parent));
                },
                [](auto &) { /* Success, continue */ }},
            topoSortResult);

        /* Existing paths may have visible aliases even if this transaction
           rolls back. Reconcile all of those against committed witnesses
           before exposing any of their new candidate rows. */
        for (size_t n = 0; n < publications.size(); ++n)
            if (updated.contains(publications[n].path))
                coalesceBlobRows(*state, publications[n].path, *hold, ingestionIds[n], publications[n].rows);
        for (size_t n = 0; n < publications.size(); ++n)
            if (updated.contains(publications[n].path)) {
                auto logical = printStorePath(publications[n].path);
                state->stmts->DeletePathBlobs.use().apply(logical).exec();
                state->stmts->DeletePathIngestions.use().apply(logical).exec();
                state->stmts->SetIngestionPath.use().apply(logical).apply(static_cast<int64_t>(ingestionIds[n])).exec();
            }

        for (auto & [path, info] : infos) {
            if (updated.contains(path)) {
                updatePathInfo(*state, info);
                continue;
            }
            /* A new path is made eligible only after all its candidates
               are reconciled. Later outputs may share this finished one. */
            for (size_t n = 0; n < publications.size(); ++n)
                if (publications[n].path == path) {
                    coalesceBlobRows(*state, path, *hold, ingestionIds[n], publications[n].rows);
                    state->stmts->DeletePathBlobs.use().apply(printStorePath(path)).exec();
                    state->stmts->DeletePathIngestions.use().apply(printStorePath(path)).exec();
                    state->stmts->SetIngestionPath.use()
                        .apply(printStorePath(path))
                        .apply(static_cast<int64_t>(ingestionIds[n]))
                        .exec();
                }
            pathIds.emplace(path, addValidPath(*state, info));
        }
        for (auto & [_, info] : infos) {
            auto referrer = pathIds.at(info.path);
            for (auto & reference : info.references)
                state->stmts->AddReference.use().apply(referrer).apply(pathIds.at(reference)).exec();
        }
        txn.commit();
        if (ingestion && !ingestion->path && !publications.empty())
            ingestion->id = ingestionIds.front();
        /* A description becomes cacheable only with the committed path
           and references. A rejected reference or cycle rolls the SQL
           back; publishing earlier would leave a false validity hit. */
        if (pathInfoCache) {
            auto cache = pathInfoCache->lock();
            for (auto & [path, info] : infos)
                if (updated.contains(path))
                    cache->erase(path);
                else
                    cache->upsert(path, PathInfoCacheValue{.value = std::make_shared<const ValidPathInfo>(info)});
        }
    });
}

/* Invalidate a path.  The caller is responsible for checking that
   there are no referrers. */
void LocalStore::invalidatePath(State & state, const StorePath & path)
{
    debug("invalidating path '%s'", printStorePath(path));

    state.stmts->InvalidatePath.use().apply(printStorePath(path)).exec();

    /* Note that the foreign key constraints on the Refs table take
       care of deleting the references entries for `path'. */

    invalidatePathInfoCacheFor(path);
}

const PublicKeys & LocalStore::getPublicKeys()
{
    auto state(_state->lock());
    if (!state->publicKeys)
        state->publicKeys = std::make_unique<PublicKeys>(getDefaultPublicKeys());
    return *state->publicKeys;
}

bool LocalStore::pathInfoIsUntrusted(const ValidPathInfo & info)
{
    return config->requireSigs && !info.checkSignatures(*this, getPublicKeys());
}

bool LocalStore::realisationIsUntrusted(const Realisation & realisation)
{
    return config->requireSigs && !realisation.checkSignatures(realisation.id, getPublicKeys());
}

std::shared_ptr<GitObjectStore::Hold> LocalStore::acquireIngestionHold()
{
    auto weak = ingestionHold.lock();
    if (auto hold = weak->lock())
        return hold;
    auto hold = std::make_shared<GitObjectStore::Hold>(objects.hold());
    *weak = hold;
    return hold;
}

void LocalStore::addMultipleToStore(
    PathsSource && pathsToCopy, Activity & act, RepairFlag repair, CheckSigsFlag checkSigs)
{
    /* Keep the shared directory alive across this batch. Every writer
       acquires its own lease as well: another overlapping batch or direct
       add may outlive this call. A batch with nothing to write makes none. */
    std::shared_ptr<GitObjectStore::Hold> hold;
    for (auto & [info, _] : pathsToCopy)
        if (repair || !isValidPath(info.path)) {
            hold = acquireIngestionHold();
            break;
        }
    /* Small local objects spend their time in pathname operations and the
       same registration connection. Spreading them over a CPU-sized pool
       serialises on that lock anyway, with a handoff for each lookup.
       Large streams still overlap their reads and writes. This changes no
       graph ordering, error handling or registration boundary. */
    bool small = std::all_of(pathsToCopy.begin(), pathsToCopy.end(), [](const auto & p) {
        return p.second->isInMemory() && p.first.narSize && p.first.narSize <= (1 << 20);
    });
    Store::addMultipleToStore(std::move(pathsToCopy), act, repair, checkSigs, small ? 1 : 0);
}

void LocalStore::addToStore(const ValidPathInfo & info, Source & source, RepairFlag repair, CheckSigsFlag checkSigs)
{
    /* Something must name the content, or nothing below verifies it: the
       path's own content address, the sender's object hash, or its NAR
       hash (an old peer's).  An input-addressed path with neither would be
       registered as whatever arrived. */
    if (!info.ca && !info.objectHash && !info.assertedNarHash)
        throw Error("path info for '%s' carries no content hash to verify", printStorePath(info.path));

    /* Signatures are checked before the stream is read, except a version-1
       signature on a description that asserts no NAR hash (01 §10, *a
       version-1 signature is checked on the stream*): that check needs
       the stream's NAR hash, so it
       is made after the restore, on the tee below, and the restored path
       is removed when it fails.  Nothing is registered before the check. */
    bool checkSigsAfterRestore = false;
    if (checkSigs && pathInfoIsUntrusted(info)) {
        if (!info.assertedNarHash && !info.sigs.empty())
            checkSigsAfterRestore = true;
        else
            throw Error(
                "cannot add path '%s' because it lacks a signature by a trusted key", printStorePath(info.path));
    }

    {
        addTempRoot(info.path);

        if (repair || !isValidPath(info.path)) {

            PathLocks outputLock;

            auto realPath = toRealPath(info.path);

            /* Lock the output path.  But don't lock if we're being called
               from a build hook (whose parent process already acquired a
               lock on this path). */
            if (!locksHeld.count(printStorePath(info.path)))
                outputLock.lockPaths({realPath});

            /* The path may have been created by another process in the meantime, so check again. */
            if (repair || !isValidPathUncached(info.path)) {

                deletePath(realPath);

                /* Before the restore: a collection completing between the
                   restore and the registration would sweep the objects the
                   restore writes, this path's root not yet in the column
                   (01 section 9.10, "Collection"; section 10, *the ingestion's
                   `autoGC()` runs before the restore*). */
                autoGC();

                /* The directory protects every overlapping writer until
                   the last lease ends. This path's rows and ingestion
                   identity are its own, and finish before its lease ends. */
                auto holdLease = acquireIngestionHold();
                auto ownHold = holdLease->ownRows();
                const GitObjectStore::Hold & hold = ownHold;

                /* A NAR hash the sender asserted is verified on a tee of
                   the stream and never stored (01 section 9.11); the same
                   tee gives the hash a deferred version-1 signature check
                   needs (SHA-256, the algorithm of every version-1
                   fingerprint made by a Nix that signs). */
                std::optional<HashSink> narHashSink;
                std::optional<TeeSource> teeSource;
                Source * restoreSource = &source;
                if (info.assertedNarHash || checkSigsAfterRestore) {
                    narHashSink.emplace(info.assertedNarHash ? info.assertedNarHash->algo : HashAlgorithm::SHA256);
                    teeSource.emplace(source, *narHashSink);
                    restoreSource = &*teeSource;
                }
                /* Verify the content address on the bytes already being
                   consumed. parseDump accepts only canonical NARs (tag
                   order, sorted entries, zero padding), so its stream is
                   also the NAR the restored tree would dump. Flat/text
                   addresses see only the root regular file's contents. */
                std::optional<MaskedHashSink> caSink;
                std::optional<TeeSource> caSource;
                Sink * rootContents = nullptr;
                if (info.ca && info.ca->method.getFileIngestionMethod() != FileIngestionMethod::Git) {
                    caSink.emplace(info.ca->hash.algo, std::string(info.path.hashPart()));
                    if (info.ca->method.getFileIngestionMethod() == FileIngestionMethod::NixArchive) {
                        caSource.emplace(*restoreSource, *caSink);
                        restoreSource = &*caSource;
                    } else
                        rootContents = &*caSink;
                }
                auto canonicalisingRestoreHooks =
                    makeCanonicalisingRestoreHooks(NIX_WHEN_SUPPORT_ACLS2(config->getLocalSettings().ignoredAcls));

                auto restored = restoreThroughObjects(
                    realPath,
                    *restoreSource,
                    config->getLocalSettings().fsyncStorePaths,
                    canonicalisingRestoreHooks.get(),
                    hold,
                    repair,
                    rootContents);

                /* The info to register: `info` with the object hash and NAR
                   size the sink computed, and without the sender's NAR-hash
                   assertion, which is checked against the tee below. */
                ValidPathInfo toRegister{info};
                /* The sender's thunk may hold the object's bytes -- the
                   evaluator's write buffer builds one over the text it
                   still has (`write-buffer.cc`, `infoFor`) -- and the
                   path-info cache keeps every description registered, so
                   those bytes would stay resident for the life of the
                   process.  The object is in the store now, so the walk
                   names it: the same hash, holding nothing (01 §9.11, the
                   cost falls on the old peer). */
                toRegister.lazyNarHash = [this, path = info.path] { return narHashOf(*this, path); };
                toRegister.objectHash = ObjectHash::of(restored.root);
                toRegister.narSize = restored.narSize;
                toRegister.assertedNarHash = std::nullopt;

                /* The content address first, then the sender's object hash,
                   NAR hash and size (01 section 9.10, "The content address
                   is checked first"). */
                if (info.ca) {
                    auto & specified = *info.ca;
                    auto actualHash = ({
                        Hash h{HashAlgorithm::SHA256}; // throwaway def to appease C++
                        auto fim = specified.method.getFileIngestionMethod();
                        switch (fim) {
                        case FileIngestionMethod::Flat:
                            if (restored.root.mode != merkle::Mode::Regular
                                && restored.root.mode != merkle::Mode::Executable)
                                throw Error(
                                    "flat content address requires a regular file at '%s'", printStorePath(info.path));
                            [[fallthrough]];
                        case FileIngestionMethod::NixArchive: {
                            h = caSink->finish().hash;
                            break;
                        }
                        case FileIngestionMethod::Git:
                            /* The sink's root is this identifier.  The SHA-1
                               form an older Nix made cannot be checked here
                               and is refused, the restored bytes removed as
                               the signature check below removes them; the
                               blobs and trees the restore entered stay in
                               `.objects` until the next whole-store
                               collection sweeps them (01 §10, *a version-1
                               signature is checked on the stream*: the same
                               cost as the signature refusal's). */
                            if (specified.hash.algo != merkle::hashAlgo) {
                                deletePath(realPath);
                                throw Error(
                                    "cannot import path '%s': its content address '%s' is under %s, and the git "
                                    "content-address method admits SHA-256 only; re-add the source with this Nix "
                                    "('nix store add --mode git'), or carry the path with 'nix-store --export | "
                                    "nix-store --import', which asserts no content address (the path's files were "
                                    "removed; the objects its restore entered go at the next 'nix-store --gc')",
                                    printStorePath(info.path),
                                    specified.render(),
                                    printHashAlgo(specified.hash.algo));
                            }
                            h = merkle::objectHash(restored.root);
                            break;
                        }
                        ContentAddress{
                            .method = specified.method,
                            .hash = std::move(h),
                        };
                    });
                    if (specified.hash != actualHash.hash) {
                        throw Error(
                            "ca hash mismatch importing path '%s';\n  specified: %s\n  got:       %s",
                            printStorePath(info.path),
                            specified.hash.to_string(HashFormat::Nix32, true),
                            actualHash.hash.to_string(HashFormat::Nix32, true));
                    }
                }

                if (info.objectHash && *info.objectHash != *toRegister.objectHash)
                    throw Error(
                        "object hash mismatch importing path '%s';\n  specified: %s\n  got:       %s",
                        printStorePath(info.path),
                        info.objectHash->render(),
                        toRegister.objectHash->render());

                std::optional<Hash> streamNarHash;
                if (narHashSink)
                    streamNarHash = narHashSink->finish().hash;

                if (info.assertedNarHash && *streamNarHash != *info.assertedNarHash)
                    throw Error(
                        "hash mismatch importing path '%s';\n  specified: %s\n  got:       %s",
                        printStorePath(info.path),
                        info.assertedNarHash->to_string(HashFormat::SRI, true),
                        streamNarHash->to_string(HashFormat::SRI, true));

                /* The sink's size is the NAR's length (`merkle::nar`, pinned
                   against `dumpPath`), so no tee is needed for this check. */
                if (info.narSize != 0 && restored.narSize != info.narSize)
                    throw Error(
                        "size mismatch importing path '%s';\n  specified: %s\n  got:       %s",
                        printStorePath(info.path),
                        info.narSize,
                        restored.narSize);

                /* The deferred signature check: the description with the
                   stream's NAR hash and size in place of the assertion it
                   lacked, under the same rule as before the stream. */
                if (checkSigsAfterRestore) {
                    ValidPathInfo described{info};
                    described.assertedNarHash = streamNarHash;
                    described.narSize = restored.narSize;
                    if (pathInfoIsUntrusted(described)) {
                        deletePath(realPath);
                        throw Error(
                            "cannot add path '%s' because it lacks a signature by a trusted key",
                            printStorePath(info.path));
                    }
                }

                if (config->getLocalSettings().fsyncStorePaths) {
                    recursiveSync(realPath);
                    syncParent(realPath);
                }

                registerValidPath(toRegister, hold);
            } else {
                /* Nothing restored, so a deferred signature check has no
                   stream to check against: refused, as every untrusted
                   description was before the deferral. */
                if (checkSigsAfterRestore)
                    throw Error(
                        "cannot add path '%s' because it lacks a signature by a trusted key",
                        printStorePath(info.path));
                // We may have a negative cache entry for this path, so get rid of it.
                invalidatePathInfoCacheFor(info.path);
            }

            outputLock.setDeletion(true);
        }
    }
}

StorePath LocalStore::addToStoreFromDump(
    Source & source0,
    std::string_view name,
    FileSerialisationMethod dumpMethod,
    ContentAddressMethod hashMethod,
    HashAlgorithm hashAlgo,
    const StorePathSet & references,
    RepairFlag repair)
{
    return addToStoreFromDump(source0, name, dumpMethod, hashMethod, hashAlgo, references, repair, false);
}

StorePath LocalStore::addToStoreFromDump(
    Source & source0,
    std::string_view name,
    FileSerialisationMethod dumpMethod,
    ContentAddressMethod hashMethod,
    HashAlgorithm hashAlgo,
    const StorePathSet & originalReferences,
    RepairFlag repair,
    bool filterReferences)
{
    checkIngestionAlgorithm(hashMethod.getFileIngestionMethod(), hashAlgo);

    bool methodsMatch = static_cast<FileIngestionMethod>(dumpMethod) == hashMethod.getFileIngestionMethod();

    /* The dump's hash is the content address only when the methods match;
       under the git method the sink's root is, and the dump is not hashed. */
    std::optional<HashSink> hashSink;
    if (methodsMatch)
        hashSink.emplace(hashAlgo);
    std::optional<PathRefScanSink> refSink;
    if (filterReferences)
        // Only scan if we really need to, since it's slower.
        refSink = PathRefScanSink::fromPaths(originalReferences);
    LambdaSink tap{[&](std::string_view data) {
        if (hashSink)
            (*hashSink)(data);
        if (refSink)
            (*refSink)(data);
    }};
    TeeSource source{source0, tap};
    const LocalSettings & localSettings = config->getLocalSettings();

    /* Read the source path into memory, but only if it's up to
       narBufferSize bytes. If it's larger, write it to a temporary
       location in the Nix store. If the subsequently computed
       destination store path is already valid, we just delete the
       temporary path. Otherwise, we move it to the destination store
       path. */
    bool inMemory = false;

    /* Because std::string has resize_and_overwrite. */
    std::string dump;

    /* Fill out buffer, and decide whether we are working strictly in
       memory based on whether we break out because the buffer is full
       or the original source is empty */
    while (dump.size() < localSettings.narBufferSize) {
        const auto oldSize = dump.size();
        constexpr size_t chunkSize = 65536;
        auto want = std::min(chunkSize, localSettings.narBufferSize - oldSize);
        std::exception_ptr ex;
        dump.resize_and_overwrite(
            oldSize + want,
            [&inMemory, &source, &ex, sz = oldSize](char * buf, std::size_t bufSize) -> std::string::size_type {
                try {
                    auto got = source.read(buf + sz, bufSize - sz);
                    return sz + got;
                } catch (EndOfFile &) {
                    inMemory = true;
                    return sz;
                } catch (...) {
                    ex = std::current_exception();
                    return sz;
                }
            });
        if (ex)
            std::rethrow_exception(ex);
        if (inMemory)
            break;
    }

    std::unique_ptr<AutoDelete> delTempDir;
    std::filesystem::path tempPath;
    std::filesystem::path tempDir;
    AutoCloseFD tempDirFd;
    /* The sink's result when a NAR was restored through it; a flat file
       or a plain copy is entered by one walk below instead. */
    std::optional<ObjectHashSink::Result> restored;

    /* If the methods don't match, our streaming hash of the dump is the
       wrong sort, and we need to rehash.
       References are also in store path, if scanning we will need to move */
    bool inMemoryAndDontNeedRestore = inMemory && methodsMatch && !filterReferences;
    auto canonicalisingRestoreHooks =
        makeCanonicalisingRestoreHooks(NIX_WHEN_SUPPORT_ACLS2(config->getLocalSettings().ignoredAcls));

    /* The ingestion's hold on the objects it writes, made before its first
       write and released after the registration below, against a
       collection running meanwhile (`GitObjectStore::Hold`; `08` K16).
       Made at the restore, on either route, so that an add whose path is
       valid already makes none. */
    std::optional<GitObjectStore::Hold> hold;

    if (!inMemoryAndDontNeedRestore) {
        /* Before the restore, for the reason `addToStore` gives: the spilled
           restore below writes objects into the store before the path is
           registered, and whether the path is valid already is known only
           from the restored tree, so this route pays the free-space check
           whatever the outcome (01 section 10, *the ingestion's `autoGC()`
           runs before the restore*). */
        autoGC();

        /* Drain what we pulled so far, and then keep on pulling */
        StringSource dumpSource{dump};
        ChainSource bothSource{dumpSource, source};

        std::tie(tempDir, tempDirFd) = createTempDirInStore();
        delTempDir = std::make_unique<AutoDelete>(tempDir);
        tempPath = tempDir / "x";
        hold.emplace(objects.hold());

        if (dumpMethod == FileSerialisationMethod::NixArchive)
            restored = restoreThroughObjects(
                tempPath, bothSource, localSettings.fsyncStorePaths, canonicalisingRestoreHooks.get(), *hold, repair);
        else
            restorePath(
                tempPath, bothSource, dumpMethod, localSettings.fsyncStorePaths, canonicalisingRestoreHooks.get());

        std::string().swap(dump);
    }

    StorePathSet references;
    if (refSink.has_value()) {
        references = refSink->getResultPaths();
    } else {
        references = originalReferences;
    }

    /* The content address: the dump's own hash when the methods match; the
       sink's root for the git method; else a read of the restored tree. */
    auto desc = ContentAddressWithReferences::fromParts(
        hashMethod,
        methodsMatch ? hashSink->finish().hash
        : (restored && hashMethod.getFileIngestionMethod() == FileIngestionMethod::Git)
            ? merkle::objectHash(restored->root)
            : hashPath(makeFSSourceAccessor(tempPath), hashMethod.getFileIngestionMethod(), hashAlgo).first,
        {
            .others = references,
            // caller is not capable of creating a self-reference, because this is content-addressed without modulus
            .self = false,
        });

    auto dstPath = makeFixedOutputPathFromCA(name, desc);

    addTempRoot(dstPath);

    if (repair || !isValidPath(dstPath)) {

        /* The first check above is an optimisation to prevent
           unnecessary lock acquisition. */

        auto realPath = toRealPath(dstPath);

        PathLocks outputLock({realPath});

        /* The path may have been created by another process in the meantime, so check again. */
        if (repair || !isValidPathUncached(dstPath)) {

            deletePath(realPath);

            if (inMemoryAndDontNeedRestore) {
                /* Before the restore, for the same reason, and after the
                   validity check: an add whose path is valid already
                   restores nothing and runs no collection (master's
                   order; `LocalStoreAutoGCTest`). */
                autoGC();
                hold.emplace(objects.hold());

                StringSource dumpSource{dump};
                /* Restore from the buffer in memory. */
                auto fim = hashMethod.getFileIngestionMethod();
                switch (fim) {
                case FileIngestionMethod::NixArchive:
                    restored = restoreThroughObjects(
                        realPath,
                        dumpSource,
                        localSettings.fsyncStorePaths,
                        canonicalisingRestoreHooks.get(),
                        *hold,
                        repair);
                    break;
                case FileIngestionMethod::Flat:
                    restorePath(
                        realPath,
                        dumpSource,
                        (FileSerialisationMethod) fim,
                        localSettings.fsyncStorePaths,
                        canonicalisingRestoreHooks.get());
                    break;
                case FileIngestionMethod::Git:
                    // not a serialisation method: unreachable
                    assert(false);
                }
            } else {
                /* Move the temporary path we restored above. */
                try {
                    /* movePath and not renameFile because at this point the top-level directory is
                       read-only. */
                    movePath(tempPath, realPath);
                } catch (const SystemError & e) {
                    if (!e.is(std::errc::cross_device_link))
                        throw;

                    /* Apparently this can happen even on the same filesystem (and the paths that are renamed above
                       are on the same filesystem) with overlayfs https://github.com/NixOS/nix/issues/6262.
                       Since we couldn't rename, this won't be atomic and we have to gradually copy to the realPath. */
                    warn("can't rename %s as %s, copying instead", PathFmt(tempPath), PathFmt(realPath));
                    RestoreSink copySink{/*startFsync=*/false, /*hooks=*/canonicalisingRestoreHooks.get()};
                    copySink.dstPath = realPath;
                    copyRecursive(*makeFSSourceAccessor(tempPath), CanonPath::root, copySink, CanonPath::root);
                    delTempDir->deletePath();
                    /* Copied plainly: not in the object store; the walk below enters it. */
                    restored = std::nullopt;
                }
            }

            /* A NAR went through the sink, which entered the object store
               and computed the root; a flat file or a plain copy is entered
               now, by the one walk that also computes it. */
            auto result = restored ? *restored : enterPath(realPath, repair, *hold);

            if (localSettings.fsyncStorePaths) {
                recursiveSync(realPath);
                syncParent(realPath);
            }

            auto info = ValidPathInfo::makeFromCA(*this, name, std::move(desc), ObjectHash::of(result.root));
            info.narSize = result.narSize;
            registerValidPath(info, *hold);
        } else
            // We may have a negative cache entry for this path, so get rid of it.
            invalidatePathInfoCacheFor(dstPath);

        outputLock.setDeletion(true);
    }

    return dstPath;
}

/* Create a temporary directory in the store that won't be
   garbage-collected until the returned FD is closed. */
std::pair<std::filesystem::path, AutoCloseFD> LocalStore::createTempDirInStore()
{
    std::filesystem::path tmpDirFn;
    AutoCloseFD tmpDirFd;
    bool lockedByUs = false;
    do {
        /* There is a slight possibility that `tmpDir' gets deleted by
           the GC between createTempDir() and when we acquire a lock on it.
           We'll repeat until 'tmpDir' exists and we've locked it.
           Make the directory accessible only to the current user. */
        tmpDirFn = createTempDir(std::filesystem::path{config->realStoreDir.get()}, "tmp", /*mode=*/0700);
        tmpDirFd = openDirectory(tmpDirFn, FinalSymlink::DontFollow);
        if (!tmpDirFd) {
            continue;
        }
        lockedByUs = lockFile(tmpDirFd.get(), ltWrite, true);
    } while (!pathExists(tmpDirFn) || !lockedByUs);
    return {tmpDirFn, std::move(tmpDirFd)};
}

void PathInUse::anchor() {}

void LocalStore::invalidatePathChecked(const StorePath & path)
{
    retrySQLite<void>([&]() {
        auto state(_state->lock());

        SQLiteTxn txn(state->db);

        if (isValidPath_(*state, path)) {
            StorePathSet referrers;
            queryReferrers(*state, path, referrers);
            referrers.erase(path); /* ignore self-references */
            if (!referrers.empty())
                throw PathInUse(
                    "cannot delete path '%s' because it is in use by %s",
                    printStorePath(path),
                    concatMapStringsSep(", ", referrers, [&](auto & p) { return "'" + printStorePath(p) + "'"; }));
            invalidatePath(*state, path);
        }

        txn.commit();
    });
}

bool LocalStore::verifyStore(bool checkContents, RepairFlag repair)
{
    printInfo("reading the Nix store...");

    /* Acquire the global GC lock to get a consistent snapshot of
       existing and valid paths. */
    auto fdGCLock = openGCLock();
    FdLock gcLock(fdGCLock.get(), ltRead, true, "waiting for the big garbage collector lock...");

    auto [errors, validPaths] = verifyAllValidPaths(repair);

    /* Optionally, check the content hashes (slow). */
    if (checkContents) {

        /* The object files and law 4 are checked for a store whose object
           store is its own (`ownsObjectStore`); on the overlay store the
           merged `.objects` shows the lower store's files, which a repair
           would unlink (a whiteout) or copy up. */
        bool ownObjects = ownsObjectStore();
        if (!ownObjects)
            printInfo("the object store is shared with the lower store: its objects are not checked here");

        if (ownObjects)
            errors = objects.verify(repair) || errors;

        printInfo("checking store hashes...");

        for (auto & i : validPaths) {
            try {
                /* Migrated by `queryPathInfoUncached` if the row was old. */
                auto info =
                    std::const_pointer_cast<ValidPathInfo>(std::shared_ptr<const ValidPathInfo>(queryPathInfo(i)));

                /* Check the content hash (optionally - slow). */
                printMsg(lvlTalkative, "checking contents of '%s'", printStorePath(i));

                /* The object hash recomputed from the files, the names
                   unhacked as the serialiser unhacks them, and by the same
                   walk every object the path reaches that the object store
                   lacks (law 4 of 01 section 9.10, with law 3's exceptions:
                   `verifyObjects`). */
                auto realPath = toRealPath(i);

                if (!info->objectHash) {
                    /* A schema-10 row the migration left as read: the walk's
                       NAR hash was not the one the row asserts (`walkOldRow`,
                       01 section 10, *a schema-10 row's migration checks the
                       NAR hash the row asserts*).  Master's check, made here
                       with master's report and remedy: "was modified!", and
                       under --repair the path is fetched again; the row is
                       migrated by the next query once the bytes are right. */
                    if (!info->assertedNarHash)
                        throw Error("path '%s' has neither an object hash nor a NAR hash to check", printStorePath(i));
                    HashSink narSink{info->assertedNarHash->algo};
                    dumpPath(realPath, narSink);
                    auto current = narSink.finish().hash;
                    if (current != *info->assertedNarHash) {
                        printError(
                            "path '%s' was modified! expected hash '%s', got '%s'",
                            printStorePath(i),
                            info->assertedNarHash->to_string(HashFormat::Nix32, true),
                            current.to_string(HashFormat::Nix32, true));
                        if (repair)
                            getBuilder()->repairPath(i);
                        else
                            errors = true;
                    } else
                        /* Sound now (repaired since the row was read): the
                           next query migrates it. */
                        printInfo("path '%s' is not yet migrated to the object hash", printStorePath(i));
                    continue;
                }

                auto [current, missing] = verifyObjects(realPath);
                auto currentHash = ObjectHash::of(current.root);
                if (!ownObjects)
                    /* Law 4 is not this store's to check or repair. */
                    missing.clear();

                if (*info->objectHash != currentHash) {
                    printError(
                        "path '%s' was modified! expected object hash '%s', got '%s'",
                        printStorePath(i),
                        info->objectHash->render(),
                        currentHash.render());
                    if (repair)
                        getBuilder()->repairPath(i);
                    else
                        errors = true;
                } else {

                    /* Fill in missing narSize fields (from old stores). */
                    if (info->narSize == 0) {
                        printInfo("updating size field on '%s' to %s", printStorePath(i), current.narSize);
                        info->narSize = current.narSize;
                        updatePathInfo(*_state->lock(), *info);
                    }

                    /* Law 4 (01 section 9.10): every object the path's hash
                       reaches is in the object store.  A path that was
                       never entered (an older Nix wrote it; the shim
                       migrated its row without entering), or whose objects
                       a full directory index or an interrupted repair left
                       missing, is re-entered under --repair by the walk
                       that enters a built output, and "entered" is said
                       only once the same check finds nothing missing.  The
                       hold is the type's demand; no collection runs beside
                       this verifier, which holds the GC lock shared. */
                    if (!missing.empty()) {
                        printError(
                            "path '%s' reaches %d objects the object store lacks (first: %s)",
                            printStorePath(i),
                            missing.size(),
                            PathFmt(missing.front()));
                        if (repair) {
                            /* The verifier holds GC shared. Ingestion can
                               hold this output lock while waiting for auto-GC,
                               so waiting here would invert that order. */
                            PathLocks outputLock;
                            if (!locksHeld.count(printStorePath(i)) && !outputLock.lockPaths({realPath}, "", false)) {
                                printError(
                                    "cannot repair objects of '%s': the path is being modified; verification is incomplete",
                                    printStorePath(i));
                                errors = true;
                                continue;
                            }
                            auto hold = objects.hold();
                            enterPath(realPath, Repair, hold);
                            /* The rows the re-entry made. */
                            endIngestion(hold, i);
                            auto still = verifyObjects(realPath).second;
                            outputLock.setDeletion(true);
                            if (still.empty())
                                printInfo("entered '%s' into the object store", printStorePath(i));
                            else {
                                printError(
                                    "could not enter '%s' into the object store: %d objects still missing (first: %s)",
                                    printStorePath(i),
                                    still.size(),
                                    PathFmt(still.front()));
                                errors = true;
                            }
                        } else
                            errors = true;
                    }
                }

            } catch (Error & e) {
                /* It's possible that the path got GC'ed, so ignore
                   errors on invalid paths. */
                if (isValidPath(i))
                    logError(e.info());
                else
                    logWarning(e.info());
                errors = true;
            }
        }
    }

    return errors;
}

LocalStore::VerificationResult LocalStore::verifyAllValidPaths(RepairFlag repair)
{
    StorePathSet storePathsInStoreDir;
    /* Why aren't we using `queryAllValidPaths`? Because that would
       tell us about all the paths than the database knows about. Here we
       want to know about all the store paths in the store directory,
       regardless of what the database thinks.

       We will end up cross-referencing these two sources of truth (the
       database and the filesystem) in the loop below, in order to catch
       invalid states.
     */
    for (auto & i : DirectoryIterator{config->realStoreDir.get()}) {
        checkInterrupt();
        try {
            storePathsInStoreDir.insert({i.path().filename().string()});
        } catch (BadStorePath &) {
        }
    }

    /* Check whether all valid paths actually exist. */
    printInfo("checking path existence...");

    StorePathSet done;

    auto existsInStoreDir = [&](const StorePath & storePath) { return storePathsInStoreDir.count(storePath); };

    bool errors = false;
    StorePathSet validPaths;

    for (auto & i : queryAllValidPaths())
        verifyPath(i, existsInStoreDir, done, validPaths, repair, errors);

    return {
        .errors = errors,
        .validPaths = validPaths,
    };
}

void LocalStore::verifyPath(
    const StorePath & path,
    fun<bool(const StorePath &)> existsInStoreDir,
    StorePathSet & done,
    StorePathSet & validPaths,
    RepairFlag repair,
    bool & errors)
{
    checkInterrupt();

    if (!done.insert(path).second)
        return;

    if (!existsInStoreDir(path)) {
        /* Check any referrers first.  If we can invalidate them
           first, then we can invalidate this path as well. */
        bool canInvalidate = true;
        StorePathSet referrers;
        queryReferrers(path, referrers);
        for (auto & i : referrers)
            if (i != path) {
                verifyPath(i, existsInStoreDir, done, validPaths, repair, errors);
                if (validPaths.count(i))
                    canInvalidate = false;
            }

        auto pathS = printStorePath(path);

        if (canInvalidate) {
            printInfo("path '%s' disappeared, removing from database...", pathS);
            invalidatePath(*_state->lock(), path);
        } else {
            printError("path '%s' disappeared, but it still has valid referrers!", pathS);
            if (repair)
                try {
                    getBuilder()->repairPath(path);
                } catch (Error & e) {
                    logWarning(e.info());
                    errors = true;
                }
            else
                errors = true;
        }

        return;
    }

    validPaths.insert(std::move(path));
}

unsigned int LocalStore::getProtocol()
{
    return WorkerProto::latest.number.toWire();
}

std::optional<TrustedFlag> LocalStore::isTrustedClient()
{
    return Trusted;
}

void LocalStore::vacuumDB()
{
    _state->lock()->db.exec("vacuum");
}

void LocalStore::addSignatures(const StorePath & storePath, const std::set<Signature> & sigs)
{
    /* The transaction below re-writes the row from `queryPathInfoInternal`,
       which does not migrate; a schema-10 row is migrated first, outside
       the lock, so that the row written holds its object hash. */
    migratePathInfo(storePath);

    retrySQLite<void>([&]() {
        auto state(_state->lock());

        SQLiteTxn txn(state->db);

        auto info = std::const_pointer_cast<ValidPathInfo>(queryPathInfoInternal(*state, storePath));

        info->sigs.insert(sigs.begin(), sigs.end());

        updatePathInfo(*state, *info);

        txn.commit();
    });
}

std::optional<std::pair<int64_t, UnkeyedRealisation>>
LocalStore::queryRealisationCore_(LocalStore::State & state, const DrvOutput & id)
{
    auto useQueryRealisedOutput(
        state.stmts->QueryRealisedOutput.use().apply(id.drvPath.to_string()).apply(id.outputName));
    if (!useQueryRealisedOutput.next())
        return std::nullopt;
    auto realisationDbId = useQueryRealisedOutput.getInt(0);
    auto outputPath = parseStorePath(useQueryRealisedOutput.getStr(1));
    auto signatures = tokenizeString<StringSet>(useQueryRealisedOutput.getStr(2));

    return {
        {realisationDbId,
         UnkeyedRealisation{
             .outPath = outputPath,
             .signatures = Signature::parseMany(signatures),
         }}};
}

std::optional<const UnkeyedRealisation> LocalStore::queryRealisation_(LocalStore::State & state, const DrvOutput & id)
{
    auto maybeCore = queryRealisationCore_(state, id);
    if (!maybeCore)
        return std::nullopt;
    auto [realisationDbId, res] = *maybeCore;

    return {res};
}

void LocalStore::queryRealisationUncached(
    const DrvOutput & id, Callback<std::shared_ptr<const UnkeyedRealisation>> callback) noexcept
{
    try {
        auto maybeRealisation = retrySQLite<std::optional<const UnkeyedRealisation>>(
            [&]() { return queryRealisation_(*_state->lock(), id); });
        if (maybeRealisation)
            callback(std::make_shared<const UnkeyedRealisation>(maybeRealisation.value()));
        else
            callback(nullptr);

    } catch (...) {
        callback.rethrow();
    }
}

void LocalStore::addBuildLog(const StorePath & drvPath, std::string_view log)
{
    assert(drvPath.isDerivation());

    auto baseName = drvPath.to_string();

    auto logPath =
        config->logDir.get() / drvsLogDir / baseName.substr(0, 2) / (std::string(baseName.substr(2)) + ".bz2");

    if (pathExists(logPath))
        return;

    createDirs(logPath.parent_path());

    auto tmpFile = logPath;
    tmpFile += ".tmp." + std::to_string(getpid());

    writeFile(tmpFile, compress(CompressionAlgo::bzip2, log));

    std::filesystem::rename(tmpFile, logPath);
}

std::optional<std::string> LocalStore::getVersion()
{
    return nixVersion;
}

static RegisterStoreImplementation<LocalStore::Config> regLocalStore;

} // namespace nix
