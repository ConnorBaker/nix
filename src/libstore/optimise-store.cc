#include "nix/store/local-store.hh"
#include "nix/store/local-settings.hh"
#include "nix/store/pathlocks.hh"
#include "nix/util/signals.hh"
#include "nix/store/posix-fs-canonicalise.hh"
#include "nix/util/posix-source-accessor.hh"
#include "nix/util/file-system.hh"

#include <oneapi/tbb/parallel_for_each.h>
#include <oneapi/tbb/task_arena.h>

#include <boost/scope/scope_exit.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <optional>
#ifdef __APPLE__
#  include <regex>
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#include "store-config-private.hh"

namespace nix {

static void makeWritable(const std::filesystem::path & path)
{
    auto st = lstat(path);
    chmod(path, st.st_mode | S_IWUSR);
}

struct MakeReadOnly
{
    std::filesystem::path path;

    MakeReadOnly(std::filesystem::path path)
        : path(std::move(path))
    {
    }

    ~MakeReadOnly()
    {
        try {
            /* This will make the path read-only. */
            if (!path.empty())
                canonicaliseTimestampAndPermissions(path.string());
        } catch (...) {
            ignoreExceptionInDestructor();
        }
    }
};

/* Lazy per-directory writability guard. Threaded through
   `optimisePath_`'s recursion so that all sibling-file replacements
   in one directory share a single chmod(+w) + canonicalise(-w) pair,
   rather than toggling permissions per file.

   Why this matters: a directory with K files that all need replacing
   used to incur 2K chmods (one +w, one -w per file) plus 2K journal
   commits on ext4. On a store with 4M paths containing an average
   of ~20 files each, that's tens of millions of redundant metadata
   writes. Batching at the directory level reduces this to 2 chmods
   per directory — a ~20× reduction on typical stores.

   The guard is safe under parallel `optimiseStore` because we
   parallelise at top-level store path granularity: sibling files in
   the same subdirectory always land on the same worker thread, so
   the ensure/restore sequence is single-threaded per directory.

   Usage:
     LocalStore::DirWritability w(dirPath, storeRoot);
     // recurse into children — they may call w.ensureWritable() before
     // modifying their entries in `dirPath`
     // dtor canonicalises dir permissions back to read-only if we
     // actually toggled.
 */
LocalStore::DirWritability::DirWritability(
    std::filesystem::path d, const std::filesystem::path & storeRoot)
    : dir(std::move(d))
    , isStoreRoot(dir == storeRoot)
{
}

void LocalStore::DirWritability::ensureWritable()
{
    if (isStoreRoot || needsCanonicalise)
        return;
    makeWritable(dir);
    needsCanonicalise = true;
}

LocalStore::DirWritability::~DirWritability()
{
    if (!needsCanonicalise)
        return;
    try {
        canonicaliseTimestampAndPermissions(dir.string());
    } catch (...) {
        ignoreExceptionInDestructor();
    }
}

void LocalStore::loadInodeHashInto(InodeHash & inodeHash)
{
    debug("loading hash inodes in memory");

    /* Uses `readdir` directly so we get `dirent::d_ino` for free —
       we only want inodes, not full `stat()` results. */
    AutoCloseDir dir(opendir(linksDir.string().c_str()));
    if (!dir)
        throw SysError("opening directory %1%", PathFmt(linksDir));
    struct dirent * dirent;
    while (errno = 0, dirent = readdir(dir.get())) { /* sic */
        checkInterrupt();
        const char * name = dirent->d_name;
        if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0)))
            continue;
        inodeHash.insert(dirent->d_ino);
    }
    if (errno)
        throw SysError("reading directory %1%", PathFmt(linksDir));

    printMsg(lvlTalkative, "loaded %1% hash inodes", inodeHash.size());
}

std::vector<std::string>
LocalStore::readDirectoryIgnoringInodes(const std::filesystem::path & path, const InodeHash & inodeHash)
{
    std::vector<std::string> names;
    names.reserve(64);

    AutoCloseDir dir(opendir(path.string().c_str()));
    if (!dir)
        throw SysError("opening directory %s", PathFmt(path));

    struct dirent * dirent;
    while (errno = 0, dirent = readdir(dir.get())) { /* sic */
        checkInterrupt();

        if (inodeHash.contains(dirent->d_ino)) {
            debug("'%1%' is already linked", dirent->d_name);
            continue;
        }

        std::string name = dirent->d_name;
        if (name == "." || name == "..")
            continue;
        names.push_back(std::move(name));
    }
    if (errno)
        throw SysError("reading directory %s", PathFmt(path));

    return names;
}

/* Optimise a single store path.
 *
 * Per-file operation:
 *
 *     hash(file) -> H
 *     candidate = linksDir / H
 *     if candidate doesn't exist:
 *         link(file, candidate); done
 *     if inode(file) == inode(candidate):
 *         already deduped; done
 *     replace `file` with hardlink to candidate
 *         (tempLink + rename — POSIX atomic replace)
 *     EMLINK on link(2)/rename(2): give up on dedup for this file.
 *
 * Race tolerance for concurrency with parallel workers and GC:
 *   - ENOENT on source: source GC'd; benign skip.
 *   - ENOENT on candidate: GC'd between our lstat and link; recreate.
 *   - EEXIST on link(2): a concurrent optimiser created the canonical;
 *     re-inspect and dedupe against it.
 *   - EMLINK: filesystem hardlink ceiling reached; leave undeduped.
 *   - ENOSPC on `.links/`: directory index full; abandon this file.
 *
 * Concurrency with another `optimiseStore` invocation is serialised
 * by a per-store advisory lock in `optimiseStore`. Per-file
 * parallelism within one invocation is unlocked. */
void LocalStore::optimisePath_(
    Activity * act,
    OptimiseStats & stats,
    const std::filesystem::path & path,
    InodeHash & inodeHash,
    RepairFlag repair,
    DirWritability * parentDirWritability)
{
    checkInterrupt();

    /* If the path is gone (e.g. a concurrent GC deleted it between
       `queryAllValidPaths` and us reaching it), silently skip. This
       makes `optimisePath_` safe to call on paths that may race with
       a concurrent GC without needing an outer catch. */
    auto stOpt = maybeLstat(path);
    if (!stOpt)
        return;
    auto & st = *stOpt;

#ifdef __APPLE__
    /* HFS/macOS has some undocumented security feature disabling hardlinking for
       special files within .app dirs. Known affected paths include
       *.app/Contents/{PkgInfo,Resources/\*.lproj,_CodeSignature} and .DS_Store.
       See https://github.com/NixOS/nix/issues/1443 and
       https://github.com/NixOS/nix/pull/2230 for more discussion. */

    if (std::regex_search(path.string(), std::regex("\\.app/Contents/.+$"))) {
        debug("%s is not allowed to be linked in macOS", PathFmt(path));
        return;
    }
#endif

    if (S_ISDIR(st.st_mode)) {
        auto names = readDirectoryIgnoringInodes(path, inodeHash);
        /* Single writability guard shared across this directory's
           children: all per-child tempLink+rename operations toggle
           `path` writable at most once. The dtor canonicalises the
           directory back to its store-canonical read-only state. */
        DirWritability dirW(path, config->realStoreDir.get());
        for (auto & i : names)
            optimisePath_(act, stats, path / i, inodeHash, repair, &dirW);
        return;
    }

    /* We can hard link regular files and maybe symlinks. */
    if (!S_ISREG(st.st_mode)
#if CAN_LINK_SYMLINK
        && !S_ISLNK(st.st_mode)
#endif
    )
        return;

    /* Sometimes SNAFUs can cause files in the Nix store to be
       modified, in particular when running programs as root under
       NixOS (example: $fontconfig/var/cache being modified).  Skip
       those files.  FIXME: check the modification time. */
    if (S_ISREG(st.st_mode) && (st.st_mode & S_IWUSR)) {
        warn("skipping suspicious writable file '%s'", PathFmt(path));
        return;
    }

    /* This can still happen on top-level files. */
    if (st.st_nlink > 1 && inodeHash.contains(st.st_ino)) {
        debug("%s is already linked, with %d other file(s)", PathFmt(path), st.st_nlink - 2);
        return;
    }

    /* Hash the file.  Note that hashPath() returns the hash over the
       NAR serialisation, which includes the execute bit on the file.
       Thus, executable and non-executable files with the same
       contents *won't* be linked (which is good because otherwise the
       permissions would be screwed up).

       Also note that if `path' is a symlink, then we're hashing the
       contents of the symlink (i.e. the result of readlink()), not
       the contents of the target (which may not even exist). */
    Hash hash = hashPath(makeFSSourceAccessor(path), FileSerialisationMethod::NixArchive, HashAlgorithm::SHA256).hash;
    debug("%s has hash '%s'", PathFmt(path), hash.to_string(HashFormat::Nix32, true));

    auto candidate = linksDir / hash.to_string(HashFormat::Nix32, false);

    /* Corruption recovery: size/hash mismatch means a broken entry.
       Remove it and retry from scratch. */
    auto stCanon = maybeLstat(candidate);
    if (stCanon) {
        bool corrupted = st.st_size != stCanon->st_size;
        if (!corrupted && repair) {
            auto linkHash = hashPath(
                                makeFSSourceAccessor(candidate),
                                FileSerialisationMethod::NixArchive,
                                HashAlgorithm::SHA256)
                                .hash;
            corrupted = hash != linkHash;
        }
        if (corrupted) {
            warn("removing corrupted link %s", PathFmt(candidate));
            warn(
                "There may be more corrupted paths."
                "\nYou should run `nix-store --verify --check-contents --repair` to fix them all");
            try {
                nix::unlink(candidate);
            } catch (SysError & e) {
                if (e.errNo != ENOENT)
                    throw;
            }
            stCanon.reset();
        }
    }

    /* Create the canonical entry if it doesn't exist. */
    if (!stCanon) {
        std::error_code ec;
        std::filesystem::create_hard_link(path, candidate, ec);
        if (!ec) {
            inodeHash.insert(st.st_ino);
            return;
        }
        if (ec == std::errc::no_such_file_or_directory) {
            /* Source GC'd between our lstat and the link. */
            if (!maybeLstat(path))
                return;
            throw SysError(
                "creating hard link from %1% to %2%: destination parent missing",
                PathFmt(candidate),
                PathFmt(path));
        }
        if (ec == std::errc::too_many_links) {
            if (st.st_size)
                printInfo("%1% has maximum number of links", PathFmt(candidate));
            return;
        }
        if (ec == std::errc::no_space_on_device) {
            /* On ext4, `.links/` htree exhaustion. Disable dedup for
               this file. */
            printInfo("cannot link %s to %s: %s", PathFmt(candidate), PathFmt(path), ec.message());
            return;
        }
        if (ec != std::errc::file_exists)
            throw SystemError(ec, "creating hard link from %1% to %2%", PathFmt(candidate), PathFmt(path));
        /* file_exists: a concurrent optimiser created the entry. Fall
           through and re-inspect. */
        stCanon = maybeLstat(candidate);
        if (!stCanon)
            return;
    }

    if (st.st_ino == stCanon->st_ino) {
        debug("%1% is already linked to %2%", PathFmt(path), PathFmt(candidate));
        return;
    }

    /* Replace `path` with a hardlink to `candidate` via tempLink +
       rename(2). EMLINK at either step is "too many links to the
       canonical inode" — give up on dedup for this file. */
    const auto dirOfPath = path.parent_path();
    std::optional<MakeReadOnly> perCallReadOnly;
    if (parentDirWritability) {
        parentDirWritability->ensureWritable();
    } else {
        bool mustToggle = dirOfPath != config->realStoreDir.get();
        if (mustToggle)
            makeWritable(dirOfPath);
        perCallReadOnly.emplace(mustToggle ? dirOfPath : std::filesystem::path{});
    }

    auto tempLink = makeTempPath(config->realStoreDir.get(), ".tmp-link");

    /* Record our source inode in `inodeHash` before linking so sibling
       files sharing this inode can short-circuit the hash step. */
    inodeHash.insert(st.st_ino);

    /* Cleanup guard: removes the tempLink if any exception escapes
       between `create_hard_link` succeeding and the rename completing.
       Created inactive — armed after the link succeeds, released
       after the rename succeeds. */
    auto tempLinkGuard = boost::scope::make_scope_exit([&] {
        try {
            std::error_code ec;
            std::filesystem::remove(tempLink, ec);
            if (ec && ec != std::errc::no_such_file_or_directory)
                printError("unable to unlink %1%: %2%", PathFmt(tempLink), ec.message());
        } catch (...) {
            ignoreExceptionInDestructor();
        }
    });
    tempLinkGuard.set_active(false);

    try {
        std::filesystem::create_hard_link(candidate, tempLink);
        tempLinkGuard.set_active(true);
    } catch (std::filesystem::filesystem_error & e) {
        if (e.code() == std::errc::too_many_links) {
            if (st.st_size)
                printInfo("%1% has maximum number of links", PathFmt(candidate));
            return;
        }
        throw SystemError(
            e.code(), "creating hard link from %1% to %2%", PathFmt(candidate), PathFmt(tempLink));
    }

    try {
        std::filesystem::rename(tempLink, path);
        tempLinkGuard.set_active(false);
    } catch (std::filesystem::filesystem_error & e) {
        if (e.code() == std::errc::too_many_links) {
            /* Some FSes raise EMLINK on rename rather than link, by
               briefly bumping nlink during the operation. */
            debug("%s has reached maximum number of links", PathFmt(candidate));
            return;
        }
        if (e.code() == std::errc::no_such_file_or_directory) {
            /* Source GC'd while we were working. */
            if (!maybeLstat(path))
                return;
            throw SystemError(
                e.code(),
                "renaming %1% to %2%: tempLink disappeared before rename",
                PathFmt(tempLink),
                PathFmt(path));
        }
        throw SystemError(e.code(), "renaming %1% to %2%", PathFmt(tempLink), PathFmt(path));
    }

    stats.filesLinked.fetch_add(1, std::memory_order_relaxed);
    stats.bytesFreed.fetch_add(st.st_size, std::memory_order_relaxed);

    if (act)
        act->result(
            resFileLinked,
            st.st_size
#ifndef _WIN32
            ,
            st.st_blocks
#endif
        );
}

void LocalStore::optimiseStore(OptimiseStats & stats)
{
    Activity act(*logger, actOptimiseStore);

    /* Serialise whole-store optimise with a process-level advisory
       lock. Otherwise two concurrent optimise runs will ping-pong
       each directory's writable bit via `DirWritability`: each
       invocation's guard dtor canonicalises the directory back to
       read-only, which can race an in-flight `rename(2)` in the
       other invocation and surface as EACCES.

       Per-file parallelism inside a single run is still unlocked —
       only whole-store invocations mutually exclude each other. */
    auto fnOptLock = config->stateDir.get() / "optimise.lock";
    AutoCloseFD fdOptLock = openLockFile(fnOptLock, true);
    if (!lockFile(fdOptLock.get(), ltWrite, false)) {
        printInfo("waiting for exclusive access to optimise the Nix store...");
        lockFile(fdOptLock.get(), ltWrite, true);
    }

    /* `std::set` has no random access, so materialise into a vector
       for index-based partitioning across workers. */
    auto pathSet = queryAllValidPaths();
    std::vector<StorePath> paths{pathSet.begin(), pathSet.end()};

    InodeHash inodeHash;
    loadInodeHashInto(inodeHash);

    const uint64_t total = paths.size();
    act.progress(0, total);

    /* Cache-line-aligned to avoid false sharing with surrounding
       locals under high worker counts. */
    alignas(std::hardware_destructive_interference_size) std::atomic<uint64_t> done{0};

    auto nThreads = Settings::resolveThreadCount(config->getLocalSettings().optimiseThreads.get());

    /* TBB's `auto_partitioner` adapts chunk size to per-item work
       cost, so we don't tune `maxChunkSize` here. The `task_arena`
       caps concurrency at `nThreads` without touching any global
       scheduler state — concurrent operations (e.g. a parallel GC
       running alongside) get their own arenas and share TBB's
       global thread pool cooperatively.

       We skip `isValidPath(path)` here and rely on `optimisePath_`'s
       built-in ENOENT tolerance: if GC deleted the path between
       `queryAllValidPaths` and our `maybeLstat`, the inner function
       returns silently. This saves up to N SQLite point-lookups per
       `optimiseStore` run on cold caches. */
    tbb::task_arena arena(static_cast<int>(nThreads));
    arena.execute([&] {
        tbb::parallel_for_each(paths, [&](const StorePath & path) {
            checkInterrupt();
            addTempRoot(path);
            optimisePath_(
                nullptr, stats, config->realStoreDir.get() / path.to_string(), inodeHash, NoRepair);
            auto d = done.fetch_add(1, std::memory_order_relaxed) + 1;
            if ((d & 1023) == 0 || d == total)
                act.progress(d, total);
        });
    });
}

void LocalStore::optimiseStore()
{
    OptimiseStats stats;

    optimiseStore(stats);

    printInfo(
        "%s freed by hard-linking %d files",
        renderSize(stats.bytesFreed.load(std::memory_order_relaxed)),
        stats.filesLinked.load(std::memory_order_relaxed));
}

void LocalStore::optimisePath(const std::filesystem::path & path, RepairFlag repair)
{
    OptimiseStats stats;
    InodeHash inodeHash;

    if (config->getLocalSettings().autoOptimiseStore)
        optimisePath_(nullptr, stats, path, inodeHash, repair);
}

} // namespace nix
