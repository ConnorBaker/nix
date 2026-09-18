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

struct LocalStore::State::Stmts
{
    /* Some precompiled SQLite statements. */
    SQLiteStmt RegisterValidPath;
    SQLiteStmt UpdatePathInfo;
    SQLiteStmt AddReference;
    SQLiteStmt QueryPathInfo;
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
};

LocalStore::LocalStore(ref<const Config> config)
    : Store{*config}
    , LocalFSStore{*config}
    , config{config}
    , _state(make_ref<Sync<State>>())
    , dbDir(config->stateDir.get() / "db")
    , objects(config->realStoreDir.get() / ".objects", config->getLocalSettings().fsyncStorePaths.get())
    , reservedPath(dbDir / "reserved")
    , schemaPath(dbDir / "schema")
    , tempRootsDir(config->stateDir.get() / "temproots")
    , fnTempRoots(tempRootsDir / std::to_string(getpid()))
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
        if (*fdTempRoots) {
            fdTempRoots->close();
            tryUnlink(fnTempRoots);
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
    state.db = SQLite(dbDir / "db.sqlite", {.mode = openMode, .useWAL = settings.useSQLiteWAL});

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

    if (pathInfoCache)
        pathInfoCache->lock()->upsert(
            info.path, PathInfoCacheValue{.value = std::make_shared<const ValidPathInfo>(info)});

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
    auto use(state.stmts->QueryPathInfo.use().apply(printStorePath(path)));
    if (!use.next())
        throw InvalidPath("path '%s' is not valid", printStorePath(path));
    return use.getInt(0);
}

bool LocalStore::isValidPath_(State & state, const StorePath & path)
{
    return state.stmts->QueryPathInfo.use().apply(printStorePath(path)).next();
}

bool LocalStore::isValidPathUncached(const StorePath & path)
{
    return retrySQLite<bool>([&]() { return isValidPath_(*_state->lock(), path); });
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

        for (auto & [_, i] : infos) {
            i.requireObjectHash(*this);
            if (isValidPath_(*state, i.path))
                updatePathInfo(*state, i);
            else
                addValidPath(*state, i);
            paths.insert(i.path);
        }

        for (auto & [_, i] : infos) {
            auto referrer = queryValidPathId(*state, i.path);
            for (auto & j : i.references)
                state->stmts->AddReference.use().apply(referrer).apply(queryValidPathId(*state, j)).exec();
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

        txn.commit();
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
                auto canonicalisingRestoreHooks =
                    makeCanonicalisingRestoreHooks(NIX_WHEN_SUPPORT_ACLS2(config->getLocalSettings().ignoredAcls));

                auto restored = restoreThroughObjects(
                    realPath,
                    *restoreSource,
                    config->getLocalSettings().fsyncStorePaths,
                    canonicalisingRestoreHooks.get(),
                    repair);

                /* The info to register: `info` with the object hash and NAR
                   size the sink computed, and without the sender's NAR-hash
                   assertion, which is checked against the tee below. */
                ValidPathInfo toRegister{info};
                toRegister.objectHash = ObjectHash::of(restored.root);
                toRegister.narSize = restored.narSize;
                toRegister.assertedNarHash = std::nullopt;

                /* The content address first, then the sender's object hash,
                   NAR hash and size (01 section 9.10, "The content address
                   is checked first"). */
                if (info.ca) {
                    auto & specified = *info.ca;
                    auto actualHash = ({
                        SourcePath sourcePath = requireStoreObjectAccessor(info.path, /*requireValidPath=*/false);
                        Hash h{HashAlgorithm::SHA256}; // throwaway def to appease C++
                        auto fim = specified.method.getFileIngestionMethod();
                        switch (fim) {
                        case FileIngestionMethod::Flat:
                        case FileIngestionMethod::NixArchive: {
                            MaskedHashSink caSink{
                                specified.hash.algo,
                                std::string{info.path.hashPart()},
                            };
                            dumpPath(sourcePath, caSink, (FileSerialisationMethod) fim);
                            h = caSink.finish().hash;
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

                registerValidPath(toRegister);
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

        if (dumpMethod == FileSerialisationMethod::NixArchive)
            restored = restoreThroughObjects(
                tempPath, bothSource, localSettings.fsyncStorePaths, canonicalisingRestoreHooks.get(), repair);
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

                StringSource dumpSource{dump};
                /* Restore from the buffer in memory. */
                auto fim = hashMethod.getFileIngestionMethod();
                switch (fim) {
                case FileIngestionMethod::NixArchive:
                    restored = restoreThroughObjects(
                        realPath, dumpSource, localSettings.fsyncStorePaths, canonicalisingRestoreHooks.get(), repair);
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
            auto result = restored ? *restored : enterPath(realPath, repair);

            if (localSettings.fsyncStorePaths) {
                recursiveSync(realPath);
                syncParent(realPath);
            }

            auto info = ValidPathInfo::makeFromCA(*this, name, std::move(desc), ObjectHash::of(result.root));
            info.narSize = result.narSize;
            registerValidPath(info);
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

        std::vector<std::string> blobDirs;
        if (ownObjects) {
            printInfo("checking object hashes...");
            blobDirs = {"blobs", "blobs-x"};
        }

        for (auto & sub : blobDirs) {
            if (!pathExists(objects.dir / sub))
                continue;
            for (auto & object : DirectoryIterator{objects.dir / sub}) {
                checkInterrupt();
                auto name = object.path().filename().string();
                printMsg(lvlTalkative, "checking contents of %s", PathFmt(object.path()));
                auto id = GitObjectStore::idString(GitObjectStore::blobIdOfFile(object.path()));
                if (id != name) {
                    printError(
                        "object %s was modified! expected identifier %s, got '%s'", PathFmt(object.path()), name, id);
                    if (repair) {
                        if (::unlink(object.path().c_str()) == 0)
                            printInfo("removed object %s", PathFmt(object.path()));
                        else
                            throw SysError("removing corrupt object %s", PathFmt(object.path()));
                    } else
                        errors = true;
                }
            }
        }

        if (ownObjects && pathExists(objects.dir / "trees")) {
            for (auto & object : DirectoryIterator{objects.dir / "trees"}) {
                checkInterrupt();
                auto name = object.path().filename().string();
                auto id = GitObjectStore::idString(merkle::treeId(readFile(object.path())));
                if (id != name) {
                    printError(
                        "tree object %s was modified! expected identifier %s, got '%s'",
                        PathFmt(object.path()),
                        name,
                        id);
                    if (repair) {
                        if (::unlink(object.path().c_str()) == 0)
                            printInfo("removed tree object %s", PathFmt(object.path()));
                        else
                            throw SysError("removing corrupt tree object %s", PathFmt(object.path()));
                    } else
                        errors = true;
                }
            }
        }

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
                       reaches is in the object store.  A path whose tree a
                       collection took between its restore and its
                       registration, or that was never entered (an older Nix
                       wrote it; the shim migrated its row without entering)
                       is re-entered under --repair by the walk that enters
                       a built output, and "entered" is said only once the
                       same check finds nothing missing. */
                    if (!missing.empty()) {
                        printError(
                            "path '%s' reaches %d objects the object store lacks (first: %s)",
                            printStorePath(i),
                            missing.size(),
                            PathFmt(missing.front()));
                        if (repair) {
                            enterPath(realPath, Repair);
                            auto still = verifyObjects(realPath).second;
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
