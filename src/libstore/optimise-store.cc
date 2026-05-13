#include "nix/store/local-store.hh"
#include "nix/store/local-settings.hh"
#include "nix/store/pathlocks.hh"
#include "nix/util/signals.hh"
#include "nix/store/posix-fs-canonicalise.hh"
#include "nix/util/posix-source-accessor.hh"
#include "nix/util/file-system.hh"

#include "nix/util/finally.hh"

#include <oneapi/tbb/parallel_for_each.h>
#include <oneapi/tbb/task_arena.h>

#include <algorithm>
#include <atomic>
#include <chrono>
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

/* Lazy per-directory writability guard. Threaded through
   `optimisePath_`'s recursion so all sibling-file replacements in
   one directory share a single chmod(+w)/canonicalise(-w) pair
   instead of toggling permissions per file (a ~20× reduction in
   metadata writes on typical stores).

   Safe under parallel `optimiseStore` because parallelism is at
   top-level store-path granularity: sibling files in one subdir
   always land on the same worker. */
struct LocalStore::DirWritability
{
    std::filesystem::path dir;
    bool needsCanonicalise = false;
    bool isStoreRoot;

    DirWritability(std::filesystem::path d, const std::filesystem::path & storeRoot)
        : dir(std::move(d))
        , isStoreRoot(dir == storeRoot)
    {
    }

    void ensureWritable()
    {
        if (isStoreRoot || needsCanonicalise)
            return;
        makeWritable(dir);
        needsCanonicalise = true;
    }

    ~DirWritability()
    {
        if (!needsCanonicalise)
            return;
        try {
            canonicaliseTimestampAndPermissions(dir.string());
        } catch (...) {
            ignoreExceptionInDestructor();
        }
    }

    DirWritability(const DirWritability &) = delete;
    DirWritability & operator=(const DirWritability &) = delete;
};

void LocalStore::loadInodeHashInto(InodeHash & inodeHash)
{
    debug("loading hash inodes in memory");

    /* Uses `readdir` directly so we get `dirent::d_ino` for free — we
       only want inodes, not full `stat()` results. `dirent::d_type`
       also lets us recognise shard subdirectories in the same pass.

       Both layouts are walked unconditionally: a store can contain
       entries from a previous session that had `shardedLinks` set
       differently, and we need every existing canonical inode in
       the hash so the fast-path in `optimisePath_` works. */
    std::vector<std::filesystem::path> shards;

    auto scanDir = [&](const std::filesystem::path & d, bool collectShards) {
        AutoCloseDir dir(opendir(d.string().c_str()));
        if (!dir)
            throw SysError("opening directory %1%", PathFmt(d));
        struct dirent * dirent;
        while (errno = 0, dirent = readdir(dir.get())) { /* sic */
            checkInterrupt();
            const char * name = dirent->d_name;
            if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0)))
                continue;
#ifdef _DIRENT_HAVE_D_TYPE
            if (collectShards && dirent->d_type == DT_DIR) {
                shards.push_back(d / name);
                continue;
            }
#endif
            inodeHash.insert(dirent->d_ino);
        }
        if (errno)
            throw SysError("reading directory %1%", PathFmt(d));
    };

    scanDir(linksDir, true);
    for (auto & shard : shards)
        scanDir(shard, false);

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
 *     for replica = 0 .. replicaCap - 1:
 *         candidate = linkPathFor(H, replica)
 *         if candidate doesn't exist:
 *             link(file, candidate); done
 *         if inode(file) == inode(candidate):
 *             already deduped; done
 *         if candidate has room:
 *             replace `file` with hardlink to candidate
 *             (tempLink + rename — POSIX atomic replace); done
 *     # fall through: maximum links across all replicas — leave undeduped
 *
 * Under the legacy flat layout (`shardedLinks == false`), the
 * replica loop terminates after replica 0 because higher slots are
 * unreachable. Under the sharded layout each replica is an
 * independent canonical inode holding up to the filesystem's
 * per-inode hardlink ceiling (ext4 = 65 000), giving ~6.5M total
 * dedupes per logical hash across 100 replicas.
 *
 * Every syscall has at least one race we must handle without
 * aborting the whole optimise run:
 *
 *   - ENOENT (source GC'd mid-op) -> benign skip; ENOENT on
 *     destination means a racing GC unlinked the canonical —
 *     we recreate from our source if possible.
 *   - EEXIST on link(2) -> a concurrent optimiser beat us to
 *     the slot; re-inspect its inode, either dedupe against it
 *     or move to the next replica.
 *   - EMLINK on either the link(2) or the rename(2) -> this
 *     replica is full; advance to the next. No pre-reservation
 *     is needed because the commit path handles EMLINK, so an
 *     opportunistic `st_nlink >= linkMax` pre-flight is just a
 *     free round-trip save, not a correctness requirement.
 *   - ENOSPC on the `.links/` directory's htree (very large
 *     flat stores only) -> abandon the file for this pass.
 *
 * Concurrency with GC is safe by construction: every syscall is
 * retry-tolerant on ENOENT, and GC unlinks only entries with
 * st_nlink == 1 (so we can't race it into deleting a replica
 * another optimiser is mid-link against).
 *
 * Concurrency with *another optimise invocation* (two processes
 * calling `optimiseStore`) is serialised by a per-store advisory
 * file lock in `optimiseStore`. Per-file parallelism inside one
 * invocation remains unlocked. */
void LocalStore::optimisePath_(
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
            optimisePath_(stats, path / i, inodeHash, repair, &dirW);
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

    /* NB: bulk migration of legacy flat `.links/<H>` entries into the
       sharded layout happens up-front in `optimiseStore`, before any
       `optimisePath_` calls, so by the time we're here the inodeHash
       fast-path above covers both layouts uniformly. */

    /* Replica walk: we try replica 0 first (the primary entry); on
       EMLINK — either detected up front via `st_nlink` saturation, or
       returned from `link(2)` — we fall through to replica 1, and
       so on, until we find a replica with room or exhaust the cap.
       Each replica is an independent canonical inode, identified by
       the `.<NN>` suffix on its filename (replica 0 has no suffix in
       either the flat or sharded layout — see `linkPathFor`). The
       `max-link-replicas` setting controls the walk length and is
       independent of `sharded-links`: a flat store can have replicas
       too, they just sit alongside the primary in the same directory.

       The caller distinguishes three outcomes: `Done` (we're finished
       with this user file — either linked successfully, or the source
       vanished, or the FS refused for a terminal reason like ENOSPC);
       `AlreadyExists` (the slot is occupied — inspect its inode to
       decide whether to use it or move on); `ReplicaFull` (source or
       candidate hit the per-inode ceiling — try the next slot). */
    enum class LinkResult {
        Done,
        AlreadyExists,
        ReplicaFull,
    };

    auto tryCreateCanonicalAt = [&](const std::filesystem::path & candidate) -> LinkResult {
        std::error_code ec;
        std::filesystem::create_hard_link(path, candidate, ec);
        if (!ec) {
            inodeHash.insert(st.st_ino);
            return LinkResult::Done;
        }
        if (ec == std::errc::file_exists)
            return LinkResult::AlreadyExists;
        if (ec == std::errc::no_such_file_or_directory) {
            /* POSIX link(2) returns ENOENT in two cases:
               1. the source (`path`) no longer exists — benign GC race;
               2. a directory component of the destination (`candidate`)
                  no longer exists — structural error (e.g. a shard
                  subdirectory is missing, which should have been
                  pre-created at store open).
               Distinguish by re-lstat-ing the source. */
            if (!maybeLstat(path))
                return LinkResult::Done; /* case 1 */
            throw SysError( /* case 2 */
                "creating hard link from %1% to %2%: destination parent directory missing",
                PathFmt(candidate),
                PathFmt(path));
        }
        if (ec == std::errc::too_many_links)
            return LinkResult::ReplicaFull;
        if (ec == std::errc::no_space_on_device) {
            printInfo("cannot link %s to %s: %s", PathFmt(candidate), PathFmt(path), ec.message());
            return LinkResult::Done;
        }
        throw SystemError(ec, "creating hard link from %1% to %2%", PathFmt(candidate), PathFmt(path));
    };

    /* Detect corruption at a given candidate. */
    auto isCorruptedAt = [&](const std::filesystem::path & candidate, const PosixStat & stLink) -> bool {
        if (st.st_size != stLink.st_size)
            return true;
        if (!repair)
            return false;
        auto linkHash = hashPath(
                            makeFSSourceAccessor(candidate),
                            FileSerialisationMethod::NixArchive,
                            HashAlgorithm::SHA256)
                            .hash;
        return hash != linkHash;
    };

    /* Atomically replace `path` with a hardlink to `candidate`. Any
       EMLINK we get from either the `link(2)` on `tempLink` or the
       `rename(2)` onto `path` surfaces as `ReplicaFull` so the
       replica loop can advance — there is no need to "reserve
       headroom" by pre-checking `st_nlink`; the kernel is the source
       of truth.

       When called recursively from a directory, `parentDirWritability`
       points at the parent dir's shared guard (one chmod toggle
       amortised across all siblings). Top-level leaf calls have no
       parent, so we lazily create our own. */
    std::optional<DirWritability> ownGuard;
    DirWritability * dirWritability = parentDirWritability;
    if (!dirWritability) {
        ownGuard.emplace(path.parent_path(), config->realStoreDir.get());
        dirWritability = &*ownGuard;
    }

    auto tryReplaceWith = [&](const std::filesystem::path & candidate) -> LinkResult {
        dirWritability->ensureWritable();
        auto tempLink = makeTempPath(config->realStoreDir.get(), ".tmp-link");

        /* Don't insert into `inodeHash` here. Siblings lstat to
           their original inode, which won't match `candidate`'s
           inode; `path`'s old inode is gone after the rename below.
           The cache is populated from `.links/` at startup (which
           is what siblings need) and extended by
           `tryCreateCanonicalAt` on canonical creation. */

        /* Cleanup guard: removes the tempLink if any exception
           escapes between `create_hard_link` succeeding and the
           rename completing. `armed` is flipped to `true` after the
           link succeeds and back to `false` after the rename
           succeeds. */
        bool armed = false;
        Finally tempLinkGuard([&] {
            if (!armed)
                return;
            try {
                std::error_code ec;
                std::filesystem::remove(tempLink, ec);
                if (ec && ec != std::errc::no_such_file_or_directory)
                    printError("unable to unlink %1%: %2%", PathFmt(tempLink), ec.message());
            } catch (...) {
                ignoreExceptionInDestructor();
            }
        });

        std::error_code linkEc;
        std::filesystem::create_hard_link(candidate, tempLink, linkEc);

        if (!linkEc) {
            armed = true;
        } else {
            if (linkEc == std::errc::too_many_links)
                return LinkResult::ReplicaFull;
            if (linkEc == std::errc::no_such_file_or_directory) {
                /* GC unlinked `candidate` while we were working.
                   Recreate from our own file; if that succeeds the
                   caller is done, otherwise fall through and let
                   the replica walk retry. */
                if (tryCreateCanonicalAt(candidate) == LinkResult::Done)
                    return LinkResult::Done;
                return LinkResult::AlreadyExists;
            }
            throw SystemError(
                linkEc,
                "creating hard link from %1% to %2%",
                PathFmt(candidate),
                PathFmt(tempLink));
        }

        try {
            std::filesystem::rename(tempLink, path);
            /* Success: `rename(2)` atomically moved `tempLink` onto
               `path`, so there's no longer a tempLink name to
               clean up. Disarm before the guard dtor runs. */
            armed = false;
        } catch (std::filesystem::filesystem_error & e) {
            if (e.code() == std::errc::too_many_links)
                return LinkResult::ReplicaFull;
            if (e.code() == std::errc::no_such_file_or_directory) {
                /* ENOENT from rename(2) can mean the source user file
                   (`path`) or its parent was GC'd — benign — or that
                   our freshly-created `tempLink` vanished, which
                   would indicate a real bug. Disambiguate by
                   re-lstat-ing `path`. */
                if (!maybeLstat(path))
                    return LinkResult::Done; /* source GC'd */
                throw SystemError(
                    e.code(),
                    "renaming %1% to %2%: tempLink disappeared before rename",
                    PathFmt(tempLink),
                    PathFmt(path));
            }
            throw SystemError(e.code(), "renaming %1% to %2%", PathFmt(tempLink), PathFmt(path));
        }

        stats.filesLinked.fetch_add(1, std::memory_order_relaxed);
        /* `bytesFreed` is exact: each `tryReplaceWith` fires at most
           once per (path, hash) pair, because subsequent passes hit
           the `st.st_ino == stCanon->st_ino` short-circuit. */
        stats.bytesFreed.fetch_add(st.st_size, std::memory_order_relaxed);

        return LinkResult::Done;
    };

    /* Walk replicas until we find one that's either our inode already
       (done), has our content and can accept another hardlink
       (replace), or can host a new canonical entry. EMLINK at any
       step naturally drives us to the next replica.

       The walk length is the runtime `max-link-replicas` setting,
       clamped to `maxReplicaSlots` (the compile-time encoding cap).
       This is independent of `sharded-links`: both layouts can host
       multiple replicas per hash. With `max-link-replicas=1` the
       walk collapses to the primary slot only, which on a fresh
       store is equivalent to the pre-`fffca655b` behaviour. */
    const uint8_t replicaCount = static_cast<uint8_t>(std::min<size_t>(
        config->getLocalSettings().maxLinkReplicas.get(),
        maxReplicaSlots));
    for (uint8_t replica = 0; replica < replicaCount; ++replica) {
        auto candidate = linkPathFor(hash, replica);
        auto stCanon = maybeLstat(candidate);

        /* Corruption recovery: size/hash mismatch means a broken
           entry. Remove it and retry this replica slot as empty. */
        if (stCanon && isCorruptedAt(candidate, *stCanon)) {
            warn("removing corrupted link %s", PathFmt(candidate));
            warn(
                "There may be more corrupted paths."
                "\nYou should run `nix-store --verify --check-contents --repair` to fix them all");
            unlinkIfExists(candidate);
            stCanon.reset();
        }

        if (!stCanon) {
            auto r = tryCreateCanonicalAt(candidate);
            if (r == LinkResult::Done)
                return;
            if (r == LinkResult::ReplicaFull)
                /* Source inode already has `linkMax` references —
                   typically because a prior optimise pointed many
                   user files at it. Next replica has independent
                   link-count headroom. */
                continue;
            /* r == AlreadyExists: a concurrent process created the
               entry between our lstat and our link. Fall through. */
            stCanon = maybeLstat(candidate);
            if (!stCanon)
                continue; /* vanished again; try next replica */
        }

        if (st.st_ino == stCanon->st_ino) {
            debug("%1% is already linked to %2%", PathFmt(path), PathFmt(candidate));
            return;
        }

        /* Opportunistic pre-flight: if we can see from the lstat
           that the candidate inode is already at the filesystem's
           hardlink ceiling, skip the link(2) attempt entirely. A
           racing writer that crosses the limit between here and
           tryReplaceWith is handled by the EMLINK path in that
           lambda, which advances to the next replica. */
        if (static_cast<int64_t>(stCanon->st_nlink) >= linkMax) {
            debug(
                "replica %1% of %2% has %3%/%4% links, trying next",
                replica,
                hash.to_string(HashFormat::Nix32, false),
                stCanon->st_nlink,
                linkMax);
            continue;
        }

        printMsg(lvlTalkative, "linking %1% to %2%", PathFmt(path), PathFmt(candidate));
        auto r = tryReplaceWith(candidate);
        if (r == LinkResult::Done)
            return;
        /* ReplicaFull (filled up between our lstat and link(2)) or
           AlreadyExists (vanished + recreated by a racing optimiser):
           either way, the current replica is unusable for this file.
           Move on. */
    }

    /* Exhausted all replicas; leave this file undeduped. */
    if (st.st_size)
        printInfo(
            "%1% has maximum number of links across %2% replicas",
            hash.to_string(HashFormat::Nix32, false),
            static_cast<unsigned>(replicaCount));
}

/* Bulk migration of legacy flat `.links/` entries into the sharded
   layout.

   A flat-layout source can be either the primary `<H>` (replica 0)
   or a replica `<H>.<NN>` from a flat-layout-with-replicas store.
   Both end up in the corresponding sharded slot: primary goes to
   `<pfx>/<H>`, replica N goes to `<pfx>/<H>.<NN>`. If the natural
   destination is occupied (the feature was toggled off, then on
   again after new flat entries were created alongside existing
   sharded ones), we walk forward through the replica space looking
   for a free slot. This preserves dedup: the flat inode becomes an
   additional replica for the same logical hash rather than being
   orphaned.

   Idempotent (re-running finds nothing to move). Race-tolerant
   under concurrent GC: ENOENT / EEXIST on the dst attempt are
   benign, we skip or retry the next replica slot. */
void LocalStore::migrateLinksDirToSharded()
{
    std::vector<std::filesystem::path> flatEntries;
    for (auto & ent : DirectoryIterator{linksDir}) {
        checkInterrupt();
        std::error_code ec;
        if (ent.is_directory(ec) && !ec)
            continue; /* shard subdir, already migrated */
        flatEntries.push_back(ent.path());
    }

    if (flatEntries.empty())
        return;

    printInfo("migrating %d flat `.links/` entries to the sharded layout...", flatEntries.size());

    uint64_t migrated = 0;
    for (auto & src : flatEntries) {
        checkInterrupt();
        auto srcName = src.filename().string();
        auto baseHash = std::string(stripReplicaSuffix(srcName));
        if (baseHash.size() < 2) {
            /* Malformed entry — skip rather than crash. */
            continue;
        }
        auto shardDir = linksDir / baseHash.substr(0, 2);

        /* Walk replica slots. The common case is the primary slot
           empty → one `link(2)` + one `unlink(2)` and done. If
           that slot is occupied we first check for the same-inode
           corner case (flat entry already hardlinked to its
           sharded counterpart — outside-of-Nix tampering, or
           crash-recovery from a prior partial migration);
           otherwise we spill into the next replica. Subsequent
           `optimiseStore` runs coalesce user-store-path inodes
           toward the primary slot via the normal replica walk.

           We deliberately do NOT use `rename(2)`: POSIX rename
           atomically *replaces* the destination, so the spill
           branch would never be reached and we would silently
           clobber a pre-existing primary slot (orphaning all
           user-store hardlinks pointing at it). `link(2)` returns
           EEXIST without touching the destination, which is exactly
           the semantics the spill walk needs. After a successful
           link, we unlink the source. If we crash between link and
           unlink, the next migration run finds the flat entry,
           retries the link at the primary slot (now sharing an
           inode), detects the same-inode case below, and unlinks
           the stranded flat entry — i.e. fully idempotent under
           crash recovery. */
        bool done = false;
        for (uint8_t replica = 0; replica < maxReplicaSlots; ++replica) {
            auto dst = shardDir / replicaFilename(baseHash, replica);
            if (::link(src.string().c_str(), dst.string().c_str()) == 0) {
                /* Linked; now unlink the source. ENOENT is fine —
                   a concurrent GC may have pulled it out from under
                   us, but the sharded entry exists and is intact. */
                unlinkIfExists(src);
                ++migrated;
                done = true;
                break;
            }
            int e = errno;
            if (e == ENOENT) {
                done = true;
                break; /* src vanished — concurrent GC, treat as migrated */
            }
            if (e == EEXIST) {
                /* On the primary slot only: check for the
                   same-inode case. Reaching this branch means a
                   previous run linked `src` to the primary but
                   crashed before unlinking — so we pay the two
                   lstats exactly once per stranded entry, not per
                   entry overall. */
                if (replica == 0) {
                    auto srcStat = maybeLstat(src);
                    if (!srcStat) {
                        done = true;
                        break; /* vanished under us */
                    }
                    if (auto primaryStat = maybeLstat(dst);
                        primaryStat && primaryStat->st_ino == srcStat->st_ino) {
                        unlinkIfExists(src);
                        ++migrated;
                        done = true;
                        break;
                    }
                }
                continue; /* slot taken by different inode — try next */
            }
            throw SysError(e, "migrating %1% to sharded layout", PathFmt(src));
        }
        if (!done) {
            printInfo(
                "could not migrate %1%: all %2% replica slots occupied",
                PathFmt(src),
                static_cast<unsigned>(maxReplicaSlots));
        }
    }

    printInfo("migrated %d flat entries to the sharded layout", migrated);
}

void LocalStore::optimiseStore(OptimiseStats & stats)
{
    Activity act(*logger, actOptimiseStore);

    auto t0 = std::chrono::steady_clock::now();
    auto stage = [&](uint64_t & f) {
        auto t1 = std::chrono::steady_clock::now();
        f += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        t0 = t1;
    };

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
    stage(stats.timings.setupNs);

    /* If the sharded-links feature is enabled, first migrate any
       pre-existing legacy flat entries into their shards. One-shot;
       subsequent `optimisePath_` calls use `linkPathFor` exclusively
       and place new canonical entries directly into the sharded
       layout. */
    if (shardedLinks)
        migrateLinksDirToSharded();
    stage(stats.timings.migrateLinksNs);

    /* `std::set` has no random access, so materialise into a vector
       for index-based partitioning across workers. */
    auto pathSet = queryAllValidPaths();
    std::vector<StorePath> paths{pathSet.begin(), pathSet.end()};
    stage(stats.timings.queryAllPathsNs);

    InodeHash inodeHash;
    loadInodeHashInto(inodeHash);
    stage(stats.timings.loadInodeHashNs);

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
                stats, config->realStoreDir.get() / path.to_string(), inodeHash, NoRepair);
            auto d = done.fetch_add(1, std::memory_order_relaxed) + 1;
            if ((d & 1023) == 0 || d == total)
                act.progress(d, total);
        });
    });
    stage(stats.timings.parallelOptimiseNs);
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
        optimisePath_(stats, path, inodeHash, repair);
}

} // namespace nix
