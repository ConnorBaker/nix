#include "nix/store/pathlocks.hh"
#include "nix/util/file-system-at.hh"
#include "nix/util/signals.hh"

#include <cerrno>
#include <cstdlib>

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
        throw SysError("opening lock file %1%", PathFmt(path));

    return fd;
}

void deleteLockFile(const std::filesystem::path & path, Descriptor desc)
{
    /* Get rid of the lock file.  Have to be careful not to introduce
       races.  Write a (meaningless) token to the file to indicate to
       other processes waiting on this lock that the lock is stale
       (deleted). */
    tryUnlink(path);
    writeFull(desc, "d");
    /* We just try to unlink don't care if it fails; removing the lock
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
            else
                return false;
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

void PathLocks::unlock()
{
    for (auto & i : fds) {
        if (deletePaths)
            deleteLockFile(i.second, i.first);

        if (close(i.first) == -1)
            printError("error (ignored): cannot close lock file on %1%", PathFmt(i.second));

        debug("lock released on %1%", PathFmt(i.second));
    }

    fds.clear();
}

} // namespace nix
