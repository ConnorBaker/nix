#pragma once
///@file

#include "nix/store/sqlite.hh"

#include "nix/store/pathlocks.hh"
#include "nix/store/store-api.hh"
#include "nix/store/indirect-root-store.hh"
#include "nix/store/git-object-store.hh"
#include "nix/util/object-hash-sink.hh"
#include "nix/util/fun.hh"
#include "nix/util/sync.hh"

#include <chrono>
#include <future>
#include <string>
#include <boost/unordered/unordered_flat_set.hpp>

namespace nix {

/**
 * Nix store and database schema version.
 *
 * Version 1 (or 0) was Nix <=
 * 0.7.  Version 2 was Nix 0.8 and 0.9.  Version 3 is Nix 0.10.
 * Version 4 is Nix 0.11.  Version 5 is Nix 0.12-0.16.  Version 6 is
 * Nix 1.0.  Version 7 is Nix 1.3. Version 10 is 2.0.
 *
 * Version 11 changes no table: `ValidPaths.hash` holds the object hash (01 section 9.11, `git:sha256:<base16>`)
 * where 10 held the NAR hash (`sha256:<base16>`); a version-10 row is migrated on first query or by `nix store
 * migrate`.  Bumped, against the additive-migration convention below, because the column's meaning changed.
 */
const int nixSchemaVersion = 11;

struct OptimiseStats
{
    /** Regular files replaced by a link to a blob file the store held. */
    unsigned long filesLinked = 0;
    uint64_t bytesFreed = 0;
    /** Regular files that became the store's blob file (a blob the store lacked). */
    unsigned long filesEntered = 0;
    /** Rows of an older schema given their object hash by the walk (`nix store migrate`'s work). */
    unsigned long rowsMigrated = 0;
};

struct LocalSettings;

struct LocalBuildStoreConfig : virtual LocalFSStoreConfig
{
private:
    void anchor() override;

    /**
      Input for computing the build directory. See `getBuildDir()`.
     */
    Setting<std::optional<AbsolutePath>> buildDir{
        this,
        std::nullopt,
        "build-dir",
        R"(
            The directory on the host, in which derivations' temporary build directories are created.

            If not set, Nix will use the `builds` subdirectory of its configured state directory.

            Note that builds are often performed by the Nix daemon, so its `build-dir` applies.

            Nix will create this directory automatically with suitable permissions if it does not exist.
            Otherwise its permissions must allow all users to traverse the directory (i.e. it must have `o+x` set, in unix parlance) for non-sandboxed builds to work correctly.

            This is also the location where [`--keep-failed`](@docroot@/command-ref/opt-common.md#opt-keep-failed) leaves its files.

            If Nix runs without sandbox, or if the platform does not support sandboxing with bind mounts (e.g. macOS), then the [`builder`](@docroot@/language/derivations.md#attr-builder)'s environment will contain this directory, instead of the virtual location [`sandbox-build-dir`](@docroot@/command-ref/conf-file.md#conf-sandbox-build-dir).

            > **Warning**
            >
            > `build-dir` must not be set to a world-writable directory.
            > Placing temporary build directories in a world-writable place allows other users to access or modify build data that is currently in use.
            > This alone is merely an impurity, but combined with another factor this has allowed malicious derivations to escape the build sandbox.

            See also the global [`build-dir`](@docroot@/command-ref/conf-file.md#conf-build-dir) setting.
        )"};
public:
    /**
     * For now, this just grabs the global local settings, but by having this method we get ready for these being
     * per-store settings instead.
     */
    const LocalSettings & getLocalSettings() const;

    std::filesystem::path getBuildDir() const;
};

struct LocalStoreConfig : std::enable_shared_from_this<LocalStoreConfig>,
                          virtual LocalFSStoreConfig,
                          virtual LocalBuildStoreConfig
{
    LocalStoreConfig(const Params & params)
        : StoreConfig(params, FilePathType::Native)
        , LocalFSStoreConfig(params)
    {
    }

    LocalStoreConfig(const std::filesystem::path & path, const Params & params);

private:
    void anchor() override;

    /**
     * An indirection so that we don't need to refer to global settings
     * in headers.
     */
    bool getDefaultRequireSigs();

public:
    Setting<bool> requireSigs{
        this,
        getDefaultRequireSigs(),
        "require-sigs",
        "Whether store paths copied into this store should have a trusted signature."};

    Setting<bool> readOnly{
        this,
        false,
        "read-only",
        R"(
          Allow this store to be opened when its [database](@docroot@/glossary.md#gloss-nix-database) is on a read-only filesystem.

          Normally Nix attempts to open the store database in read-write mode, even for querying (when write access is not needed), causing it to fail if the database is on a read-only filesystem.

          Enable read-only mode to disable locking and open the SQLite database with the [`immutable` parameter](https://www.sqlite.org/c3ref/open.html) set.

          > **Warning**
          > Do not use this unless the filesystem is read-only.
          >
          > Using it when the filesystem is writable can cause incorrect query results or corruption errors if the database is changed by another process.
          > While the filesystem the database resides on might appear to be read-only, consider whether another user or system might have write access to it.
        )"};

    bool getReadOnly() const override;

    Setting<bool> ignoreGcDeleteFailure{
        this,
        false,
        "ignore-gc-delete-failure",
        R"(
          Whether to ignore failures when deleting items with the garbage collector.

          Normally the garbage collector will fail with an error if the nix daemon cannot delete a file, with this setting such errors will only be printed as warnings.
        )",
        {},
        true,
        Xp::LocalOverlayStore,
    };

    Setting<bool> useRootsDaemon{
        this,
        false,
        "use-roots-daemon",
        R"(
          Whether to request garbage collector roots from an external daemon.

          When enabled, the garbage collector connects to a Unix domain socket
          at [`<state-dir>`](@docroot@/store/types/local-store.md#store-local-store-state)`/gc-roots-socket/socket` to discover additional roots
          that should not be collected. This is useful when the Nix daemon runs
          without root privileges and cannot scan `/proc` for runtime roots.

          The daemon can be started with [`nix store roots-daemon`](@docroot@/command-ref/new-cli/nix3-store-roots-daemon.md).
        )",
        {},
        true,
        Xp::LocalOverlayStore,
    };

    std::filesystem::path getRootsSocketPath() const;

    static const std::string name()
    {
        return "Local Store";
    }

    static StringSet uriSchemes()
    {
        return {"local"};
    }

    static std::string doc();

    ref<Store> openStore() const override;

    StoreReference getReference() const override;
};

MakeError(PathInUse, Error);

class LocalStore : public virtual IndirectRootStore, public virtual GcStore
{
    void anchor() override;

public:

    using Config = LocalStoreConfig;

    ref<const LocalStoreConfig> config;

private:

    /**
     * Lock file used for upgrading.
     */
    AutoCloseFD globalLock;

    struct State
    {
        /**
         * The SQLite database object.
         */
        SQLite db;

        struct Stmts;
        std::unique_ptr<Stmts> stmts;

        /**
         * The last time we checked whether to do an auto-GC, or an
         * auto-GC finished.
         */
        std::chrono::time_point<std::chrono::steady_clock> lastGCCheck;

        /**
         * Whether auto-GC is running. If so, get gcFuture to wait for
         * the GC to finish.
         */
        bool gcRunning = false;
        std::shared_future<void> gcFuture;
        std::thread gcThread;

        /**
         * How much disk space was available after the previous
         * auto-GC. If the current available disk space is below
         * minFree but not much below availAfterGC, then there is no
         * point in starting a new GC.
         */
        uint64_t availAfterGC = std::numeric_limits<uint64_t>::max();

        std::unique_ptr<PublicKeys> publicKeys;
    };

    /**
     * Mutable state. It's behind a `ref` to reduce false sharing
     * between immutable and mutable fields.
     */
    ref<Sync<State>> _state;

public:

    const std::filesystem::path dbDir;

    /**
     * The store's object store (01 §9.10): the local store's
     * representation, unconditionally; every path added is entered.
     */
    GitObjectStore objects;
    const std::filesystem::path reservedPath;
    const std::filesystem::path schemaPath;
    const std::filesystem::path tempRootsDir;
    const std::filesystem::path fnTempRoots;

private:

    const PublicKeys & getPublicKeys();

public:

    /**
     * Hack for build-remote.cc.
     */
    PathSet locksHeld;

    /**
     * Initialise the local store, upgrading the schema if
     * necessary.
     */
    LocalStore(ref<const Config> params);

    ~LocalStore();

    /**
     * Implementations of abstract store API methods.
     */

    bool isValidPathUncached(const StorePath & path) override;

    StorePathSet queryValidPaths(const StorePathSet & paths, SubstituteFlag maybeSubstitute = NoSubstitute) override;

    StorePathSet queryAllValidPaths() override;

    /**
     * The row, migrated (`migratedPathInfo`): a schema-10 row is given
     * its object hash by one walk of the path, outside the database lock,
     * and returned with `assertedNarHash` set to the NAR hash it held.
     */
    void queryPathInfoUncached(
        const StorePath & path, Callback<std::shared_ptr<const ValidPathInfo>> callback) noexcept override;

    /**
     * Migrate one row as `queryPathInfoUncached` does, for `nix store
     * migrate` and `addSignatures`: a row holding a NAR hash is given its
     * object hash and, when missing, its NAR size, and re-rendered.  A row
     * already migrated is left alone.
     *
     * @throws InvalidPath if the path is not valid.
     * @throws Error, naming the remedy, if the path's files do not hash to
     * the NAR hash the row asserts (both hashes named) or are missing
     * (`walkOldRow`); the row is left as it is.
     */
    void migratePathInfo(const StorePath & path);

    void queryReferrers(const StorePath & path, StorePathSet & referrers) override;

    StorePathSet queryValidDerivers(const StorePath & path) override;

    std::map<std::string, std::optional<StorePath>>
    queryStaticPartialDerivationOutputMap(const StorePath & path) override;

    std::optional<StorePath>
    queryStaticPartialDerivationOutput(const StorePath & path, const std::string & outputName) override;

    std::optional<StorePath> queryPathFromHashPart(const std::string & hashPart) override;

    bool pathInfoIsUntrusted(const ValidPathInfo &) override;
    bool realisationIsUntrusted(const Realisation &) override;

    void addToStore(const ValidPathInfo & info, Source & source, RepairFlag repair, CheckSigsFlag checkSigs) override;

    StorePath addToStoreFromDump(
        Source & dump,
        std::string_view name,
        FileSerialisationMethod dumpMethod,
        ContentAddressMethod hashMethod,
        HashAlgorithm hashAlgo,
        const StorePathSet & references,
        RepairFlag repair) override;

    // Designed to be used from RestrictedStore,
    // allows filtering the references while scanning.
    // Not an entirely separate function in order to reduce duplication
    StorePath addToStoreFromDump(
        Source & dump,
        std::string_view name,
        FileSerialisationMethod dumpMethod,
        ContentAddressMethod hashMethod,
        HashAlgorithm hashAlgo,
        const StorePathSet & references,
        RepairFlag repair,
        bool filterReferences);

    void addTempRoot(const StorePath & path) override;

private:

    void createTempRootsFile();

    /**
     * The file to which we write our temporary roots.
     */
    Sync<AutoCloseFD> _fdTempRoots;

    /**
     * The global GC lock.
     */
    Sync<AutoCloseFD> _fdGCLock;

    /**
     * Connection to the garbage collector.
     */
    Sync<AutoCloseFD> _fdRootsSocket;

public:

    /**
     * Implementation of IndirectRootStore::addIndirectRoot().
     *
     * The weak reference merely is a symlink to `path' from
     * /nix/var/nix/gcroots/auto/<hash of `path'>.
     */
    void addIndirectRoot(const std::filesystem::path & path) override;

private:

    void findTempRoots(Roots & roots, bool censor);

    AutoCloseFD openGCLock();

public:

    Roots findRoots(bool censor) override;

    void collectGarbage(const GCOptions & options, GCResults & results) override;

    void deleteBuildTraces(const std::set<DrvOutput> & keys) override;

    /**
     * Called by `collectGarbage` to trace in reverse.
     *
     * Using this rather than `queryReferrers` directly allows us to
     * fine-tune which referrers we consider for garbage collection;
     * some store implementations take advantage of this.
     */
    virtual void queryGCReferrers(const StorePath & path, StorePathSet & referrers)
    {
        return queryReferrers(path, referrers);
    }

    /**
     * Called by `collectGarbage` to recursively delete a path.
     * The default implementation simply calls `deletePath`, but it can be
     * overridden by stores that wish to provide their own deletion behaviour.
     *
     * @param isKnownPath true if this is a known store path, false if it's
     *        garbage/unknown content found in the store directory
     */
    virtual void deleteStorePath(const std::filesystem::path & path, uint64_t & bytesFreed, bool isKnownPath);

    /**
     * Optimise the disk space usage of the Nix store by hard-linking
     * files with the same contents.
     */
    void optimiseStore(OptimiseStats & stats);

    void optimiseStore() override;

    /**
     * Remove the `.links` table an older Nix kept: every entry is a hard
     * link to a file a store path also links, or a dead one, so removing
     * the whole directory frees the dead entries and loses nothing.  The
     * one deleter of it, called by the whole-store collection and by
     * `--optimise`; nothing when the directory does not exist.
     */
    void removeLegacyLinks();

    /**
     * Enter a materialised store path into the object store by one walk
     * -- after a build, and for a flat file or a plain copy
     * `addToStoreFromDump` wrote -- and return what that walk computed,
     * the root's entry and the NAR size, for the caller to register
     * (`ValidPathInfo::objectHash`, the database's fact).
     */
    ObjectHashSink::Result enterPath(const std::filesystem::path & path, RepairFlag repair);

    /**
     * Restore a NAR into `path` through the store's composite (`08` §1.6;
     * 01 §9.10, "Ingestion writes only what the store lacks").  The caller
     * registers the root's object hash.  `hooks->regularFileCreated` fires
     * only for the files written, not for those linked.
     */
    ObjectHashSink::Result restoreThroughObjects(
        const std::filesystem::path & path,
        Source & source,
        bool startFsync,
        RestoreSinkHooks * hooks,
        RepairFlag repair = NoRepair);

    /**
     * What the naming of a source knows about the tree being copied, as
     * the copy from the store's own objects consults it (01 §9.10, the
     * second route; `04` §1.7): the entry -- mode and identifier -- of
     * the node at `path`, relative to the tree's root, when the naming
     * has one (a memo row), else nullopt.  Consulted for the root and
     * for directories; never for a file inside a directory the store
     * lacks, which is read as the one route reads it.  What it answers
     * is trusted as the naming trusted it: the tree copied is the tree
     * named, within the memo's own window (01 §2.2).
     */
    using TreeNamer = fun<std::optional<merkle::TreeEntry>(const CanonPath & path, const SourceAccessor::Stat & st)>;

    /**
     * Whether a copy from an accessor may be made from the store's own
     * objects (01 §9.10, the second route): a store that owns its object
     * store (`ownsObjectStore`), on a system where the object store
     * links -- not Windows, where it shares nothing and the `*at` calls
     * the materialisation rests on have no counterpart.  The one
     * predicate the route is taken under; `fetchToStore2` asks it.
     */
    bool materialisesFromObjects() const;

    /**
     * Restore the tree at `path` into `dst` from the store's own objects
     * where it holds them, and through the composite of
     * `restoreThroughObjects` where it does not (01 §9.10, the second
     * route; `08` §1.6, `MaterialisingVisitor`): a node `namer` names
     * whose objects the store holds -- each directory's body in `trees/`,
     * verified against its identifier as it is read, each blob's file in
     * `blobs/` -- is made on disk by `mkdirat`, `linkat` and `symlinkat`
     * without reading the accessor; a node it does not name, or whose
     * objects the store lacks or holds corrupt, is read from the
     * accessor and written through the one route's visitor, which links
     * what the store holds and writes what it lacks.  Same result as
     * `restoreThroughObjects` on the accessor's NAR (law 2), same
     * hooks: `hooks->regularFileCreated` fires for the files written and
     * for none linked.  The caller registers the root's object hash.
     */
    ObjectHashSink::Result materialiseThroughObjects(
        const std::filesystem::path & dst,
        const SourcePath & path,
        PathFilter & filter,
        bool startFsync,
        RestoreSinkHooks * hooks,
        RepairFlag repair,
        TreeNamer namer);

    /**
     * `Store::addToStore(name, path, Git, …)` by `materialiseThroughObjects`:
     * the second route of 01 §9.10, with the registration of
     * `addToStoreFromDump` -- `autoGC` before the restore, a locked
     * temporary directory in the store, the path locked, moved into
     * place and registered with the root's object hash and the NAR size
     * the walk computed.  Only when `materialisesFromObjects()`.
     */
    StorePath materialise(
        std::string_view name, const SourcePath & path, PathFilter & filter, RepairFlag repair, TreeNamer namer);

    bool verifyStore(bool checkContents, RepairFlag repair) override;

protected:

    /**
     * Result of `verifyAllValidPaths`
     */
    struct VerificationResult
    {
        /**
         * Whether any errors were encountered
         */
        bool errors;

        /**
         * A set of so-far valid paths. The store objects pointed to by
         * those paths are suitable for further validation checking.
         */
        StorePathSet validPaths;
    };

    /**
     * First, unconditional step of `verifyStore`
     */
    virtual VerificationResult verifyAllValidPaths(RepairFlag repair);

    /**
     * Whether the object store under `realStoreDir` -- `.objects`, and an
     * older Nix's `.links` -- is this store's alone.  When it is not (the
     * overlay store: its store directory is a merged mount, so `.objects`
     * and `.links` there show the lower store's too, which is read-only
     * and shared), the collector sweeps no object and removes no `.links`,
     * and `--verify --check-contents` checks no path's objects and hashes
     * no object file, since an unlink or a repair through the merged path
     * would white out or copy up the lower store's files (01 §10, *the
     * local overlay store's object store is not its own*).  The upper
     * layer's own objects are then not
     * reclaimed; a sweep over the upper layer alone is the design owed
     * for the Linux run.
     */
    virtual bool ownsObjectStore() const
    {
        return true;
    }

public:

    /**
     * Register the validity of a path, i.e., that `path` exists, that
     * the paths referenced by it exists, and in the case of an output
     * path of a derivation, that it has been produced by a successful
     * execution of the derivation (or something equivalent).  Also
     * register the object hash of the path's contents, which `info`
     * must carry: an info without one is refused.
     */
    void registerValidPath(const ValidPathInfo & info);

    virtual void registerValidPaths(const ValidPathInfos & infos);

    unsigned int getProtocol() override;

    std::optional<TrustedFlag> isTrustedClient() override;

    void vacuumDB();

    void addSignatures(const StorePath & storePath, const std::set<Signature> & sigs) override;

    /**
     * If free disk space in /nix/store if below minFree, delete
     * garbage until it exceeds maxFree.
     */
    void autoGC(bool sync = true);

    /**
     * Register the store path 'output' as the output named 'outputName' of
     * derivation 'deriver'.
     */
    void registerDrvOutputUnchecked(const Realisation & info) override;
    void registerDrvOutput(const Realisation & info, CheckSigsFlag checkSigs) override;
    void cacheDrvOutputMapping(
        State & state, const uint64_t deriver, const std::string & outputName, const StorePath & output);

    std::optional<const UnkeyedRealisation> queryRealisation_(State & state, const DrvOutput & id);
    std::optional<std::pair<int64_t, UnkeyedRealisation>> queryRealisationCore_(State & state, const DrvOutput & id);
    void queryRealisationUncached(
        const DrvOutput &, Callback<std::shared_ptr<const UnkeyedRealisation>> callback) noexcept override;

    std::optional<std::string> getVersion() override;

protected:

    void verifyPath(
        const StorePath & path,
        fun<bool(const StorePath &)> existsInStoreDir,
        StorePathSet & done,
        StorePathSet & validPaths,
        RepairFlag repair,
        bool & errors);

private:

    /**
     * Retrieve the current version of the database schema.
     * If the database does not exist yet, the version returned will be 0.
     */
    int getSchema();

    void openDB(State & state, bool create);

    /**
     * Perform or check if a database schema upgrade is needed.
     * @param dryRun only check if an upgrade is needed.
     * @return true if an upgrade is needed or was performed, false otherwise.
     */
    bool upgradeDBSchema(State & state, bool dryRun);

    void makeStoreWritable();

    uint64_t queryValidPathId(State & state, const StorePath & path);

    uint64_t addValidPath(State & state, const ValidPathInfo & info);

    void invalidatePath(State & state, const StorePath & path);

    /**
     * Delete a path from the Nix store.
     */
    void invalidatePathChecked(const StorePath & path);

    /**
     * The row as it is, under the lock: `objectHash` set when the
     * column holds one, else `assertedNarHash` set to the NAR hash a
     * schema-10 row holds.  Never migrates -- `addSignatures` calls
     * this inside a transaction.
     */
    std::shared_ptr<const ValidPathInfo> queryPathInfoInternal(State & state, const StorePath & path);

    /**
     * `queryPathInfoInternal` under the lock it takes: the row as the
     * database has it, never migrated, null for a path that is not
     * valid.  For the collector, whose dead set needs a row's references
     * and deriver alone: migrating a row on its way to deletion would
     * read and hash every byte of the path first (`walkOldRow`), a cost
     * proportional to the garbage; the live rows its closure reaches are
     * migrated by `computeFSClosure`'s queries, which is right, since
     * they are kept (01 section 10, *the collector reads a dead row
     * without migrating it*).
     */
    std::shared_ptr<const ValidPathInfo> queryPathInfoUnmigrated(const StorePath & path);

    /**
     * What the walk of a schema-10 row's path found (01 §10, *a schema-10
     * row's migration checks the NAR hash the row asserts*):
     * `Migrated` -- the walk's NAR hash is the one the row asserts, the
     * object hash written; `Modified` -- it is not, nothing written;
     * `FilesMissing` -- the path's directory, or a file under it, is gone
     * while the row is valid (what `nix-store --verify` finds and
     * removes), nothing written.
     */
    enum struct OldRowWalk { Migrated, Modified, FilesMissing };

    /**
     * One walk of the path of a schema-10 row: the object hash and the NAR
     * size, and on a tee of the same bytes the NAR hash, compared with the
     * one the row asserts -- as `--load-db` checks a registration
     * (`nix-store.cc`).  On `Migrated`, `info` carries the object hash (and
     * the size when the row lacked it) and the column is written
     * (`recordObjectHash`); on `Modified` the two NAR hashes -- asserted,
     * walked -- are stored in `narHashes` when given.  No root is taken
     * (see `migratedPathInfo`).
     *
     * @throws InvalidPath if the row was deleted before or during the walk.
     */
    OldRowWalk walkOldRow(ValidPathInfo & info, std::optional<std::pair<Hash, Hash>> * narHashes = nullptr);

    /**
     * The migration behind `queryPathInfoUncached` and `migratePathInfo`
     * (`08` §1.1): outside the lock, the row re-read before it is written
     * (`recordObjectHash`), and no root taken -- the caller holds one, or
     * is the collector.  A row whose path the walk finds modified, or
     * whose files are missing, is returned as the database has it --
     * `assertedNarHash` set, no object hash -- so that a query answers as
     * master's did and the verifiers report what is wrong: the
     * modification through the NAR hash (`contentMismatch`, `verifyStore
     * --check-contents`), the missing files by `nix-store --verify`, which
     * removes the row; nothing writes the column until then.
     *
     * @throws InvalidPath if the row was deleted before or during the walk.
     */
    std::shared_ptr<const ValidPathInfo> migratedPathInfo(std::shared_ptr<const ValidPathInfo> info);

    /**
     * The one write of an older row's object hash: the row re-read under
     * the lock, and written only if it still holds no object hash (another
     * process may have migrated, signed or deleted it meanwhile).  Nothing
     * on a read-only store.  Called by `migratedPathInfo` with the walk's
     * result and by `optimiseStore` with the entering walk's, which is the
     * same walk.
     *
     * @return Whether the row was written.
     */
    bool recordObjectHash(const StorePath & path, const ObjectHash & objectHash, uint64_t narSize);

    /**
     * Write the row from `info`.
     *
     * @throws Error if `info` lacks its object hash: nothing stores a NAR
     * hash any more.
     */
    void updatePathInfo(State & state, const ValidPathInfo & info);

    void findRoots(const std::filesystem::path & path, std::filesystem::file_type type, Roots & roots);

    void findRootsNoTemp(Roots & roots, bool censor);

    void findRuntimeRoots(Roots & roots, bool censor);

    std::pair<std::filesystem::path, AutoCloseFD> createTempDirInStore();

    /**
     * The object hash of every valid path, from the column, under the
     * database lock: the sweep's live roots.  A row older than schema 11
     * is skipped, not migrated (the migration walks), and counted.
     */
    uint64_t forEachValidObjectHash(fun<void(const ObjectHash &)> callback);

    /**
     * The verifier's walk of the path at `realPath` (`VerifyVisitor`; law 4
     * of 01 §9.10 with law 3's exceptions): its object hash and NAR size,
     * and every object the walk reached that the object store lacks and
     * should hold -- each tree on the way, the synthetic root tree of a
     * bare executable or symlink, each symlink's target blob, and the blob
     * of each regular file the store places (`placementOf`: none is
     * expected for a file it leaves as written).  In walk order; the
     * second is empty when the law holds.
     */
    std::pair<ObjectHashSink::Result, std::vector<std::filesystem::path>>
    verifyObjects(const std::filesystem::path & realPath);

    /**
     * The walk behind `enterPath` and `optimiseStore` (`EnterVisitor`,
     * `08` §1.6): a file whose inode is in `blobInodes` (when given) is
     * not read; `act` and `stats`, when given, count the files entered and
     * replaced.
     */
    ObjectHashSink::Result enterIntoObjects(
        const std::filesystem::path & path,
        RepairFlag repair,
        Activity * act = nullptr,
        OptimiseStats * stats = nullptr,
        GitObjectStore::BlobInodes * blobInodes = nullptr);

    // Internal versions that are not wrapped in retry_sqlite.
    bool isValidPath_(State & state, const StorePath & path);
    void queryReferrers(State & state, const StorePath & path, StorePathSet & referrers);

    void addBuildLog(const StorePath & drvPath, std::string_view log) override;

    friend struct PathSubstitutionGoal;
    friend struct DerivationGoal;
    /* Only used for createTempDirInStore. */
    friend class DerivationBuilderImpl;
    /* Drives `enterIntoObjects` alone (src/libstore-tests/local-store.cc). */
    friend class LocalStoreObjectsTest;
};

} // namespace nix
