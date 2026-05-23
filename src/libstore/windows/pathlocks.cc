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
static bool warnOrThrowWine(DWORD lastError, const std::string & fs, const Args &... args)
{
    using namespace nix::windows;
    if (isWine()) {
        warn(fs + ": %s (ignored under Wine)", args..., lastError);
        return true;
    }
    throw WinError(lastError, fs, args...);
}

bool lockFile(Descriptor desc, LockType lockType, bool wait)
{
    switch (lockType) {
    case ltNone: {
        OVERLAPPED ov = {0};
        if (!UnlockFileEx(desc, 0, 2, 0, &ov))
            return warnOrThrowWine(GetLastError(), "Failed to unlock file %s", PathFmt(descriptorToPath(desc)));
        return true;
    }
    case ltRead: {
        OVERLAPPED ov = {0};
        if (!LockFileEx(desc, wait ? 0 : LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) {
            auto lastError = GetLastError();
            if (lastError == ERROR_LOCK_VIOLATION)
                return false;
            return warnOrThrowWine(lastError, "Failed to lock file %s", PathFmt(descriptorToPath(desc)));
        }

        ov.Offset = 1;
        if (!UnlockFileEx(desc, 0, 1, 0, &ov)) {
            auto lastError = GetLastError();
            if (lastError != ERROR_NOT_LOCKED)
                return warnOrThrowWine(lastError, "Failed to unlock file %s", PathFmt(descriptorToPath(desc)));
        }
        return true;
    }
    case ltWrite: {
        OVERLAPPED ov = {0};
        ov.Offset = 1;
        if (!LockFileEx(desc, LOCKFILE_EXCLUSIVE_LOCK | (wait ? 0 : LOCKFILE_FAIL_IMMEDIATELY), 0, 1, 0, &ov)) {
            auto lastError = GetLastError();
            if (lastError == ERROR_LOCK_VIOLATION)
                return false;
            return warnOrThrowWine(lastError, "Failed to lock file %s", PathFmt(descriptorToPath(desc)));
        }

        ov.Offset = 0;
        if (!UnlockFileEx(desc, 0, 1, 0, &ov)) {
            auto lastError = GetLastError();
            if (lastError != ERROR_NOT_LOCKED)
                return warnOrThrowWine(lastError, "Failed to unlock file %s", PathFmt(descriptorToPath(desc)));
        }
        return true;
    }
    default:
        assert(false);
    }
}

} // namespace nix
