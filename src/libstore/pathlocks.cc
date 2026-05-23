#include "nix/store/pathlocks.hh"
#include "nix/util/logging.hh"
#include "nix/util/signals.hh"
#include "nix/util/util.hh"

#include <cerrno>
#include <cstdlib>

namespace nix {

PathLocks::PathLocks()
    : deletePaths(false)
{
}

PathLocks::PathLocks(const std::set<std::filesystem::path> & paths, const std::string & waitMsg)
    : deletePaths(false)
{
    lockPaths(paths, waitMsg);
}

PathLocks::~PathLocks()
{
    try {
        unlock();
    } catch (...) {
        ignoreExceptionInDestructor();
    }
}

void PathLocks::setDeletion(bool deletePaths)
{
    this->deletePaths = deletePaths;
}

/* Differences between Unix and Windows are confined to the
   `openLockFile`/`lockFile` shims declared in `pathlocks.hh` and to
   the staleness check, which uses `getFileSize` (available on both
   platforms). */
bool PathLocks::lockPaths(const std::set<std::filesystem::path> & paths, const std::string & waitMsg, bool wait)
{
    assert(fds.empty());

    /* Note that `fds' is built incrementally so that the destructor
       will only release those locks that we have already acquired. */

    /* Acquire the lock for each path in sorted order. This ensures
       that locks are always acquired in the same order, thus
       preventing deadlocks. */
    for (auto & path : paths) {
        checkInterrupt();
        auto lockPath = path;
        lockPath += ".lock";

        debug("locking path %1%", PathFmt(path));

        AutoCloseFD fd;

        while (1) {

            /* Open/create the lock file. */
            fd = openLockFile(lockPath, true);

            /* Acquire an exclusive lock. */
            if (!lockFile(fd.get(), ltWrite, false)) {
                if (wait) {
                    if (waitMsg != "")
                        printError(waitMsg);
                    lockFile(fd.get(), ltWrite, true);
                } else {
                    /* Failed to lock this path; release all other
                       locks. */
                    unlock();
                    return false;
                }
            }

            debug("lock acquired on %1%", PathFmt(lockPath));

            /* Check that the lock file hasn't become stale (i.e.,
               hasn't been unlinked). A non-empty lock file is the
               token written by `deleteLockFile` to signal staleness
               to other waiters; staying inside the `while (1)` and
               re-opening the lock file is necessary because another
               process may have deleted the file we just locked, then
               recreated and acquired its own lock on the new path,
               so we must retry to avoid both processes believing
               they hold the lock. */
            if (getFileSize(fd.get()) != 0)
                debug("open lock file %1% has become stale", PathFmt(lockPath));
            else
                break;
        }

        /* Use borrow so that the descriptor isn't closed. */
        fds.push_back(FDPair(fd.release(), lockPath));
    }

    return true;
}

FdLock::FdLock(Descriptor desc, LockType lockType, bool wait, std::string_view waitMsg)
    : desc(desc)
{
    if (wait) {
        if (!lockFile(desc, lockType, false)) {
            printInfo("%s", waitMsg);
            acquired = lockFile(desc, lockType, true);
        }
    } else
        acquired = lockFile(desc, lockType, false);
}

} // namespace nix
