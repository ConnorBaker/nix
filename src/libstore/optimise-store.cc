#include "nix/store/local-store.hh"
#include "nix/store/local-settings.hh"
#include "nix/store/pathlocks.hh"
#include "nix/util/signals.hh"
#include "nix/util/thread-pool.hh"
#include "nix/store/posix-fs-canonicalise.hh"
#include "nix/util/posix-source-accessor.hh"
#include "nix/util/file-system.hh"

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

    /* Uses `readdir` directly so we get `dirent::d_ino` for free — we
       only want inodes, not full `stat()` results. `dirent::d_type`
       also lets us recognise shard subdirectories in the same pass.

       Both layouts are walked unconditionally: a store can contain
       entries from a previous session that had `shardedLinks` set
       differently, and we need every existing canonical inode in
       the hash so the fast-path in `optimisePath_` works. */
    std::vector<std::filesystem::path> shards;

    {
        AutoCloseDir dir(opendir(linksDir.string().c_str()));
        if (!dir)
            throw SysError("opening directory %1%", PathFmt(linksDir));
        struct dirent * dirent;
        while (errno = 0, dirent = readdir(dir.get())) { /* sic */
            checkInterrupt();
            const char * name = dirent->d_name;
            if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0)))
                continue;
#ifdef _DIRENT_HAVE_D_TYPE
            if (dirent->d_type == DT_DIR) {
                shards.push_back(linksDir / name);
                continue;
            }
#endif
            inodeHash.insert(dirent->d_ino);
        }
        if (errno)
            throw SysError("reading directory %1%", PathFmt(linksDir));
    }

    for (auto & shard : shards) {
        AutoCloseDir dir(opendir(shard.string().c_str()));
        if (!dir)
            throw SysError("opening directory %1%", PathFmt(shard));
        struct dirent * dirent;
        while (errno = 0, dirent = readdir(dir.get())) { /* sic */
            checkInterrupt();
            const char * name = dirent->d_name;
            if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0)))
                continue;
            inodeHash.insert(dirent->d_ino);
        }
        if (errno)
            throw SysError("reading directory %1%", PathFmt(shard));
    }

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
 *     for replica = 0 .. maxLinkReplicas - 1:
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
 * The replica walk replaces the old single-slot scheme: each
 * replica is an independent canonical inode holding up to the
 * filesystem's per-inode hardlink ceiling (ext4 = 65 000), giving
 * ~6.5M total dedupes per logical hash across 100 replicas.
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

    /* NB: bulk migration of legacy flat `.links/<H>` entries into the
       sharded layout happens up-front in `optimiseStore`, before any
       `optimisePath_` calls, so by the time we're here the inodeHash
       fast-path above covers both layouts uniformly. */

    /* Replica walk: we try replica 0 first (the primary entry); on
       EMLINK — either detected up front via `st_nlink` saturation, or
       returned from `link(2)` — we fall through to replica 1, and
       so on, until we find a replica with room or exhaust the cap.
       Each replica is an independent canonical inode.

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
       of truth. */
    std::optional<MakeReadOnly> perCallReadOnly;
    bool dirWritabilityEnsured = false;
    auto ensureDirWritable = [&] {
        if (dirWritabilityEnsured)
            return;
        dirWritabilityEnsured = true;
        const auto dirOfPath = path.parent_path();
        if (parentDirWritability) {
            parentDirWritability->ensureWritable();
        } else {
            bool mustToggle = dirOfPath != config->realStoreDir.get();
            if (mustToggle)
                makeWritable(dirOfPath);
            perCallReadOnly.emplace(mustToggle ? dirOfPath : std::filesystem::path{});
        }
    };

    auto tryReplaceWith = [&](const std::filesystem::path & candidate) -> LinkResult {
        ensureDirWritable();
        auto tempLink = makeTempPath(config->realStoreDir.get(), ".tmp-link");

        /* Record our source inode in `inodeHash` before linking so
           that sibling files sharing this inode (hardlinked in the
           user store path) can short-circuit the hash step in their
           own `optimisePath_` call. This is advisory: if the link or
           rename below fails, the worst outcome is that siblings
           briefly skip dedup work that will be re-attempted on the
           next optimise pass. */
        inodeHash.insert(st.st_ino);

        /* RAII guard: after `create_hard_link` succeeds the tempLink
           exists on disk. Any exception that escapes before we
           reach `release()` must remove it, or we leak an orphan
           `.tmp-link-*` file every failed optimise. Covers
           `filesystem_error` from `rename`, plus non-filesystem
           exceptions (`std::bad_alloc`, interrupts surfaced as
           `BaseError`, etc.) that the old per-catch cleanup missed. */
        struct TempLinkGuard
        {
            const std::filesystem::path & p;
            bool armed = false;
            ~TempLinkGuard()
            {
                if (!armed)
                    return;
                /* Dtor runs during stack unwinding; swallow
                   anything `printError` might throw (allocation
                   failure, etc.) so we don't call std::terminate on
                   top of the original exception. */
                try {
                    std::error_code ec;
                    std::filesystem::remove(p, ec);
                    if (ec && ec != std::errc::no_such_file_or_directory)
                        printError("unable to unlink %1%: %2%", PathFmt(p), ec.message());
                } catch (...) {
                    ignoreExceptionInDestructor();
                }
            }
            void arm() { armed = true; }
            void release() { armed = false; }
        } tempLinkGuard{tempLink};

        try {
            std::filesystem::create_hard_link(candidate, tempLink);
            tempLinkGuard.arm();
        } catch (std::filesystem::filesystem_error & e) {
            if (e.code() == std::errc::too_many_links)
                return LinkResult::ReplicaFull;
            if (e.code() == std::errc::no_such_file_or_directory) {
                /* GC unlinked `candidate` while we were working.
                   Recreate from our own file; if that succeeds the
                   caller is done, otherwise fall through and let
                   the replica walk retry. */
                if (tryCreateCanonicalAt(candidate) == LinkResult::Done)
                    return LinkResult::Done;
                return LinkResult::AlreadyExists;
            }
            throw SystemError(
                e.code(), "creating hard link from %1% to %2%", PathFmt(candidate), PathFmt(tempLink));
        }

        try {
            std::filesystem::rename(tempLink, path);
            /* Success: `rename(2)` atomically moved `tempLink` onto
               `path`, so there's no longer a tempLink name to
               clean up. Disarm before the guard dtor runs. */
            tempLinkGuard.release();
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

        return LinkResult::Done;
    };

    /* Walk replicas until we find one that's either our inode already
       (done), has our content and can accept another hardlink
       (replace), or can host a new canonical entry. EMLINK at any
       step naturally drives us to the next replica. */
    for (uint8_t replica = 0; replica < maxLinkReplicas; ++replica) {
        auto candidate = linkPathFor(hash, replica);
        auto stOpt = maybeLstat(candidate);

        /* Corruption recovery: size/hash mismatch means a broken
           entry. Remove it and retry this replica slot as empty. */
        if (stOpt && isCorruptedAt(candidate, *stOpt)) {
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
            stOpt.reset();
        }

        if (!stOpt) {
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
            stOpt = maybeLstat(candidate);
            if (!stOpt)
                continue; /* vanished again; try next replica */
        }

        if (st.st_ino == stOpt->st_ino) {
            debug("%1% is already linked to %2%", PathFmt(path), PathFmt(candidate));
            return;
        }

        /* Opportunistic pre-flight: if we can see from the lstat
           that the candidate inode is already at the filesystem's
           hardlink ceiling, skip the link(2) attempt entirely. A
           racing writer that crosses the limit between here and
           tryReplaceWith is handled by the EMLINK path in that
           lambda, which advances to the next replica. */
        if (static_cast<int64_t>(stOpt->st_nlink) >= linkMax) {
            debug(
                "replica %1% of %2% has %3%/%4% links, trying next",
                replica,
                hash.to_string(HashFormat::Nix32, false),
                stOpt->st_nlink,
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
            maxLinkReplicas);
}

/* Bulk migration of legacy flat `.links/<H>` entries into the
   sharded layout.

   Normally a flat entry moves to `<pfx>/<H>.00` (the primary replica
   slot). But if that slot is already occupied — which happens when
   the feature was toggled off, then on again after new flat entries
   were created alongside the existing sharded ones — we walk the
   replica space looking for a free slot (`.01`, `.02`, ...). This
   preserves dedup: the flat inode becomes an additional replica for
   the same logical hash rather than being orphaned.

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
        auto hashStr = src.filename().string();
        if (hashStr.size() < 2) {
            /* Malformed entry — skip rather than crash. */
            continue;
        }
        auto shardDir = linksDir / hashStr.substr(0, 2);

        /* Walk replica slots. The common case is `.00` empty → one
           `rename(2)` and done. If `.00` is occupied we first check
           for the same-inode corner case (flat entry already
           hardlinked to its sharded counterpart — outside-of-Nix
           tampering); otherwise we spill into the next replica. This
           preserves the flat inode as an additional canonical
           replica; subsequent `optimiseStore` runs will coalesce
           user-store-path inodes toward `.00` via the normal replica
           walk. */
        bool done = false;
        for (uint8_t replica = 0; replica < maxLinkReplicas; ++replica) {
            char suffix[4];
            std::snprintf(suffix, sizeof suffix, ".%02u", static_cast<unsigned>(replica));
            auto dst = shardDir / (hashStr + suffix);
            std::error_code ec;
            std::filesystem::rename(src, dst, ec);
            if (!ec) {
                ++migrated;
                done = true;
                break;
            }
            if (ec == std::errc::no_such_file_or_directory) {
                done = true;
                break;
            }
            if (ec == std::errc::file_exists) {
                /* On `.00` only: check for the same-inode case.
                   Reaching this branch means a rename already
                   attempted this slot, so we pay the two lstats
                   exactly once per migrated entry in the pathological
                   case — not per entry overall. */
                if (replica == 0) {
                    auto srcStat = maybeLstat(src);
                    if (!srcStat) {
                        done = true;
                        break; /* vanished under us */
                    }
                    if (auto primaryStat = maybeLstat(dst);
                        primaryStat && primaryStat->st_ino == srcStat->st_ino) {
                        warn(
                            "flat entry %1% shares an inode with its sharded counterpart; "
                            "removing the redundant flat entry. This should not happen in "
                            "normal Nix operation — please report if you did not perform "
                            "manual intervention on `.links/`.",
                            PathFmt(src));
                        try {
                            nix::unlink(src);
                            ++migrated;
                        } catch (SysError & e) {
                            if (e.errNo != ENOENT)
                                throw;
                        }
                        done = true;
                        break;
                    }
                }
                continue; /* slot taken by different inode — try next */
            }
            throw SystemError(ec, "migrating %1% to sharded layout", PathFmt(src));
        }
        if (!done) {
            printInfo(
                "could not migrate %1%: all %2% replica slots occupied",
                PathFmt(src),
                static_cast<unsigned>(maxLinkReplicas));
        }
    }

    printInfo("migrated %d flat entries to the sharded layout", migrated);
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

    /* If the sharded-links feature is enabled, first migrate any
       pre-existing legacy flat entries into their shards. One-shot;
       subsequent `optimisePath_` calls use `linkPathFor` exclusively
       and place new canonical entries directly into the sharded
       layout. */
    if (shardedLinks)
        migrateLinksDirToSharded();

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

    /* Cap of 256 reflects that optimise's per-path work (hash +
       link) is heavy; smaller chunks give better load balancing
       when some paths take much longer than others.

       We skip `isValidPath(path)` here and rely on `optimisePath_`'s
       built-in ENOENT tolerance: if GC deleted the path between
       `queryAllValidPaths` and our `maybeLstat`, the inner function
       returns silently. This saves up to N SQLite point-lookups per
       `optimiseStore` run on cold caches. */
    parallelForEachChunked(paths, nThreads, /*maxChunkSize=*/256, [&](const StorePath & path) {
        addTempRoot(path);
        optimisePath_(
            nullptr, stats, config->realStoreDir.get() / path.to_string(), inodeHash, NoRepair);
        auto d = done.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((d & 1023) == 0 || d == total)
            act.progress(d, total);
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
