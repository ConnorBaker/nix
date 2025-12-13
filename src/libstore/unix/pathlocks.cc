#include "nix/store/pathlocks.hh"
#include "nix/util/util.hh"
#include "nix/util/sync.hh"
#include "nix/util/signals.hh"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <thread>

using namespace std::chrono_literals;

#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>

namespace nix {

AutoCloseFD openLockFile(const std::filesystem::path & path, bool create)
{
    AutoCloseFD fd;

    fd = open(path.c_str(), O_CLOEXEC | O_RDWR | (create ? O_CREAT : 0), 0600);
    if (!fd && (create || errno != ENOENT))
        throw SysError("opening lock file %1%", path);

    return fd;
}

void deleteLockFile(const std::filesystem::path & path, Descriptor desc)
{
    /* Get rid of the lock file.  Have to be careful not to introduce
       races.  Write a (meaningless) token to the file to indicate to
       other processes waiting on this lock that the lock is stale
       (deleted). */
    unlink(path.c_str());
    writeFull(desc, "d");
    /* Note that the result of unlink() is ignored; removing the lock
       file is an optimisation, not a necessity. */
}

bool lockFile(Descriptor desc, LockType lockType, bool wait)
{
    int type;
    if (lockType == ltRead)
        type = LOCK_SH;
    else if (lockType == ltWrite)
        type = LOCK_EX;
    else if (lockType == ltNone)
        type = LOCK_UN;
    else
        unreachable();

    if (wait) {
        while (flock(desc, type) != 0) {
            checkInterrupt();
            if (errno != EINTR)
                throw SysError("acquiring/releasing lock");
            /* If EINTR, the loop continues and retries flock() */
        }
    } else {
        while (flock(desc, type | LOCK_NB) != 0) {
            checkInterrupt();
            if (errno == EWOULDBLOCK)
                return false;
            if (errno != EINTR)
                throw SysError("acquiring/releasing lock");
        }
    }

    return true;
}

bool lockFileWithTimeout(Descriptor desc, LockType lockType, unsigned int timeout)
{
    if (timeout == 0) {
        /* No timeout - wait indefinitely */
        return lockFile(desc, lockType, true);
    }

    /*
     * flock() doesn't support timeouts natively. We use a polling approach
     * with exponential backoff, which is the standard solution for timed
     * flock() since:
     *
     * 1. Using alarm()/SIGALRM is not thread-safe and interferes with
     *    other signal handling.
     *
     * 2. poll()/select() don't work with flock() - you can't wait on
     *    lock acquisition in a select-like manner.
     *
     * 3. Boost Interprocess file_lock uses a different locking mechanism
     *    (fcntl F_SETLK) that's incompatible with flock(), so mixing them
     *    would cause deadlocks with other Nix processes.
     *
     * The exponential backoff (10ms -> 20ms -> ... -> 500ms cap) minimizes
     * CPU usage while remaining responsive to lock availability.
     */
    auto startTime = std::chrono::steady_clock::now();
    auto timeoutDuration = std::chrono::seconds(timeout);
    auto sleepDuration = 10ms;
    constexpr auto maxSleep = 500ms;

    while (true) {
        checkInterrupt();

        /* Try non-blocking lock */
        if (lockFile(desc, lockType, false))
            return true;

        /* Check if we've exceeded the timeout */
        auto elapsed = std::chrono::steady_clock::now() - startTime;
        if (elapsed >= timeoutDuration)
            return false;

        /* Sleep with exponential backoff, capped at 500ms */
        std::this_thread::sleep_for(sleepDuration);
        sleepDuration = std::min(sleepDuration * 2, maxSleep);
    }
}

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
        std::filesystem::path lockPath = path + ".lock";

        debug("locking path %1%", path);

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

            debug("lock acquired on %1%", lockPath);

            /* Check that the lock file hasn't become stale (i.e.,
               hasn't been unlinked). */
            struct stat st;
            if (fstat(fd.get(), &st) == -1)
                throw SysError("statting lock file %1%", lockPath);
            if (st.st_size != 0)
                /* This lock file has been unlinked, so we're holding
                   a lock on a deleted file.  This means that other
                   processes may create and acquire a lock on
                   `lockPath', and proceed.  So we must retry. */
                debug("open lock file %1% has become stale", lockPath);
            else
                break;
        }

        /* Use borrow so that the descriptor isn't closed. */
        fds.push_back(FDPair(fd.release(), lockPath));
    }

    return true;
}

void PathLocks::unlock()
{
    for (auto & i : fds) {
        if (deletePaths)
            deleteLockFile(i.second, i.first);

        if (close(i.first) == -1)
            printError("error (ignored): cannot close lock file on %1%", i.second);

        debug("lock released on %1%", i.second);
    }

    fds.clear();
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
