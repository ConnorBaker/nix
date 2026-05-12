#pragma once
///@file

#include "nix/store/sqlite.hh"

#include "nix/store/pathlocks.hh"
#include "nix/store/store-api.hh"
#include "nix/store/indirect-root-store.hh"
#include "nix/util/sync.hh"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <boost/unordered/unordered_flat_set.hpp>
#include <boost/unordered/concurrent_flat_set.hpp>

#include <oneapi/tbb/enumerable_thread_specific.h>

namespace nix {

/**
 * Nix store and database schema version.
 *
 * Version 1 (or 0) was Nix <=
 * 0.7.  Version 2 was Nix 0.8 and 0.9.  Version 3 is Nix 0.10.
 * Version 4 is Nix 0.11.  Version 5 is Nix 0.12-0.16.  Version 6 is
 * Nix 1.0.  Version 7 is Nix 1.3. Version 10 is 2.0.
 */
const int nixSchemaVersion = 10;

struct OptimiseStats
{
    /* Cache-line-aligned so the two counters don't share a line with
       each other or with surrounding state; under high worker counts,
       `fetch_add` on a shared line is dominated by cache-coherence
       traffic rather than the actual arithmetic. */
    alignas(std::hardware_destructive_interference_size) std::atomic<uint64_t> filesLinked{0};
    alignas(std::hardware_destructive_interference_size) std::atomic<uint64_t> bytesFreed{0};

    /**
     * Wall-clock per stage of `optimiseStore`, in nanoseconds.
     * Updated sequentially by the main thread between stage
     * boundaries — no atomics needed.
     *
     * Stages, in order:
     *   - `setupNs`: lock acquisition + minor bookkeeping
     *   - `migrateLinksNs`: one-shot `migrateLinksDirToSharded`
     *     call (zero when `shardedLinks == false`)
     *   - `queryAllPathsNs`: `queryAllValidPaths()` SQLite scan
     *   - `loadInodeHashNs`: `loadInodeHashInto()` walk of
     *     `.links/` (both flat and sharded entries)
     *   - `parallelOptimiseNs`: the `tbb::parallel_for_each`
     *     covering `optimisePath_` over every path
     *
     * Benches surface these as `state.counters` so the matrix
     * can attribute wall-clock differences to specific stages
     * rather than just reporting the total.
     */
    struct Timings
    {
        uint64_t setupNs = 0;
        uint64_t migrateLinksNs = 0;
        uint64_t queryAllPathsNs = 0;
        uint64_t loadInodeHashNs = 0;
        uint64_t parallelOptimiseNs = 0;
    };
    Timings timings;
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
          at [`<state-dir>`](@docroot@/store/types/local-store.md#store-option-state)`/gc-roots-socket/socket` to discover additional roots
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
    const std::filesystem::path linksDir;

    /**
     * Whether `linksDir` is organised as a sharded tree
     * (`<linksDir>/<pfx>/<hash>[.<NN>]`) vs. the legacy flat layout
     * (`<linksDir>/<hash>[.<NN>]`). Set at construction time from
     * the `sharded-links` experimental feature. Both layouts are
     * readable regardless of this flag; the flag only decides where
     * newly-created canonical entries land and whether
     * `optimiseStore` performs a one-shot migration of pre-existing
     * flat entries into shards.
     *
     * Independent of `maxLinkReplicas` / replica spillover: both
     * the flat and sharded layouts can host `<hash>` (replica 0)
     * alongside `<hash>.01`, `<hash>.02`, … when the replica-spill
     * walk is enabled.
     */
    const bool shardedLinks;

    /**
     * Encoding-level cap on the number of replica inodes per hash.
     * The on-disk filename scheme uses a 2-digit zero-padded decimal
     * suffix (`.00`..`.99`) for replicas > 0; replica 0 has no
     * suffix. 100 slots is therefore the maximum that fits in the
     * path format.
     *
     * The *runtime* replica cap (how many replicas `optimise`
     * actually walks before giving up) is the `max-link-replicas`
     * setting in `LocalSettings`, clamped to this constant.
     * Decoupling the runtime cap from the encoding cap is what lets
     * the bench enumerate `sharded_single_replica_hardlink` (runtime
     * cap = 1, isolates the directory-layout cost) separately from
     * `sharded_multi_replica_hardlink` (runtime cap = 100, exercises
     * spill).
     */
    static constexpr uint8_t maxReplicaSlots = 100;

    /**
     * Per-file hardlink ceiling for the filesystem hosting
     * `linksDir`, queried once at construction via `pathconf(3)`
     * with `_PC_LINK_MAX`.
     *
     * Used as an opportunistic pre-flight in the replica walk: when
     * we `lstat` a candidate replica and observe `st_nlink >= linkMax`,
     * we skip to the next replica without issuing a `link(2)` that we
     * already know the kernel would reject with EMLINK. This is an
     * optimisation, not a correctness requirement — the commit path
     * handles EMLINK by advancing to the next replica regardless, so
     * even a stale or conservative value is safe. That is why there
     * is no margin: the kernel is the source of truth for the actual
     * ceiling, so a racing writer that pushes us over between lstat
     * and link(2) simply surfaces EMLINK and we advance.
     *
     * Typical values: ext4 = 65000, tmpfs = INT_MAX, XFS = 2^31-1.
     * If `pathconf` is unavailable or returns `-1` we fall back to
     * a conservative hardcoded 32000, which is safe everywhere.
     */
    /* Default to the conservative fallback at declaration time. The
       constructor body overwrites this with the pathconf probe (or
       `_link-max-override` if set), but if any earlier ctor step
       throws — `createDirs(linksDir / shard)` for sharded stores
       can throw EACCES, EIO, ENOSPC — we'd leave the member
       uninitialised. Today nothing reads `linkMax` during stack
       unwinding, but a default keeps the field well-defined under
       all error paths. */
    int64_t linkMax = 32000;

    /**
     * Compute the path for a canonical `.links/` entry, given a hash
     * and a replica index (0..maxReplicaSlots).
     *
     * The directory and filename are independent axes:
     *
     *   * Directory: `<linksDir>` when `shardedLinks == false`,
     *     `<linksDir>/<first-2-chars>` when `shardedLinks == true`.
     *   * Filename: `<hash>` for replica 0, `<hash>.<NN>` for
     *     replica > 0.
     *
     * Combined, the four cases are:
     *   - flat,    0:    `<linksDir>/<hash>`
     *   - flat,    N>0:  `<linksDir>/<hash>.<NN>`
     *   - sharded, 0:    `<linksDir>/<pfx>/<hash>`
     *   - sharded, N>0:  `<linksDir>/<pfx>/<hash>.<NN>`
     *
     * The unsuffixed primary preserves backward compatibility with
     * pre-`fffca655b` `.links/<hash>` entries: a store toggled into
     * `sharded-links` keeps the unsuffixed primary form per shard.
     */
    std::filesystem::path linkPathFor(const Hash & hash, uint8_t replica = 0) const;

    /**
     * Given a `.links/` entry filename, return the base hash portion
     * (everything before the `.<NN>` replica suffix, if any). Names
     * without a replica suffix (the primary slot in either layout)
     * are returned unchanged.
     */
    std::string_view stripReplicaSuffix(std::string_view name) const;

    /**
     * Bulk one-shot migration of legacy flat `.links/<H>` entries
     * into their sharded slots. Called from `optimiseStore` when the
     * `sharded-links` experimental feature is enabled. Idempotent
     * and race-tolerant.
     */
    void migrateLinksDirToSharded();

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

    void queryPathInfoUncached(
        const StorePath & path, Callback<std::shared_ptr<const ValidPathInfo>> callback) noexcept override;

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
     * Per-thread connections to the garbage collector. Each thread
     * that calls `addTempRoot` under an active GC gets its own
     * socket, so N workers send temp-root announcements in parallel
     * instead of serialising on one shared `Sync<AutoCloseFD>`.
     *
     * Per-thread storage via `tbb::enumerable_thread_specific`: O(1)
     * `local()` access via TLS, default-constructs `AutoCloseFD{}`
     * on first access per thread. Works for any thread (TBB-spawned
     * or `std::thread`).
     *
     * Lifetime caveat: TBB does *not* destroy entries at thread
     * exit. Per-thread `AutoCloseFD`s live until `~LocalStore` runs
     * `~enumerable_thread_specific`, which iterates and destroys
     * each entry. Consequences:
     *
     *  - Long-running stores accumulate one fd per distinct worker
     *    thread that ever called `addTempRoot`. The GC's per-thread
     *    `connect`-on-demand caps this at the number of unique
     *    worker IDs the daemon has ever seen for a GC cycle (in
     *    practice, ≲ the configured `gcLinksThreads` setting), so
     *    it's bounded but non-zero.
     *  - On `thread::id` reuse — a new worker thread may receive an
     *    entry created by a previous thread with the same id — the
     *    old socket may already be closed daemon-side (the GC
     *    server tears down inactive client conns). The next
     *    `writeFull`/`readFull` call will surface `EPIPE` or
     *    `ECONNRESET`; `addTempRoot`'s catch blocks handle these
     *    by dropping the fd and reconnecting on the next loop
     *    iteration. So this is a small extra round-trip, not a
     *    correctness issue.
     */
    tbb::enumerable_thread_specific<AutoCloseFD> _fdRootsSockets;

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
     * Whether this store supports the two-phase GC deletion scheme
     * (rename dead paths aside to `.gc-<pid>-<n>-<basename>` under
     * the GC lock, then recursively delete them outside the hot
     * path of the traversal).
     *
     * Plain `LocalStore` returns true. `LocalOverlayStore` returns
     * false because the rename-aside would leave a "hole" in the
     * upper layer that the lower layer would fill from — so a path
     * present in both layers would appear to "come back to life"
     * from the overlay view. Overlay-aware deletion has to go
     * through the overridden `deleteStorePath` which knows how to
     * create a whiteout instead.
     */
    virtual bool supportsTwoPhaseDelete() const
    {
        return true;
    }

    /**
     * Optimise the disk space usage of the Nix store by hard-linking
     * files with the same contents.
     */
    void optimiseStore(OptimiseStats & stats);

    void optimiseStore() override;

    /**
     * Optimise a single store path. Optionally, test the encountered
     * symlinks for corruption.
     */
    void optimisePath(const std::filesystem::path & path, RepairFlag repair);

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

public:

    /**
     * Register the validity of a path, i.e., that `path` exists, that
     * the paths referenced by it exists, and in the case of an output
     * path of a derivation, that it has been produced by a successful
     * execution of the derivation (or something equivalent).  Also
     * register the hash of the file system contents of the path.  The
     * hash must be a SHA-256 hash.
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
     * Delete a path from the Nix store.
     *
     * Fails with `PathInUse` if there are paths still referring to
     * `path`; the caller is responsible for ensuring that isn't the
     * case (usually by inspecting the referrer graph beforehand).
     */
    void invalidatePathChecked(const StorePath & path);

    /**
     * Batched variant of `invalidatePathChecked`. Runs the whole
     * list through a single SQLite transaction, amortising the
     * fsync cost of commit over many paths. In-batch referrers are
     * tolerated (they'll be invalidated together); out-of-batch
     * referrers raise `PathInUse` and roll the transaction back
     * without affecting any path.
     *
     * Cache invalidation is deferred until after commit so a
     * rollback leaves no stale negative entries in the in-memory
     * path-info cache.
     */
    void invalidatePathsChecked(const std::vector<StorePath> & paths);

    /**
     * Register the store path 'output' as the output named 'outputName' of
     * derivation 'deriver'.
     */
    void registerDrvOutput(const Realisation & info) override;
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

    void upgradeDBSchema(State & state);

    void makeStoreWritable();

    uint64_t queryValidPathId(State & state, const StorePath & path);

    uint64_t addValidPath(State & state, const ValidPathInfo & info);

    void invalidatePath(State & state, const StorePath & path);

    std::shared_ptr<const ValidPathInfo> queryPathInfoInternal(State & state, const StorePath & path);

    void updatePathInfo(State & state, const ValidPathInfo & info);

    void findRoots(const std::filesystem::path & path, std::filesystem::file_type type, Roots & roots);

    void findRootsNoTemp(Roots & roots, bool censor);

    void findRuntimeRoots(Roots & roots, bool censor);

    std::pair<std::filesystem::path, AutoCloseFD> createTempDirInStore();

    /* A set of inodes already present in ‘.links/’. Used as a fast
       skip check for files that are already deduplicated. The
       concurrent container allows lock-free reads/writes from
       multiple optimise worker threads. */
    using InodeHash = boost::concurrent_flat_set<ino_t>;

    void loadInodeHashInto(InodeHash & inodeHash);
    std::vector<std::string>
    readDirectoryIgnoringInodes(const std::filesystem::path & path, const InodeHash & inodeHash);

    /* Walk every canonical-link leaf entry under `linksDir`, whether
       flat (`linksDir/<hash>`) or sharded
       (`linksDir/<pfx>/<hash>.<NN>`). Errors enumerating a top-level
       entry are logged and the entry is skipped. */
    void forEachLinkEntry(std::function<void(const std::filesystem::path &)> onLink);

    /* Lazy per-directory writability guard used by `optimisePath_` to
       batch the chmod(+w) / canonicalise(-w) dance across all files
       in a directory that need replacement. Declared here (rather
       than as a file-local struct in `optimise-store.cc`) so the
       pointer can appear in `optimisePath_`'s signature for
       recursion. */
    struct DirWritability
    {
        std::filesystem::path dir;
        bool needsCanonicalise = false;
        bool isStoreRoot;

        DirWritability(std::filesystem::path dir, const std::filesystem::path & storeRoot);
        void ensureWritable();
        ~DirWritability();

        DirWritability(const DirWritability &) = delete;
        DirWritability & operator=(const DirWritability &) = delete;
    };

    void optimisePath_(
        OptimiseStats & stats,
        const std::filesystem::path & path,
        InodeHash & inodeHash,
        RepairFlag repair,
        DirWritability * parentDirWritability = nullptr);

    // Internal versions that are not wrapped in retry_sqlite.
    bool isValidPath_(State & state, const StorePath & path);
    void queryReferrers(State & state, const StorePath & path, StorePathSet & referrers);

    void addBuildLog(const StorePath & drvPath, std::string_view log) override;

    friend struct PathSubstitutionGoal;
    friend struct DerivationGoal;
    /* Only used for createTempDirInStore. */
    friend class DerivationBuilderImpl;
};

} // namespace nix
