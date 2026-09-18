#pragma once

#include "nix/fetchers/filtering-source-accessor.hh"
#include "nix/fetchers/merkle-tar-adapter.hh"

namespace nix {

namespace fetchers {
struct PublicKey;
struct Settings;
} // namespace fetchers

struct GitAccessorOptions
{
    bool exportIgnore = false;
    bool smudgeLfs = false;
    /**
     * For a commit accessor with `exportIgnore`: whether no attributes
     * source the filter consults names `export-ignore`, so that the filter
     * is the identity and the accessor returned is the inner one, with its
     * names and without an attribute lookup per path.  The caller answers
     * it (`treeMentionsAttribute`, `attributesOutsideTheTreeMention`),
     * since the tree's part of the answer is a function of the commit and
     * worth keeping across processes.
     */
    bool exportIgnoreHidesNothing = false;
};

struct GitRepo
{
    virtual ~GitRepo() = default;

    struct Options
    {
        bool create = false;
        bool bare = false;
        bool packfilesOnly = false;
        /**
         * Whether to avoid finding deltas when writing packfiles. It's an
         * expensive operation, which should be avoided if no benefit is
         * expected from possible deduplication in the same packfile.
         */
        bool dontFindDeltas = false;
        /**
         * The object-id type a repository is created with: SHA-1, git's
         * default, or SHA-256 (libgit2 2.0).  An existing repository keeps
         * its own.
         */
        HashAlgorithm oidType = HashAlgorithm::SHA1;
    };

    static ref<GitRepo> openRepo(const std::filesystem::path & path, Options options);

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
     * Whether any `.gitattributes` file in the tree of `rev`, at any depth,
     * names `attribute`.  One walk of the tree, reading only those blobs; a
     * function of the commit.  Any failure to read answers yes.
     */
    virtual bool treeMentionsAttribute(const Hash & rev, std::string_view attribute) = 0;

    /**
     * Whether an attributes source other than the commit's own tree that
     * libgit2 consults for this repository names `attribute`.  For both
     * accessors: `info/attributes`; `core.attributesFile`, or the user's
     * `git/attributes` when it is unset; the system's `gitattributes`,
     * whose rules `GIT_ATTR_CHECK_NO_SYSTEM` excludes but whose macro
     * definitions load regardless; the working directory's root
     * `.gitattributes`, likewise loaded for macros; and the index's
     * `.gitattributes` entries.  For the commit accessor, whose lookup
     * reads the working directory before the index and the commit
     * (`GIT_ATTR_CHECK_FILE_THEN_INDEX`, the low bits of its flags), also
     * the working directory's `.gitattributes` in every directory of
     * `rev`'s tree, one stat each; `rev` is given for that accessor and
     * absent for the working directory's, which asks with
     * `GIT_ATTR_CHECK_INDEX_ONLY`.  Derived from libgit2's `attr.c`
     * (`attr_setup`, `attr_decide_sources`, `collect_attr_files`); any
     * failure to read answers yes.
     */
    virtual bool attributesOutsideTheTreeMention(std::string_view attribute, std::optional<Hash> rev) = 0;

    virtual ref<SourceAccessor>
    getAccessor(const Hash & rev, const GitAccessorOptions & options, std::string displayPrefix) = 0;

    virtual ref<SourceAccessor> getAccessor(
        const WorkdirInfo & wd, const GitAccessorOptions & options, MakeNotAllowedError makeNotAllowedError) = 0;

    virtual void flush() = 0;

    virtual std::unique_ptr<merkle::DirectorySinkWithFinalize> makeDirectorySink() = 0;
    virtual std::unique_ptr<merkle::RegularFileSinkWithFinalize> makeRegularFileSink() = 0;

    virtual void fetch(const std::string & url, const std::string & refspec, bool shallow) = 0;

    /**
     * Verify that commit `rev` is signed by one of the keys in
     * `publicKeys`. Throw an error if it isn't.
     */
    virtual void verifyCommit(const Hash & rev, const std::vector<fetchers::PublicKey> & publicKeys) = 0;

    /**
     * If the specified Git object is a directory with a single entry
     * that is a directory, return the ID of that object.
     * Otherwise, return the passed ID unchanged.
     */
    virtual Hash dereferenceSingletonDirectory(const Hash & oid) = 0;
};

struct GitRepoPool : merkle::FileSinkBuilder
{
    virtual ~GitRepoPool() = default;

    static ref<GitRepoPool> create(const std::filesystem::path & path, GitRepo::Options options);

    virtual uint64_t getRevCount(const Hash & rev) = 0;
};

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
