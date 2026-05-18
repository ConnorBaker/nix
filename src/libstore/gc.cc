#include "nix/store/gc-store.hh"
#include "nix/store/globals.hh"
#include "nix/store/local-gc.hh"
#include "nix/store/local-settings.hh"
#include "nix/store/local-store.hh"
#include "nix/store/path.hh"
#include "nix/util/configuration.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/finally.hh"
#include "nix/util/unix-domain-socket.hh"
#include "nix/util/signals.hh"
#include "nix/util/serialise.hh"
#include "nix/util/thread-pool.hh"

#include <chrono>

#include <oneapi/tbb/enumerable_thread_specific.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/parallel_for_each.h>
#include <oneapi/tbb/task_arena.h>
#include "nix/util/util.hh"
#include "nix/util/file-system.hh"
#include "nix/util/topo-sort.hh"
#include "nix/store/posix-fs-canonicalise.hh"
#include "nix/store/sqlite.hh"

#include "store-config-private.hh"

#if HAVE_LIBURING
#  include <liburing.h>
#endif

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include <boost/regex.hpp>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <mutex>
#include <queue>
#include <thread>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <variant>
#if HAVE_STATVFS
#  include <sys/statvfs.h>
#endif
#ifndef _WIN32
#  include <poll.h>
#  include <sys/socket.h>
#  include <sys/un.h>
#endif
#include <sys/types.h>
#include <unistd.h>

namespace nix {

/* Compile-time constant mirroring `HAVE_LIBURING`. Lets the
   io_uring-vs-syscall dispatches in `collectGarbage` use a plain
   `if` rather than `#if HAVE_LIBURING`; the stub definitions in the
   `#else` branch below keep the call sites well-formed when
   `kHaveLibUring == false`. */
inline constexpr bool kHaveLibUring = HAVE_LIBURING;

/* Dispatch predicate shared by Phase-2 orphan deletion and Phase-3
   `.links/` cleanup. Folds the build-time and runtime axes into one
   bool. When `kHaveLibUring == false` the `&&` is dead so the
   setting lookup is short-circuited away. */
static bool useIoUringDispatch(const LocalStoreConfig & cfg)
{
    return kHaveLibUring
        && cfg.getLocalSettings().getGCSettings().gcLinksUseIoUring.get();
}

#if HAVE_LIBURING
namespace {

/* Two-phase io_uring sweep of `.links/` entries, used by
   collectGarbage() when `gc-links-use-io-uring` is enabled.

   Phase 1 issues batched IORING_OP_STATX for every entry and
   accumulates the same `actualSize` / `unsharedSize` counters the
   thread-pool path computes.

   Phase 2 issues batched IORING_OP_UNLINKAT for entries that returned
   nlink == 1 in phase 1. ENOENT on either phase is benign (the entry
   was already removed by a concurrent optimise or GC).

   This is *not* expected to be faster than the thread-pool path on
   most workloads — the io-wq workers serialise on the same
   `.links/` inode `i_rwsem` that thread-pool pthreads serialise on,
   so the kernel-side wait time is the same. The hypothesis is that
   reduced syscall and context-switch overhead may shave the dispatch
   tail. See bench results for the actual answer per scenario. */
static void cleanupLinksIoUring(
    const std::filesystem::path & linksDir,
    const std::vector<std::filesystem::path> & entries,
    std::atomic<int64_t> & actualSize,
    std::atomic<int64_t> & unsharedSize,
    size_t iowqBoundedWorkers)
{
    AutoCloseFD dirfd(open(linksDir.string().c_str(),
        O_PATH | O_DIRECTORY | O_CLOEXEC));
    if (!dirfd)
        throw SysError("opening %1% for io_uring", PathFmt(linksDir));

    constexpr unsigned QD = 256;

    io_uring ring;
    int rc = io_uring_queue_init(QD * 2, &ring, 0);
    if (rc < 0) {
        errno = -rc;
        throw SysError("io_uring_queue_init");
    }
    Finally exitRing([&]() { io_uring_queue_exit(&ring); });

    if (iowqBoundedWorkers > 0) {
        unsigned vals[2] = {static_cast<unsigned>(iowqBoundedWorkers), 0};
        /* Best-effort; older kernels may not support this. Ignore. */
        io_uring_register_iowq_max_workers(&ring, vals);
    }

    const size_t n = entries.size();

    /* Per-entry buffers. statx writes into stxBufs[i]; unlink-phase
       reads names[i] from this same array. Both arrays outlive the
       SQEs that reference them. `names[i]` is the path of entry i
       relative to `linksDir` — under the flat layout this is just
       the hash basename, under the sharded layout it's
       `<pfx>/<hash>.<NN>`. The kernel resolves the relative path
       against `dirfd` per syscall. */
    std::vector<struct statx> stxBufs(n);
    std::vector<std::string> names;
    names.reserve(n);
    for (const auto & e : entries)
        names.push_back(e.lexically_relative(linksDir).string());

    std::vector<uint8_t> needsUnlink(n, 0);

    int firstErrno = 0;
    size_t firstErrorIdx = 0;

    auto submitAndDrain = [&](auto onCqe) {
        int s = io_uring_submit(&ring);
        if (s < 0) {
            errno = -s;
            throw SysError("io_uring_submit");
        }
        io_uring_cqe * cqe;
        int wr;
        /* EINTR loop: SIGINT from a Ctrl-C surfaces here while we're
           in the kernel; route through `checkInterrupt` so the
           top-level GC driver throws `Interrupted` cleanly instead
           of "io_uring_wait_cqe: Interrupted system call". */
        while ((wr = io_uring_wait_cqe(&ring, &cqe)) == -EINTR)
            checkInterrupt();
        if (wr < 0) {
            errno = -wr;
            throw SysError("io_uring_wait_cqe");
        }
        do {
            onCqe(cqe);
            io_uring_cqe_seen(&ring, cqe);
        } while (io_uring_peek_cqe(&ring, &cqe) == 0);
    };

    /* Phase 1: STATX every entry. */
    size_t submitted = 0, completed = 0, nextIdx = 0;
    while (completed < n) {
        checkInterrupt();
        while (nextIdx < n && submitted - completed < QD) {
            io_uring_sqe * sqe = io_uring_get_sqe(&ring);
            if (!sqe)
                break;
            io_uring_prep_statx(
                sqe, dirfd.get(), names[nextIdx].c_str(),
                AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS, &stxBufs[nextIdx]);
            io_uring_sqe_set_data(
                sqe, reinterpret_cast<void *>(static_cast<uintptr_t>(nextIdx)));
            nextIdx++;
            submitted++;
        }
        submitAndDrain([&](io_uring_cqe * cqe) {
            auto idx = static_cast<size_t>(
                reinterpret_cast<uintptr_t>(io_uring_cqe_get_data(cqe)));
            if (cqe->res == 0) {
                const auto & st = stxBufs[idx];
                if (st.stx_nlink != 1) {
                    actualSize.fetch_add(
                        st.stx_size, std::memory_order_relaxed);
                    unsharedSize.fetch_add(
                        (st.stx_nlink - 1) * st.stx_size,
                        std::memory_order_relaxed);
                } else {
                    needsUnlink[idx] = 1;
                }
            } else if (cqe->res != -ENOENT && cqe->res != -ENOTDIR) {
                /* Match thread-pool path: maybeLstat throws on errors
                   other than ENOENT/ENOTDIR; do the same here by
                   tracking the first error and throwing after the
                   drain loop completes. */
                if (firstErrno == 0) {
                    firstErrno = -cqe->res;
                    firstErrorIdx = idx;
                }
            }
            completed++;
        });
    }

    if (firstErrno != 0) {
        errno = firstErrno;
        throw SysError("io_uring statx %1%", PathFmt(linksDir / names[firstErrorIdx]));
    }

    /* Phase 2: UNLINKAT each entry flagged in phase 1. */
    std::vector<size_t> unlinkIdx;
    unlinkIdx.reserve(n);
    for (size_t i = 0; i < n; i++)
        if (needsUnlink[i])
            unlinkIdx.push_back(i);

    firstErrno = 0;
    firstErrorIdx = 0;

    submitted = 0;
    completed = 0;
    nextIdx = 0;
    while (completed < unlinkIdx.size()) {
        checkInterrupt();
        while (nextIdx < unlinkIdx.size() && submitted - completed < QD) {
            io_uring_sqe * sqe = io_uring_get_sqe(&ring);
            if (!sqe)
                break;
            size_t idx = unlinkIdx[nextIdx];
            io_uring_prep_unlinkat(sqe, dirfd.get(), names[idx].c_str(), 0);
            io_uring_sqe_set_data(
                sqe, reinterpret_cast<void *>(static_cast<uintptr_t>(idx)));
            nextIdx++;
            submitted++;
        }
        submitAndDrain([&](io_uring_cqe * cqe) {
            int res = cqe->res;
            auto idx = static_cast<size_t>(
                reinterpret_cast<uintptr_t>(io_uring_cqe_get_data(cqe)));
            if (res < 0 && res != -ENOENT) {
                /* Match thread-pool path: unlink rethrows non-ENOENT. */
                if (firstErrno == 0) {
                    firstErrno = -res;
                    firstErrorIdx = idx;
                }
            }
            completed++;
        });
    }

    if (firstErrno != 0) {
        errno = firstErrno;
        throw SysError("io_uring unlinkat %1%", PathFmt(linksDir / names[firstErrorIdx]));
    }
}

/* Bulk-delete the phase-2 orphan subtrees via io_uring.

   Stage A (TBB-parallel): for each orphan, walk the subtree and
   collect every regular file into `files` and every directory into
   `dirs` in post-order (children before parent). The walk itself
   stays in userspace — readdir/openat aren't usefully batchable
   here, and the per-orphan walk is already independent so TBB gives
   real parallelism. Symlinks are recorded as files (unlinkat
   without AT_REMOVEDIR is correct for symlinks pointing anywhere,
   matching `_deletePath`). Directory permissions are rescued
   in-line via `::chmod` to add u+rwx before recursing; this
   matches `_deletePath`'s behaviour and is required because
   `optimisePath_` canonicalises store directories to 0555.

   Stage B (io_uring): submit IORING_OP_UNLINKAT for every file
   path. AT_FDCWD + absolute path lets a single ring drive unlinks
   across many parent directories — io-wq workers serialise on each
   individual parent's `i_rwsem`, but distinct parents can proceed
   in parallel. With ~10 files per orphan dir and thousands of
   orphans, this exposes far more parent-level parallelism than
   `gcDeleteThreads` (default 4) provides on the TBB path.

   Stage C (io_uring): submit IORING_OP_UNLINKAT with AT_REMOVEDIR
   for every directory. Post-order means each parent's children
   have already been unlinked (Stage B) and any deeper directories
   already rmdir'd, so the directory is empty when we hit it. Stage
   A skips pushing a directory whose children iteration failed
   partway through, so we never reach this stage with a non-empty
   AT_REMOVEDIR target on the happy path.

   Caveats vs `_deletePath`:
   - No per-orphan `ignoreGcDeleteFailure` partitioning: a fatal
     error in any unlinkat returns the first error globally.
     ENOENT is silently tolerated. When `ignoreFailure==true` we
     log a warning for each non-ENOENT failure so operators have
     a paper trail (the TBB path does the same via `logWarning`).
   - No bytesFreed accounting (matches the existing comment in
     `collectGarbage`: bytesFreed is charged at rename time, not
     here). */
static void cleanupOrphansIoUring(
    const std::vector<std::filesystem::path> & orphans,
    size_t nThreads,
    bool ignoreFailure)
{
    if (orphans.empty())
        return;

    /* Post-order walk: files first, then their parent dirs. The two
       lists feed Stage B (`UNLINKAT` on files) and Stage C (`UNLINKAT
       | AT_REMOVEDIR` on now-empty dirs) of the io_uring pipeline. */
    auto walk = [&](this auto & self,
                    const std::filesystem::path & p,
                    std::vector<std::string> & files,
                    std::vector<std::string> & dirs) -> void {
        checkInterrupt();
        struct stat st;
        if (::lstat(p.c_str(), &st) == -1) {
            if (errno == ENOENT)
                return;
            throw SysError("lstat %1%", PathFmt(p));
        }
        if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
            /* Match `_deletePath`: optimisePath_ canonicalises store
               directories to read-only (mode 0555), and our orphans
               are renamed-aside post-optimise. We must add u+rwx
               before we can readdir, unlink entries, or rmdir. */
            constexpr mode_t PERM_MASK = S_IRUSR | S_IWUSR | S_IXUSR;
            if ((st.st_mode & PERM_MASK) != PERM_MASK) {
                if (::chmod(p.c_str(), st.st_mode | PERM_MASK) == -1) {
                    if (errno != ENOENT)
                        throw SysError("chmod +rwx %1%", PathFmt(p));
                    return;
                }
            }
            std::error_code ec;
            std::filesystem::directory_iterator it(p, ec);
            if (ec) {
                /* Couldn't open the dir for reading after the chmod;
                   the dir likely vanished under us (ENOENT) or
                   another process holds it open. Skip — don't push
                   to `dirs` because Stage C would otherwise try to
                   AT_REMOVEDIR a directory we never enumerated. */
                return;
            }
            bool walkOk = true;
            for (auto end = std::filesystem::directory_iterator{};
                 it != end;
                 it.increment(ec)) {
                if (ec) {
                    /* Same rationale: a mid-walk failure means we
                       don't know if all children were collected, so
                       this dir is not safe to AT_REMOVEDIR. */
                    logWarning({
                        .msg = HintFmt(
                            "directory iteration failed mid-walk for %1%: %2%",
                            PathFmt(p),
                            ec.message()),
                    });
                    walkOk = false;
                    break;
                }
                self(it->path(), files, dirs);
            }
            if (walkOk)
                dirs.push_back(p.string());
        } else {
            files.push_back(p.string());
        }
    };

    /* Per-TBB-worker io_uring state. Each worker walks one orphan
       and submits + drains its own unlinks before picking the next,
       mirroring deletePath's structure on the syscall side. Walk
       and submit interleave across orphans rather than running as
       two strict sequential phases over the whole orphan set.
       `std::optional` encodes the lazy-init invariant — an absent
       slot means the thread hasn't touched a ring yet; `OwnedRing`'s
       RAII handles cleanup including the exception path. */
    struct OwnedRing
    {
        io_uring ring;
        explicit OwnedRing(unsigned qd)
        {
            int rc = io_uring_queue_init(qd, &ring, 0);
            if (rc < 0) {
                errno = -rc;
                throw SysError("io_uring_queue_init");
            }
        }
        ~OwnedRing() { io_uring_queue_exit(&ring); }
        OwnedRing(const OwnedRing &) = delete;
        OwnedRing & operator=(const OwnedRing &) = delete;
    };
    tbb::enumerable_thread_specific<std::optional<OwnedRing>> rings;

    constexpr unsigned QD = 1024;

    /* io-wq worker cap per ring. Chosen so the aggregate across all
       N rings (`N * IOWQ_WORKERS_PER_RING`) tracks the old
       single-ring total of `N * 16` for typical N — keeps total
       kernel-side parallelism comparable while letting walks and
       submits pipeline across the per-worker rings. */
    constexpr unsigned IOWQ_WORKERS_PER_RING = 2;

    /* Cross-thread error reporting. firstErrno wins-first via CAS;
       firstErrorPath/Flags are guarded by errorMutex. */
    std::atomic<int> firstErrno{0};
    std::string firstErrorPath;
    int firstErrorFlags = 0;
    std::mutex errorMutex;
    std::atomic<size_t> ignoredCount{0};

    auto submitBatchOnRing = [&](io_uring & ring,
                                  const std::vector<std::string> & paths,
                                  int flags) {
        const size_t n = paths.size();
        if (n == 0) return;
        size_t submitted = 0, completed = 0, nextIdx = 0;
        while (completed < n) {
            checkInterrupt();
            while (nextIdx < n && submitted - completed < QD) {
                io_uring_sqe * sqe = io_uring_get_sqe(&ring);
                if (!sqe) break;
                io_uring_prep_unlinkat(
                    sqe, AT_FDCWD, paths[nextIdx].c_str(), flags);
                io_uring_sqe_set_data(
                    sqe,
                    reinterpret_cast<void *>(static_cast<uintptr_t>(nextIdx)));
                nextIdx++;
                submitted++;
            }
            int s = io_uring_submit(&ring);
            if (s < 0) {
                errno = -s;
                throw SysError("io_uring_submit");
            }
            io_uring_cqe * cqe;
            int wr = io_uring_wait_cqe(&ring, &cqe);
            if (wr < 0) {
                /* EINTR routing matches the syscall path: a Ctrl-C
                   surfaces as `Interrupted` rather than a SysError on
                   io_uring_wait_cqe. */
                if (wr == -EINTR) {
                    checkInterrupt();
                    continue;
                }
                errno = -wr;
                throw SysError("io_uring_wait_cqe");
            }
            do {
                int res = cqe->res;
                auto idx = static_cast<size_t>(
                    reinterpret_cast<uintptr_t>(io_uring_cqe_get_data(cqe)));
                io_uring_cqe_seen(&ring, cqe);
                completed++;
                if (res < 0 && res != -ENOENT) {
                    int expected = 0;
                    if (firstErrno.compare_exchange_strong(expected, -res)) {
                        std::lock_guard lock(errorMutex);
                        firstErrorPath = paths[idx];
                        firstErrorFlags = flags;
                    }
                    if (ignoreFailure
                        && ignoredCount.fetch_add(1) < 10) {
                        /* Bound the log spam: if every unlink fails
                           we'd otherwise emit one warning per file.
                           Ten samples is enough to characterise the
                           failure pattern; the total count is logged
                           below. */
                        errno = -res;
                        logWarning({
                            .msg = HintFmt(
                                "io_uring unlinkat ignored failure on %1% (flags=%2%): %3%",
                                PathFmt(paths[idx]),
                                flags,
                                strerror(-res)),
                        });
                    }
                }
            } while (io_uring_peek_cqe(&ring, &cqe) == 0);
        }
    };

    tbb::task_arena arena(static_cast<int>(nThreads));
    arena.execute([&] {
        tbb::parallel_for(size_t(0), orphans.size(), [&](size_t i) {
            auto & wr = rings.local();
            if (!wr) {
                wr.emplace(QD * 2);
                unsigned vals[2] = {
                    static_cast<unsigned>(nThreads * IOWQ_WORKERS_PER_RING),
                    static_cast<unsigned>(nThreads * IOWQ_WORKERS_PER_RING)};
                io_uring_register_iowq_max_workers(&wr->ring, vals);
            }

            std::vector<std::string> files, dirs;
            walk(orphans[i], files, dirs);
            submitBatchOnRing(wr->ring, files, 0);
            submitBatchOnRing(wr->ring, dirs, AT_REMOVEDIR);
        });
    });

    /* `rings` going out of scope runs `~OwnedRing` on each engaged
       per-thread `optional` — RAII cleanup including the exception
       path. */

    if (firstErrno.load() != 0 && !ignoreFailure) {
        std::lock_guard lock(errorMutex);
        errno = firstErrno.load();
        throw SysError(
            "io_uring unlinkat %1% (flags=%2%)",
            PathFmt(firstErrorPath),
            firstErrorFlags);
    }
    if (ignoredCount.load() > 10) {
        logWarning({
            .msg = HintFmt(
                "io_uring unlinkat ignored %1% additional failures (only first 10 logged above)",
                ignoredCount.load() - 10),
        });
    }
}

} // namespace
#else // !HAVE_LIBURING
namespace {
/* Stubs so the dispatch call sites in `collectGarbage` compile when
   `liburing` isn't available. `kHaveLibUring == false` makes the
   guarding `if` dead at every call site, so these stubs are
   unreachable; assert that loudly if a future refactor breaks the
   invariant. */
static void cleanupLinksIoUring(
    const std::filesystem::path &,
    const std::vector<std::filesystem::path> &,
    std::atomic<int64_t> &,
    std::atomic<int64_t> &,
    size_t)
{
    assert(false && "cleanupLinksIoUring called without HAVE_LIBURING");
}
static void cleanupOrphansIoUring(
    const std::vector<std::filesystem::path> &,
    size_t,
    bool)
{
    assert(false && "cleanupOrphansIoUring called without HAVE_LIBURING");
}
} // namespace
#endif // HAVE_LIBURING

static std::string gcSocketPath = "gc-socket/socket";
static std::string gcRootsDir = "gcroots";

void LocalStore::addIndirectRoot(const std::filesystem::path & path)
{
    std::string hash = hashString(HashAlgorithm::SHA1, path.string()).to_string(HashFormat::Nix32, false);
    auto realRoot = canonPath(config->stateDir.get() / gcRootsDir / "auto" / hash);
    makeSymlink(realRoot, path);
}

void LocalStore::createTempRootsFile()
{
    auto fdTempRoots(_fdTempRoots.lock());

    /* Create the temporary roots file for this process. */
    if (*fdTempRoots)
        return;

    while (1) {
        if (pathExists(fnTempRoots))
            /* It *must* be stale, since there can be no two
               processes with the same pid. */
            tryUnlink(fnTempRoots);

        *fdTempRoots = openLockFile(fnTempRoots, true);

        debug("acquiring write lock on %s", PathFmt(fnTempRoots));
        lockFile(fdTempRoots->get(), ltWrite, true);

        /* Check whether the garbage collector didn't get in our
           way. */
        if (getFileSize(fdTempRoots->get()) == 0)
            break;

        /* The garbage collector deleted this file before we could get
           a lock.  (It won't delete the file after we get a lock.)
           Try again. */
    }
}

/* Register `path` as a temporary GC root for the current process.
 *
 * Fast path (no GC running): take a shared read lock on the global
 * GC lock file and append to our per-process temproots file. Cheap,
 * contention-free across callers.
 *
 * Slow path (GC running): send the path over a Unix-domain socket to
 * the GC server thread, which acknowledges with a single byte. To
 * keep this slow path from becoming a scalability bottleneck when N
 * worker threads of a parallel operation (e.g. `optimiseStore`) all
 * call `addTempRoot` under an active GC, each thread keeps its own
 * gc-socket connection in `_fdRootsSockets`. The TBB
 * `enumerable_thread_specific` uses an opaque per-thread slot
 * (not exposed as `std::thread::id`); see the field comment in
 * `local-store.hh` for the lifetime caveats around thread-id
 * reuse. Without this, the previous shared-socket design — one
 * `Sync<AutoCloseFD>` — serialised all workers across blocking
 * writev+read round-trips to the GC server. Measured impact at
 * 16 workers: ~135× speedup on the concurrent optimise+GC
 * benchmark (see `src/libstore-tests/optimise-bench.cc`). */
void LocalStore::addTempRoot(const StorePath & path)
{
    if (config->readOnly) {
        debug(
            "Read-only store doesn't support creating lock files for temp roots, but nothing can be deleted anyways.");
        return;
    }

    createTempRootsFile();

    /* Open/create the global GC lock file. */
    {
        auto fdGCLock(_fdGCLock.lock());
        if (!*fdGCLock)
            *fdGCLock = openGCLock();
    }

    /* Reconnect / retry loop. On `goto restart`-equivalent we
       drop any stale socket for this thread and try again after a
       small backoff if appropriate.

       Lock-ordering invariant: when we *acquire* the shared GC lock
       (fast path), we must hold it until our entry is durably
       appended to `_fdTempRoots`. Otherwise a GC could start in
       the window between releasing the shared lock and writing,
       read a temproots file that doesn't yet contain our entry,
       and invalidate our path — silently breaking the documented
       "addTempRoot ⇒ path is safe from GC" guarantee. */
    while (true) {
        checkInterrupt();

        /* Try to acquire a shared global GC lock (non-blocking). This
           only succeeds if the garbage collector is not currently
           running. */
        FdLock gcLock(_fdGCLock.lock()->get(), ltRead, false, "");

        if (gcLock.acquired) {
            /* No GC running. Append to the temproots file *while
               still holding the shared lock*: any GC that wakes up
               between now and our return will block on the exclusive
               GC lock until our writeFull commits. */
            auto s = printStorePath(path) + '\0';
            writeFull(_fdTempRoots.lock()->get(), s);
            return;
        }

        /* GC is running. Send our new temp root over this thread's
           own gc-socket so N workers don't serialise on a shared
           connection. */
        auto & fd = _fdRootsSockets.local();
        if (!fd) {
            auto socketPath = config->stateDir.get() / gcSocketPath;
            debug("connecting to '%s'", PathFmt(socketPath));
            fd = createUnixDomainSocket();
            try {
                nix::connect(toSocket(fd.get()), socketPath);
            } catch (SystemError & e) {
                if (e.is(std::errc::connection_refused) || e.is(std::errc::no_such_file_or_directory)) {
                    debug("GC socket connection refused: %s", e.msg());
                    fd.close();
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
                throw;
            }
        }

        /* Send the path and await a single-byte ack. On disconnect,
           drop this thread's fd and retry from the top. */
        try {
            debug("sending GC root '%s'", printStorePath(path));
            writeFull(fd.get(), printStorePath(path) + "\n", false);
            char c;
            readFull(fd.get(), &c, 1);
            assert(c == '1');
            debug("got ack for GC root '%s'", printStorePath(path));
            break; /* success — socket ack means current GC sees us */
        } catch (SystemError & e) {
            if (e.is(std::errc::broken_pipe) || e.is(std::errc::connection_reset)) {
                debug("GC socket disconnected");
                fd.close();
                continue;
            }
            throw;
        } catch (EndOfFile &) {
            debug("GC socket disconnected");
            fd.close();
            continue;
        }
    }

    /* Record the store path in the temporary roots file so it will be
       seen by a *future* run of the garbage collector. (The current
       GC already saw us via the socket ack above.) No lock is
       required: the current GC is already past `findTempRoots`, and
       a *next* GC will go through findRoots again. */
    auto s = printStorePath(path) + '\0';
    writeFull(_fdTempRoots.lock()->get(), s);
}

static std::string censored = "{censored}";

void LocalStore::findTempRoots(Roots & tempRoots, bool censor)
{
    /* Read the `temproots' directory for per-process temporary root
       files. */
    for (auto & i : DirectoryIterator{tempRootsDir}) {
        checkInterrupt();
        auto name = i.path().filename().string();
        if (name[0] == '.') {
            // Ignore hidden files. Some package managers (notably portage) create
            // those to keep the directory alive.
            continue;
        }
        auto path = i.path();

        pid_t pid = std::stoi(name);

        debug("reading temporary root file %1%", PathFmt(path));
        AutoCloseFD fd(toDescriptor(open(
            path.string().c_str(),
#ifndef _WIN32
            O_CLOEXEC |
#endif
                O_RDWR,
            0666)));
        if (!fd) {
            /* It's okay if the file has disappeared. */
            if (errno == ENOENT)
                continue;
            throw SysError("opening temporary roots file %1%", PathFmt(path));
        }

        /* Try to acquire a write lock without blocking.  This can
           only succeed if the owning process has died.  In that case
           we don't care about its temporary roots. */
        if (lockFile(fd.get(), ltWrite, false)) {
            printInfo("removing stale temporary roots file %1%", PathFmt(path));
            tryUnlink(path);
            writeFull(fd.get(), "d");
            continue;
        }

        /* Read the entire file. */
        auto contents = readFile(fd.get());

        /* Extract the roots. */
        std::string::size_type pos = 0, end;

        while ((end = contents.find((char) 0, pos)) != std::string::npos) {
            auto root = std::string_view(contents).substr(pos, end - pos);
            debug("got temporary root '%s'", root);
            tempRoots[parseStorePath(root)].emplace(censor ? censored : fmt("{temp:%d}", pid));
            pos = end + 1;
        }
    }
}

void LocalStore::findRoots(const std::filesystem::path & path, std::filesystem::file_type type, Roots & roots)
{
    auto foundRoot = [&](const std::filesystem::path & path, const std::filesystem::path & target) {
        try {
            auto storePath = toStorePath(target.string()).first;
            if (isValidPath(storePath))
                roots[std::move(storePath)].emplace(path.string());
            else
                printInfo("skipping invalid root from %1% to %2%", PathFmt(path), PathFmt(target));
        } catch (BadStorePath &) {
        }
    };

    try {

        if (type == std::filesystem::file_type::unknown)
            type = std::filesystem::symlink_status(path).type();

        if (type == std::filesystem::file_type::directory) {
            for (auto & i : DirectoryIterator{path}) {
                checkInterrupt();
                findRoots(i.path(), i.symlink_status().type(), roots);
            }
        }

        else if (type == std::filesystem::file_type::symlink) {
            auto target = readLink(path);
            if (isInStore(target.string()))
                foundRoot(path, target);

            /* Handle indirect roots. */
            else {
                auto parentPath = path.parent_path();
                target = absPath(target, &parentPath);
                if (!pathExists(target)) {
                    if (isInDir(path, config->stateDir.get() / gcRootsDir / "auto")) {
                        printInfo("removing stale link from %1% to %2%", PathFmt(path), PathFmt(target));
                        tryUnlink(path);
                    }
                } else {
                    if (!std::filesystem::is_symlink(target))
                        return;
                    auto target2 = readLink(target);
                    if (isInStore(target2.string()))
                        foundRoot(target, target2);
                }
            }
        }

        else if (type == std::filesystem::file_type::regular) {
            auto storePath = maybeParseStorePath(storeDir + "/" + std::string(baseNameOf(path.string())));
            if (storePath && isValidPath(*storePath))
                roots[std::move(*storePath)].emplace(path.string());
        }

    }

    catch (std::filesystem::filesystem_error & e) {
        /* We only ignore permanent failures. */
        if (e.code() == std::errc::permission_denied || e.code() == std::errc::no_such_file_or_directory
            || e.code() == std::errc::not_a_directory)
            printInfo("cannot read potential root %1%", PathFmt(path));
        else
            throw SystemError(e.code(), "finding GC roots in %1%", PathFmt(path));
    }

    catch (SystemError & e) {
        /* We only ignore permanent failures. */
        if (e.is(std::errc::permission_denied) || e.is(std::errc::no_such_file_or_directory)
            || e.is(std::errc::not_a_directory))
            printInfo("cannot read potential root %1%", PathFmt(path));
        else
            throw;
    }
}

void LocalStore::findRootsNoTemp(Roots & roots, bool censor)
{
    /* Process direct roots in {gcroots,profiles}. */
    findRoots(config->stateDir.get() / gcRootsDir, std::filesystem::file_type::unknown, roots);
    findRoots(config->stateDir.get() / "profiles", std::filesystem::file_type::unknown, roots);

    /* Add additional roots returned by different platforms-specific
       heuristics.  This is typically used to add running programs to
       the set of roots (to prevent them from being garbage collected). */
    findRuntimeRoots(roots, censor);
}

Roots LocalStore::findRoots(bool censor)
{
    Roots roots;
    findRootsNoTemp(roots, censor);

    findTempRoots(roots, censor);

    return roots;
}

static Roots requestRuntimeRoots(const LocalStoreConfig & config, const std::filesystem::path & socketPath)
{
    Roots roots;

    auto socket = connect(socketPath);
    auto socketSource = FdSource(socket.get());

    while (1) {
        auto line = socketSource.readLine(true, '\0');
        if (line == "")
            break;
        roots[config.parseStorePath(line)].insert(censored);
    };

    return roots;
}

void LocalStore::findRuntimeRoots(Roots & roots, bool censor)
{
    Roots unchecked;

    if (config->useRootsDaemon) {
        experimentalFeatureSettings.require(Xp::LocalOverlayStore);
        unchecked = requestRuntimeRoots(*config, config->getRootsSocketPath());
    } else {
        unchecked = findRuntimeRootsUnchecked(*config);
    }

    for (auto & [path, links] : unchecked) {
        if (!isValidPath(path))
            continue;
        debug("got additional root '%1%'", printStorePath(path));
        if (censor)
            roots[path].insert(censored);
        else
            roots[path].insert(links.begin(), links.end());
    }
}

struct GCLimitReached
{};

void LocalStore::collectGarbage(const GCOptions & options, GCResults & results)
{
    const auto & gcSettings = config->getLocalSettings().getGCSettings();

    auto t0 = std::chrono::steady_clock::now();
    auto stage = [&](uint64_t & f) {
        auto t1 = std::chrono::steady_clock::now();
        f += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        t0 = t1;
    };

    bool shouldDelete = options.action == GCOptions::gcDeleteDead || options.action == GCOptions::gcDeleteSpecific;

    boost::unordered_flat_set<StorePath, std::hash<StorePath>> roots, dead, alive;

    /* Return early if nothing to delete */
    if (std::visit(
            overloaded{
                [](const GCOptions::SpecificPaths & pathsToDelete) { return pathsToDelete.paths.empty(); },
                [](const GCOptions::WholeStore & _) { return false; }},
            options.pathsToDelete))
        return;

    struct Shared
    {
        // The temp roots only store the hash part to make it easier to
        // ignore suffixes like '.lock', '.chroot' and '.check'.
        boost::unordered_flat_set<std::string, StringViewHash, std::equal_to<>> tempRoots;

        // Hash part of the store path currently being deleted, if
        // any.
        std::optional<std::string> pending;
    };

    Sync<Shared> _shared;

    std::condition_variable wakeup;

    if (shouldDelete)
        deletePath(reservedPath);

    /* Acquire the global GC root. Note: we don't use fdGCLock
       here because then in auto-gc mode, another thread could
       downgrade our exclusive lock. */
    auto fdGCLock = openGCLock();
    FdLock gcLock(fdGCLock.get(), ltWrite, true, "waiting for the big garbage collector lock...");

    /* Synchronisation point to test ENOENT handling in
       addTempRoot(), see tests/gc-non-blocking.sh. */
    if (auto p = getEnv("_NIX_TEST_GC_SYNC_1"))
        readFile(*p);

    /* Start the server for receiving new roots. */
    auto socketPath = config->stateDir.get() / gcSocketPath;
    createDirs(socketPath.parent_path());
    auto fdServer = createUnixDomainSocket(socketPath, 0666);

    // TODO nonblocking socket on windows?
#ifdef _WIN32
    throw UnimplementedError("External GC client not implemented yet");
#else
    if (fcntl(fdServer.get(), F_SETFL, fcntl(fdServer.get(), F_GETFL) | O_NONBLOCK) == -1)
        throw SysError("making socket %s non-blocking", PathFmt(socketPath));

    Pipe shutdownPipe;
    shutdownPipe.create();

    std::thread serverThread([&]() {
        Sync<std::map<int, std::thread>> connections;

        Finally cleanup([&]() {
            debug("GC roots server shutting down");
            fdServer.close();
            while (true) {
                auto item = remove_begin(*connections.lock());
                if (!item)
                    break;
                auto & [fd, thread] = *item;
                shutdown(fd, SHUT_RDWR);
                thread.join();
            }
        });

        while (true) {
            std::vector<struct pollfd> fds;
            fds.push_back({.fd = shutdownPipe.readSide.get(), .events = POLLIN});
            fds.push_back({.fd = fdServer.get(), .events = POLLIN});
            auto count = poll(fds.data(), fds.size(), -1);
            assert(count != -1);

            if (fds[0].revents)
                /* Parent is asking us to quit. */
                break;

            if (fds[1].revents) {
                /* Accept a new connection. */
                assert(fds[1].revents & POLLIN);
                AutoCloseFD fdClient = accept(fdServer.get(), nullptr, nullptr);
                if (!fdClient)
                    continue;

                debug("GC roots server accepted new client");

                /* Process the connection in a separate thread. */
                auto fdClient_ = fdClient.get();
                std::thread clientThread([&, fdClient = std::move(fdClient)]() {
                    Finally cleanup([&]() {
                        auto conn(connections.lock());
                        auto i = conn->find(fdClient.get());
                        if (i != conn->end()) {
                            i->second.detach();
                            conn->erase(i);
                        }
                    });

                    /* On macOS, accepted sockets inherit the
                       non-blocking flag from the server socket, so
                       explicitly make it blocking. */
                    if (fcntl(fdClient.get(), F_SETFL, fcntl(fdClient.get(), F_GETFL) & ~O_NONBLOCK) == -1)
                        panic("Could not set non-blocking flag on client socket");

                    FdSource source(fdClient.get());
                    while (true) {
                        try {
                            auto path = source.readLine();
                            auto storePath = maybeParseStorePath(path);
                            if (storePath) {
                                debug("got new GC root '%s'", path);
                                auto hashPart = storePath->hashPart();
                                auto shared(_shared.lock());
                                shared->tempRoots.emplace(hashPart);
                                /* If this path is currently being
                                   deleted, then we have to wait until
                                   deletion is finished to ensure that
                                   the client doesn't start
                                   re-creating it before we're
                                   done. FIXME: ideally we would use a
                                   FD for this so we don't block the
                                   poll loop. */
                                while (shared->pending == hashPart) {
                                    debug("synchronising with deletion of path '%s'", path);
                                    shared.wait(wakeup);
                                }
                            } else
                                printError("received garbage instead of a root from client");
                            writeFull(fdClient.get(), "1", false);
                        } catch (Error & e) {
                            debug("reading GC root from client: %s", e.msg());
                            break;
                        }
                    }
                });

                connections.lock()->insert({fdClient_, std::move(clientThread)});
            }
        }
    });

    Finally stopServer([&]() {
        writeFull(shutdownPipe.writeSide.get(), "x", false);
        wakeup.notify_all();
        if (serverThread.joinable())
            serverThread.join();
    });

#endif

    /* Find the roots.  Since we've grabbed the GC lock, the set of
       permanent roots cannot increase now. */
    printInfo("finding garbage collector roots...");
    Roots rootMap;
    if (!options.ignoreLiveness)
        findRootsNoTemp(rootMap, true);

    for (auto & i : rootMap)
        roots.insert(i.first);

    /* Read the temporary roots created before we acquired the global
       GC root. Any new roots will be sent to our socket. */
    Roots tempRoots;
    findTempRoots(tempRoots, true);
    for (auto & root : tempRoots) {
        _shared.lock()->tempRoots.emplace(root.first.hashPart());
        roots.insert(root.first);
    }

    /* Synchronisation point for testing, see tests/functional/gc-non-blocking.sh. */
    if (auto p = getEnv("_NIX_TEST_GC_SYNC_2"))
        readFile(*p);

    /* End-of-roots-collection boundary. Everything up to here is
       socket setup + root enumeration. The DB snapshot below is the
       next stage. */
    stage(results.timings.findRootsNs);

    /* Two-phase deletion state.

       Phase 1 (during the traversal, still under `fdGCLock`) renames
       every dead path to `.gc-<pid>-<counter>-<basename>` under the
       store dir. The rename is journaled atomically on
       ext4/XFS/btrfs, so on crash or SIGINT the live namespace is
       always consistent: the path is either present or gone, never
       half-deleted. `.gc-` prefix guarantees `maybeParseStorePath`
       rejects the name (the leading `.` is not in the base-32
       hash alphabet), so no existing code can misinterpret an
       orphan as a live path.

       Phase 2 (later in this function, still under `fdGCLock` — a
       future refinement may release the lock first) recursively
       removes the renamed-aside orphans in parallel via the
       thread pool.

       PID reuse safety: the counter is seeded from
       `phase2DeleteQueue.size()`, which starts at whatever the
       start-of-run sweep picked up. If an old daemon with the same
       pid crashed leaving M orphans `.gc-<pid>-0..M-1-*`, the new
       daemon's sweep enqueues all M, so its first rename starts at
       counter M — no collision. */
    std::vector<std::filesystem::path> phase2DeleteQueue;
    std::string gcOrphanPrefix = fmt(".gc-%d-", getpid());

    /* Start-of-run sweep for orphaned rename-aside entries from a
       prior GC that crashed between phase 1 (rename) and phase 2
       (recursive delete). Any `.gc-*` entry in the store dir has
       an already-invalidated DB row (phase 1 invalidates before
       renaming), so it's pure filesystem work. Enqueue them into
       the phase-2 queue so they get deleted alongside the current
       run's orphans, in parallel.

       Only runs when we're actually going to delete — `gcReturnLive`
       and `gcReturnDead` both short-circuit before phase 2, so
       queueing orphans in those modes would silently drop them.
       Also skip for stores that don't use the two-phase scheme in
       the first place; they never create `.gc-*` entries. */
    if (shouldDelete && supportsTwoPhaseDelete()) {
        AutoCloseDir dir(opendir(config->realStoreDir.get().string().c_str()));
        if (!dir)
            throw SysError("opening directory %1%", PathFmt(config->realStoreDir.get()));
        struct dirent * dirent;
        size_t reaped = 0;
        while (errno = 0, dirent = readdir(dir.get())) {
            checkInterrupt();
            const char * name = dirent->d_name;
            if (std::string_view(name).starts_with(".gc-")) {
                phase2DeleteQueue.push_back(config->realStoreDir.get() / name);
                ++reaped;
            }
        }
        if (errno)
            throw SysError("reading directory %1%", PathFmt(config->realStoreDir.get()));
        if (reaped)
            printInfo("reaping %d orphans from a previous interrupted GC", reaped);
    }

    /* Bulk-loaded snapshot of the ValidPaths and Refs tables, taken
       once while holding `_state`, used for lock-free lookups during
       the traversal. Replaces millions of per-path SQLite queries
       (`queryGCReferrers`, `isValidPath`, `topoSortPaths` via
       `queryPathInfo`) with in-memory hash lookups on identity-hashed
       int64 ids.

       Consistency is guaranteed because:
       - We hold the file-based `fdGCLock` exclusively, so no other
         Nix process can mutate the DB.
       - Our own process mutates the DB only via `invalidatePathsChecked`
         in this function, which only removes paths we've already
         decided are dead (their rows can't be referenced by the
         traversal any more).

       Memory on a 4M-path / 32M-edge store is ~1.3 GB resident
       end-to-end:
         ~500 MB: two path<->id hashmaps (each ~80 B/entry
                  including the `StorePath` heap string)
         ~500 MB: two referrer/reference edge-list maps
         ~150 MB: `deriverById` + `outputsByDrvId` +
                  `deriversByOutputId` (most paths have a deriver,
                  DerivationOutputs mirrored in two directions)
         ~30 MB:  narSizeById
       Plus ~200 MB transient during scan for the `pendingDerivers`
       string vector. Amortised against millions of SQLite
       statement executions and the `_state`-mutex serialisation
       they imposed. */
    struct GCRefsSnapshot
    {
        /* id -> path, used to materialise `StorePath`s for the traversal
           boundary (roots lookup, tempRoots lookup, error messages,
           enqueue, invalidatePathsChecked, deleteFromStore). */
        boost::unordered_flat_map<int64_t, StorePath> idToPath;

        /* path -> id, used to resolve incoming `StorePath`s (outer
           walk, `queryPartialDerivationOutputMap`, `queryValidDerivers`)
           into id space. */
        boost::unordered_flat_map<StorePath, int64_t> pathToId;

        /* Forward edges (referrer id -> reference ids). Used by the
           topo-sort pass to linearise the deletion set in refs-first
           order, and by `markAliveClosure` to walk a live path's
           forward closure without hitting SQLite. */
        boost::unordered_flat_map<int64_t, std::vector<int64_t>> referencesById;

        /* Reverse edges (reference id -> referrer ids). Drives the
           main traversal — replaces `queryGCReferrers`. */
        boost::unordered_flat_map<int64_t, std::vector<int64_t>> referrersById;

        /* id -> NAR size. Read from `ValidPaths.narSize` for bytes-
           freed accounting at rename time (two-phase delete can't
           measure actual FS bytes freed, since the recursive delete
           runs after the rename and outside the GC lock, so we
           estimate from narSize which is typically very close). */
        boost::unordered_flat_map<int64_t, uint64_t> narSizeById;

        /* id -> deriver id (only when the deriver itself is a live
           ValidPaths entry). Mirrors the
           `info->deriver && isValidPath(*info->deriver)` filter in
           the old `computeFSClosure` forward walk. Consulted by
           `markAliveClosure` when `keepDerivations` is set. */
        boost::unordered_flat_map<int64_t, int64_t> deriverById;

        /* drv id -> output path ids (pre-filtered to those that are
           also live ValidPaths entries). Mirrors the
           `queryPartialDerivationOutputMap + isValidPath` pair in
           the old `computeFSClosure` forward walk. Consulted by
           `markAliveClosure` when `keepOutputs` is set. Only
           populated from `DerivationOutputs`; realisations-resolved
           outputs (ca-derivations) require reading `.drv` files and
           are not precomputed here — that case falls back to the
           SQLite path. */
        boost::unordered_flat_map<int64_t, std::vector<int64_t>> outputsByDrvId;

        /* output path id -> drv path ids (the reverse of
           `outputsByDrvId`). Replaces `queryValidDerivers` during
           the main traversal: given a live output path, pull its
           derivation(s) alive when `keep-outputs` is set. */
        boost::unordered_flat_map<int64_t, std::vector<int64_t>> deriversByOutputId;
    };

    GCRefsSnapshot snapshot;
    {
        printInfo("loading valid paths and references...");
        auto state(_state->lock());
        /* Collect (referencing-id, deriver-string) pairs during the
           ValidPaths scan; resolve them against `pathToId` in a
           post-pass, since the deriver's own id may be interleaved
           with the row that names it. */
        std::vector<std::pair<int64_t, std::string>> pendingDerivers;
        {
            SQLiteStmt stmt(state->db,
                "select id, path, narSize, deriver from ValidPaths");
            auto use(stmt.use());
            while (use.next()) {
                checkInterrupt();
                auto id = use.getInt(0);
                auto path = parseStorePath(use.getStr(1));
                auto narSize = static_cast<uint64_t>(use.getInt(2));
                snapshot.narSizeById.emplace(id, narSize);
                if (!use.isNull(3))
                    pendingDerivers.emplace_back(id, use.getStr(3));
                snapshot.pathToId.emplace(path, id);
                snapshot.idToPath.emplace(id, std::move(path));
            }
        }
        for (auto & [childId, deriverStr] : pendingDerivers) {
            checkInterrupt();
            try {
                auto deriverPath = parseStorePath(deriverStr);
                if (auto dIt = snapshot.pathToId.find(deriverPath);
                    dIt != snapshot.pathToId.end())
                    snapshot.deriverById.emplace(childId, dIt->second);
            } catch (BadStorePath &) {
                /* A malformed deriver string in the DB is a
                   pre-existing data-corruption condition. The old
                   `computeFSClosure` path would have surfaced this
                   as an uncaught `BadStorePath` crashing the whole
                   GC (its outer catch only matched `InvalidPath`).
                   We tolerate it here because crashing GC on a
                   data-integrity problem is user-hostile and the
                   only consequence is that this particular edge is
                   missed — the closure walk is still correct for
                   every well-formed row. */
            }
        }
        {
            SQLiteStmt stmt(state->db, "select referrer, reference from Refs");
            auto use(stmt.use());
            while (use.next()) {
                checkInterrupt();
                auto referrer = use.getInt(0);
                auto reference = use.getInt(1);
                snapshot.referencesById[referrer].push_back(reference);
                snapshot.referrersById[reference].push_back(referrer);
            }
        }
        {
            SQLiteStmt stmt(state->db, "select drv, path from DerivationOutputs");
            auto use(stmt.use());
            while (use.next()) {
                checkInterrupt();
                auto drvId = use.getInt(0);
                try {
                    auto outputPath = parseStorePath(use.getStr(1));
                    if (auto pIt = snapshot.pathToId.find(outputPath);
                        pIt != snapshot.pathToId.end()) {
                        auto outId = pIt->second;
                        snapshot.outputsByDrvId[drvId].push_back(outId);
                        snapshot.deriversByOutputId[outId].push_back(drvId);
                    }
                } catch (BadStorePath &) {
                    /* Same tolerance as for deriver strings: the
                       old path would have crashed GC with an
                       uncaught `BadStorePath`; we drop the edge
                       instead. */
                }
            }
        }
    }
    debug(
        "loaded %d valid paths with %d ref edges, %d derivers, %d drv-output rows",
        snapshot.pathToId.size(),
        snapshot.referrersById.size(),
        snapshot.deriverById.size(),
        snapshot.outputsByDrvId.size());

    /* End-of-snapshot boundary. The traversal + Phase 1 invalidate-
       and-rename starts immediately after the helper closures are
       defined below. */
    stage(results.timings.loadValidPathsNs);

    /* Whether we can compute derivation-/output-related edges
       purely in-memory using the snapshot, or whether we need to
       fall back to the virtual SQLite-backed queries.

       Two reasons to fall back:
       - `ca-derivations`: `queryPartialDerivationOutputMap` on a
         ca-derivations store resolves outputs through
         `Realisations` + `.drv` reads, which we can't amortise via
         bulk load (realisations are keyed on output-hash, not
         drv-id). Running `markAlive`'s forward closure from the
         snapshot alone would miss realisation-resolved outputs.
       - `LocalOverlayStore`: `queryValidDerivers` is overridden to
         union upper + lower, and our snapshot is upper-only. Using
         the snapshot would silently drop lower-store derivers. The
         same concern doesn't apply to `LocalStore::queryValidDerivers`
         which is upper-only by construction, so plain `LocalStore`
         stays on the fast path. */
    const bool drvClosureFromSnapshot =
        !experimentalFeatureSettings.isEnabled(Xp::CaDerivations)
        && supportsTwoPhaseDelete(); /* serves as a "plain LocalStore" proxy */

    /* Whether we can rename dead paths aside and batch-delete them
       later. False for stores (e.g. `LocalOverlayStore`) where a
       plain rename within the store dir has different semantics
       than a full delete — on overlay, renaming a dual-layer path
       within the upper would reveal the lower copy. Those stores
       fall back to synchronous `deleteStorePath` per dead path. */
    const bool twoPhase = supportsTwoPhaseDelete();

    /* Rename a dead store path aside into the phase-2 delete queue,
       or (for non-two-phase stores) delete it synchronously.
       Accounts for bytes-to-be-freed using `narSize` from the
       snapshot (best available estimate — actual FS bytes freed may
       differ slightly when the path shares inodes via `.links/`).
       Throws `GCLimitReached` if the estimate crosses `maxFreed`,
       preserving the pre-two-phase termination semantics. */
    auto scheduleDelete = [&](const std::filesystem::path & realPath) {
        auto baseName = realPath.filename().string();
        auto logicalPath = storeDir + "/" + baseName;

        /* Accounting is based on snapshot narSize whether we rename
           aside or delete inline. For the two-phase path we must
           estimate (the real total isn't known until phase 2, after
           the GCLimitReached decision). For the inline path we
           *could* use the real subtree total but deliberately
           don't, so both paths agree on the meaning of
           `results.bytesFreed`. Under-counting from the `nlink<=2`
           filter in `_deletePath` would otherwise make overlay
           stores report smaller byte totals than two-phase stores
           for identical workloads. */
        uint64_t estimated = 0;
        if (auto storePath = maybeParseStorePath(logicalPath))
            if (auto idIt = snapshot.pathToId.find(*storePath); idIt != snapshot.pathToId.end())
                if (auto nIt = snapshot.narSizeById.find(idIt->second); nIt != snapshot.narSizeById.end())
                    estimated = nIt->second;

        if (twoPhase) {
            auto orphanName =
                fmt("%s%d-%s",
                    gcOrphanPrefix,
                    phase2DeleteQueue.size(),
                    baseName);
            auto orphanPath = config->realStoreDir.get() / orphanName;

            std::error_code ec;
            std::filesystem::rename(realPath, orphanPath, ec);
            if (ec) {
                if (ec == std::errc::no_such_file_or_directory)
                    return; /* concurrent delete or already-gone */
                throw SystemError(ec, "renaming %1% to %2%", PathFmt(realPath), PathFmt(orphanPath));
            }
            phase2DeleteQueue.push_back(orphanPath);
        } else {
            /* Store can't safely two-phase; delete synchronously
               through the virtual path so overlay-aware logic
               (whiteouts, remount-required flag) runs. We discard
               the `deleteStorePath`-reported byte total and use the
               snapshot estimate for `results.bytesFreed` below, to
               keep accounting consistent with the two-phase branch. */
            printInfo("deleting '%1%'", logicalPath);
            uint64_t discardedBytes = 0;
            try {
                deleteStorePath(realPath, discardedBytes, /*isKnownPath=*/true);
            } catch (SysError & e) {
                if (e.errNo == ENOENT)
                    return;
                throw;
            }
        }

        results.paths.insert(logicalPath);
        results.bytesFreed += estimated;

        if (results.bytesFreed > options.maxFreed) {
            printInfo("freed more than %d bytes for deletion; stopping", options.maxFreed);
            throw GCLimitReached();
        }
    };

    /* Helper function that deletes a path from the store and throws
       GCLimitReached if we've deleted enough garbage.

       Used for small-volume, non-batched deletions: orphan FS
       content found in the store directory (tmp-*, unparseable
       names, previously-leaked chroot dirs). Batched dead-path
       deletion goes through `scheduleDelete` above. */
    auto deleteFromStore = [&](std::string_view baseName, bool isKnownPath) {
        assert(!std::filesystem::path(baseName).is_absolute());
        /* Using `std::string` since this is the logical store dir. Hopefully that is the right choice. */
        std::string path = storeDir + "/" + std::string(baseName);
        auto realPath = config->realStoreDir.get() / std::string(baseName);

        /* There may be temp directories in the store that are still in use
           by another process. We need to be sure that we can acquire an
           exclusive lock before deleting them. */
        if (baseName.find("tmp-", 0) == 0) {
            /* TODO Reconsider whether Follow is the right choice, here */
            auto tmpDirFd = openDirectory(realPath, FinalSymlink::Follow);
            if (!tmpDirFd || !lockFile(tmpDirFd.get(), ltWrite, false)) {
                debug("skipping locked tempdir %s", PathFmt(realPath));
                return;
            }
        }

        printInfo("deleting '%1%'", path);

        results.paths.insert(path);

        uint64_t bytesFreed;
        deleteStorePath(realPath, bytesFreed, isKnownPath);

        results.bytesFreed += bytesFreed;

        if (results.bytesFreed > options.maxFreed) {
            printInfo("deleted more than %d bytes; stopping", options.maxFreed);
            throw GCLimitReached();
        }
    };

    /* Helper function that visits all paths reachable from `start`
       via the referrers edges and optionally derivers and derivation
       output edges. If none of those paths are roots, then all
       visited paths are garbage and are deleted. */
    auto maybeDeleteReferrersClosure = [&](const StorePath & start) {
        /* Hot-loop sets are int64_t-keyed (identity-hashed) to keep
           the traversal's per-step cost cache-line-tight. Orphan
           paths (not in the snapshot because they're rogue FS
           content rather than DB-registered paths) are tracked in a
           parallel StorePathSet because they have no id. */
        boost::unordered_flat_set<int64_t> visitedIds;
        StorePathSet visitedOrphans;
        std::queue<StorePath> todo;

        /* Wake up any GC client waiting for deletion of the paths in
           'visited' to finish. */
        Finally releasePending([&]() {
            auto shared(_shared.lock());
            shared->pending.reset();
            wakeup.notify_all();
        });

        /* Hoist the variant dispatch once per closure call: the three
           `std::visit` sites inside the traversal all read the same
           variant. */
        const auto * specificPaths =
            std::get_if<GCOptions::SpecificPaths>(&options.pathsToDelete);
        const bool isWholeStore = !specificPaths;

        auto enqueue = [&](const StorePath & path) {
            if (auto idIt = snapshot.pathToId.find(path); idIt != snapshot.pathToId.end()) {
                if (visitedIds.insert(idIt->second).second)
                    todo.push(path);
            } else {
                if (visitedOrphans.insert(path).second)
                    todo.push(path);
            }
        };

        auto enqueueById = [&](int64_t id) {
            if (visitedIds.insert(id).second)
                todo.push(snapshot.idToPath.at(id));
        };

        enqueue(start);

        while (auto path = pop(todo)) {
            checkInterrupt();

            /* Bail out if we've previously discovered that this path
               is alive. */
            if (alive.contains(*path)) {
                debug("cannot delete '%s' because '%s' is alive", printStorePath(start), printStorePath(*path));
                alive.insert(start);
                return;
            }

            /* If we've previously deleted this path, we don't have to
               handle it again. */
            if (dead.contains(*path))
                continue;

            auto markAlive = [&]() {
                alive.insert(*path);
                alive.insert(start);

                /* Only include outputs/derivers in the forward
                   closure for `WholeStore` — `SpecificPaths` runs
                   never recurse into kept outputs/derivers (matches
                   the main-traversal gating below). */
                const bool includeOutputs = isWholeStore && gcSettings.keepOutputs;
                const bool includeDerivers = isWholeStore && gcSettings.keepDerivations;

                /* Fast path: traverse the forward closure in memory
                   against the snapshot's precomputed edges, avoiding
                   the 2-to-4 SQLite statements per visited path that
                   `computeFSClosure` would otherwise issue. Only used
                   when `ca-derivations` is off — see
                   `drvClosureFromSnapshot` above for the rationale. */
                if (drvClosureFromSnapshot) {
                    auto startIdIt = snapshot.pathToId.find(*path);
                    if (startIdIt == snapshot.pathToId.end())
                        return; /* orphan / not in ValidPaths */

                    boost::unordered_flat_set<int64_t> closureIds;
                    std::queue<int64_t> closureTodo;
                    closureIds.insert(startIdIt->second);
                    closureTodo.push(startIdIt->second);

                    while (auto idOpt = pop(closureTodo)) {
                        checkInterrupt();
                        auto id = *idOpt;
                        alive.insert(snapshot.idToPath.at(id));

                        /* Forward refs (path depends on these). */
                        if (auto rIt = snapshot.referencesById.find(id);
                            rIt != snapshot.referencesById.end())
                            for (auto refId : rIt->second)
                                if (refId != id && closureIds.insert(refId).second)
                                    closureTodo.push(refId);

                        /* keep-outputs: a live derivation keeps its
                           statically-registered outputs alive too.
                           Only derivations have entries in
                           `outputsByDrvId`, but we check the name
                           first to skip the hashmap lookup on the
                           overwhelming majority of paths. */
                        if (includeOutputs && snapshot.idToPath.at(id).isDerivation()) {
                            if (auto oIt = snapshot.outputsByDrvId.find(id);
                                oIt != snapshot.outputsByDrvId.end())
                                for (auto outId : oIt->second)
                                    if (closureIds.insert(outId).second)
                                        closureTodo.push(outId);
                        }

                        /* keep-derivations: a live output keeps its
                           deriver alive (only when the deriver is
                           itself a live ValidPaths row). */
                        if (includeDerivers) {
                            if (auto dIt = snapshot.deriverById.find(id);
                                dIt != snapshot.deriverById.end())
                                if (closureIds.insert(dIt->second).second)
                                    closureTodo.push(dIt->second);
                        }
                    }
                    return;
                }

                /* ca-derivations fallback: the realisation-resolved
                   outputs can't be derived from the snapshot, so use
                   the SQLite-backed closure walk. */
                try {
                    StorePathSet closure;
                    computeFSClosure(
                        *path,
                        closure,
                        /* flipDirection */ false,
                        includeOutputs,
                        includeDerivers);
                    for (auto & p : closure)
                        alive.insert(p);
                } catch (InvalidPath &) {
                }
            };

            /* If this is a root, bail out. */
            if (roots.contains(*path)) {
                debug("cannot delete '%s' because it's a root", printStorePath(*path));
                return markAlive();
            }

            if (specificPaths
                && !specificPaths->deleteReferrers
                && !specificPaths->paths.contains(*path)) {
                debug(
                    "cannot delete '%s' because '%s' is not in the specified paths to delete",
                    printStorePath(start),
                    printStorePath(*path));
                return;
            }

            {
                auto hashPart = path->hashPart();
                auto shared(_shared.lock());
                if (shared->tempRoots.contains(hashPart)) {
                    debug("cannot delete '%s' because it's a temporary root", printStorePath(*path));
                    return markAlive();
                }
                shared->pending = hashPart;
            }

            /* Resolve to the snapshot id once; all subsequent
               per-path lookups use it. An absent id means the path
               isn't in `ValidPaths` — i.e. it's orphan FS content
               (the outer store-dir walk may pass us such paths). */
            if (auto idIt = snapshot.pathToId.find(*path); idIt != snapshot.pathToId.end()) {
                auto id = idIt->second;

                /* Visit the referrers of this path. Use the id-keyed
                   `enqueueById` to skip the path-hash lookup in the
                   inner loop. */
                if (auto refIt = snapshot.referrersById.find(id); refIt != snapshot.referrersById.end())
                    for (auto referrerId : refIt->second)
                        enqueueById(referrerId);

                if (isWholeStore) {
                    /* keepDerivations: a live derivation pulls its
                       statically-registered outputs into the traversal.
                       Preserves the pre-patch filter that the output
                       must have been registered under *this* derivation
                       (`info->deriver == *path` in the old code), which
                       maps to `snapshot.deriverById[outId] == id`. The
                       ca-derivations realisation path isn't precomputed
                       in the snapshot — for those stores, fall through
                       to the SQLite path. */
                    if (gcSettings.keepDerivations && path->isDerivation()) {
                        if (drvClosureFromSnapshot) {
                            if (auto oIt = snapshot.outputsByDrvId.find(id);
                                oIt != snapshot.outputsByDrvId.end())
                                for (auto outId : oIt->second)
                                    if (auto dIt = snapshot.deriverById.find(outId);
                                        dIt != snapshot.deriverById.end() && dIt->second == id)
                                        enqueueById(outId);
                        } else {
                            for (auto & [name, maybeOutPath] : queryPartialDerivationOutputMap(*path))
                                if (maybeOutPath && isValidPath(*maybeOutPath)
                                    && queryPathInfo(*maybeOutPath)->deriver == *path)
                                    enqueue(*maybeOutPath);
                        }
                    }

                    /* keepOutputs: a live output pulls its derivation
                       into the traversal. Uses the reverse of
                       `outputsByDrvId` to skip `queryValidDerivers` on
                       plain `LocalStore`; on overlay stores (which
                       union upper+lower derivers) fall back to the
                       virtual query to preserve the lower-store
                       contribution. */
                    if (gcSettings.keepOutputs) {
                        if (drvClosureFromSnapshot) {
                            if (auto dIt = snapshot.deriversByOutputId.find(id);
                                dIt != snapshot.deriversByOutputId.end())
                                for (auto drvId : dIt->second)
                                    enqueueById(drvId);
                        } else {
                            for (auto & i : queryValidDerivers(*path))
                                enqueue(i);
                        }
                    }
                }
            }
        }
        /* Collect the set we will actually delete, topologically
           sorted so refs are dropped before the things they reference.
           `dead.insert` has side effects (marks as processed), so we
           must iterate the sorted order even for non-deleting modes.

           Sort directly on the snapshot's id-keyed forward-edge map,
           avoiding the `queryPathInfo` SQLite round-trip that the
           generic `topoSortPaths` would do per visited path.
           Orphans have no ids and no references; they can be appended
           in any order since they have no dependency edges. */
        std::set<int64_t> visitedIdsSorted(visitedIds.begin(), visitedIds.end());
        auto sortedVariant = topoSort(visitedIdsSorted, [&](int64_t id) -> std::set<int64_t> {
            std::set<int64_t> refs;
            if (auto rIt = snapshot.referencesById.find(id); rIt != snapshot.referencesById.end())
                for (auto refId : rIt->second)
                    refs.insert(refId);
            return refs;
        });
        auto * sortedIds = std::get_if<std::vector<int64_t>>(&sortedVariant);
        if (!sortedIds) {
            auto & cycle = std::get<Cycle<int64_t>>(sortedVariant);
            throw Error(
                "cycle detected in the references of '%s' from '%s'",
                printStorePath(snapshot.idToPath.at(cycle.path)),
                printStorePath(snapshot.idToPath.at(cycle.parent)));
        }

        std::vector<StorePath> toInvalidate;
        toInvalidate.reserve(sortedIds->size() + visitedOrphans.size());
        for (auto id : *sortedIds) {
            const auto & path = snapshot.idToPath.at(id);
            if (!dead.insert(path).second)
                continue;
            if (shouldDelete)
                toInvalidate.push_back(path);
        }
        for (auto & path : visitedOrphans) {
            if (!dead.insert(path).second)
                continue;
            if (shouldDelete)
                toInvalidate.push_back(path);
        }

        if (shouldDelete && !toInvalidate.empty()) {
            /* Batch DB invalidations to amortise SQLite fsync cost.
               Interleave with filesystem rename-aside in chunks so
               GCLimitReached cleanly bounds how much gets queued
               for phase-2 deletion. */
            constexpr size_t batchSize = 256;

            for (size_t start = 0; start < toInvalidate.size(); start += batchSize) {
                size_t end = std::min(start + batchSize, toInvalidate.size());
                std::vector<StorePath> chunk(
                    toInvalidate.begin() + start, toInvalidate.begin() + end);

                try {
                    invalidatePathsChecked(chunk);
                    for (auto & path : chunk)
                        scheduleDelete(config->realStoreDir.get() / path.to_string());
                } catch (PathInUse &) {
                    /* Fall back to per-path to report the offending
                       path precisely (matches legacy behaviour). */
                    for (auto & path : chunk) {
                        try {
                            invalidatePathChecked(path);
                        } catch (PathInUse & e) {
                            // https://github.com/NixOS/nix/issues/11923
                            printError("BUG: %s", e.what());
                            continue;
                        }
                        scheduleDelete(config->realStoreDir.get() / path.to_string());
                    }
                }
            }
        }
    };

    try {
        /* Either delete all garbage paths, or just the specified paths. */
        std::visit(
            overloaded{
                [&](const GCOptions::SpecificPaths & pathsToDelete) {
                    switch (options.action) {
                    case GCOptions::gcDeleteDead:
                        printInfo("deleting garbage within specified paths...");
                        break;
                    case GCOptions::gcDeleteSpecific:
                        printInfo("deleting specified paths...");
                        break;
                    case GCOptions::gcReturnDead:
                    case GCOptions::gcReturnLive:
                        printInfo("determining live/dead paths...");
                    }

                    for (auto & i : pathsToDelete.paths) {
                        maybeDeleteReferrersClosure(i);

                        if (options.action == GCOptions::gcDeleteSpecific && !dead.contains(i))
                            throw Error(
                                "Cannot delete path '%1%' since it is still alive. "
                                "To find out why, use: "
                                "nix-store --query --roots and nix-store --query --referrers",
                                printStorePath(i));
                        else if (!dead.contains(i))
                            debug("cannot delete '%s' because it's still alive", printStorePath(i));
                    }
                },
                [&](const GCOptions::WholeStore & _) {
                    if (options.maxFreed == 0)
                        return;

                    switch (options.action) {
                    case GCOptions::gcDeleteDead:
                        printInfo("deleting garbage...");
                        break;
                    case GCOptions::gcDeleteSpecific:
                        throw Error("Cannot delete the entire store");
                    case GCOptions::gcReturnDead:
                    case GCOptions::gcReturnLive:
                        printInfo("determining live/dead paths...");
                    }

                    AutoCloseDir dir(opendir(config->realStoreDir.get().string().c_str()));
                    if (!dir)
                        throw SysError("opening directory %1%", PathFmt(config->realStoreDir.get()));

                    /* Read the store and delete all paths that are invalid or
                    unreachable. We don't use readDirectory() here so that
                    GCing can start faster. */
                    auto linksName = linksDir.filename();
                    struct dirent * dirent;
                    while (errno = 0, dirent = readdir(dir.get())) {
                        checkInterrupt();
                        std::string name = dirent->d_name;
                        if (name == "." || name == ".." || name == linksName)
                            continue;

                        /* Skip orphans queued by the start-of-run sweep and
                           any entries this very readdir might see that were
                           renamed aside by a prior iteration of this loop
                           — `scheduleDelete` inside `maybeDeleteReferrersClosure`
                           creates `.gc-*` entries in the same directory
                           we're iterating, and POSIX `readdir` may or may
                           not return newly-added entries. */
                        if (name.starts_with(".gc-"))
                            continue;

                        if (auto storePath = maybeParseStorePath(storeDir + "/" + name))
                            maybeDeleteReferrersClosure(*storePath);
                        else
                            deleteFromStore(name, false);
                    }
                },
            },
            options.pathsToDelete);
    } catch (GCLimitReached & e) {
    }

    if (options.action == GCOptions::gcReturnLive) {
        for (auto & i : alive)
            results.paths.insert(printStorePath(i));
        return;
    }

    if (options.action == GCOptions::gcReturnDead) {
        for (auto & i : dead)
            results.paths.insert(printStorePath(i));
        return;
    }

    /* End-of-traversal boundary. Includes the helper-closure
       definitions, the closure walk, and Phase 1 (invalidate +
       rename-aside) for every dead path. */
    stage(results.timings.traverseAndPhase1Ns);

    /* Phase 2 of the two-phase delete: recursively remove every
       `.gc-*` orphan we queued. The live store namespace has
       already been made consistent by the phase-1 renames under
       the GC lock; this step is pure filesystem work. Parallelise
       across `gc-delete-threads` workers — each store-path subtree
       is independent, so `_deletePath`'s single-threaded `readdir`
       per directory remains safe.

       We call `nix::deletePath` directly rather than the virtual
       `deleteStorePath` for two reasons:
       1. We already hand-inline the `ignoreGcDeleteFailure` logic
          below, so the virtual wrapping buys us nothing.
       2. Phase 2 only runs for stores where
          `supportsTwoPhaseDelete()` returned true, which excludes
          `LocalOverlayStore` — whose override of `deleteStorePath`
          would try to parse the basename as a `StorePath` and
          throw `BadStorePath` on our `.gc-*` orphan names.

       Bytes-freed accounting is *not* updated here because
       `_deletePath` accumulates `st_size` per inode only when
       `nlink <= 2`, under-counting heavily deduped files (ext4's
       `.links/` hardlinking pushes many of our store files to
       `nlink = 3`). The narSize estimate charged at rename time
       is closer to the logical content total; we prefer it for
       both the `GCLimitReached` rate limit and `results.bytesFreed`
       reporting. */
    if (!phase2DeleteQueue.empty()) {
        printInfo("recursively deleting %d dead paths...", phase2DeleteQueue.size());
        auto nThreads =
            nix::Settings::resolveThreadCount(config->getLocalSettings().getGCSettings().gcDeleteThreads.get());

        if (useIoUringDispatch(*config)) {
            /* io_uring fanout across orphans: walks subtrees with TBB
               (purely CPU) to collect files+dirs, then bulk-unlinks via
               a single high-QD ring. Across orphans this exposes far
               more parent-directory parallelism than gc-delete-threads
               TBB workers (default 4); within a single orphan dir the
               i_rwsem still serialises, but that's the same in both
               paths. See cleanupOrphansIoUring docstring for caveats. */
            cleanupOrphansIoUring(
                phase2DeleteQueue, nThreads, config->ignoreGcDeleteFailure);
        } else {
            tbb::task_arena arena(static_cast<int>(nThreads));
            arena.execute([&] {
                tbb::parallel_for_each(phase2DeleteQueue, [&](const std::filesystem::path & orphan) {
                    checkInterrupt();
                    try {
                        uint64_t bytesFreedTree = 0;
                        deletePath(orphan, bytesFreedTree);
                    } catch (SysError & e) {
                        if (e.errNo == ENOENT)
                            return; /* vanished between rename and delete */
                        if (config->ignoreGcDeleteFailure) {
                            logWarning({.msg = HintFmt(
                                            "ignoring failure to reap orphan %1%: %2%",
                                            PathFmt(orphan),
                                            e.info().msg)});
                            return;
                        }
                        throw;
                    }
                });
            });
        }
    }

    /* End-of-Phase-2 boundary. The `.links/` sweep below is the
       last stage we time. */
    stage(results.timings.phase2DeleteNs);

    /* Unlink all files in /nix/store/.links that have a link count of 1,
       which indicates that there are no other links and so they can be
       safely deleted.

       The race condition with optimisePath() where we might see a link
       count of 1 just before optimisePath() increases it is now handled
       on the optimise side: optimisePath_ retries canonical-link
       creation on ENOENT. */
    if (options.action == GCOptions::gcDeleteDead || options.action == GCOptions::gcDeleteSpecific) {
        printInfo("deleting unused links...");

        /* Enumerate entries first so the DIR* is released quickly and
           workers can fan out over independent lstat/unlink pairs. We
           walk both layouts unconditionally to handle stores that
           were toggled between sharded and flat modes.

           `DirectoryIterator::is_directory()` is cached from the
           entry's `d_type` by libstdc++, so no follow-up stat. */
        std::vector<std::filesystem::path> entries;
        entries.reserve(1 << 16);
        forEachLinkEntry([&](const std::filesystem::path & p) {
            entries.push_back(p);
        });

        /* Cache-line-aligned to avoid false sharing between the two
           counters under high worker counts. */
        alignas(std::hardware_destructive_interference_size) std::atomic<int64_t> actualSize{0};
        alignas(std::hardware_destructive_interference_size) std::atomic<int64_t> unsharedSize{0};

        auto nThreads =
            nix::Settings::resolveThreadCount(config->getLocalSettings().getGCSettings().gcLinksThreads.get());

        if (useIoUringDispatch(*config)) {
            cleanupLinksIoUring(
                linksDir, entries, actualSize, unsharedSize, nThreads);
        } else {
            tbb::task_arena arena(static_cast<int>(nThreads));
            arena.execute([&] {
            tbb::parallel_for_each(entries, [&](const std::filesystem::path & path) {
                checkInterrupt();
                /* A concurrent optimise or another GC pass may have
                   removed this entry; maybeLstat returns std::nullopt
                   for ENOENT/ENOTDIR. */
                auto stOpt = maybeLstat(path);
                if (!stOpt)
                    return;

                if (stOpt->st_nlink != 1) {
                    actualSize.fetch_add(stOpt->st_size, std::memory_order_relaxed);
                    unsharedSize.fetch_add(
                        (stOpt->st_nlink - 1) * stOpt->st_size, std::memory_order_relaxed);
                    return;
                }

                printMsg(lvlTalkative, "deleting unused link %1%", PathFmt(path));
                unlinkIfExists(path);
                /* Do not account for deleted file here. Rely on
                   deletePath() accounting. */
            });
            });
        }

        int64_t overhead =
#ifdef _WIN32
            0
#else
            [&] {
                auto st = stat(linksDir);
                return st.st_blocks * 512ULL;
            }()
#endif
            ;

        printInfo(
            "note: hard linking is currently saving %s",
            renderSize(
                unsharedSize.load(std::memory_order_relaxed)
                - actualSize.load(std::memory_order_relaxed) - overhead));
    }

    /* End-of-cleanupLinks boundary. Includes the directory walk and
       the parallel/io_uring stat+unlink pass over all canonicals. */
    stage(results.timings.cleanupLinksNs);

    /* While we're at it, vacuum the database. */
    // if (options.action == GCOptions::gcDeleteDead) vacuumDB();
}

void LocalStore::autoGC(bool sync)
{
#if HAVE_STATVFS
    const auto & gcSettings = config->getLocalSettings().getGCSettings();

    static auto fakeFreeSpaceFile = getEnv("_NIX_TEST_FREE_SPACE_FILE");

    auto getAvail = [this]() -> uint64_t {
        if (fakeFreeSpaceFile)
            return std::stoll(readFile(*fakeFreeSpaceFile));

        struct statvfs st;
        if (statvfs(config->realStoreDir.get().c_str(), &st))
            throw SysError("getting filesystem info about '%s'", PathFmt(config->realStoreDir.get()));

        return (uint64_t) st.f_bavail * st.f_frsize;
    };

    std::shared_future<void> future;

    {
        auto state(_state->lock());

        if (state->gcRunning) {
            future = state->gcFuture;
            debug("waiting for auto-GC to finish");
            goto sync;
        }

        auto now = std::chrono::steady_clock::now();

        if (now < state->lastGCCheck + std::chrono::seconds(gcSettings.minFreeCheckInterval))
            return;

        auto avail = getAvail();

        state->lastGCCheck = now;

        if (avail >= gcSettings.minFree || avail >= gcSettings.maxFree)
            return;

        if (avail > state->availAfterGC * 0.97)
            return;

        state->gcRunning = true;

        std::promise<void> promise;
        future = state->gcFuture = promise.get_future().share();

        std::thread([promise{std::move(promise)}, this, avail, getAvail, &gcSettings]() mutable {
            try {

                /* Wake up any threads waiting for the auto-GC to finish. */
                Finally wakeup([&]() {
                    auto state(_state->lock());
                    state->gcRunning = false;
                    state->lastGCCheck = std::chrono::steady_clock::now();
                    promise.set_value();
                });

                GCOptions options;
                options.maxFreed = gcSettings.maxFree - avail;

                printInfo("running auto-GC to free %d bytes", options.maxFreed);

                GCResults results;

                collectGarbage(options, results);

                _state->lock()->availAfterGC = getAvail();

            } catch (...) {
                // FIXME: we could propagate the exception to the
                // future, but we don't really care. (what??)
                ignoreExceptionInDestructor();
            }
        }).detach();
    }

sync:
    // Wait for the future outside of the state lock.
    if (sync)
        future.get();
#endif
}

} // namespace nix
