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
#include "nix/util/finally.hh"
#include "nix/util/topo-sort.hh"
#include "nix/util/compression.hh"
#include "nix/util/signals.hh"
#include "nix/store/posix-fs-canonicalise.hh"
#include "nix/util/source-accessor.hh"
#include "nix/store/keys.hh"
#include "nix/util/users.hh"
#include "nix/store/store-registration.hh"

#include <algorithm>
#include <array>
#include <ranges>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>

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

[[gnu::always_inline]]
static inline void writeBlobRow(auto && use, uint64_t ingestion, const GitObjectStore::Hold::Row & row)
{
    use.apply(row.id.hash, row.id.hashSize)
        .apply(int64_t(row.executable ? 1 : 0))
        .apply(static_cast<int64_t>(row.ino))
        .apply(static_cast<int64_t>(row.dev))
        .apply(static_cast<int64_t>(ingestion))
        .apply(row.name)
        .apply(static_cast<int64_t>(row.size))
        .exec();
}

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
    return ref{std::shared_ptr<LocalStore>{new LocalStore(ref{shared_from_this()}), LocalStore::Deleter{}}};
}

bool LocalStoreConfig::getDefaultRequireSigs()
{
    return settings.requireSigs;
}

/* Decode the index's raw SHA-256 key. */
static Hash blobIdOf(std::string_view bytes)
{
    Hash id(GitObjectStore::hashAlgo);
    if (bytes.size() != id.hashSize)
        throw Error("blob index row holds a %d-byte identifier, not %d", bytes.size(), id.hashSize);
    memcpy(id.hash, bytes.data(), id.hashSize);
    return id;
}

/* SQL holder names are exact canonical coordinates inside a registered root. */
static GitObjectStore::Holder rootedBlobHolder(
    GitObjectStore::Holder holder, std::filesystem::path root, std::string_view rootName, std::string_view rawName)
{
    auto name = CanonPath(rawName);
    if (name.rel() != rawName || std::filesystem::path(rawName).has_root_path()
        || rootName.find_first_of(":\\") != rootName.npos)
        throw Error("blob index row has a noncanonical root-relative name");
    auto coordinate = CanonPath::fromFilename(rootName) / name;
    holder.address = GitObjectStore::Holder::Registered{std::move(root), std::move(coordinate)};
    return holder;
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
    SQLiteStmt ReadAuxiliary;
    std::array<SQLiteStmt, 2> ReadAuxiliaryExact;
    SQLiteStmt InsertAuxiliary;
    std::array<SQLiteStmt, 2> DeleteAuxiliary;
    /* The blob index (`06-holder-index.md`). */
    SQLiteStmt LookupBlob;
    SQLiteStmt InsertBlob;
    SQLiteStmt DeleteBlob;
    SQLiteStmt DeleteOwnBlob;
    SQLiteStmt DeleteOwnBlobs;
    SQLiteStmt QueryBlobCollisions;
    SQLiteStmt DeletePathIngestions;
    SQLiteStmt NewIngestion;
    SQLiteStmt SetIngestionPath;
};

static constexpr auto lookupBlobSql =
    "select b.ino, b.dev, b.size, i.path, b.name, b.ingestion from Blobs b"
    " join Ingestions i on i.id = b.ingestion join ValidPaths v on v.path = i.path"
    " where b.id = ? and b.exec = ? and i.id > 0"
    " and typeof(b.dev) = 'integer' and typeof(b.ino) = 'integer'"
    " and (b.dev, b.ino, b.ingestion) > (?, ?, ?);";

/* Storage class is part of an incarnation key; unary + removes column affinity. */
static constexpr auto auxiliaryExactKeySql =
    "typeof(kind) = typeof(?1) and +kind collate binary is ?1"
    " and typeof(id) = typeof(?2) and +id collate binary is ?2"
    " and typeof(ingestion) = typeof(?3) and +ingestion collate binary is ?3";

LocalStore::LocalStore(ref<const Config> config)
    : Store{*config}
    , LocalFSStore{*config}
    , config{config}
    , _state(make_ref<Sync<State>>())
    , dbDir(config->stateDir.get() / "db")
    , objects(config->realStoreDir.get() / ".objects")
    , reservedPath(dbDir / "reserved")
    , schemaPath(dbDir / "schema")
    , tempRootsDir(config->stateDir.get() / "temproots")
{
    auto state(_state->lock());
    state->stmts = std::make_unique<State::Stmts>();

    /* Create missing state directories if they don't already exist. */
    createDirs(config->realStoreDir.get());
    if (config->readOnly)
        experimentalFeatureSettings.require(Xp::ReadOnlyLocalStore);
    else
        makeStoreWritable();
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
    /* Cache tables are optional in an older read-only store. Its valid
       paths and their hashes remain readable without migrating a cache. */
    SQLiteStmt tables(state->db, "select 1 from sqlite_master where type = 'table' and name = ?;");
    bool hasBlobs = tables.use(SQLiteStmt::Bindings::Borrow).apply("Blobs").next();
    bool hasBlobIndex = hasBlobs && tables.use(SQLiteStmt::Bindings::Borrow).apply("Ingestions").next();
    bool hasAuxiliary = tables.use(SQLiteStmt::Bindings::Borrow).apply("AuxValues").next();
    if (hasAuxiliary) {
        state->stmts->ReadAuxiliary.create(
            state->db,
            "select a.ingestion, a.chunk, a.body from AuxValues a"
            " where a.kind = ?1 and a.id = ?2 and typeof(a.kind) = 'integer'"
            " and typeof(a.ingestion) = 'integer' and a.ingestion = ("
            " select min(ingestion) from AuxValues where typeof(kind) = 'integer' and typeof(ingestion) = 'integer'"
            " and kind = ?1 and id = ?2 and (?3 is null or ingestion > ?3)) order by a.chunk;");
        state->stmts->InsertAuxiliary.create(
            state->db, "insert into AuxValues (kind, id, ingestion, chunk, body) values (?, ?, ?, ?, ?);");
    }
    if (hasBlobIndex) {
        state->stmts->LookupBlob.create(state->db, lookupBlobSql);
        state->stmts->InsertBlob.create(
            state->db,
            "insert or replace into Blobs (id, exec, ino, dev, ingestion, name, size) values (?, ?, ?, ?, ?, ?, ?);");
        state->stmts->DeleteBlob.create(
            state->db,
            "delete from Blobs where id = ? and exec = ? and ino = ? and dev = ? and ingestion = ? and name = ? and size = ?;");
        state->stmts->DeleteOwnBlob.create(
            state->db, "delete from Blobs where id = ? and exec = ? and ingestion = ?;");
        state->stmts->DeleteOwnBlobs.create(state->db, "delete from Blobs where ingestion = ?;");
        state->stmts->QueryBlobCollisions.create(
            state->db,
            "select own.id, own.exec,"
            " donor.ino, donor.dev, donor.size, i.path, donor.name, donor.ingestion"
            " from Blobs own join Blobs donor on donor.id = own.id and donor.exec = own.exec"
            " join Ingestions i on i.id = donor.ingestion join ValidPaths v on v.path = i.path"
            " where own.ingestion = ? and i.id > 0"
            " and exists (select 1 from Blobs other join Ingestions oi on oi.id = other.ingestion"
            " join ValidPaths ov on ov.path = oi.path where other.id = own.id and other.exec = own.exec and oi.id > 0"
            " and (other.ino != own.ino or other.dev != own.dev))"
            " order by own.id, own.exec, donor.dev, donor.ino, donor.ingestion;");
        state->stmts->DeletePathIngestions.create(state->db, "delete from Ingestions where path = ?;");
        state->stmts->NewIngestion.create(state->db, "insert into Ingestions (path, hold) values (?, ?);");
        state->stmts->SetIngestionPath.create(state->db, "update Ingestions set path = ? where id = ?;");
    }
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
                select outputPath, signatures from BuildTraceV3
                    where drvPath = ? and outputName = ?
                    ;
            )");
    }

    /* Install the holder provider after its statements exist; bare stores miss. */
    if (state->stmts->LookupBlob) {
        blobIndex.lookup = [this](const Hash & id, bool executable, GitObjectStore::Holder::Cursor cursor)
            -> std::optional<GitObjectStore::Holder> {
            return retrySQLite<std::optional<GitObjectStore::Holder>>([&]() -> std::optional<GitObjectStore::Holder> {
                auto state(_state->lock());
                auto use(state->stmts->LookupBlob.use(SQLiteStmt::Bindings::Borrow));
                /* Bind the identifier bytes directly, without hexadecimal encoding. */
                use.apply(id.hash, id.hashSize)
                    .apply(int64_t(executable ? 1 : 0))
                    .apply(std::get<0>(cursor))
                    .apply(std::get<1>(cursor))
                    .apply(static_cast<int64_t>(std::min<uint64_t>(
                        std::get<2>(cursor), std::numeric_limits<int64_t>::max())));
                std::optional<GitObjectStore::Holder> holder;
                /* Keep only exact native cursors, selecting their signed minimum
                   without opening SQLite's temporary sorter on every miss. */
                while (use.next()) {
                    auto key = GitObjectStore::Holder::Cursor{
                        use.getInt(1), use.getInt(0), static_cast<uint64_t>(use.getInt(5))};
                    auto ino = static_cast<ino_t>(std::get<1>(key));
                    auto dev = static_cast<dev_t>(std::get<0>(key));
                    if (static_cast<int64_t>(dev) != std::get<0>(key)
                        || static_cast<int64_t>(ino) != std::get<1>(key) || (holder && key >= holder->cursor()))
                        continue;
                    holder = rootedBlobHolder(
                        {.ino = ino,
                         .dev = dev,
                         .size = static_cast<uint64_t>(use.getInt(2)),
                         .ingestion = std::get<2>(key)},
                        getRealStoreDir(), baseNameOf(use.getStr(3)), use.getStr(4));
                }
                return holder;
            });
        };
        blobIndex.forget = [this](const Hash & id, bool executable, const GitObjectStore::Holder & holder) {
            if (this->config->readOnly)
                return;
            auto name = (++std::get<GitObjectStore::Holder::Registered>(holder.address).second.begin()).remaining;
            retrySQLite<void>([&]() {
                auto state(_state->lock());
                writeBlobRow(
                    state->stmts->DeleteBlob.use(),
                    holder.ingestion,
                    {id, executable, holder.ino, holder.dev, holder.size,
                     std::string(name)});
            });
        };
    }
    if (state->stmts->ReadAuxiliary)
        blobIndex.readAuxiliary = [this](const merkle::TreeEntry & entry) { return readAuxiliary(entry); };
    objects.index = &blobIndex;
}

GitObjectStore::BlobInodes LocalStore::blobInodes()
{
    GitObjectStore::BlobInodes inodes;
#ifndef _WIN32
    retrySQLite<void>([&] {
        auto state(_state->lock());
        if (!state->stmts->LookupBlob)
            return;
        /* Only the first registered candidate is canonical to the optimiser's
           inode shortcut. Other candidates must reach place() to coalesce. */
        SQLiteStmt queryBlobInodes(
            state->db,
            "select b.ino, b.id from Blobs b join Ingestions i on i.id = b.ingestion "
            "join ValidPaths v on v.path = i.path where i.id > 0 and not exists ("
            "select 1 from Blobs earlier join Ingestions ei on ei.id = earlier.ingestion "
            "join ValidPaths ev on ev.path = ei.path "
            "where earlier.id = b.id and earlier.exec = b.exec and ei.id > 0"
            " and (earlier.dev, earlier.ino, earlier.ingestion) < (b.dev, b.ino, b.ingestion));");
        auto use(queryBlobInodes.use());
        while (use.next())
            inodes.insert_or_assign(static_cast<ino_t>(use.getInt(0)), blobIdOf(use.getBlobView(1)));
    });
#endif
    return inodes;
}

void LocalStore::beginIngestion(const GitObjectStore::Hold & hold, const std::filesystem::path & root)
{
    hold.emptyRegistered = nullptr;
    if (config->readOnly)
        return;
    hold.setRoot(root);
    /* Rows are inserted only when this path is completed. An empty
       projection also replaces the path's preceding candidate rows. */
    hold.ingesting = true;
}

void LocalStore::insertBlobRows(State & state, uint64_t ingestion, std::span<const GitObjectStore::Hold::Row> rows)
{
    if (rows.empty())
        return;
    auto use = state.stmts->InsertBlob.use(SQLiteStmt::Bindings::Borrow);
    for (auto & row : rows) {
        if (&row != rows.data())
            use.reset();
        writeBlobRow(use, ingestion, row);
    }
}

/* SQLite's selected 1,000,000,000-byte row limit and bind_blob's signed
   length are both larger than this complete row. Chunk ordinals, not byte
   offsets, also fit int64_t for every supported size_t/string domain. */
static constexpr size_t auxiliaryChunkSize = 1 << 20;
static_assert(std::numeric_limits<size_t>::digits <= 64);
static_assert(auxiliaryChunkSize <= std::numeric_limits<int>::max());

struct AuxiliaryCandidate
{
    int64_t ingestion;
    std::optional<std::string> body;
};

/* Both queries select one incarnation in ordinal order. Keep the statement
   active until every chunk is owned: no body can mix two SQLite snapshots. */
static std::optional<AuxiliaryCandidate> readAuxiliaryChunks(SQLiteStmt & stmt, SQLiteStmt::Use & use)
{
    std::optional<AuxiliaryCandidate> result;
    while (use.next()) {
        bool first = !result;
        if (first)
            result.emplace(use.getInt(0), std::string());
        if (sqlite3_column_type(stmt, 1) != SQLITE_INTEGER || sqlite3_column_type(stmt, 2) != SQLITE_BLOB)
            return AuxiliaryCandidate{result->ingestion, std::nullopt};
        auto data = use.getBlobView(2);
        auto size = data.size();
        if (!std::cmp_equal(use.getInt(1), result->body->size() / auxiliaryChunkSize) || size > auxiliaryChunkSize
            || result->body->size() % auxiliaryChunkSize != 0 || (!first && result->body->empty())
            || (size == 0 && !first)
            || size > result->body->max_size() - result->body->size())
            return AuxiliaryCandidate{result->ingestion, std::nullopt};
        result->body->append(data);
    }
    return result;
}

static bool auxiliaryBodyMatches(int64_t kind, std::string_view id, const std::optional<std::string> & body)
{
    if (!body || id.size() != regularHashSize(GitObjectStore::hashAlgo)
        || (kind != static_cast<int64_t>(merkle::Mode::Directory)
            && kind != static_cast<int64_t>(merkle::Mode::Symlink)))
        return false;
    auto actual = kind == static_cast<int64_t>(merkle::Mode::Directory) ? merkle::treeId(*body) : merkle::blobId(*body);
    return id == std::string_view(reinterpret_cast<const char *>(actual.hash), id.size());
}

std::optional<std::string> LocalStore::readAuxiliary(const merkle::TreeEntry & entry)
{
    std::optional<int64_t> cursor;
    while (true) {
        auto candidate = retrySQLite<std::optional<AuxiliaryCandidate>>([&] {
            auto state(_state->lock());
            auto & stmt = state->stmts->ReadAuxiliary;
            auto use(stmt.use(SQLiteStmt::Bindings::Borrow));
            use.apply(static_cast<int64_t>(entry.mode))
                .apply(entry.hash.hash, entry.hash.hashSize)
                .apply(cursor.value_or(0), cursor.has_value());
            return readAuxiliaryChunks(stmt, use);
        });
        if (!candidate)
            return std::nullopt;
        /* Hash outside the database scope. An old corrupt incarnation must
           not hide a later sound copy; NULL, not zero, is the first cursor. */
        if (auxiliaryBodyMatches(static_cast<int64_t>(entry.mode), entry.hash.bytes(), candidate->body))
            return std::move(candidate->body);
        cursor = candidate->ingestion;
    }
}

void LocalStore::insertAuxiliaryRows(
    State & state, uint64_t ingestion, std::span<const GitObjectStore::Auxiliary> values)
{
    if (values.empty())
        return;
    auto use = state.stmts->InsertAuxiliary.use(SQLiteStmt::Bindings::Borrow);
    for (auto & value : values) {
        auto & entry = value.entry;
        /* Hold::addAuxiliary validated these keys before staging. Nonempty
           production projections belong to a fresh ingestion; a retry rolled
           its previous inserts back. No value is overwritten. */
        size_t offset = 0;
        do {
            auto size = std::min(auxiliaryChunkSize, value.body.size() - offset);
            if (offset || &value != values.data())
                use.reset();
            use.apply(static_cast<int64_t>(entry.mode))
                .apply(entry.hash.hash, entry.hash.hashSize)
                .apply(static_cast<int64_t>(ingestion))
                .apply(static_cast<int64_t>(offset / auxiliaryChunkSize))
                /* string::data is non-null even for the one empty chunk. */
                .apply(reinterpret_cast<const unsigned char *>(value.body.data() + offset), size)
                .exec();
            offset += size;
        } while (offset != value.body.size());
    }
}

bool LocalStore::verifyAuxiliary(RepairFlag repair)
{
    struct Key
    {
        std::array<SQLiteValue, 3> values;
        std::string ingestion;

        bool hasDeclaredTypes() const
        {
            return sqlite3_value_type(values[0].get()) == SQLITE_INTEGER
                && sqlite3_value_type(values[1].get()) == SQLITE_BLOB
                && sqlite3_value_type(values[2].get()) == SQLITE_INTEGER;
        }
    };

    auto keys = retrySQLite<std::vector<Key>>([&] {
        auto state(_state->lock());
        std::vector<Key> keys;
        if (!state->stmts->ReadAuxiliary)
            return keys;
        SQLiteStmt query(
            state->db,
            "select distinct kind collate binary, id collate binary, ingestion collate binary,"
            " typeof(kind), typeof(id), typeof(ingestion),"
            " quote(ingestion) from AuxValues;");
        auto use(query.use());
        while (use.next())
            keys.push_back({{use.getValue(0), use.getValue(1), use.getValue(2)}, use.getStr(6)});
        return keys;
    });
    auto statement = [&](State & state, const Key & key, bool erase) -> SQLiteStmt & {
        bool indexed = key.hasDeclaredTypes();
        auto & stmt = (erase ? state.stmts->DeleteAuxiliary : state.stmts->ReadAuxiliaryExact)[indexed];
        if (!stmt) {
            auto where = std::string(auxiliaryExactKeySql);
            if (indexed)
                where += " and kind collate binary is ?1 and id collate binary is ?2 and ingestion collate binary is ?3";
            stmt.create(
                state.db,
                (erase ? "delete from AuxValues where " : "select ingestion, chunk, body from AuxValues where ") + where
                    + (erase ? ";" : " order by chunk;"));
        }
        return stmt;
    };
    auto inspect = [&](State & state, const Key & key) {
        auto & stmt = statement(state, key, false);
        auto use(stmt.use());
        for (auto & value : key.values)
            use.apply(*value);
        return readAuxiliaryChunks(stmt, use);
    };
    auto bad = [&](const Key & key, const std::optional<AuxiliaryCandidate> & candidate) {
        if (!candidate)
            return false;
        auto & [kind, id, ingestion] = key.values;
        if (!key.hasDeclaredTypes())
            return true;
        auto bytes = static_cast<const char *>(sqlite3_value_blob(id.get()));
        return !auxiliaryBodyMatches(
            sqlite3_value_int64(kind.get()),
            bytes ? std::string_view(bytes, sqlite3_value_bytes(id.get())) : std::string_view(),
            candidate->body);
    };
    bool errors = false;
    for (auto & key : keys) {
        auto candidate = retrySQLite<std::optional<AuxiliaryCandidate>>([&] {
            auto state(_state->lock());
            return inspect(*state, key);
        });
        if (!bad(key, candidate))
            continue;
        /* Recheck under the transaction: a conflicting writer forces rollback
           and retry. Hashing invokes no callbacks; diagnose after unlocking. */
        if (repair && !config->readOnly)
            retrySQLite<void>([&] {
                auto state(_state->lock());
                SQLiteTxn txn(state->db);
                if (bad(key, inspect(*state, key))) {
                    auto use = statement(*state, key, true).use();
                    for (auto & value : key.values)
                        use.apply(*value);
                    use.exec();
                }
                txn.commit();
            });
        else
            errors = true;
        printError(
            "auxiliary cache body in ingestion %s was modified or has an incomplete chunk sequence", key.ingestion);
    }
    return errors;
}

/* The SQL key is the active digest bytes and mode, not the Hash algorithm tag. */
static auto blobRowKey(const GitObjectStore::Hold::Row & row) noexcept
{
    return std::pair{row.id.bytes(), row.executable};
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
    auto deleteOwn = [&](const Hash & id, bool executable) {
        state.stmts->DeleteOwnBlob.use(SQLiteStmt::Bindings::Borrow).apply(id.hash, id.hashSize)
            .apply(int64_t(executable)).apply(static_cast<int64_t>(ingestion)).exec();
    };
    {
        auto & statement = state.stmts->QueryBlobCollisions;
        auto use(statement.use());
        use.apply(static_cast<int64_t>(ingestion));
        while (use.next()) {
            auto id = use.getBlobView(0);
            bool executable = use.getInt(1) != 0;
            if (collisions.empty() || blobRowKey(collisions.back().row) != std::pair{id, executable}) {
                auto own = std::ranges::lower_bound(rows, std::pair{id, executable}, {}, blobRowKey);
                assert((own != rows.end() && blobRowKey(*own) == std::pair{id, executable}));
                collisions.push_back({*own, {}});
            }
            collisions.back().candidates.push_back(rootedBlobHolder(
                {.ino = static_cast<ino_t>(use.getInt(2)),
                 .dev = static_cast<dev_t>(use.getInt(3)),
                 .size = static_cast<uint64_t>(use.getInt(4)),
                 .ingestion = static_cast<uint64_t>(use.getInt(7))},
                getRealStoreDir(), baseNameOf(use.getStr(5)), use.getStr(6)));
        }
    }
    if (collisions.empty())
        return;
    /* Refused physical claims disappear even when a later physical or SQL operation fails. */
    Finally prune{[&] {
        for (auto & collision : collisions | std::views::reverse)
            if (!collision.eligible)
                rows.erase(rows.begin() + (&collision.row - rows.data()));
    }};
    auto rejected = coalesceBlobs(toRealPath(path), hold, collisions);
    for (auto & collision : collisions)
        if (collision.eligible)
            writeBlobRow(state.stmts->InsertBlob.use(SQLiteStmt::Bindings::Borrow), ingestion, collision.row);
        else
            deleteOwn(collision.row.id, collision.row.executable);
    for (auto & [row, holder] : rejected) {
        auto name = (++std::get<GitObjectStore::Holder::Registered>(holder.address).second.begin()).remaining;
        writeBlobRow(
            state.stmts->DeleteBlob.use(),
            holder.ingestion,
            {row.id, row.executable, holder.ino, holder.dev, holder.size,
             std::string(name)});
    }
}

/* Order each initial publication by its SQLite primary key. Ingestion is
   constant; auxiliary chunks are already emitted in increasing ordinal order. */
static void sortProjectionForPublication(GitObjectStore::Hold::Projection & projection)
{
    static_assert(std::is_nothrow_move_assignable_v<GitObjectStore::Hold::Row>);
    static_assert(std::is_nothrow_move_assignable_v<GitObjectStore::Auxiliary>);
    std::ranges::sort(projection.regular, {}, blobRowKey);
    std::ranges::sort(projection.auxiliary, {}, [&](const auto & value) noexcept {
        return std::pair{value.entry.mode, value.entry.hash.bytes()};
    });
}

void LocalStore::syncStorePath(const StorePath & path)
{
    auto realPath = toRealPath(path);
    recursiveSync(realPath);
    syncParent(realPath);
}

void LocalStore::endIngestion(const GitObjectStore::Hold & hold, const StorePath & path)
{
    if (config->readOnly || !hold.ingesting)
        return;
    /* An already-valid repair/optimisation path must not expose its new
       candidates before reconciling the complete projection. */
    auto & projection = hold.projection();
    sortProjectionForPublication(projection);
    auto & rows = projection.regular;
    auto logical = printStorePath(path);
    auto & completed = hold.completedIngestions;
    /* Reserve before committing; the concrete string/id insertion cannot throw afterwards. */
    completed.reserve(completed.size() + !completed.contains(logical));
    auto holdName = hold.directory().filename().string();
    retrySQLite<void>([&]() {
        auto state(_state->lock());
        SQLiteTxn txn(state->db);
        state->stmts->NewIngestion.exec(std::nullopt, holdName);
        auto id = static_cast<uint64_t>(sqlite3_last_insert_rowid(state->db));
        insertBlobRows(*state, id, rows);
        insertAuxiliaryRows(*state, id, projection.auxiliary);
        /* This naming is itself publication for a path already valid.
           Builder outputs are reconciled when their validity is committed. */
        if (isValidPath_(*state, logical)) {
            coalesceBlobRows(*state, path, hold, id, rows);
            if (config->getLocalSettings().fsyncStorePaths)
                syncStorePath(path);
#ifndef _WIN32
            if (config->getLocalSettings().syncBeforeRegistering)
                sync();
#endif
        }

        state->stmts->DeletePathIngestions.exec(logical);
        state->stmts->SetIngestionPath.exec(logical, static_cast<int64_t>(id));
        txn.commit();
        completed.insert_or_assign(std::move(logical), std::pair{id, std::move(rows)});
        hold.ingesting = false;
        hold.takeProjection();
    });
}

/* After dead-lease cleanup, remove stale ingestions under the GC lock;
   foreign keys cascade both projections. Holder probes reject stale files
   until this sweep runs. */
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
            /* The sweep's stale-row rule (schema.sql): a row whose ingestion never
               got its path, or whose path is no longer valid, names no holder. */
            /* Dead-lease cleanup precedes this query. sweepStaleBlobRows checks each
               returned hold path before removing its projection. */
            SQLiteStmt query(
                state->db, "select id, hold from Ingestions where path is null or path not in (select path from ValidPaths);");
            auto use(query.use());
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
        SQLiteStmt erase(state->db, "delete from Ingestions where id = ?;");
        for (auto id : stale) {

            erase.exec(static_cast<int64_t>(id));
        }
        txn.commit();
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

void LocalStore::Deleter::operator()(LocalStore * store) const noexcept
{
    if (!store)
        return;
    store->waitForAutoGC();
    delete store;
}

void LocalStore::waitForAutoGC()
{
    std::shared_future<void> future;

    {
        auto state(_state->lock());
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
}

LocalStore::~LocalStore()
{
    waitForAutoGC();

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

    /* Install the current owned projections atomically. Existing unmarked
       branch layouts are unsupported; never replace their tables. */
    maybeUpgrade(
        "20260928-ingestion-owned-projections",
        "create table Ingestions ("
        " id integer primary key autoincrement not null, path text, hold text);"
        "\ncreate table Blobs ("
        " id blob not null, exec integer not null, ino integer not null, dev integer not null,"
        " ingestion integer not null references Ingestions(id) on delete cascade,"
        " name text not null, size integer not null, primary key (id, exec, ingestion)) without rowid;"
        "\ncreate index IndexBlobsIngestion on Blobs(ingestion);"
        "\ncreate index IndexIngestionsPath on Ingestions(path, hold);"
        "\ncreate table AuxValues ("
        " kind integer not null check(kind in (16384, 40960)),"
        " id blob not null check(typeof(id) = 'blob' and length(id) = 32),"
        " ingestion integer not null references Ingestions(id) on delete cascade,"
        " chunk integer not null check(chunk >= 0), body blob not null,"
        " primary key(kind, id, ingestion, chunk)) without rowid;"
        "\ncreate index IndexAuxValuesIngestion on AuxValues(ingestion);");

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
                oldR->signatures.insert(info.signatures.begin(), info.signatures.end());
                state->stmts->UpdateRealisedOutput.exec(
                    concatStringsSep(" ", Signature::toStrings(oldR->signatures)),
                    info.id.drvPath.to_string(),
                    info.id.outputName);
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
            state->stmts->RegisterRealisedOutput.exec(
                info.id.drvPath.to_string(),
                info.id.outputName,
                printStorePath(info.outPath),
                concatStringsSep(" ", Signature::toStrings(info.signatures)));
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
            state->stmts->DeleteRealisedOutputByName.exec(key.drvPath.to_string(), key.outputName);
        }
        txn.commit();
    });
}

void LocalStore::cacheDrvOutputMapping(
    State & state, const uint64_t deriver, const std::string & outputName, const StorePath & output)
{
    retrySQLite<void>([&]() {
        state.stmts->AddDerivationOutput.exec(deriver, outputName, printStorePath(output));
    });
}

std::pair<uint64_t, time_t> LocalStore::addValidPath(State & state, const ValidPathInfo & info)
{
    if (info.ca.has_value() && !info.isContentAddressed(*this))
        throw Error(
            "cannot add path '%s' to the Nix store because it claims to be content-addressed but isn't",
            printStorePath(info.path));

    time_t registrationTime;
    /* Legacy NAR-only descriptions must pass ingestion to obtain an object hash. */
    state.stmts->RegisterValidPath.use()
        .apply(printStorePath(info.path))
        .apply(info.requireObjectHash(*this).render())
        .apply(registrationTime = info.registrationTime == 0 ? time(nullptr) : info.registrationTime)
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

    return {id, registrationTime};
}

void LocalStore::queryPathInfoUncached(
    const StorePath & path, Callback<std::shared_ptr<const ValidPathInfo>> callback) noexcept
{
    try {
        auto info = retrySQLite<std::shared_ptr<ValidPathInfo>>(
            [&]() { return queryPathInfoInternal(*_state->lock(), path); });

        /* Outside the lock: a schema-10 row's migration may walk the path. */
        callback(info ? migratedPathInfo(std::move(info)) : nullptr);

    } catch (...) {
        callback.rethrow();
    }
}

std::shared_ptr<const ValidPathInfo> LocalStore::queryPathInfoUnmigrated(const StorePath & path)
{
    return retrySQLite<std::shared_ptr<ValidPathInfo>>(
        [&]() { return queryPathInfoInternal(*_state->lock(), path); });
}

void LocalStore::migratePathInfo(const StorePath & path)
{
    auto info = retrySQLite<std::shared_ptr<ValidPathInfo>>(
        [&]() { return queryPathInfoInternal(*_state->lock(), path); });

    if (!info)
        throw InvalidPath("path '%s' is not valid", printStorePath(path));
    if (info->objectHash)
        return;

    if (auto failure = walkOldRow(*info))
        throw Error(*failure);
}

std::optional<HintFmt> LocalStore::walkOldRow(ValidPathInfo & info)
{
    auto & path = info.path;
    auto missing = [&] {
        return HintFmt(
            "path '%s' is registered but its files are missing; run `nix-store --verify` to remove the row",
            printStorePath(path));
    };

    /* Hash the same NAR stream for both identities before updating the row. */
    HashSink narSink{info.assertedNarHash ? info.assertedNarHash->algo : HashAlgorithm::SHA256};
    /* Distinguish missing files under a valid row from concurrent collection. */
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
        return missing();
    } catch (SysError & e) {
        if (!rowValid())
            throw InvalidPath("path '%s' is not valid", printStorePath(path));
        if (e.errNo == ENOENT)
            return missing();
        throw;
    }
    auto narHash = narSink.finish().hash;

    /* The row's assertion checked before anything is written, as master's
       verifier checked it and `--load-db` still does.  (The NAR size need
       not be compared: the hash is over the whole serialisation.) */
    if (info.assertedNarHash && *info.assertedNarHash != narHash)
        return HintFmt(
            "path '%s' was modified! expected hash '%s', got '%s'; its row is left as it is -- run `nix-store --verify --check-contents --repair`",
            printStorePath(path),
            info.assertedNarHash->to_string(HashFormat::Nix32, true),
            narHash.to_string(HashFormat::Nix32, true));

    info.objectHash = ObjectHash::of(object->root);
    if (info.narSize == 0)
        info.narSize = object->narSize.value();
    if (!config->readOnly)
        retrySQLite<void>([&]() {
            auto state(_state->lock());
            SQLiteTxn txn(state->db);

            /* Re-read under the lock: another process may have migrated the
               row, signed it, or deleted the path meanwhile. Only the hash
               and a missing size are ours to write. */
            auto current = queryPathInfoInternal(*state, path);
            if (current && !current->objectHash) {
                current->objectHash = *info.objectHash;
                if (current->narSize == 0)
                    current->narSize = object->narSize.value();
                updatePathInfo(*state, *current);
            }
            txn.commit();
        });
    return std::nullopt;
}

std::shared_ptr<const ValidPathInfo> LocalStore::migratedPathInfo(std::shared_ptr<ValidPathInfo> info)
{
    if (info->objectHash)
        return info;

    /* Preserve assertedNarHash until migration checks the bytes. Do not add
       a temp root here: GC calls this while holding its exclusive lock;
       other callers already hold a root or shared lock. */
    if (auto failure = walkOldRow(*info))
        debug("%s; row left unmigrated", failure->str());
    return info;
}

std::shared_ptr<ValidPathInfo> LocalStore::queryPathInfoInternal(State & state, const StorePath & path)
{
    /* Get the path info. */
    auto useQueryPathInfo(state.stmts->QueryPathInfo.use());
    useQueryPathInfo.apply(printStorePath(path));

    if (!useQueryPathInfo.next())
        return std::shared_ptr<ValidPathInfo>();

    auto id = useQueryPathInfo.getInt(0);

    auto info = std::make_shared<ValidPathInfo>(path, UnkeyedValidPathInfo(*this, std::nullopt));

    /* Recognise git:sha256 before the legacy parser, whose algorithm error
       is not BadHash. Preserve the legacy error for other malformed rows. */
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
    auto useQueryReferences(state.stmts->QueryReferences.use());
    useQueryReferences.apply(id);

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
    auto use(state.stmts->QueryValidPathId.use());
    use.apply(printStorePath(path));
    if (!use.next())
        throw InvalidPath("path '%s' is not valid", printStorePath(path));
    return use.getInt(0);
}

bool LocalStore::isValidPath_(State & state, std::string_view logical)
{
    return state.stmts->QueryValidPathId.use(SQLiteStmt::Bindings::Borrow).apply(logical).next();
}

bool LocalStore::isValidPathUncached(const StorePath & path)
{
    return retrySQLite<bool>([&]() {
        auto state(_state->lock());
        bool valid = isValidPath_(*state, printStorePath(path));
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

bool LocalStore::mayHaveValidPaths()
{
    return retrySQLite<bool>([&] {
        auto state(_state->lock());
        return state->stmts->QueryValidPaths.use().next();
    });
}

void LocalStore::queryReferrers(State & state, const StorePath & path, StorePathSet & referrers)
{
    auto useQueryReferrers(state.stmts->QueryReferrers.use());
    useQueryReferrers.apply(printStorePath(path));

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

        auto useQueryValidDerivers(state->stmts->QueryValidDerivers.use());
        useQueryValidDerivers.apply(printStorePath(path));

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
        auto use(state->stmts->QueryDerivationOutputs.use());
        use.apply(drvId);
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

        auto useQueryPathFromHashPart(state->stmts->QueryPathFromHashPart.use(SQLiteStmt::Bindings::Borrow));
        useQueryPathFromHashPart.apply(prefix);

        if (!useQueryPathFromHashPart.next())
            return {};

        const char * s = (const char *) sqlite3_column_text(state->stmts->QueryPathFromHashPart, 0);
        if (s && prefix.compare(0, prefix.size(), s, prefix.size()) == 0)
            return parseStorePath(s);
        return {};
    });
}

void LocalStore::registerValidPath(const ValidPathInfo & info, const GitObjectStore::Hold * hold)
{
    registerValidPaths({{info.path, info}}, hold);
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

    /* Take the completed ingestion before locking the database. Its rows
       and validity are published through one connection and transaction. */
    bool active = hold && hold->ingesting;
    GitObjectStore::Hold::Projection empty;
    auto & projection = active ? hold->projection() : empty;

    struct Publication
    {
        const ValidPathInfo * info;
        uint64_t id;
        std::string logical;
        std::vector<GitObjectStore::Hold::Row> * rows;
    };

    std::vector<Publication> publications;
    if (hold) {
        if (active) {
            assert(infos.size() == 1);
            publications.push_back({&infos.begin()->second, 0, printStorePath(infos.begin()->second.path), &projection.regular});
            sortProjectionForPublication(projection);
            auto & completed = hold->completedIngestions;
            completed.reserve(completed.size() + !completed.contains(publications.front().logical));
        } else {
            for (auto & [path, info] : infos) {
                auto logical = printStorePath(path);
                if (auto completed = hold->completedIngestions.find(logical);
                    completed != hold->completedIngestions.end())
                    publications.push_back({&info, completed->second.first, std::move(logical), &completed->second.second});
            }
        }
    }
    return retrySQLite<void>([&]() {
        auto state(_state->lock());

        SQLiteTxn txn(state->db);
        /* Within this transaction, the finite path/reference domain has
           stable row ids. Retain each lookup through reference insertion. */
        std::map<StorePath, std::pair<uint64_t, std::optional<time_t>>> pathIds;
        for (auto publication = publications.begin(); publication != publications.end();) {
            /* Active IDs are private to this transaction; completed IDs are stable. */
            auto id = publication->id;
            if (active) {
                // This projection and its validity become visible together.
                state->stmts->NewIngestion.use().apply("", false).apply("", false).exec();
                id = static_cast<uint64_t>(sqlite3_last_insert_rowid(state->db));
            } else {
                state->stmts->SetIngestionPath.use().apply("", false).apply(static_cast<int64_t>(id)).exec();
                /* Another completed output may have replaced this old ID. Its
                   current projection must survive registration of the old hold. */
                if (sqlite3_changes(state->db) == 0) {
                    publication = publications.erase(publication);
                    continue;
                }
                state->stmts->DeleteOwnBlobs.exec(static_cast<int64_t>(id));
            }
            publication->id = id;
            insertBlobRows(*state, id, *publication->rows);
            insertAuxiliaryRows(*state, id, projection.auxiliary);
            ++publication;
        }

        for (auto & [_, i] : infos) {
            i.requireObjectHash(*this);
            {
                auto use(state->stmts->QueryValidPathId.use());
                use.apply(printStorePath(i.path));
                if (use.next()) {
                    pathIds.emplace(i.path, std::pair{static_cast<uint64_t>(use.getInt(0)), std::nullopt});
                }
            }
            for (auto & reference : i.references)
                if (!infos.contains(reference) && !pathIds.contains(reference))
                    pathIds.emplace(reference, std::pair{queryValidPathId(*state, reference), std::nullopt});
        }

        /* Do a topological sort of the paths.  This will throw an
           error if a cycle is detected and roll back the
           transaction.  Cycles can only occur when a derivation
           has multiple outputs. */
        if (auto cycle = visitDependencies(
                infos,
                [&](const StorePath & path) -> const StorePathSet & { return infos.at(path).references; },
                [](const StorePath &) {}))
            throw BuildError(
                BuildResult::Failure::OutputRejected,
                "cycle detected in the references of '%s' from '%s'",
                printStorePath(cycle->path),
                printStorePath(cycle->parent));

        /* Existing paths may have visible aliases even if this transaction
           rolls back. Reconcile all of those against committed witnesses
           before exposing any of their new candidate rows. */
        for (auto & publication : publications)
            if (pathIds.contains(publication.info->path))
                coalesceBlobRows(*state, publication.info->path, *hold, publication.id, *publication.rows);
        auto publish = [&](const Publication & publication) {
            auto & logical = publication.logical;
            state->stmts->DeletePathIngestions.exec(logical);
            state->stmts->SetIngestionPath.exec(logical, static_cast<int64_t>(publication.id));
        };
        for (auto & publication : publications)
            if (pathIds.contains(publication.info->path))
                publish(publication);

        auto nextPublication = publications.begin();
        for (auto & [path, info] : infos) {
            auto * publication =
                nextPublication != publications.end() && nextPublication->info == &info ? &*nextPublication++ : nullptr;
            if (pathIds.contains(path)) {
                updatePathInfo(*state, info);
                continue;
            }
            /* A new path is made eligible only after all its candidates
               are reconciled. Later outputs may share this finished one. */
            if (publication) {
                if (!pathIds.empty() || state->stmts->QueryValidPaths.use().next())
                    coalesceBlobRows(*state, path, *hold, publication->id, *publication->rows);
                publish(*publication);
            }
            pathIds.emplace(path, addValidPath(*state, info));
        }
        for (auto & [_, info] : infos) {
            auto referrer = pathIds.at(info.path).first;
            for (auto & reference : info.references)
                state->stmts->AddReference.exec(referrer, pathIds.at(reference).first);
        }
        /* Physical publication must finish durability on every attempt, even
           when an earlier attempt changed aliases and its SQL rolled back. */
        if (hold && config->getLocalSettings().fsyncStorePaths)
            for (auto & [path, _] : infos)
                if (active || hold->completedIngestions.contains(printStorePath(path)))
                    syncStorePath(path);
#ifndef _WIN32
        /* SQLite will fsync by default, but the new valid paths may not
           be fsync-ed.  So some may want to fsync them before registering
           the validity, at the expense of some speed of the path
           registering operation. */
        if (config->getLocalSettings().syncBeforeRegistering)
            sync();
#endif
        txn.commit();
        if (active) {
            auto & publication = publications.front();
            hold->completedIngestions.insert_or_assign(
                std::move(publication.logical), std::pair{publication.id, std::move(projection.regular)});
            hold->ingesting = false;
            hold->takeProjection();
        }
        /* A description becomes cacheable only with the committed path
           and references. A rejected reference or cycle rolls the SQL
           back; publishing earlier would leave a false validity hit. */
        if (pathInfoCache) {
            auto cache = pathInfoCache->lock();
            for (auto & [path, info] : infos)
                if (auto registrationTime = pathIds.at(path).second) {
                    auto & hash = info.objectHash->hash;
                    if (hash.algo != HashAlgorithm::SHA256 || hash.hashSize != regularHashSize(HashAlgorithm::SHA256)
                        || (info.ca && (info.ca->hash.hashSize != regularHashSize(info.ca->hash.algo)
                                        || (info.ca->hash.algo == HashAlgorithm::BLAKE3
                                            && !experimentalFeatureSettings.isEnabled(Xp::BLAKE3Hashes))))
                        || !std::ranges::all_of(info.sigs, [](const Signature & sig) {
                               return !sig.keyName.empty() && !sig.sig.empty()
                                      && sig.keyName.find_first_of(" :\0", 0, 3) == std::string::npos;
                           })) {
                        cache->upsert(path, PathInfoCacheValue{.value = PathInfoCacheValue::ValidWithoutInfo{}});
                        continue;
                    }
                    auto registered = std::make_shared<ValidPathInfo>(info);
                    registered->registrationTime = *registrationTime;
                    registered->storeDir = storeDir;
                    registered->assertedNarHash.reset();
                    cache->upsert(path, PathInfoCacheValue{.value = std::move(registered)});
                } else
                    cache->erase(path);
        }
    });
}

/* Invalidate a path.  The caller is responsible for checking that
   there are no referrers. */
void LocalStore::invalidatePath(State & state, const StorePath & path)
{
    debug("invalidating path '%s'", printStorePath(path));

    state.stmts->InvalidatePath.exec(printStorePath(path));

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

void LocalStore::addToStore(
    const ValidPathInfo & info,
    Source & source,
    RepairFlag repair,
    CheckSigsFlag checkSigs,
    const std::function<Hash()> & narHash)
{
    /* Require at least one asserted identity before accepting the stream. */
    if (!info.ca && !info.objectHash && !info.assertedNarHash)
        throw Error("path info for '%s' carries no content hash to verify", printStorePath(info.path));

    /* A v1 signature without an asserted NAR hash needs the stream first.
       Check it after restore and remove the path on failure, before registration. */
    bool checkSigsAfterRestore = false;
    if (checkSigs && pathInfoIsUntrusted(info)) {
        if (!info.assertedNarHash && !info.sigs.empty())
            checkSigsAfterRestore = true;
        else
            throw Error(
                "cannot add path '%s' because it lacks a signature by a trusted key", printStorePath(info.path));
    }

    withLockedOutput(info.path, repair, locksHeld, [&](const auto & realPath, bool needsWrite) {
        /* A raced validity hit supplies no stream for a deferred signature. */
        if (!needsWrite) {
            if (checkSigsAfterRestore)
                throw Error(
                    "cannot add path '%s' because it lacks a signature by a trusted key", printStorePath(info.path));
            return;
        }

        /* Run autoGC before restoring unregistered content. */
        autoGC();

        /* This publication owns its projection and any physical lease
           it needs until registration or failure completes. */
        auto hold = objects.hold();

        /* One tee checks the asserted NAR hash and any deferred v1 signature;
           v1 fingerprints use SHA-256. */
        std::optional<HashSink> narHashSink;
        std::optional<TeeSource> teeSource;
        Source * restoreSource = &source;
        if (info.assertedNarHash || checkSigsAfterRestore) {
            narHashSink.emplace(info.assertedNarHash ? info.assertedNarHash->algo : HashAlgorithm::SHA256);
            teeSource.emplace(source, *narHashSink);
            restoreSource = &*teeSource;
        }
        /* parseDump admits canonical NAR bytes. NAR addresses hash that stream;
           Flat/Text addresses hash only the regular root contents. */
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
            [&](FileSystemObjectSink & sink) { parseDump(sink, *restoreSource); },
            config->getLocalSettings().fsyncStorePaths,
            canonicalisingRestoreHooks.get(),
            hold,
            repair,
            rootContents);

        /* Preserve the sender's identity through signature checking.
           Install the verified object hash and clear its NAR assertion
           only when the description is ready for registration. */
        ValidPathInfo toRegister{info};
        auto objectHash = ObjectHash::of(restored.root);
        toRegister.narSize = restored.narSize.value();

        /* The content address first, then the sender's object hash,
           NAR hash and size (01 section 9.10, "The content address
           is checked first"). */
        if (info.ca) {
            auto & specified = *info.ca;
            auto actualHash = [&]() -> Hash {
                switch (specified.method.getFileIngestionMethod()) {
                case FileIngestionMethod::Flat:
                    if (restored.root.mode != merkle::Mode::Regular
                        && restored.root.mode != merkle::Mode::Executable)
                        throw Error(
                            "flat content address requires a regular file at '%s'", printStorePath(info.path));
                    [[fallthrough]];
                case FileIngestionMethod::NixArchive:
                    return caSink->finish().hash;
                case FileIngestionMethod::Git:
                    /* Git content addresses require SHA-256. Remove rejected imports
                       before registration, as for a deferred signature failure. */
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
                    return objectHash.hash;
                }
                unreachable();
            }();
            if (specified.hash != actualHash) {
                throw Error(
                    "ca hash mismatch importing path '%s';\n  specified: %s\n  got:       %s",
                    printStorePath(info.path),
                    specified.hash.to_string(HashFormat::Nix32, true),
                    actualHash.to_string(HashFormat::Nix32, true));
            }
        }

        if (info.objectHash && *info.objectHash != objectHash)
            throw Error(
                "object hash mismatch importing path '%s';\n  specified: %s\n  got:       %s",
                printStorePath(info.path),
                info.objectHash->render(),
                objectHash.render());

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
        if (info.narSize != 0 && restored.narSize.value() != info.narSize)
            throw Error(
                "size mismatch importing path '%s';\n  specified: %s\n  got:       %s",
                printStorePath(info.path),
                info.narSize,
                restored.narSize.value());

        /* The deferred signature check: the description with the
           stream's NAR hash and size in place of the assertion it
           lacked, under the same rule as before the stream. */
        if (checkSigsAfterRestore) {
            toRegister.assertedNarHash = streamNarHash;
            if (pathInfoIsUntrusted(toRegister)) {
                deletePath(realPath);
                throw Error(
                    "cannot add path '%s' because it lacks a signature by a trusted key",
                    printStorePath(info.path));
            }
        }

        toRegister.objectHash = std::move(objectHash);
        toRegister.assertedNarHash = std::nullopt;
        registerValidPath(toRegister, &hold);
    });
}

void LocalStore::withLockedOutput(
    const StorePath & path,
    RepairFlag repair,
    const PathSet & heldLocks,
    function_ref<void(const std::filesystem::path &, bool)> action)
{
    addTempRoot(path);
    if (!repair && isValidPath(path))
        return;
    PathLocks outputLock;
    auto realPath = toRealPath(path);
    if (heldLocks.empty() || !heldLocks.contains(printStorePath(path)))
        outputLock.lockPaths({realPath});
    bool needsWrite = repair || !isValidPathUncached(path);
    if (needsWrite)
        deletePath(realPath);
    action(realPath, bool(needsWrite));
    if (!needsWrite)
        invalidatePathInfoCacheFor(path);
    outputLock.setDeletion(true);
}

StorePath LocalStore::addToStorePrepared(
    std::string_view name,
    ContentAddressWithReferences desc,
    RepairFlag repair,
    fun<std::pair<ObjectHashSink::Result, const GitObjectStore::Hold &>(const std::filesystem::path &)> populate)
{
    auto dstPath = makeFixedOutputPathFromCA(name, desc);
    withLockedOutput(dstPath, repair, {}, [&](const auto & realPath, bool needsWrite) {
        if (!needsWrite)
            return;
        auto [result, hold] = populate(realPath);
        auto info = ValidPathInfo::makeFromCA(*this, name, std::move(desc), ObjectHash::of(result.root));
        info.narSize = result.narSize.value();
        registerValidPath(info, &hold);
    });
    return dstPath;
}

bool LocalStore::moveIntoStore(
    const std::filesystem::path & from,
    const std::filesystem::path & to,
    AutoDelete & staging,
    RestoreSinkHooks * hooks)
{
    try {
        // The staged root is already read-only.
        movePath(from, to);
        return true;
    } catch (const SystemError & e) {
        if (!e.is(std::errc::cross_device_link))
            throw;
        // Overlayfs can require a copy even within the same store directory.
        warn("can't rename %s as %s, copying instead", PathFmt(from), PathFmt(to));
        RestoreSink copySink{/*startFsync=*/false, hooks};
        copySink.dstPath = to;
        copyRecursive(*makeFSSourceAccessor(from), CanonPath::root, copySink, CanonPath::root);
        staging.deletePath();
        return false;
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
    auto ingestionMethod = hashMethod.getFileIngestionMethod();
    checkIngestionAlgorithm(ingestionMethod, hashAlgo);

    bool methodsMatch = static_cast<FileIngestionMethod>(dumpMethod) == ingestionMethod;

    /* Matching methods hash the transport; NAR under Flat/Text hashes only
       its regular root contents. Git uses the restore's typed root. */
    std::optional<HashSink> hashSink;
    if (methodsMatch || ingestionMethod == FileIngestionMethod::Flat)
        hashSink.emplace(hashAlgo);
    std::optional<PathRefScanSink> refSink;
    if (filterReferences)
        // Only scan if we really need to, since it's slower.
        refSink = PathRefScanSink::fromPaths(originalReferences);
    LambdaSink tap{[&](std::string_view data) {
        if (methodsMatch)
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
    auto limit = localSettings.narBufferSize.get();
    auto dump = readPrefix(limit, [&](char * buf, size_t size) -> std::optional<size_t> {
        try {
            return source.read(buf, size);
        } catch (EndOfFile &) {
            return std::nullopt;
        }
    });

    std::optional<std::pair<AutoCloseFD, AutoDelete>> tempDir;
    std::filesystem::path tempPath;
    /* The result of the composite restore; a staged flat file or a
       cross-device copy needs its physical projection below instead. */
    std::optional<ObjectHashSink::Result> restored;

    /* A complete Flat buffer directly supplies Flat/Text or Git addresses;
       scanned NARs and mismatched NAR methods still need parsing first. */
    bool inMemoryAndDontNeedRestore =
        dump.size() < limit
        && (dumpMethod == FileSerialisationMethod::Flat ? ingestionMethod != FileIngestionMethod::NixArchive
                                                        : methodsMatch && !filterReferences);
    auto canonicalisingRestoreHooks =
        makeCanonicalisingRestoreHooks(NIX_WHEN_SUPPORT_ACLS2(config->getLocalSettings().ignoredAcls));

    /* Own pending objects from restoration through registration. Deferred
       validity hits never need a hold. */
    std::optional<GitObjectStore::Hold> hold;
    auto restore = [&](const std::filesystem::path & path, Source & input) {
        auto produce = [&](FileSystemObjectSink & sink) {
            if (dumpMethod == FileSerialisationMethod::NixArchive)
                parseDump(sink, input);
            else
                sink.createRegularFile(CanonPath::root, [&](CreateRegularFileSink & file) {
                    file.preallocateContents(dump.size());
                    file(dump);
                });
        };
        return restoreThroughObjects(
            path, std::ref(produce), localSettings.fsyncStorePaths, canonicalisingRestoreHooks.get(), *hold, repair,
            !methodsMatch && hashSink ? &*hashSink : nullptr);
    };

    if (!inMemoryAndDontNeedRestore) {
        /* Staging writes before validity is known, so run autoGC first. */
        autoGC();

        /* Drain what we pulled so far, and then keep on pulling */
        StringSource dumpSource{dump};
        ChainSource bothSource{dumpSource, source};

        tempDir.emplace(createTempDirInStore());
        tempPath = tempDir->second.path() / "x";
        hold.emplace(objects.hold());

        if (dumpMethod == FileSerialisationMethod::NixArchive)
            restored = restore(tempPath, bothSource);
        else
            restorePath(
                tempPath, bothSource, dumpMethod, localSettings.fsyncStorePaths, canonicalisingRestoreHooks.get());

        std::string().swap(dump);
    }

    auto references = refSink ? refSink->getResultPaths() : originalReferences;

    /* A deferred mismatch is exactly Flat under Git: its non-executable
       regular root has the blob ID as its object hash, without a projection. */
    auto desc = ContentAddressWithReferences::fromParts(
        hashMethod,
        hashSink && (methodsMatch || restored->root.mode == merkle::Mode::Regular
                     || restored->root.mode == merkle::Mode::Executable)
            ? hashSink->finish().hash
        : inMemoryAndDontNeedRestore ? merkle::blobId(dump)
        : (restored && ingestionMethod == FileIngestionMethod::Git)
            ? merkle::objectHash(restored->root)
            : hashPath(makeFSSourceAccessor(tempPath), ingestionMethod, hashAlgo).first,
        {
            .others = std::move(references),
            // caller is not capable of creating a self-reference, because this is content-addressed without modulus
            .self = false,
        });

    auto populate =
        [&](const std::filesystem::path & realPath) -> std::pair<ObjectHashSink::Result, const GitObjectStore::Hold &> {
        if (inMemoryAndDontNeedRestore) {
            // An already valid buffered path needs neither a write nor autoGC.
            autoGC();
            hold.emplace(objects.hold());
            StringSource dumpSource{dump};
            restored = restore(realPath, dumpSource);
        } else if (!moveIntoStore(tempPath, realPath, tempDir->second, canonicalisingRestoreHooks.get()))
            restored.reset();

        // A staged flat restore or cross-device copy needs its new physical projection.
        return {restored ? *restored : enterPath(realPath, repair, *hold), *hold};
    };
    return addToStorePrepared(name, std::move(desc), repair, std::ref(populate));
}

/* Create a temporary directory in the store that won't be
   garbage-collected until the returned FD is closed. */
std::pair<AutoCloseFD, AutoDelete> LocalStore::createTempDirInStore()
{
    for (;;) {
        /* There is a slight possibility that `tmpDir' gets deleted by
           the GC between createTempDir() and when we acquire a lock on it.
           We'll repeat until 'tmpDir' exists and we've locked it.
           Make the directory accessible only to the current user. */
        auto tmpDirFn = createTempDir(std::filesystem::path{config->realStoreDir.get()}, "tmp", /*mode=*/0700);
        auto tmpDirFd = openDirectory(tmpDirFn, FinalSymlink::DontFollow);
        if (!tmpDirFd) {
            continue;
        }
        bool lockedByUs = lockFile(tmpDirFd.get(), ltWrite, true);
        if (pathExists(tmpDirFn) && lockedByUs)
            return {std::move(tmpDirFd), AutoDelete(std::move(tmpDirFn))};
        tmpDirFd.close();
    }
}

void PathInUse::anchor() {}

void LocalStore::invalidatePathChecked(const StorePath & path)
{
    retrySQLite<void>([&]() {
        auto state(_state->lock());

        SQLiteTxn txn(state->db);

        if (isValidPath_(*state, printStorePath(path))) {
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

        /* Metadata completeness is checked only where this store owns the
           object-provider domain. An overlay may have lower paths without
           a projection in this upper database. */
        bool ownObjects = ownsObjectStore();
        if (!ownObjects)
            printInfo("the object store is shared with the lower store: its objects are not checked here");

        if (ownObjects)
            errors = verifyAuxiliary(repair) || errors;

        printInfo("checking store hashes...");

        for (auto & i : validPaths) {
            try {
                /* Migrated by `queryPathInfoUncached` if the row was old. */
                auto info =
                    std::const_pointer_cast<ValidPathInfo>(std::shared_ptr<const ValidPathInfo>(queryPathInfo(i)));

                /* Check the content hash (optionally - slow). */
                printMsg(lvlTalkative, "checking contents of '%s'", printStorePath(i));

                /* Recompute the physical root and report missing eligible objects. */
                auto realPath = toRealPath(i);

                if (!info->objectHash) {
                    /* Migration preserves a mismatching legacy row. Check its asserted
                       NAR hash and use the existing repair path before attempting migration again. */
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

                auto [current, missing] = ownObjects ? verifyObjects(realPath) : [&] {
                    HashingVisitor hasher;
                    return std::pair{objectHashOf(*makeFSSourceAccessor(realPath), CanonPath::root, defaultPathFilter, hasher),
                                     std::vector<std::filesystem::path>{}};
                }();
                auto currentHash = ObjectHash::of(current.root);

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
                        printInfo("updating size field on '%s' to %s", printStorePath(i), current.narSize.value());
                        info->narSize = current.narSize.value();
                        updatePathInfo(*_state->lock(), *info);
                    }

                    /* Re-enter missing projections, then verify availability before reporting
                       repair. The shared GC lock excludes collection during this check. */
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

        auto info = queryPathInfoInternal(*state, storePath);
        if (!info)
            throw InvalidPath("path '%s' is not valid", printStorePath(storePath));

        info->sigs.insert(sigs.begin(), sigs.end());

        updatePathInfo(*state, *info);

        txn.commit();
    });
}

std::optional<UnkeyedRealisation> LocalStore::queryRealisation_(LocalStore::State & state, const DrvOutput & id)
{
    auto useQueryRealisedOutput(state.stmts->QueryRealisedOutput.use());
    useQueryRealisedOutput.apply(id.drvPath.to_string()).apply(id.outputName);
    if (!useQueryRealisedOutput.next())
        return std::nullopt;
    return UnkeyedRealisation{
        .outPath = parseStorePath(useQueryRealisedOutput.getStr(0)),
        .signatures = Signature::parseMany(tokenizeString<StringSet>(useQueryRealisedOutput.getStr(1))),
    };
}

void LocalStore::queryRealisationUncached(
    const DrvOutput & id, Callback<std::shared_ptr<const UnkeyedRealisation>> callback) noexcept
{
    try {
        auto maybeRealisation = retrySQLite<std::optional<UnkeyedRealisation>>(
            [&]() { return queryRealisation_(*_state->lock(), id); });
        if (maybeRealisation)
            callback(std::make_shared<const UnkeyedRealisation>(std::move(*maybeRealisation)));
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
