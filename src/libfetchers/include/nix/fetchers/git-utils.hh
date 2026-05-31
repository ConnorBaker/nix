#pragma once

#include "nix/fetchers/filtering-source-accessor.hh"
#include "nix/util/fs-sink.hh"

namespace nix {

namespace fetchers {
struct PublicKey;
struct Settings;
} // namespace fetchers

/**
 * A sink that writes into a Git repository. Note that nothing may be written
 * until `flush()` is called.
 */
struct GitFileSystemObjectSink : ExtendedFileSystemObjectSink
{
    /**
     * Flush builder and return a final Git hash.
     */
    virtual Hash flush() = 0;
};

struct GitPromisorProvider;

struct GitAccessorOptions
{
    bool exportIgnore = false;
    bool smudgeLfs = false;

    /** Optional partial-clone provider. When set, the
     *  `GitSourceAccessor`'s `prefetchSubtree` enumerates missing
     *  blob OIDs reachable from the requested subpath and asks the
     *  provider to fetch them in one coalesced round-trip.
     *
     *  When unset (the common case for fully-cloned repos),
     *  `prefetchSubtree` is a no-op. The accessor still works against
     *  whatever objects are already in the local ODB. */
    std::shared_ptr<GitPromisorProvider> provider;
};

struct GitRepo
{
    virtual ~GitRepo() {}

    struct Options
    {
        bool create = false;
        bool bare = false;
        bool packfilesOnly = false;
    };

    static ref<GitRepo> openRepo(const std::filesystem::path & path, Options options);

    virtual uint64_t getRevCount(const Hash & rev) = 0;

    virtual uint64_t getLastModified(const Hash & rev) = 0;

    virtual bool isShallow() = 0;

    /* Return the commit hash to which a ref points. */
    virtual Hash resolveRef(std::string ref) = 0;

    virtual void setRemote(const std::string & name, const std::string & url) = 0;

    /**
     * Info about a submodule.
     */
    struct Submodule
    {
        CanonPath path;
        std::string url;
        std::string branch;
    };

    struct WorkdirInfo
    {
        bool isDirty = false;

        /* The checked out commit, or nullopt if there are no commits
           in the repo yet. */
        std::optional<Hash> headRev;

        /* All files in the working directory that are unchanged,
           modified or added, but excluding deleted files. */
        std::set<CanonPath> files;

        /* All modified or added files. */
        std::set<CanonPath> dirtyFiles;

        /* The deleted files. */
        std::set<CanonPath> deletedFiles;

        /* The submodules listed in .gitmodules of this workdir. */
        std::vector<Submodule> submodules;
    };

    virtual WorkdirInfo getWorkdirInfo() = 0;

    static WorkdirInfo getCachedWorkdirInfo(const std::filesystem::path & path);

    /* Drop all entries from the getCachedWorkdirInfo() cache. */
    static void invalidateWorkdirInfoCache();

    /* Get the ref that HEAD points to. */
    virtual std::optional<std::string> getWorkdirRef() = 0;

    /**
     * Return the submodules of this repo at the indicated revision,
     * along with the revision of each submodule.
     */
    virtual std::vector<std::tuple<Submodule, Hash>> getSubmodules(const Hash & rev, bool exportIgnore) = 0;

    virtual std::string resolveSubmoduleUrl(const std::string & url) = 0;

    virtual bool hasObject(const Hash & oid) = 0;

    /**
     * If this repo is a Git partial clone (has the
     * `extensions.partialClone` setting naming a remote), return that
     * remote's URL. Otherwise return `nullopt`.
     *
     * This is the seam used by `GitInputScheme` to decide whether to
     * construct a `GitPromisorProvider`: the existence of
     * `extensions.partialClone` is what tells us we're in a clone where
     * blobs may be missing locally and need to be fetched on demand.
     */
    virtual std::optional<std::string> getPartialCloneRemoteUrl() = 0;

    /**
     * Configure this repository as a Git *partial clone* of the remote
     * named `remoteName`: set `core.repositoryformatversion = 1`,
     * `extensions.partialClone = <remoteName>`, and mark that remote as
     * a promisor with a `blob:none` filter. After this, a subsequent
     * `fetch(..., filtered = true)` pulls commit/tree objects but omits
     * blobs, and `getPartialCloneRemoteUrl()` reports the remote so the
     * on-demand blob backfill (`GitPromisorProvider`) attaches.
     *
     * Caller must ensure the remote actually supports filtered fetches
     * (protocol v2 `filter` capability) before relying on laziness;
     * see `git-lazy-fetch` in `GitInputScheme::getAccessorFromCommit`.
     */
    virtual void markAsPartialClone(const std::string & remoteName) = 0;

    virtual ref<SourceAccessor>
    getAccessor(const Hash & rev, const GitAccessorOptions & options, std::string displayPrefix) = 0;

    virtual ref<SourceAccessor> getAccessor(
        const WorkdirInfo & wd, const GitAccessorOptions & options, MakeNotAllowedError makeNotAllowedError) = 0;

    virtual ref<GitFileSystemObjectSink> getFileSystemObjectSink() = 0;

    virtual void flush() = 0;

    /**
     * Fetch `refspec` from `url` into this repository.
     *
     * If `filtered` is true, fetch with `--filter=blob:none` so blobs
     * are omitted (a partial clone). The repository should already be
     * marked as a partial clone (see `markAsPartialClone`) so the
     * omitted blobs are backfilled on demand. If the remote does not
     * support filtered fetches, Git transparently performs a full
     * fetch instead, so passing `filtered = true` is never fatal.
     */
    virtual void fetch(const std::string & url, const std::string & refspec, bool shallow, bool filtered = false) = 0;

    /**
     * Verify that commit `rev` is signed by one of the keys in
     * `publicKeys`. Throw an error if it isn't.
     */
    virtual void verifyCommit(const Hash & rev, const std::vector<fetchers::PublicKey> & publicKeys) = 0;

    /**
     * Given a Git tree hash, compute the hash of its NAR
     * serialisation. This is memoised on-disk.
     */
    virtual Hash treeHashToNarHash(const fetchers::Settings & settings, const Hash & treeHash) = 0;

    /**
     * If the specified Git object is a directory with a single entry
     * that is a directory, return the ID of that object.
     * Otherwise, return the passed ID unchanged.
     */
    virtual Hash dereferenceSingletonDirectory(const Hash & oid) = 0;

    /**
     * Synthesise a Git tree object containing only the entries from
     * `baseTree` whose paths appear in `acceptedRelativePaths`, and
     * write it into this repo's ODB. Returns the new tree OID.
     *
     * The synthesised tree is *equivalent* to the result of running
     * the user's filter against `baseTree` and serialising the
     * accepted bytes — but builds *purely from existing tree/blob
     * OIDs in `baseTree`*. **No blob bytes are read.** Costs scale
     * with the number of accepted entries, not their size.
     *
     * The returned tree's NAR hash can be looked up in the persistent
     * `treeHashToNarHash` projection (Track A) for a walk-free cache
     * hit. This is the §"Source Views" stretch that makes Git-shaped
     * subsets and overlays cheap on first eval.
     *
     * `acceptedRelativePaths` are paths relative to `baseTree`'s root
     * (e.g. `/foo/bar.txt`). Including a path implies including all
     * ancestor directories; the synthesiser doesn't require explicit
     * ancestors in the set, but does require the base tree to actually
     * contain each accepted path (otherwise the path is silently
     * dropped — synthesis is best-effort).
     */
    virtual Hash synthesiseTree(const Hash & baseTree, const std::set<CanonPath> & acceptedRelativePaths) = 0;

    /**
     * Non-flushing sibling of `synthesiseTree`: build the same synthetic
     * tree (identical OID — both reuse the base's blob/tree OIDs
     * verbatim and write via `git_treebuilder`), but DO NOT `flush()`
     * the mempack backend to a packfile.
     *
     * The Track Z.gap5 use is purely as a `treeHashToNarHash` CACHE KEY:
     * the OID is computed in-process and never retrieved as a Git object
     * afterwards. `git_treebuilder_write` makes the OID readable from the
     * mempack backend immediately, so the flush — which writes a
     * permanent, never-GC'd packfile to disk — is pure waste here. Use
     * this variant when you only need the OID's *identity*, not the
     * object's *retrievability*.
     *
     * `synthesiseTree` (above) keeps flushing for callers (e.g. the
     * test fixture) that open an accessor over the synthetic OID and
     * must find it on disk.
     */
    virtual Hash synthesiseTreeOid(const Hash & baseTree, const std::set<CanonPath> & acceptedRelativePaths) = 0;
};

/**
 * Recover the `GitRepo &` backing a `SourceAccessor`, if it is one of
 * the Git-rooted accessor shapes. Returns `nullptr` for any other
 * accessor (memory, POSIX, a non-Git view substrate, …).
 *
 * The concrete Git accessor types (`GitSourceAccessor`,
 * `GitExportIgnoreSourceAccessor`) are file-private to `git-utils.cc`,
 * so this downcast helper lives there and is the only sanctioned way
 * for out-of-file callers (e.g. `source-view-git.cc`) to reach the
 * underlying repo for tree synthesis.
 *
 * `GitExportIgnoreSourceAccessor` is a `FilteringSourceAccessor`
 * wrapping a `GitSourceAccessor`; this helper holds a `ref<GitRepoImpl>`
 * directly on both, so it recovers the repo from either without
 * unwrapping the inner `next` accessor.
 */
GitRepo * getGitRepoOf(SourceAccessor & accessor);

// A helper to ensure that we don't leak objects returned by libgit2.
template<typename T>
struct Setter
{
    T & t;
    typename T::pointer p = nullptr;

    Setter(T & t)
        : t(t)
    {
    }

    ~Setter()
    {
        if (p)
            t = T(p);
    }

    operator typename T::pointer *()
    {
        return &p;
    }
};

/**
 * Checks that the string can be a valid git reference, branch or tag name.
 * Accepts shorthand references (one-level refnames are allowed), pseudorefs
 * like `HEAD`.
 *
 * @note This is a coarse test to make sure that the refname is at least something
 * that Git can make sense of.
 */
bool isLegalRefName(const std::string & refName);

} // namespace nix
