#pragma once
///@file

#include "nix/util/file-descriptor.hh"

namespace nix {

/**
 * Open (possibly create) a lock file and return the file descriptor.
 * -1 is returned if create is false and the lock could not be opened
 * because it doesn't exist.  Any other error throws an exception.
 */
AutoCloseFD openLockFile(const Path & path, bool create);

/**
 * Delete an open lock file.
 */
void deleteLockFile(const Path & path, Descriptor desc);

enum LockType { ltRead, ltWrite, ltNone };

/**
 * Acquire or release a lock on a file descriptor using flock().
 *
 * @param desc File descriptor to lock
 * @param lockType Type of lock: ltRead (shared), ltWrite (exclusive), or ltNone (unlock)
 * @param wait If true, block until lock is acquired; if false, return immediately
 * @return true if lock was acquired/released, false if would block (when wait=false)
 * @throws SysError on system errors
 */
bool lockFile(Descriptor desc, LockType lockType, bool wait);

/**
 * Try to acquire a lock with a timeout.
 *
 * @param desc File descriptor to lock
 * @param lockType Type of lock (read/write/none)
 * @param timeout Timeout in seconds (0 = no timeout, wait indefinitely)
 * @return true if lock was acquired, false if timed out
 * @throws SysError on system errors
 */
bool lockFileWithTimeout(Descriptor desc, LockType lockType, unsigned int timeout);

class PathLocks
{
private:
    typedef std::pair<Descriptor, Path> FDPair;
    std::list<FDPair> fds;
    bool deletePaths;

public:
    PathLocks();
    PathLocks(const PathSet & paths, const std::string & waitMsg = "");
    bool lockPaths(const PathSet & _paths, const std::string & waitMsg = "", bool wait = true);
    ~PathLocks();
    void unlock();
    void setDeletion(bool deletePaths);
};

struct FdLock
{
    Descriptor desc;
    bool acquired = false;

    FdLock(Descriptor desc, LockType lockType, bool wait, std::string_view waitMsg);

    ~FdLock()
    {
        if (acquired)
            lockFile(desc, ltNone, false);
    }
};

} // namespace nix
