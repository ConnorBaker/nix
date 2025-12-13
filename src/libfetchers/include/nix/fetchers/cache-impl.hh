#pragma once
/**
 * @file
 *
 * Template implementations for cache.hh.
 *
 * Include this file when you need to use withFetchLock().
 */

#include "nix/fetchers/cache.hh"
#include "nix/store/pathlocks.hh"
#include "nix/util/finally.hh"
#include "nix/util/logging.hh"

#include <sys/stat.h>

namespace nix::fetchers {

template<typename CheckCache, typename DoFetch>
auto withFetchLock(
    std::string_view lockIdentity, unsigned int lockTimeout, CheckCache && checkCache, DoFetch && doFetch)
    -> decltype(doFetch())
{
    auto lockPath = getFetchLockPath(lockIdentity);

    debug("acquiring fetch lock '%s' for '%s'", lockPath, lockIdentity);

    AutoCloseFD fd;

    /* Loop to handle stale lock files. A lock file becomes stale when
       another process deletes it while we're waiting to acquire it.
       We detect this by checking if the file has content (deleteLockFile
       writes a marker byte before unlinking). */
    while (true) {
        /* Open/create the lock file. */
        fd = openLockFile(lockPath, true);
        if (!fd)
            throw Error("failed to open fetch lock file '%s'", lockPath);

        /* Try to acquire the lock without blocking first. */
        if (!lockFile(fd.get(), ltWrite, false)) {
            /* Lock is contested - log that we're waiting, then block. */
            if (lockTimeout > 0) {
                printInfo("waiting for fetch lock on '%s' (timeout: %us)...", lockIdentity, lockTimeout);
            } else {
                printInfo("waiting for fetch lock on '%s'...", lockIdentity);
            }

            if (!lockFileWithTimeout(fd.get(), ltWrite, lockTimeout)) {
                throw Error("timed out waiting for fetch lock on '%s' after %u seconds", lockIdentity, lockTimeout);
            }
        }

        debug("fetch lock acquired on '%s'", lockPath);

        /* Check if the lock file has become stale. */
        struct stat st;
        if (fstat(fd.get(), &st) == -1)
            throw SysError("statting lock file '%s'", lockPath);
        if (st.st_size != 0) {
            debug("lock file '%s' is stale, retrying", lockPath);
            fd.close();
            continue;
        }
        break;
    }

    /* Ensure lock file is cleaned up on all exit paths, including exceptions.
       The flock is automatically released when fd closes, but we also want
       to remove the lock file from disk. Errors during cleanup are ignored
       since lock file removal is an optimization, not a necessity. */
    Finally cleanup([&]() {
        try {
            deleteLockFile(lockPath, fd.get());
        } catch (...) {
            /* Ignore errors during cleanup - the flock is released when fd closes */
        }
    });

    /* Double-check: another process may have populated the cache
       while we were waiting for the lock. */
    if (auto cached = checkCache()) {
        return *cached;
    }

    /* Perform the actual fetch. */
    return doFetch();
}

} // namespace nix::fetchers
