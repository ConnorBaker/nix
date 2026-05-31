#include "nix/fetchers/git-utils.hh"
#include "nix/fetchers/git-lfs-fetch.hh"
#include "nix/fetchers/git-promisor.hh"
#include "nix/fetchers/cache.hh"
#include "nix/fetchers/projection.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/util/base-n.hh"
#include "nix/util/finally.hh"
#include "nix/util/fingerprint.hh"
#include "nix/util/os-string.hh"
#include "nix/util/processes.hh"
#include "nix/util/signals.hh"
#include "nix/util/users.hh"
#include "nix/util/fs-sink.hh"
#include "nix/util/sync.hh"
#include "nix/util/util.hh"
#include "nix/util/thread-pool.hh"
#include "nix/util/pool.hh"
#include "nix/util/deleter.hh"

#include <git2/attr.h>
#include <git2/blob.h>
#include <git2/branch.h>
#include <git2/commit.h>
#include <git2/common.h>
#include <git2/config.h>
#include <git2/describe.h>
#include <git2/errors.h>
#include <git2/global.h>
#include <git2/indexer.h>
#include <git2/object.h>
#include <git2/odb.h>
#include <git2/odb_backend.h>
#include <git2/refs.h>
#include <git2/remote.h>
#include <git2/repository.h>
#include <git2/revparse.h>
#include <git2/status.h>
#include <git2/submodule.h>
#include <git2/sys/odb_backend.h>
#include <git2/sys/repository.h>
#include <git2/sys/mempack.h>
#include <git2/tag.h>
#include <git2/tree.h>

#include <boost/unordered/concurrent_flat_set.hpp>
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include <iostream>
#include <regex>
#include <ranges>

namespace std {

template<>
struct hash<git_oid>
{
    size_t operator()(const git_oid & oid) const
    {
        return *(size_t *) oid.id;
    }
};

} // namespace std

std::ostream & operator<<(std::ostream & str, const git_oid & oid)
{
    str << git_oid_tostr_s(&oid);
    return str;
}

bool operator==(const git_oid & oid1, const git_oid & oid2)
{
    return git_oid_equal(&oid1, &oid2);
}

namespace nix {

struct GitSourceAccessor;

struct GitError final : public CloneableError<GitError, Error>
{
    template<typename... Ts>
    GitError(const git_error & error, Ts &&... args)
        : CloneableError("")
    {
        auto hf = HintFmt(std::forward<Ts>(args)...);
        err.msg = HintFmt("%1%: %2% (libgit2 error code = %3%)", Uncolored(hf.str()), error.message, error.klass);
    }

    template<typename... Ts>
    GitError(Ts &&... args)
        : GitError(
              []() -> const git_error & {
                  const git_error * p = git_error_last();
                  assert(p && "git_error_last() is unexpectedly null");
                  return *p;
              }(),
              std::forward<Ts>(args)...)
    {
    }
};

typedef std::unique_ptr<git_repository, Deleter<git_repository_free>> Repository;
typedef std::unique_ptr<git_tree_entry, Deleter<git_tree_entry_free>> TreeEntry;
typedef std::unique_ptr<git_tree, Deleter<git_tree_free>> Tree;
typedef std::unique_ptr<git_treebuilder, Deleter<git_treebuilder_free>> TreeBuilder;
typedef std::unique_ptr<git_blob, Deleter<git_blob_free>> Blob;
typedef std::unique_ptr<git_object, Deleter<git_object_free>> Object;
typedef std::unique_ptr<git_commit, Deleter<git_commit_free>> Commit;
typedef std::unique_ptr<git_reference, Deleter<git_reference_free>> Reference;
typedef std::unique_ptr<git_describe_result, Deleter<git_describe_result_free>> DescribeResult;
typedef std::unique_ptr<git_status_list, Deleter<git_status_list_free>> StatusList;
typedef std::unique_ptr<git_remote, Deleter<git_remote_free>> Remote;
typedef std::unique_ptr<git_config, Deleter<git_config_free>> GitConfig;
typedef std::unique_ptr<git_config_iterator, Deleter<git_config_iterator_free>> ConfigIterator;
typedef std::unique_ptr<git_odb, Deleter<git_odb_free>> ObjectDb;
typedef std::unique_ptr<git_packbuilder, Deleter<git_packbuilder_free>> PackBuilder;
typedef std::unique_ptr<git_indexer, Deleter<git_indexer_free>> Indexer;

static Hash toHash(const git_oid & oid)
{
#ifdef GIT_EXPERIMENTAL_SHA256
    assert(oid.type == GIT_OID_SHA1);
#endif
    Hash hash(HashAlgorithm::SHA1);
    memcpy(hash.hash, oid.id, hash.hashSize);
    return hash;
}

static void initLibGit2()
{
    static std::once_flag initialized;
    std::call_once(initialized, []() {
        if (git_libgit2_init() < 0)
            throw GitError("initialising libgit2");

        /* Allow opening repos with `extensions.partialClone` set —
           libgit2 strict-validates extensions and rejects unknown
           ones by default. The whitelist lets us *open* such repos;
           fetching missing objects is our `GitPromisorProvider`'s
           job (the whitelist installs no fetcher).
         *
           Note: this mutates a process-global vector inside libgit2
           (`user_extensions` in `repository.c`). If the host process
           later calls `GIT_OPT_SET_EXTENSIONS` with a different list
           it will overwrite ours. Document for embedders. */
        const char * exts[] = {"partialclone"};
        if (git_libgit2_opts(GIT_OPT_SET_EXTENSIONS, (const char **) exts, sizeof(exts) / sizeof(exts[0])))
            throw GitError("setting libgit2 partialclone extension whitelist");
    });
}

static git_oid hashToOID(const Hash & hash)
{
    git_oid oid;
    if (git_oid_fromstr(&oid, hash.gitRev().c_str()))
        throw GitError("cannot convert '%s' to a Git OID", hash.gitRev());
    return oid;
}

static Object lookupObject(git_repository * repo, const git_oid & oid, git_object_t type = GIT_OBJECT_ANY)
{
    Object obj;
    if (git_object_lookup(Setter(obj), repo, &oid, type)) {
        throw GitError("getting Git object '%s'", oid);
    }
    return obj;
}

template<typename T>
static T peelObject(git_object * obj, git_object_t type)
{
    T obj2;
    if (git_object_peel((git_object **) (typename T::pointer *) Setter(obj2), obj, type)) {
        throw Error("peeling Git object '%s'", *git_object_id(obj));
    }
    return obj2;
}

template<typename T>
static T dupObject(typename T::pointer obj)
{
    T obj2;
    if (git_object_dup((git_object **) (typename T::pointer *) Setter(obj2), (git_object *) obj))
        throw GitError("duplicating object '%s'", *git_object_id((git_object *) obj));
    return obj2;
}

/**
 * Peel the specified object (i.e. follow tag and commit objects) to
 * either a blob or a tree.
 */
static Object peelToTreeOrBlob(git_object * obj)
{
    /* git_object_peel() doesn't handle blob objects, so handle those
       specially. */
    if (git_object_type(obj) == GIT_OBJECT_BLOB)
        return dupObject<Object>(obj);
    else
        return peelObject<Object>(obj, GIT_OBJECT_TREE);
}

struct PackBuilderContext
{
    std::exception_ptr exception;

    void handleException(const char * activity, int errCode)
    {
        switch (errCode) {
        case GIT_OK:
            break;
        case GIT_EUSER:
            if (!exception)
                panic("PackBuilderContext::handleException: user error, but exception was not set");

            std::rethrow_exception(exception);
        default:
            throw Error("%s: %i, %s", Uncolored(activity), errCode, git_error_last()->message);
        }
    }
};

extern "C" {

/**
 * A `git_packbuilder_progress` implementation that aborts the pack building if needed.
 */
static int packBuilderProgressCheckInterrupt(int stage, uint32_t current, uint32_t total, void * payload)
{
    PackBuilderContext & args = *(PackBuilderContext *) payload;
    try {
        checkInterrupt();
        return GIT_OK;
    } catch (const std::exception & e) {
        args.exception = std::current_exception();
        return GIT_EUSER;
    }
};

static git_packbuilder_progress PACKBUILDER_PROGRESS_CHECK_INTERRUPT = &packBuilderProgressCheckInterrupt;

} // extern "C"

static void initRepoAtomically(std::filesystem::path & path, GitRepo::Options options)
{
    if (pathExists(path))
        return;

    if (!options.create)
        throw Error("Git repository %s does not exist.", PathFmt(path));

    std::filesystem::path tmpDir = createTempDir(path.parent_path());
    AutoDelete delTmpDir(tmpDir, true);
    Repository tmpRepo;

    if (git_repository_init(Setter(tmpRepo), tmpDir.string().c_str(), options.bare))
        throw GitError("creating Git repository %s", PathFmt(path));
    try {
        std::filesystem::rename(tmpDir, path);
    } catch (std::filesystem::filesystem_error & e) {
        // Someone may race us to create the repository.
        if (e.code() == std::errc::file_exists
            // `path` may be attempted to be deleted by s::f::rename, in which case the code is:
            || e.code() == std::errc::directory_not_empty) {
            return;
        } else
            throw SystemError(
                e.code(), "moving temporary git repository from %s to %s", PathFmt(tmpDir), PathFmt(path));
    }
    // we successfully moved the repository, so the temporary directory no longer exists.
    delTmpDir.cancel();
}

struct GitRepoImpl : GitRepo, std::enable_shared_from_this<GitRepoImpl>
{
    /** Location of the repository on disk. */
    std::filesystem::path path;

    Options options;

    /**
     * libgit2 repository. Note that new objects are not written to disk,
     * because we are using a mempack backend. For writing to disk, see
     * `flush()`, which is also called by `GitFileSystemObjectSink::sync()`.
     */
    Repository repo;

    /**
     * In-memory object store for efficient batched writing to packfiles.
     * Owned by `repo`.
     */
    git_odb_backend * mempackBackend = nullptr;

    /**
     * On-disk packfile object store.
     * Owned by `repo`.
     */
    git_odb_backend * packBackend = nullptr;

    GitRepoImpl(std::filesystem::path _path, Options _options)
        : path(std::move(_path))
        , options(_options)
    {
        initLibGit2();

        initRepoAtomically(path, options);
        if (git_repository_open(Setter(repo), path.string().c_str()))
            throw GitError("opening Git repository %s", PathFmt(path));

        ObjectDb odb;
        if (options.packfilesOnly) {
            /* Create a fresh object database because by default the repo also
               loose object backends. We are not using any of those for the
               tarball cache, but libgit2 still does a bunch of unnecessary
               syscalls that always fail with ENOENT. NOTE: We are only creating
               a libgit2 object here and not modifying the repo. Think of this as
               enabling the specific backend.
               */

            if (git_odb_new(Setter(odb)))
                throw GitError("creating Git object database");

            if (git_odb_backend_pack(&packBackend, (path / "objects").string().c_str()))
                throw GitError("creating pack backend");

            if (git_odb_add_backend(odb.get(), packBackend, 1))
                throw GitError("adding pack backend to Git object database");
        } else {
            if (git_repository_odb(Setter(odb), repo.get()))
                throw GitError("getting Git object database");
        }

        // mempack_backend will be owned by the repository, so we are not expected to free it ourselves.
        if (git_mempack_new(&mempackBackend))
            throw GitError("creating mempack backend");

        if (git_odb_add_backend(odb.get(), mempackBackend, 999))
            throw GitError("adding mempack backend to Git object database");

        if (options.packfilesOnly) {
            if (git_repository_set_odb(repo.get(), odb.get()))
                throw GitError("setting Git object database");
        }
    }

    operator git_repository *()
    {
        return repo.get();
    }

    void flush() override
    {
        checkInterrupt();

        git_buf buf = GIT_BUF_INIT;
        Finally _disposeBuf{[&] { git_buf_dispose(&buf); }};
        PackBuilder packBuilder;
        PackBuilderContext packBuilderContext;
        git_packbuilder_new(Setter(packBuilder), *this);
        git_packbuilder_set_callbacks(packBuilder.get(), PACKBUILDER_PROGRESS_CHECK_INTERRUPT, &packBuilderContext);
        git_packbuilder_set_threads(packBuilder.get(), 0 /* autodetect */);

        packBuilderContext.handleException(
            "preparing packfile", git_mempack_write_thin_pack(mempackBackend, packBuilder.get()));
        checkInterrupt();
        packBuilderContext.handleException("writing packfile", git_packbuilder_write_buf(&buf, packBuilder.get()));
        checkInterrupt();

        std::string repo_path = std::string(git_repository_path(repo.get()));
        while (!repo_path.empty() && repo_path.back() == '/')
            repo_path.pop_back();
        std::string pack_dir_path = repo_path + "/objects/pack";

        // TODO (performance): could the indexing be done in a separate thread?
        //                     we'd need a more streaming variation of
        //                     git_packbuilder_write_buf, or incur the cost of
        //                     copying parts of the buffer to a separate thread.
        //                     (synchronously on the git_packbuilder_write_buf thread)
        Indexer indexer;
        git_indexer_progress stats;
        if (git_indexer_new(Setter(indexer), pack_dir_path.c_str(), 0, nullptr, nullptr))
            throw GitError("creating git packfile indexer");

        // TODO: provide index callback for checkInterrupt() termination
        //       though this is about an order of magnitude faster than the packbuilder
        //       expect up to 1 sec latency due to uninterruptible git_indexer_append.
        constexpr size_t chunkSize = 128 * 1024;
        for (size_t offset = 0; offset < buf.size; offset += chunkSize) {
            if (git_indexer_append(indexer.get(), buf.ptr + offset, std::min(chunkSize, buf.size - offset), &stats))
                throw GitError("appending to git packfile index");
            checkInterrupt();
        }

        if (git_indexer_commit(indexer.get(), &stats))
            throw GitError("committing git packfile index");

        if (git_mempack_reset(mempackBackend))
            throw GitError("resetting git mempack backend");

        checkInterrupt();
    }

    /**
     * Return a connection pool for this repo. Useful for
     * multithreaded access.
     */
    Pool<GitRepoImpl> getPool()
    {
        // TODO: as an optimization, it would be nice to include `this` in the pool.
        return Pool<GitRepoImpl>(std::numeric_limits<size_t>::max(), [this]() -> ref<GitRepoImpl> {
            auto repo = make_ref<GitRepoImpl>(path, options);

            /* Monkey-patching the pack backend to only read the pack directory
               once. Otherwise it will do a readdir for each added oid when it's
               not found and that translates to ~6 syscalls. Since we are never
               writing pack files until flushing we can force the odb backend to
               read the directory just once. It's very convenient that the vtable is
               semi-public interface and is up for grabs.

               This is purely an optimization for our use-case with a tarball cache.
               libgit2 calls refresh() if the backend provides it when an oid isn't found.
               We are only writing objects to a mempack (it has higher priority) and there isn't
               a realistic use-case where a previously missing object would appear from thin air
               on the disk (unless another process happens to be unpacking a similar tarball to
               the cache at the same time, but that's a very unrealistic scenario).
            */
            if (auto * backend = repo->packBackend)
                backend->refresh = nullptr;

            return repo;
        });
    }

    uint64_t getRevCount(const Hash & rev) override
    {
        boost::concurrent_flat_set<git_oid, std::hash<git_oid>> done;

        auto startCommit = peelObject<Commit>(lookupObject(*this, hashToOID(rev)).get(), GIT_OBJECT_COMMIT);
        auto startOid = *git_commit_id(startCommit.get());
        done.insert(startOid);

        auto repoPool(getPool());

        ThreadPool pool;

        auto process = [&done, &pool, &repoPool](this const auto & process, const git_oid & oid) -> void {
            auto repo(repoPool.get());

            auto _commit = lookupObject(*repo, oid, GIT_OBJECT_COMMIT);
            auto commit = (const git_commit *) &*_commit;

            for (auto n : std::views::iota(0U, git_commit_parentcount(commit))) {
                auto parentOid = git_commit_parent_id(commit, n);
                if (!parentOid) {
                    throw Error(
                        "Failed to retrieve the parent of Git commit '%s': %s. "
                        "This may be due to an incomplete repository history. "
                        "To resolve this, either enable the shallow parameter in your flake URL (?shallow=1) "
                        "or add set the shallow parameter to true in builtins.fetchGit, "
                        "or fetch the complete history for this branch.",
                        *git_commit_id(commit),
                        git_error_last()->message);
                }
                if (done.insert(*parentOid))
                    pool.enqueue(std::bind(process, *parentOid));
            }
        };

        pool.enqueue(std::bind(process, startOid));

        pool.process();

        return done.size();
    }

    uint64_t getLastModified(const Hash & rev) override
    {
        auto commit = peelObject<Commit>(lookupObject(*this, hashToOID(rev)).get(), GIT_OBJECT_COMMIT);

        return git_commit_time(commit.get());
    }

    bool isShallow() override
    {
        return git_repository_is_shallow(*this);
    }

    void setRemote(const std::string & name, const std::string & url) override
    {
        if (git_remote_set_url(*this, name.c_str(), url.c_str()))
            throw GitError("setting remote '%s' URL to '%s'", name, url);
    }

    Hash resolveRef(std::string ref) override
    {
        Object object;

        // Using the rev-parse notation which libgit2 supports, make sure we peel
        // the ref ultimately down to the underlying commit.
        // This is to handle the case where it may be an annotated tag which itself has
        // an object_id.
        std::string peeledRef = ref + "^{commit}";
        if (git_revparse_single(Setter(object), *this, peeledRef.c_str()))
            throw GitError("resolving Git reference '%s'", ref);
        auto oid = git_object_id(object.get());
        return toHash(*oid);
    }

    std::vector<Submodule> parseSubmodules(const std::filesystem::path & configFile)
    {
        GitConfig config;
        if (git_config_open_ondisk(Setter(config), configFile.string().c_str()))
            throw GitError("parsing .gitmodules file");

        ConfigIterator it;
        if (git_config_iterator_glob_new(Setter(it), config.get(), "^submodule\\..*\\.(path|url|branch)$"))
            throw GitError("iterating over .gitmodules");

        StringMap entries;

        while (true) {
            git_config_entry * entry = nullptr;
            if (auto err = git_config_next(&entry, it.get())) {
                if (err == GIT_ITEROVER)
                    break;
                throw GitError("iterating over .gitmodules");
            }
            entries.emplace(entry->name + 10, entry->value);
        }

        std::vector<Submodule> result;

        for (auto & [key, value] : entries) {
            if (!hasSuffix(key, ".path"))
                continue;
            std::string key2(key, 0, key.size() - 5);
            auto path = CanonPath(value);
            result.push_back(
                Submodule{
                    .path = path,
                    .url = entries[key2 + ".url"],
                    .branch = entries[key2 + ".branch"],
                });
        }

        return result;
    }

    // Helper for statusCallback below.
    static int statusCallbackTrampoline(const char * path, unsigned int statusFlags, void * payload)
    {
        return (*((std::function<int(const char * path, unsigned int statusFlags)> *) payload))(path, statusFlags);
    }

    WorkdirInfo getWorkdirInfo() override
    {
        WorkdirInfo info;

        /* Get the head revision, if any. */
        git_oid headRev;
        if (auto err = git_reference_name_to_id(&headRev, *this, "HEAD")) {
            if (err != GIT_ENOTFOUND)
                throw GitError("resolving HEAD");
        } else
            info.headRev = toHash(headRev);

        /* Get all tracked files and determine whether the working
           directory is dirty. */
        std::function<int(const char * path, unsigned int statusFlags)> statusCallback = [&](const char * path,
                                                                                             unsigned int statusFlags) {
            if (!(statusFlags & GIT_STATUS_INDEX_DELETED) && !(statusFlags & GIT_STATUS_WT_DELETED)) {
                info.files.insert(CanonPath(path));
                if (statusFlags != GIT_STATUS_CURRENT)
                    info.dirtyFiles.insert(CanonPath(path));
            } else
                info.deletedFiles.insert(CanonPath(path));
            if (statusFlags != GIT_STATUS_CURRENT)
                info.isDirty = true;
            return 0;
        };

        git_status_options options = GIT_STATUS_OPTIONS_INIT;
        options.flags |= GIT_STATUS_OPT_INCLUDE_UNMODIFIED;
        options.flags |= GIT_STATUS_OPT_EXCLUDE_SUBMODULES;
        if (git_status_foreach_ext(*this, &options, &statusCallbackTrampoline, &statusCallback))
            throw GitError("getting working directory status");

        /* Get submodule info. */
        auto modulesFile = path / ".gitmodules";
        if (pathExists(modulesFile))
            info.submodules = parseSubmodules(modulesFile);

        return info;
    }

    std::optional<std::string> getWorkdirRef() override
    {
        Reference ref;
        if (git_reference_lookup(Setter(ref), *this, "HEAD"))
            throw Error("looking up HEAD: %s", git_error_last()->message);

        if (auto target = git_reference_symbolic_target(ref.get()))
            return target;

        return std::nullopt;
    }

    std::vector<std::tuple<Submodule, Hash>> getSubmodules(const Hash & rev, bool exportIgnore) override;

    std::string resolveSubmoduleUrl(const std::string & url) override
    {
        git_buf buf = GIT_BUF_INIT;
        if (git_submodule_resolve_url(&buf, *this, url.c_str()))
            throw Error("resolving Git submodule URL '%s'", url);
        Finally cleanup = [&]() { git_buf_dispose(&buf); };

        std::string res(buf.ptr);
        return res;
    }

    bool hasObject(const Hash & oid_) override
    {
        auto oid = hashToOID(oid_);

        Object obj;
        if (auto errCode = git_object_lookup(Setter(obj), *this, &oid, GIT_OBJECT_ANY)) {
            if (errCode == GIT_ENOTFOUND)
                return false;
            throw GitError("getting Git object '%s'", oid);
        }

        return true;
    }

    /**
     * Read this repo's git config and return the URL of the remote it
     * is a partial clone of, or `nullopt` if it is not a partial clone.
     *
     * Two conventions are recognised:
     *   1. `extensions.partialClone = <remote>` — what Nix's own
     *      `markAsPartialClone` writes (and what some tooling sets).
     *   2. `remote.<name>.promisor = true` — what stock
     *      `git clone --filter=…` records (it does NOT set
     *      `extensions.partialClone`; verified against git 2.53). We
     *      scan for any promisor remote and use the first one's URL.
     *
     * This is the trigger for constructing a `GitPromisorProvider` —
     * see `GitInputScheme::getAccessorFromCommit` in `git.cc`. We
     * do *not* probe the network here; capability detection is
     * separate (see the per-URL TTL cache in `git.cc`).
     */
    std::optional<std::string> getPartialCloneRemoteUrl() override
    {
        GitConfig config;
        if (git_repository_config(Setter(config), *this) != 0)
            return std::nullopt;

        /* Resolve `remote.<name>.url`, returning nullopt if unset/empty. */
        auto remoteUrl = [&](const std::string & name) -> std::optional<std::string> {
            std::string urlKey = "remote." + name + ".url";
            git_buf url = GIT_BUF_INIT;
            Finally urlDispose([&] { git_buf_dispose(&url); });
            if (git_config_get_string_buf(&url, config.get(), urlKey.c_str()) != 0 || url.size == 0)
                return std::nullopt;
            return std::string(url.ptr, url.size);
        };

        /* 1. `extensions.partialClone` names the remote directly. */
        {
            git_buf remoteName = GIT_BUF_INIT;
            Finally remoteNameDispose([&] { git_buf_dispose(&remoteName); });
            if (git_config_get_string_buf(&remoteName, config.get(), "extensions.partialClone") == 0
                && remoteName.size != 0) {
                auto url = remoteUrl(std::string(remoteName.ptr, remoteName.size));
                if (url)
                    debug(
                        "lazy-fetch: recognised partial clone via extensions.partialClone='%s' (remote '%s')",
                        std::string(remoteName.ptr, remoteName.size),
                        *url);
                return url;
            }
        }

        /* 2. Fall back to the stock-Git convention: any
           `remote.<name>.promisor = true`. Iterate the matching keys
           and return the first remote whose `.url` resolves. The key
           name is `remote.<name>.promisor`; extract `<name>` (the
           segment between the first and last dot). */
        ConfigIterator it;
        if (git_config_iterator_glob_new(Setter(it), config.get(), "^remote\\..*\\.promisor$") != 0)
            return std::nullopt;
        while (true) {
            git_config_entry * entry = nullptr;
            if (auto err = git_config_next(&entry, it.get())) {
                if (err == GIT_ITEROVER)
                    break;
                return std::nullopt;
            }
            /* Only `true` promisor remotes count. */
            int isPromisor = 0;
            if (git_config_parse_bool(&isPromisor, entry->value) != 0 || !isPromisor)
                continue;
            std::string_view key = entry->name; // remote.<name>.promisor
            auto firstDot = key.find('.');
            auto lastDot = key.rfind('.');
            if (firstDot == std::string_view::npos || lastDot <= firstDot)
                continue;
            auto name = std::string(key.substr(firstDot + 1, lastDot - firstDot - 1));
            if (auto url = remoteUrl(name)) {
                debug(
                    "lazy-fetch: recognised partial clone via remote.%s.promisor (no extensions.partialClone — "
                    "externally created, e.g. stock 'git clone --filter'); remote '%s'",
                    name,
                    *url);
                return url;
            }
        }
        return std::nullopt;
    }

    void markAsPartialClone(const std::string & remoteName) override
    {
        GitConfig config;
        if (git_repository_config(Setter(config), *this) != 0)
            throw GitError("opening Git config to mark repo as a partial clone");

        /* Repository format version must be 1 for `extensions.*` to be
           honoured; a fresh `git init` leaves it at 0. */
        if (git_config_set_int32(config.get(), "core.repositoryformatversion", 1))
            throw GitError("setting core.repositoryformatversion");

        /* The marker our own `getPartialCloneRemoteUrl()` keys on, and
           the one plain `git` honours to allow opening + lazy backfill.
           libgit2 strict-validates extensions, but `initLibGit2`
           whitelists `partialclone`, so the repo still opens here. */
        if (git_config_set_string(config.get(), "extensions.partialClone", remoteName.c_str()))
            throw GitError("setting extensions.partialClone");

        /* Mark the remote as a promisor with a blob-less filter — the
           two keys stock `git clone --filter=blob:none` records
           (verified against git 2.53). We ALSO set
           `extensions.partialClone` above, which stock Git does NOT, so
           our config is a superset: recognised both by our own
           `getPartialCloneRemoteUrl` (either branch) and by plain Git's
           promisor machinery. */
        auto promisorKey = "remote." + remoteName + ".promisor";
        if (git_config_set_bool(config.get(), promisorKey.c_str(), 1))
            throw GitError("setting %s", promisorKey);
        auto filterKey = "remote." + remoteName + ".partialclonefilter";
        if (git_config_set_string(config.get(), filterKey.c_str(), "blob:none"))
            throw GitError("setting %s", filterKey);

        debug(
            "lazy-fetch: marked cache repo as a partial clone of remote '%s' "
            "(repositoryformatversion=1, extensions.partialClone, blob:none filter)",
            remoteName);
    }

    /**
     * A 'GitSourceAccessor' with no regard for export-ignore.
     */
    ref<GitSourceAccessor> getRawAccessor(const Hash & rev, const GitAccessorOptions & options);

    ref<SourceAccessor>
    getAccessor(const Hash & rev, const GitAccessorOptions & options, std::string displayPrefix) override;

    ref<SourceAccessor>
    getAccessor(const WorkdirInfo & wd, const GitAccessorOptions & options, MakeNotAllowedError e) override;

    ref<GitFileSystemObjectSink> getFileSystemObjectSink() override;

    void fetch(const std::string & url, const std::string & refspec, bool shallow, bool filtered) override
    {
        Activity act(*logger, lvlTalkative, actFetchTree, fmt("fetching Git repository '%s'", url));

        // TODO: implement git-credential helper support (preferably via libgit2, which as of 2024-01 does not support
        // that)
        //       then use code that was removed in this commit (see blame)

        auto dir = this->path;

        // Remove shallow.lock left behind by a previously interrupted `git fetch`, as it would prevent `git fetch`
        // from running. Note that we already have a repository-wide `PathLock` (see git.cc), so this is safe.
        tryUnlink(dir / "shallow.lock");

        OsStrings gitArgs = {
            OS_STR("-C"),
            dir.native(),
            OS_STR("--git-dir"),
            OS_STR("."),
            OS_STR("fetch"),
            OS_STR("--progress"),
            OS_STR("--force"),
        };
        if (shallow) {
            gitArgs.push_back(OS_STR("--depth"));
            gitArgs.push_back(OS_STR("1"));
        }
        if (filtered) {
            /* Partial (blob-less) clone: pull commits + trees but no
               blobs; blobs are backfilled on demand. If the remote
               doesn't advertise the `filter` capability, Git warns
               ("filtering not recognized by server, ignoring") and
               falls back to a full fetch — never fatal. */
            gitArgs.push_back(OS_STR("--filter=blob:none"));
        }
        gitArgs.push_back(OS_STR("--"));
        gitArgs.push_back(string_to_os_string(url));
        gitArgs.push_back(string_to_os_string(refspec));

        auto status = runProgram({.program = "git", .args = gitArgs, .isInteractive = true}).first;

        if (status > 0)
            throw Error("Failed to fetch git repository '%s'", url);
    }

    void verifyCommit(const Hash & rev, const std::vector<fetchers::PublicKey> & publicKeys) override
    {
        // Map of SSH key types to their internal OpenSSH representations
        static const boost::unordered_flat_map<std::string_view, std::string_view> keyTypeMap = {
            {"ssh-dsa", "ssh-dsa"},
            {"ssh-ecdsa", "ssh-ecdsa"},
            {"ssh-ecdsa-sk", "sk-ecdsa-sha2-nistp256@openssh.com"},
            {"ssh-ed25519", "ssh-ed25519"},
            {"ssh-ed25519-sk", "sk-ssh-ed25519@openssh.com"},
            {"ssh-rsa", "ssh-rsa"}};

        // Create ad-hoc allowedSignersFile and populate it with publicKeys
        auto allowedSignersFile = createTempFile().second;
        std::string allowedSigners;

        for (const fetchers::PublicKey & k : publicKeys) {
            auto it = keyTypeMap.find(k.type);
            if (it == keyTypeMap.end()) {
                std::string supportedTypes;
                for (const auto & [type, _] : keyTypeMap) {
                    supportedTypes += fmt("  %s\n", type);
                }
                throw Error(
                    "Invalid SSH key type '%s' in publicKeys.\n"
                    "Please use one of:\n%s",
                    k.type,
                    supportedTypes);
            }

            allowedSigners += fmt("* %s %s\n", it->second, k.key);
        }
        writeFile(allowedSignersFile, allowedSigners);

        // Run verification command
        auto [status, output] = runProgram({
            .program = "git",
            .args{
                OS_STR("-c"),
                OS_STR("gpg.ssh.allowedSignersFile=") + allowedSignersFile.native(),
                OS_STR("-C"),
                path.native(),
                OS_STR("verify-commit"),
                string_to_os_string(rev.gitRev()),
            },
            .mergeStderrToStdout = true,
        });

        /* Evaluate result through status code and checking if public
           key fingerprints appear on stderr. This is necessary
           because the git command might also succeed due to the
           commit being signed by gpg keys that are present in the
           users key agent. */
        std::string re = R"(Good "git" signature for \* with .* key SHA256:[)";
        for (const fetchers::PublicKey & k : publicKeys) {
            // Calculate sha256 fingerprint from public key and escape the regex symbol '+' to match the key literally
            std::string keyDecoded;
            try {
                keyDecoded = base64::decode(k.key);
            } catch (Error & e) {
                e.addTrace({}, "while decoding public key '%s' used for git signature", k.key);
                throw;
            }
            auto fingerprint =
                trim(hashString(HashAlgorithm::SHA256, keyDecoded).to_string(nix::HashFormat::Base64, false), "=");
            auto escaped_fingerprint = std::regex_replace(fingerprint, std::regex("\\+"), "\\+");
            re += "(" + escaped_fingerprint + ")";
        }
        re += "]";
        if (status == 0 && std::regex_search(output, std::regex(re)))
            printTalkative("Signature verification on commit %s succeeded.", rev.gitRev());
        else
            throw Error("Commit signature verification on commit %s failed: %s", rev.gitRev(), output);
    }

    Hash treeHashToNarHash(const fetchers::Settings & settings, const Hash & treeHash) override
    {
        return fetchers::TreeHashToNarHash::lookup(
            settings, treeHash, [&] { return getAccessor(treeHash, {}, "")->hashPath(CanonPath::root); });
    }

    Hash dereferenceSingletonDirectory(const Hash & oid_) override
    {
        auto oid = hashToOID(oid_);

        auto _tree = lookupObject(*this, oid, GIT_OBJECT_TREE);
        auto tree = (const git_tree *) &*_tree;

        if (git_tree_entrycount(tree) == 1) {
            auto entry = git_tree_entry_byindex(tree, 0);
            auto mode = git_tree_entry_filemode(entry);
            if (mode == GIT_FILEMODE_TREE)
                oid = *git_tree_entry_id(entry);
        }

        return toHash(oid);
    }

    /* Path trie: each node is a directory with a map of children
       (string → trie node). A leaf has no children. Used to drive
       synthesiseTree's recursive walk. */
    struct PathTrie
    {
        std::map<std::string, PathTrie> children;
    };

    static PathTrie buildPathTrie(const std::set<CanonPath> & paths)
    {
        PathTrie root;
        for (auto & p : paths) {
            if (p.isRoot())
                continue; // root is implicit; nothing to add
            PathTrie * cur = &root;
            for (auto & segment : p)
                cur = &cur->children[std::string(segment)];
        }
        return root;
    }

    /* Recursively synthesise a tree containing only entries from
       `baseTree` whose paths are present in the trie. */
    git_oid synthesiseTreeRecursive(git_tree * baseTree, const PathTrie & trie)
    {
        TreeBuilder builder;
        if (git_treebuilder_new(Setter(builder), *this, nullptr))
            throw GitError("creating tree builder for synthesiseTree");

        size_t count = git_tree_entrycount(baseTree);
        for (size_t i = 0; i < count; ++i) {
            auto * entry = git_tree_entry_byindex(baseTree, i);
            std::string name(git_tree_entry_name(entry));
            auto childIt = trie.children.find(name);
            if (childIt == trie.children.end())
                continue; // not in accepted set

            auto mode = git_tree_entry_filemode(entry);
            auto entryOid = git_tree_entry_id(entry);

            if (mode == GIT_FILEMODE_TREE) {
                /* An accepted DIRECTORY. Recurse into it: the synthesised
                   subtree contains exactly the accepted descendants.

                   Crucially this is correct even when the trie child is a
                   LEAF (the directory was accepted but none of its
                   children were — e.g. a filter that accepts `/dir` but
                   rejects `/dir/*`). In that case the recursion finds no
                   matching entries and produces an EMPTY tree, matching
                   the filtered NAR walk (which renders such a directory
                   empty). The earlier code took the verbatim-splice
                   `else` branch for a trie-leaf directory, pulling in the
                   whole base subtree incl. filter-rejected files — a
                   narHash divergence from the walk (PROPOSAL.md §6.4.9).
                   Splicing verbatim is sound ONLY for non-tree entries
                   (a file/symlink/gitlink has no contents to filter). */
                auto subTreeObj = lookupObject(*this, *entryOid, GIT_OBJECT_TREE);
                auto * subTree = (git_tree *) &*subTreeObj;
                auto syntheticSub = synthesiseTreeRecursive(subTree, childIt->second);
                if (git_treebuilder_insert(nullptr, builder.get(), name.c_str(), &syntheticSub, GIT_FILEMODE_TREE))
                    throw GitError("inserting synthesised subtree '%s'", name);
            } else {
                /* Non-tree entry (regular file, symlink, or gitlink/
                   submodule). It has no contents to filter, so take it
                   verbatim — same OID, same mode. */
                if (git_treebuilder_insert(nullptr, builder.get(), name.c_str(), entryOid, mode))
                    throw GitError("inserting verbatim entry '%s'", name);
            }
        }

        git_oid syntheticOid;
        if (git_treebuilder_write(&syntheticOid, builder.get()))
            throw GitError("writing synthesised tree");
        return syntheticOid;
    }

    Hash synthesiseTreeOid(const Hash & baseTreeHash, const std::set<CanonPath> & acceptedPaths) override
    {
        auto baseObj = lookupObject(*this, hashToOID(baseTreeHash), GIT_OBJECT_TREE);
        auto * baseTree = (git_tree *) &*baseObj;
        auto trie = buildPathTrie(acceptedPaths);
        auto syntheticOid = synthesiseTreeRecursive(baseTree, trie);
        /* NO flush(): `git_treebuilder_write` already made `syntheticOid`
           readable from the mempack backend in-process, and the Track
           Z.gap5 caller only needs the OID as a `treeHashToNarHash`
           cache key — it never re-opens the object. Flushing would
           write a permanent, un-GC'd packfile for an object nobody
           retrieves. See the `synthesiseTreeOid` docstring. */
        return toHash(syntheticOid);
    }

    Hash synthesiseTree(const Hash & baseTreeHash, const std::set<CanonPath> & acceptedPaths) override
    {
        auto hash = synthesiseTreeOid(baseTreeHash, acceptedPaths);

        /* The synthesised tree was written into the mempack backend.
           Flush to disk so subsequent reads (and other GitRepo
           handles) can find it. */
        flush();

        return hash;
    }
};

ref<GitRepo> GitRepo::openRepo(const std::filesystem::path & path, GitRepo::Options options)
{
    return make_ref<GitRepoImpl>(path, options);
}

/**
 * Raw git tree input accessor.
 */

struct GitSourceAccessor : SourceAccessor
{
    struct State
    {
        ref<GitRepoImpl> repo;
        Object root;
        std::optional<lfs::Fetch> lfsFetch = std::nullopt;
        GitAccessorOptions options;
    };

    Sync<State> state_;

    /* Cache the repo's git_odb pointer so phase-2 reads don't need to
       re-acquire it. libgit2's git_odb is internally thread-safe and
       safe to share across threads. */
    git_odb * sharedOdb;

    GitSourceAccessor(ref<GitRepoImpl> repo_, const Hash & rev, const GitAccessorOptions & options)
        : state_{State{
              .repo = repo_,
              .root = peelToTreeOrBlob(lookupObject(*repo_, hashToOID(rev)).get()),
              .lfsFetch = options.smudgeLfs ? std::make_optional(lfs::Fetch(*repo_, hashToOID(rev))) : std::nullopt,
              .options = options,
          }}
    {
        /* git_repository_odb borrows a pointer; lifetime is tied to the
           repository, which we hold a ref to. */
        if (git_repository_odb(&sharedOdb, *repo_))
            throw GitError("getting Git object database");
    }

    /**
     * Phase 1 / Phase 2 split.
     *
     * Phase 1 runs under `state_`'s mutex: walk the tree to resolve
     * `path` to a `git_oid`, capture the LFS-smudge decision (which
     * needs `state->options` and `lfsFetch`).
     *
     * Phase 2 runs lock-free: `git_odb_read` against the shared
     * thread-safe `git_odb`, plus the LFS HTTPS fetch (which is
     * self-contained network IO).
     *
     * The State lock is held only for the brief tree walk; concurrent
     * readers contend only there, not on byte transfer.
     */
    void readBlob(const CanonPath & path, bool symlink, Sink & sink, std::function<void(uint64_t)> sizeCallback)
    {
        git_oid oid;
        bool wantsLfsSmudge = false;
        const lfs::Fetch * lfsFetchPtr = nullptr;
        std::shared_ptr<GitPromisorProvider> provider;

        {
            auto state(state_.lock());
            /* Resolve the OID and validate the entry kind without
               touching ODB-backed blob bytes. On a partial clone the
               blob may not be present locally; we look it up only
               after Phase 2 (potentially via the provider). */
            getBlobOid(*state, path, symlink, oid);
            if (state->lfsFetch && state->lfsFetch->shouldFetch(path)) {
                wantsLfsSmudge = true;
                /* Borrow: lfs::Fetch is owned by state and outlives
                   this call (the accessor owns state by value). */
                lfsFetchPtr = &*state->lfsFetch;
            }
            provider = state->options.provider;
        }

        /* Phase 2: lock-free read against the shared git_odb. */
        git_odb_object * raw = nullptr;
        auto err = git_odb_read(&raw, sharedOdb, &oid);
        if (err == GIT_ENOTFOUND && provider) {
            /* On-demand single-OID fetch via the partial-clone
               provider. `prefetchSubtree` already pulled the bulk;
               this catches blobs missed by that walk (e.g. fetched
               under a different `depth` than the read site). This is
               the SLOW path — one round-trip for one blob; seeing it
               fire repeatedly means a `prefetchSubtree` coalescing
               opportunity was missed, so log it at debug. */
            debug(
                "lazy-fetch: blob '%s' missing locally — single-blob on-demand backfill (slow path; "
                "bulk prefetch missed it)",
                oid);
            try {
                std::array<Hash, 1> wants{toHash(oid)};
                provider->ensureObjects(wants, FetchFilter::Blobless);
                git_odb_refresh(sharedOdb);
                err = git_odb_read(&raw, sharedOdb, &oid);
            } catch (Error & e) {
                e.addTrace({}, "while fetching missing Git blob '%s' from partial-clone remote", oid);
                throw;
            }
        }
        if (err)
            throw GitError("reading git blob '%s'", oid);
        Finally cleanup([&] { git_odb_object_free(raw); });

        auto view = std::string_view((const char *) git_odb_object_data(raw), git_odb_object_size(raw));

        if (wantsLfsSmudge) {
            StringSink s;
            try {
                lfsFetchPtr->fetch(std::string(view), path, s, [&s](uint64_t size) { s.s.reserve(size); });
            } catch (Error & e) {
                e.addTrace({}, "while smudging git-lfs file '%s'", path);
                throw;
            }
            sizeCallback(s.s.size());
            StringSource source{s.s};
            source.drainInto(sink);
        } else {
            sizeCallback(view.size());
            StringSource source{view};
            source.drainInto(sink);
        }
    }

    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
    {
        return readBlob(path, false, sink, sizeCallback);
    }

    bool pathExists(const CanonPath & path) override
    {
        auto state(state_.lock());
        return path.isRoot() ? true : (bool) lookup(*state, path);
    }

    std::optional<Stat> maybeLstat(const CanonPath & path) override
    {
        auto state(state_.lock());

        if (path.isRoot())
            return Stat{.type = git_object_type(state->root.get()) == GIT_OBJECT_TREE ? tDirectory : tRegular};

        auto entry = lookup(*state, path);
        if (!entry)
            return std::nullopt;

        auto mode = git_tree_entry_filemode(entry);

        if (mode == GIT_FILEMODE_TREE)
            return Stat{.type = tDirectory};

        else if (mode == GIT_FILEMODE_BLOB)
            return Stat{.type = tRegular};

        else if (mode == GIT_FILEMODE_BLOB_EXECUTABLE)
            return Stat{.type = tRegular, .isExecutable = true};

        else if (mode == GIT_FILEMODE_LINK)
            return Stat{.type = tSymlink};

        else if (mode == GIT_FILEMODE_COMMIT)
            // Treat submodules as an empty directory.
            return Stat{.type = tDirectory};

        else
            throw Error("file '%s' has an unsupported Git file type");
    }

    DirEntries readDirectory(const CanonPath & path) override
    {
        auto state(state_.lock());

        return std::visit(
            overloaded{
                [&](Tree tree) {
                    DirEntries res;

                    auto count = git_tree_entrycount(tree.get());

                    for (size_t n = 0; n < count; ++n) {
                        auto entry = git_tree_entry_byindex(tree.get(), n);
                        // FIXME: add to cache
                        res.emplace(std::string(git_tree_entry_name(entry)), DirEntry{});
                    }

                    return res;
                },
                [&](Submodule) { return DirEntries(); }},
            getTree(*state, path));
    }

    std::string readLink(const CanonPath & path) override
    {
        StringSink s;
        readBlob(path, true, s, [&](uint64_t size) { s.s.reserve(size); });
        return std::move(s.s);
    }

    /**
     * If `path` exists and is a submodule, return its
     * revision. Otherwise return nothing.
     */
    std::optional<Hash> getSubmoduleRev(const CanonPath & path)
    {
        auto state(state_.lock());

        auto entry = lookup(*state, path);

        if (!entry || git_tree_entry_type(entry) != GIT_OBJECT_COMMIT)
            return std::nullopt;

        return toHash(*git_tree_entry_id(entry));
    }

    /**
     * Walk the tree at `subpath` to depth `depth`, enumerate blob
     * OIDs that aren't yet in the local ODB, and ask the
     * `GitPromisorProvider` to fetch them in one coalesced request.
     *
     * No-op when no provider is configured (the common case for
     * fully-cloned repos). When configured, this is the seam that
     * makes partial clone work transparently for evaluator reads
     * triggered by `prim_readDir`/`prim_readFile` (Track K).
     *
     * Walk happens under the State lock to use the shared tree-
     * lookup cache safely; the provider call (network I/O) runs
     * outside the lock.
     */
    void prefetchSubtree(const CanonPath & subpath, unsigned depth) override
    {
        std::shared_ptr<GitPromisorProvider> provider;
        std::vector<Hash> missing;

        {
            auto state(state_.lock());
            provider = state->options.provider;
            if (!provider)
                return;
            enumerateMissingBlobs(*state, subpath, depth, missing);
        }

        if (missing.empty())
            return;

        debug(
            "lazy-fetch: prefetchSubtree at '%s' depth %d found %d missing blob(s) — coalescing into one backfill",
            subpath,
            depth,
            missing.size());

        try {
            provider->ensureObjects(missing, FetchFilter::Blobless);
            /* Refresh the ODB so subsequent reads see the new
               objects. Outside libgit2's ODB callbacks (we're not
               in one — this is `prefetchSubtree`, called from
               eval triggers). */
            git_odb_refresh(sharedOdb);
        } catch (Error & e) {
            /* Don't fail eval just because prefetch failed — fall back to
               per-blob on-demand fetching (readBlob's ENOTFOUND backfill),
               which still works but pays one round-trip PER blob. That is
               the exact pathology this coalesced prefetch exists to avoid,
               so warn (not debug): a whole-bulk-fetch failure means every
               subsequent read on this subtree is slow, and the user should
               see why (e.g. an expired credential or a dropped
               connection — the field-bug class this subsystem was bitten
               by). The per-blob slow path itself stays at debug. */
            warn(
                "lazy-fetch prefetch of %d object(s) under '%s' failed (%s); falling back to slower per-blob "
                "on-demand fetching",
                missing.size(),
                subpath.abs(),
                e.what());
        }
    }

    /* Walk tree to `depth`, collecting blob OIDs not in the local
       ODB. Depth 0 = subpath only; depth N = subpath and N levels
       of descendants. Caller holds `state_` lock. */
    void enumerateMissingBlobs(State & state, const CanonPath & subpath, unsigned depth, std::vector<Hash> & missing)
    {
        if (subpath.isRoot() && git_object_type(state.root.get()) != GIT_OBJECT_TREE)
            return;
        std::variant<Tree, Submodule> tv;
        try {
            tv = getTree(state, subpath);
        } catch (Error &) {
            return;
        }
        auto * tree = std::get_if<Tree>(&tv);
        if (!tree)
            return;
        auto count = git_tree_entrycount(tree->get());
        for (size_t i = 0; i < count; ++i) {
            auto * entry = git_tree_entry_byindex(tree->get(), i);
            auto type = git_tree_entry_type(entry);
            auto * oid = git_tree_entry_id(entry);
            if (type == GIT_OBJECT_BLOB) {
                if (!git_odb_exists(sharedOdb, oid))
                    missing.push_back(toHash(*oid));
            } else if (type == GIT_OBJECT_TREE && depth > 0) {
                enumerateMissingBlobs(state, subpath / git_tree_entry_name(entry), depth - 1, missing);
            }
        }
    }

    /**
     * Subpath-aware fingerprint. For root paths, return the
     * input-level fingerprint (master's existing behaviour). For
     * tree-rooted subpaths, return `tree:<sha><flagSuffix>`; for blob-
     * rooted subpaths, return `blob:<sha>;m=<mode><flagSuffix>`. This
     * is the change that makes `${input}/sub` cache-share across revs
     * whose subtree-SHA matches.
     *
     * The mode is required on the blob branch because Git blob OIDs do
     * not encode the executable bit or symlink-vs-regular
     * interpretation; NAR serialisation distinguishes those.
     */
    std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & path) override
    {
        if (path.isRoot() || !fingerprint)
            return {path, fingerprint};

        auto state(state_.lock());
        auto entry = lookup(*state, path);
        if (!entry)
            return {path, fingerprint}; // not found — fall back

        /* Extract the input-level flag suffix (everything from the
           first `;` on); these flags (`;e` export-ignore, `;l` LFS,
           `;s` submodules) carry through to subpath fingerprints. */
        std::string_view flagSuffix;
        if (auto semi = fingerprint->find(';'); semi != std::string::npos)
            flagSuffix = std::string_view(*fingerprint).substr(semi);

        auto type = git_tree_entry_type(entry);
        auto oid = toHash(*git_tree_entry_id(entry)).gitRev();

        if (type == GIT_OBJECT_TREE) {
            std::string fp = treeFingerprint(oid);
            fp.append(flagSuffix);
            return {CanonPath::root, std::move(fp)};
        }

        if (type == GIT_OBJECT_BLOB) {
            auto mode = git_tree_entry_filemode(entry);
            char modeBuf[8];
            std::snprintf(modeBuf, sizeof(modeBuf), "%o", (unsigned) mode);
            std::string fp = blobFingerprint(oid, modeBuf);
            fp.append(flagSuffix);
            return {CanonPath::root, std::move(fp)};
        }

        /* Submodule (GIT_OBJECT_COMMIT) and other types fall back to
           the parent-rev-keyed input fingerprint. */
        return {path, fingerprint};
    }

    /**
     * Track Z.gap1: surface the root tree OID so whole git inputs (keyed
     * in the fetcher cache on the commit rev, not the tree OID) can
     * share one NAR walk per distinct tree across revs/pipelines.
     *
     * Only fires for the whole-input/root case: returns the OID of the
     * commit's root tree (`state.root` after `peelToTreeOrBlob`). Guards:
     *   - root must be a TREE (a blob-rooted accessor has no tree OID,
     *     and the blob bridge is a separate concern);
     *   - the input-level `fingerprint` must NOT already be `tree:`-shaped
     *     — a subtree-rooted accessor's fingerprint is `tree:<sub-sha>`
     *     and already bridges via `getFingerprint`, so we return nullopt
     *     to avoid double-handling.
     */
    std::optional<Hash> getRootTreeHash() override
    {
        if (fingerprint && fingerprint->starts_with("tree:"))
            return std::nullopt; // subtree already bridges via the tree: fingerprint

        auto state(state_.lock());

        /* SOUNDNESS: `getRootTreeHash` advertises "the NAR of this
           accessor's whole tree equals the vanilla NAR of git tree
           OID T", which is what keys the shared `treeHashToNarHash`
           bridge (consumed cross-pipeline by tarballs + plain git).
           Any option that alters the bytes while leaving the tree OID
           unchanged breaks that equality and would POISON the row.

           LFS smudging is exactly such an option: `readBlob` replaces
           pointer-file bytes with the real object content, so the NAR
           differs but the tree still points at the pointer blobs (same
           OID). Returning the OID here would let a non-LFS source that
           resolves to the same tree read back the smudged narHash —
           wrong content, wrong store path. So bail to nullopt under
           LFS (the input's `;l`-suffixed fingerprint keeps its own
           cache rows distinct; only this OID bridge is unsafe).

           Export-ignore is handled structurally (it's a wrapper that
           inherits the base `nullopt`), but guard it here too as cheap
           defence-in-depth against a future inline refactor. */
        if (state->options.smudgeLfs || state->options.exportIgnore)
            return std::nullopt;

        if (git_object_type(state->root.get()) != GIT_OBJECT_TREE)
            return std::nullopt; // blob-rooted: no root tree OID
        return toHash(*git_tree_id((const git_tree *) state->root.get()));
    }

    boost::unordered_flat_map<CanonPath, TreeEntry> lookupCache;

    /* Recursively look up 'path' relative to the root. */
    git_tree_entry * lookup(State & state, const CanonPath & path)
    {
        auto i = lookupCache.find(path);
        if (i != lookupCache.end())
            return i->second.get();

        auto parent = path.parent();
        if (!parent)
            return nullptr;

        auto name = path.baseName().value();

        auto parentTree = lookupTree(state, *parent);
        if (!parentTree)
            return nullptr;

        auto count = git_tree_entrycount(parentTree->get());

        git_tree_entry * res = nullptr;

        /* Add all the tree entries to the cache to speed up
           subsequent lookups. */
        for (size_t n = 0; n < count; ++n) {
            auto entry = git_tree_entry_byindex(parentTree->get(), n);

            TreeEntry copy;
            if (git_tree_entry_dup(Setter(copy), entry))
                throw GitError("dupping tree entry");

            auto entryName = std::string_view(git_tree_entry_name(entry));

            if (entryName == name)
                res = copy.get();

            auto path2 = *parent;
            path2.push(entryName);
            lookupCache.emplace(path2, std::move(copy)).first->second.get();
        }

        return res;
    }

    std::optional<Tree> lookupTree(State & state, const CanonPath & path)
    {
        if (path.isRoot()) {
            if (git_object_type(state.root.get()) == GIT_OBJECT_TREE)
                return dupObject<Tree>((git_tree *) &*state.root);
            else
                return std::nullopt;
        }

        auto entry = lookup(state, path);
        if (!entry || git_tree_entry_type(entry) != GIT_OBJECT_TREE)
            return std::nullopt;

        Tree tree;
        if (git_tree_entry_to_object((git_object **) (git_tree **) Setter(tree), *state.repo, entry))
            throw GitError("looking up directory '%s'", showPath(path));

        return tree;
    }

    git_tree_entry * need(State & state, const CanonPath & path)
    {
        auto entry = lookup(state, path);
        if (!entry)
            throw Error("'%s' does not exist", showPath(path));
        return entry;
    }

    struct Submodule
    {};

    std::variant<Tree, Submodule> getTree(State & state, const CanonPath & path)
    {
        if (path.isRoot()) {
            if (git_object_type(state.root.get()) == GIT_OBJECT_TREE)
                return dupObject<Tree>((git_tree *) &*state.root);
            else
                throw Error("Git root object '%s' is not a directory", *git_object_id(state.root.get()));
        }

        auto entry = need(state, path);

        if (git_tree_entry_type(entry) == GIT_OBJECT_COMMIT)
            return Submodule();

        if (git_tree_entry_type(entry) != GIT_OBJECT_TREE)
            throw Error("'%s' is not a directory", showPath(path));

        Tree tree;
        if (git_tree_entry_to_object((git_object **) (git_tree **) Setter(tree), *state.repo, entry))
            throw GitError("looking up directory '%s'", showPath(path));

        return tree;
    }

    Blob getBlob(State & state, const CanonPath & path, bool expectSymlink)
    {
        if (!expectSymlink && git_object_type(state.root.get()) == GIT_OBJECT_BLOB)
            return dupObject<Blob>((git_blob *) &*state.root);

        auto notExpected = [&]() {
            throw Error(expectSymlink ? "'%s' is not a symlink" : "'%s' is not a regular file", showPath(path));
        };

        if (path.isRoot())
            notExpected();

        auto entry = need(state, path);

        if (git_tree_entry_type(entry) != GIT_OBJECT_BLOB)
            notExpected();

        auto mode = git_tree_entry_filemode(entry);
        if (expectSymlink) {
            if (mode != GIT_FILEMODE_LINK)
                notExpected();
        } else {
            if (mode != GIT_FILEMODE_BLOB && mode != GIT_FILEMODE_BLOB_EXECUTABLE)
                notExpected();
        }

        Blob blob;
        if (git_tree_entry_to_object((git_object **) (git_blob **) Setter(blob), *state.repo, entry))
            throw GitError("looking up file '%s'", showPath(path));

        return blob;
    }

    /**
     * Variant of `getBlob` that returns *only* the blob's OID (and
     * validates the mode/type), without instantiating a `git_blob`.
     * On a partial clone the blob bytes may not be in the local ODB;
     * `git_tree_entry_to_object` would fail, but we don't need that
     * — `git_tree_entry_id` reads the OID from the tree entry
     * itself (which is already locally available since we walked
     * the tree to find the entry).
     */
    void getBlobOid(State & state, const CanonPath & path, bool expectSymlink, git_oid & oidOut)
    {
        if (!expectSymlink && git_object_type(state.root.get()) == GIT_OBJECT_BLOB) {
            oidOut = *git_object_id(state.root.get());
            return;
        }

        auto notExpected = [&]() {
            throw Error(expectSymlink ? "'%s' is not a symlink" : "'%s' is not a regular file", showPath(path));
        };

        if (path.isRoot())
            notExpected();

        auto entry = need(state, path);

        if (git_tree_entry_type(entry) != GIT_OBJECT_BLOB)
            notExpected();

        auto mode = git_tree_entry_filemode(entry);
        if (expectSymlink) {
            if (mode != GIT_FILEMODE_LINK)
                notExpected();
        } else {
            if (mode != GIT_FILEMODE_BLOB && mode != GIT_FILEMODE_BLOB_EXECUTABLE)
                notExpected();
        }

        oidOut = *git_tree_entry_id(entry);
    }
};

struct GitExportIgnoreSourceAccessor : CachingFilteringSourceAccessor
{
    ref<GitRepoImpl> repo;
    std::optional<Hash> rev;

    GitExportIgnoreSourceAccessor(ref<GitRepoImpl> repo, ref<SourceAccessor> next, std::optional<Hash> rev)
        : CachingFilteringSourceAccessor(
              next,
              [&](const CanonPath & path) {
                  return RestrictedPathError(
                      fmt("'%s' does not exist because it was fetched with exportIgnore enabled", path));
              })
        , repo(repo)
        , rev(rev)
    {
    }

    bool gitAttrGet(const CanonPath & path, const char * attrName, const char *& valueOut)
    {
        const char * pathCStr = path.rel_c_str();

        if (rev) {
            git_attr_options opts = GIT_ATTR_OPTIONS_INIT;
            opts.attr_commit_id = hashToOID(*rev);
            // TODO: test that gitattributes from global and system are not used
            //       (ie more or less: home and etc - both of them!)
            opts.flags = GIT_ATTR_CHECK_INCLUDE_COMMIT | GIT_ATTR_CHECK_NO_SYSTEM;
            return git_attr_get_ext(&valueOut, *repo, &opts, pathCStr, attrName);
        } else {
            return git_attr_get(
                &valueOut, *repo, GIT_ATTR_CHECK_INDEX_ONLY | GIT_ATTR_CHECK_NO_SYSTEM, pathCStr, attrName);
        }
    }

    bool isExportIgnored(const CanonPath & path)
    {
        const char * exportIgnoreEntry = nullptr;

        // GIT_ATTR_CHECK_INDEX_ONLY:
        // > It will use index only for creating archives or for a bare repo
        // > (if an index has been specified for the bare repo).
        // -- https://github.com/libgit2/libgit2/blob/HEAD/include/git2/attr.h#L113C62-L115C48
        if (gitAttrGet(path, "export-ignore", exportIgnoreEntry)) {
            if (git_error_last()->klass == GIT_ENOTFOUND)
                return false;
            else
                throw GitError("looking up '%s'", showPath(path));
        } else {
            // Official git will silently reject export-ignore lines that have
            // values. We do the same.
            return GIT_ATTR_IS_TRUE(exportIgnoreEntry);
        }
    }

    bool isAllowedUncached(const CanonPath & path) override
    {
        return !isExportIgnored(path);
    }
};

GitRepo * getGitRepoOf(SourceAccessor & accessor)
{
    /* The raw tree accessor: holds its `ref<GitRepoImpl>` inside the
       Sync<State> (under a mutex). Locking just to read the ref is
       cheap and the ref outlives the lock. */
    if (auto * git = dynamic_cast<GitSourceAccessor *>(&accessor))
        return &*git->state_.lock()->repo;

    /* The export-ignore wrapper: a FilteringSourceAccessor over a
       GitSourceAccessor that ALSO holds the `ref<GitRepoImpl>` directly
       (`GitRepoImpl::getAccessor` constructs it as
       `GitExportIgnoreSourceAccessor(self, rawGitAccessor, rev)`). So we
       recover the repo straight off the wrapper — no need to unwrap its
       inner `next` accessor. */
    if (auto * exp = dynamic_cast<GitExportIgnoreSourceAccessor *>(&accessor))
        return &*exp->repo;

    return nullptr;
}

struct GitFileSystemObjectSinkImpl : GitFileSystemObjectSink
{
    ref<GitRepoImpl> repo;

    Pool<GitRepoImpl> repoPool;

    unsigned int concurrency = std::min(std::thread::hardware_concurrency(), 10U);

    ThreadPool workers{concurrency};

    /** Total file contents in flight. */
    std::atomic<size_t> totalBufSize{0};

    static constexpr std::size_t maxBufSize = 16 * 1024 * 1024;

    GitFileSystemObjectSinkImpl(ref<GitRepoImpl> repo)
        : repo(repo)
        , repoPool(repo->getPool())
    {
    }

    ~GitFileSystemObjectSinkImpl()
    {
        // Make sure the worker threads are destroyed before any state
        // they're referring to.
        workers.shutdown();
    }

    struct Child;

    /// A directory to be written as a Git tree.
    struct Directory
    {
        std::map<std::string, Child> children;
        std::optional<git_oid> oid;

        Child & lookup(const CanonPath & path)
        {
            assert(!path.isRoot());
            auto parent = path.parent();
            auto cur = this;
            for (auto & name : *parent) {
                auto i = cur->children.find(std::string(name));
                if (i == cur->children.end())
                    throw Error("path '%s' does not exist", path);
                auto dir = std::get_if<Directory>(&i->second.file);
                if (!dir)
                    throw Error("path '%s' has a non-directory parent", path);
                cur = dir;
            }

            auto i = cur->children.find(std::string(*path.baseName()));
            if (i == cur->children.end())
                throw Error("path '%s' does not exist", path);
            return i->second;
        }
    };

    size_t nextId = 0; // for Child.id

    struct Child
    {
        git_filemode_t mode;
        std::variant<Directory, git_oid> file;

        /// Sequential numbering of the file in the tarball. This is
        /// used to make sure we only import the latest version of a
        /// path.
        size_t id{0};
    };

    struct State
    {
        Directory root;
    };

    Sync<State> _state;

    void addNode(State & state, const CanonPath & path, Child && child)
    {
        assert(!path.isRoot());
        auto parent = path.parent();

        Directory * cur = &state.root;

        for (auto & i : *parent) {
            auto child = std::get_if<Directory>(
                &cur->children.emplace(std::string(i), Child{GIT_FILEMODE_TREE, {Directory()}}).first->second.file);
            assert(child);
            cur = child;
        }

        std::string name(*path.baseName());

        if (auto prev = cur->children.find(name); prev == cur->children.end() || prev->second.id < child.id)
            cur->children.insert_or_assign(name, std::move(child));
    }

    void createRegularFile(const CanonPath & path, fun<void(CreateRegularFileSink &)> func) override
    {
        checkInterrupt();

        /* Multithreaded blob writing. We read the incoming file data into memory and asynchronously write it to a Git
           blob object. However, to avoid unbounded memory usage, if the amount of data in flight exceeds a threshold,
           we switch to writing directly to a Git write stream. */

        using WriteStream = std::unique_ptr<::git_writestream, decltype([](::git_writestream * stream) {
                                                if (stream)
                                                    stream->free(stream);
                                            })>;

        struct CRF : CreateRegularFileSink
        {
            CanonPath path;
            GitFileSystemObjectSinkImpl & parent;
            WriteStream stream;
            std::optional<decltype(parent.repoPool)::Handle> repo;

            std::string contents;
            bool executable = false;

            CRF(CanonPath path, GitFileSystemObjectSinkImpl & parent)
                : path(std::move(path))
                , parent(parent)
            {
            }

            ~CRF()
            {
                parent.totalBufSize -= contents.size();
            }

            void operator()(std::string_view data) override
            {
                if (!stream) {
                    contents.append(data);
                    parent.totalBufSize += data.size();

                    if (parent.totalBufSize > parent.maxBufSize) {
                        repo.emplace(parent.repoPool.get());

                        if (git_blob_create_from_stream(Setter(stream), **repo, nullptr))
                            throw GitError("creating a blob stream object");

                        if (stream->write(stream.get(), contents.data(), contents.size()))
                            throw GitError("writing a blob for tarball member '%s'", path);

                        parent.totalBufSize -= contents.size();
                        contents.clear();
                    }
                } else {
                    if (stream->write(stream.get(), data.data(), data.size()))
                        throw GitError("writing a blob for tarball member '%s'", path);
                }
            }

            void isExecutable() override
            {
                executable = true;
            }
        };

        auto crf = std::make_shared<CRF>(path, *this);

        func(*crf);

        auto id = nextId++;

        if (crf->stream) {
            /* Finish the slow path by creating the blob object synchronously.
               Call .release(), since git_blob_create_from_stream_commit
               acquires ownership and frees the stream. */
            git_oid oid;
            if (git_blob_create_from_stream_commit(&oid, crf->stream.release()))
                throw GitError("creating a blob object for '%s'", path);
            addNode(
                *_state.lock(),
                crf->path,
                Child{crf->executable ? GIT_FILEMODE_BLOB_EXECUTABLE : GIT_FILEMODE_BLOB, oid, id});
            return;
        }

        /* Fast path: create the blob object in a separate thread. */
        workers.enqueue([this, crf{std::move(crf)}, id]() {
            auto repo(repoPool.get());

            git_oid oid;
            if (git_blob_create_from_buffer(&oid, *repo, crf->contents.data(), crf->contents.size()))
                throw GitError("creating a blob object for '%s' from in-memory buffer", crf->path);

            addNode(
                *_state.lock(),
                crf->path,
                Child{crf->executable ? GIT_FILEMODE_BLOB_EXECUTABLE : GIT_FILEMODE_BLOB, oid, id});
        });
    }

    void createDirectory(const CanonPath & path) override
    {
        if (path.isRoot())
            return;
        auto state(_state.lock());
        addNode(*state, path, {GIT_FILEMODE_TREE, Directory()});
    }

    void createSymlink(const CanonPath & path, const std::string & target) override
    {
        workers.enqueue([this, path, target]() {
            auto repo(repoPool.get());

            git_oid oid;
            if (git_blob_create_from_buffer(&oid, *repo, target.c_str(), target.size()))
                throw GitError("creating a blob object for tarball symlink member '%s'", path);

            auto state(_state.lock());
            addNode(*state, path, Child{GIT_FILEMODE_LINK, oid});
        });
    }

    std::map<CanonPath, CanonPath> hardLinks;

    void createHardlink(const CanonPath & path, const CanonPath & target) override
    {
        hardLinks.insert_or_assign(path, target);
    }

    Hash flush() override
    {
        workers.process();

        /* Create hard links. */
        {
            auto state(_state.lock());
            for (auto & [path, target] : hardLinks) {
                if (target.isRoot())
                    continue;
                try {
                    auto child = state->root.lookup(target);
                    auto oid = std::get_if<git_oid>(&child.file);
                    if (!oid)
                        throw Error("cannot create a hard link to a directory");
                    addNode(*state, path, {child.mode, *oid});
                } catch (Error & e) {
                    e.addTrace(nullptr, "while creating a hard link from '%s' to '%s'", path, target);
                    throw;
                }
            }
        }

        // Flush all repo objects to disk.
        {
            auto repos = repoPool.clear();
            ThreadPool workers{repos.size()};
            for (auto & repo : repos)
                workers.enqueue([repo]() { repo->flush(); });
            workers.process();
        }

        // Write the Git trees to disk. Would be nice to have this multithreaded too, but that's hard because a tree
        // can't refer to an object that hasn't been written yet. Also it doesn't make a big difference for performance.
        auto repo(repoPool.get());

        [&](this const auto & visit, Directory & node) -> void {
            checkInterrupt();

            // Write the child directories.
            for (auto & child : node.children)
                if (auto dir = std::get_if<Directory>(&child.second.file))
                    visit(*dir);

            // Write this directory.
            git_treebuilder * b;
            if (git_treebuilder_new(&b, *repo, nullptr))
                throw GitError("creating a tree builder");
            TreeBuilder builder(b);

            for (auto & [name, child] : node.children) {
                auto oid_p = std::get_if<git_oid>(&child.file);
                auto oid = oid_p ? *oid_p : std::get<Directory>(child.file).oid.value();
                if (git_treebuilder_insert(nullptr, builder.get(), name.c_str(), &oid, child.mode))
                    throw GitError("adding a file to a tree builder");
            }

            git_oid oid;
            if (git_treebuilder_write(&oid, builder.get()))
                throw GitError("creating a tree object");
            node.oid = oid;
        }(_state.lock()->root);

        repo->flush();

        return toHash(_state.lock()->root.oid.value());
    }
};

ref<GitSourceAccessor> GitRepoImpl::getRawAccessor(const Hash & rev, const GitAccessorOptions & options)
{
    auto self = ref<GitRepoImpl>(shared_from_this());
    return make_ref<GitSourceAccessor>(self, rev, options);
}

ref<SourceAccessor>
GitRepoImpl::getAccessor(const Hash & rev, const GitAccessorOptions & options, std::string displayPrefix)
{
    auto self = ref<GitRepoImpl>(shared_from_this());
    ref<GitSourceAccessor> rawGitAccessor = getRawAccessor(rev, options);
    rawGitAccessor->setPathDisplay(std::move(displayPrefix));
    if (options.exportIgnore)
        return make_ref<GitExportIgnoreSourceAccessor>(self, rawGitAccessor, rev);
    else
        return rawGitAccessor;
}

ref<SourceAccessor> GitRepoImpl::getAccessor(
    const WorkdirInfo & wd, const GitAccessorOptions & options, MakeNotAllowedError makeNotAllowedError)
{
    auto self = ref<GitRepoImpl>(shared_from_this());
    ref<SourceAccessor> fileAccessor = AllowListSourceAccessor::create(
                                           makeFSSourceAccessor(path),
                                           /*allowedPrefixes=*/wd.files,
                                           // Always allow access to the root, but not its children.
                                           /*allowedPaths=*/{CanonPath::root},
                                           std::move(makeNotAllowedError))
                                           .cast<SourceAccessor>();
    if (options.exportIgnore)
        fileAccessor = make_ref<GitExportIgnoreSourceAccessor>(self, fileAccessor, std::nullopt);
    return fileAccessor;
}

ref<GitFileSystemObjectSink> GitRepoImpl::getFileSystemObjectSink()
{
    return make_ref<GitFileSystemObjectSinkImpl>(ref<GitRepoImpl>(shared_from_this()));
}

std::vector<std::tuple<GitRepoImpl::Submodule, Hash>> GitRepoImpl::getSubmodules(const Hash & rev, bool exportIgnore)
{
    /* Read the .gitmodules files from this revision. */
    CanonPath modulesFile(".gitmodules");

    auto accessor = getAccessor(rev, {.exportIgnore = exportIgnore}, "");
    if (!accessor->pathExists(modulesFile))
        return {};

    /* Parse it and get the revision of each submodule. */
    auto configS = accessor->readFile(modulesFile);

    auto [fdTemp, pathTemp] = createTempFile("nix-git-submodules");
    try {
        writeFull(fdTemp.get(), configS);
    } catch (SystemError & e) {
        e.addTrace({}, "while writing .gitmodules file to temporary file");
        throw;
    }

    std::vector<std::tuple<Submodule, Hash>> result;

    auto rawAccessor = getRawAccessor(rev, {});

    for (auto & submodule : parseSubmodules(pathTemp)) {
        /* Filter out .gitmodules entries that don't exist or are not
           submodules. */
        if (auto rev = rawAccessor->getSubmoduleRev(submodule.path))
            result.push_back({std::move(submodule), *rev});
    }

    return result;
}

namespace fetchers {

ref<GitRepo> Settings::getTarballCache() const
{
    /* v1: Had either only loose objects or thin packfiles referring to loose objects
     * v2: Must have only packfiles with no loose objects. Should get repacked periodically
     * for optimal packfiles.
     */
    static auto repoDir = std::filesystem::path(getCacheDir()) / "tarball-cache-v2";
    return GitRepo::openRepo(repoDir, {.create = true, .bare = true, .packfilesOnly = true});
}

} // namespace fetchers

static Sync<std::map<std::filesystem::path, GitRepo::WorkdirInfo>> workdirInfoCache_;

GitRepo::WorkdirInfo GitRepo::getCachedWorkdirInfo(const std::filesystem::path & path)
{
    auto & _cache = workdirInfoCache_;
    {
        auto cache(_cache.lock());
        auto i = cache->find(path);
        if (i != cache->end())
            return i->second;
    }
    auto workdirInfo = GitRepo::openRepo(path, {})->getWorkdirInfo();
    _cache.lock()->emplace(path, workdirInfo);
    return workdirInfo;
}

void GitRepo::invalidateWorkdirInfoCache()
{
    workdirInfoCache_.lock()->clear();
}

bool isLegalRefName(const std::string & refName)
{
    initLibGit2();

    /* Check for cases that don't get rejected by libgit2.
     * FIXME: libgit2 should reject this. */
    if (refName == "@")
        return false;

    /* libgit2 doesn't barf on DEL symbol.
     * FIXME: libgit2 should reject this. */
    if (refName.find('\177') != refName.npos)
        return false;

    for (auto * func : {
             git_reference_name_is_valid,
             git_branch_name_is_valid,
             git_tag_name_is_valid,
         }) {
        int valid = 0;
        if (func(&valid, refName.c_str()))
            throw Error("checking git reference '%s': %s", refName, git_error_last()->message);
        if (valid)
            return true;
    }

    return false;
}

} // namespace nix
