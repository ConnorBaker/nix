#include "nix/util/file-system.hh"
#include "nix/util/logging.hh"
#include "nix/store/pathlocks.hh"
#include "nix/util/signals.hh"
#include "nix/util/util.hh"
#include "nix/util/windows-environment.hh"

#include <errhandlingapi.h>
#include <fileapi.h>
#include <windows.h>

namespace nix {

void deleteLockFile(const std::filesystem::path & path, Descriptor desc)
{

    int exit = DeleteFileW(path.c_str());
    if (exit == 0)
        warn("%s: %s", PathFmt(path), std::to_string(GetLastError()));
}

void PathLocks::unlock()
{
    for (auto & i : fds) {
        if (deletePaths)
            deleteLockFile(i.second, i.first);

        if (CloseHandle(i.first) == -1)
            printError("error (ignored): cannot close lock file on %1%", PathFmt(i.second));

        debug("lock released on %1%", PathFmt(i.second));
    }

    fds.clear();
}

AutoCloseFD openLockFile(const std::filesystem::path & path, bool create)
{
    AutoCloseFD desc = CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        create ? OPEN_ALWAYS : OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_POSIX_SEMANTICS,
        NULL);
    if (desc.get() == INVALID_HANDLE_VALUE)
        warn("%s: %s", PathFmt(path), std::to_string(GetLastError()));

    return desc;
}

/**
 * Throw a WinError, or if running under Wine, just warn and return true.
 * Wine has incomplete file locking support, so we degrade gracefully.
 */
template<typename... Args>
static bool warnOrThrowWine(DWORD lastError, const std::string & fs, Args &&... args)
{
    using namespace nix::windows;
    if (isWine()) {
        warn(fs + ": %s (ignored under Wine)", args..., lastError);
        return true;
    }
    throw WinError(lastError, fs, std::forward<Args>(args)...);
}

bool lockFile(Descriptor desc, LockType lockType, bool wait)
{
    DWORD flags = wait ? 0 : LOCKFILE_FAIL_IMMEDIATELY;
    switch (lockType) {
    case ltNone:
    case ltRead:
        break;
    case ltWrite:
        flags |= LOCKFILE_EXCLUSIVE_LOCK;
        break;
    default:
        unreachable();
    }

    /* Every mode covers the same byte, and UnlockFileEx requires exactly
       the acquired range. Release this handle's previous lock before a
       conversion: LockFileEx stacks shared locks and cannot acquire an
       exclusive lock over an existing lock on the same handle. As with
       flock conversions, releasing and acquiring are not atomic.
       ERROR_NOT_LOCKED is the ordinary initially-unlocked case. */
    OVERLAPPED ov = {0};
    if (!UnlockFileEx(desc, 0, 1, 0, &ov)) {
        auto lastError = GetLastError();
        if (lastError != ERROR_NOT_LOCKED)
            return warnOrThrowWine(lastError, "Failed to unlock file %s", PathFmt(descriptorToPath(desc)));
    }
    if (lockType == ltNone)
        return true;

    if (!LockFileEx(desc, flags, 0, 1, 0, &ov)) {
        auto lastError = GetLastError();
        if (lastError == ERROR_LOCK_VIOLATION)
            return false;
        return warnOrThrowWine(lastError, "Failed to lock file %s", PathFmt(descriptorToPath(desc)));
    }
    return true;
}

bool PathLocks::lockPaths(const std::set<std::filesystem::path> & paths, const std::string & waitMsg, bool wait)
{
    assert(fds.empty());

    for (auto & path : paths) {
        checkInterrupt();
        std::filesystem::path lockPath = path;
        lockPath += L".lock";
        debug("locking path %1%", PathFmt(path));

        AutoCloseFD fd;

        while (1) {
            fd = openLockFile(lockPath, true);
            if (!lockFile(fd.get(), ltWrite, false)) {
                if (wait) {
                    if (waitMsg != "")
                        printError(waitMsg);
                    lockFile(fd.get(), ltWrite, true);
                } else {
                    unlock();
                    return false;
                }
            }

            debug("lock acquired on %1%", PathFmt(lockPath));

            if (getFileSize(fd.get()) != 0)
                debug("open lock file %1% has become stale", PathFmt(lockPath));
            else
                break;
        }

        fds.push_back(FDPair(fd.release(), lockPath));
    }
    return true;
}

FdLock::FdLock(Descriptor desc, LockType lockType, bool wait, std::string_view waitMsg)
    : desc(desc)
{
    acquired = lockFile(desc, lockType, false);
    if (!acquired && wait) {
        printInfo("%s", waitMsg);
        acquired = lockFile(desc, lockType, true);
    }
}

} // namespace nix
