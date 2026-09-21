#include <gtest/gtest.h>

#include "nix/store/pathlocks.hh"
#include "nix/util/file-system.hh"
#include "nix/util/finally.hh"
#include "nix/util/tests/capture-logging.hh"

#ifndef _WIN32
#  include <sys/file.h>
#endif

namespace nix {

namespace {

/* The wait message follows a failed nonblocking attempt. Release the
   competing lock at that point, so the contended constructor branch is
   exercised deterministically, without sleeps or a scheduler assumption. */
struct ReleaseBlockerOnWait : testing::CaptureLogger
{
    Descriptor blocker;
    bool released = false;

    explicit ReleaseBlockerOnWait(Descriptor blocker)
        : blocker(blocker)
    {
    }

    void log(Verbosity, std::string_view message) noexcept override
    {
        if (message == "waiting for test lock") {
#ifdef _WIN32
            OVERLAPPED ov = {0};
            released = ::UnlockFileEx(blocker, 0, 1, 0, &ov);
#else
            released = ::flock(blocker, LOCK_UN) == 0;
#endif
            assert(released);
        }
    }
};

} // namespace

TEST(FdLock, ownershipAndReleaseForEveryAcquisitionCase)
{
    AutoDelete dir{createTempDir()};
    for (auto held : {ltNone, ltRead, ltWrite})
        for (auto requested : {ltRead, ltWrite})
            for (bool wait : {false, true}) {
                SCOPED_TRACE(fmt("held=%d requested=%d wait=%d", held, requested, wait));
                auto path = dir.path() / "lock";
                auto blocker = openLockFile(path, true);
                auto subject = openLockFile(path, false);
                auto observer = openLockFile(path, false);
                ASSERT_TRUE(blocker && subject && observer);
                if (held != ltNone) {
                    ASSERT_TRUE(lockFile(blocker.get(), held, false));
                }

                ReleaseBlockerOnWait waiting{blocker.get()};
                auto oldLogger = std::exchange(logger, &waiting);
                auto oldVerbosity = std::exchange(verbosity, lvlInfo);
                Finally restore{[&] {
                    logger = oldLogger;
                    verbosity = oldVerbosity;
                }};
                bool conflicts = held != ltNone && (held == ltWrite || requested == ltWrite);
                {
                    FdLock lock{subject.get(), requested, wait, "waiting for test lock"};
                    EXPECT_EQ(lock.acquired, wait || !conflicts);
                    EXPECT_EQ(waiting.released, wait && conflicts);
                    if (lock.acquired) {
                        EXPECT_FALSE(lockFile(observer.get(), ltWrite, false));
                        bool readable = lockFile(observer.get(), ltRead, false);
                        EXPECT_EQ(readable, requested == ltRead);
                        if (readable) {
                            ASSERT_TRUE(lockFile(observer.get(), ltNone, false));
                        }
                    }
                }

                /* Removing the competitor leaves only the subject's lock,
                   if its destructor incorrectly retained one. Keep the
                   subject descriptor open so close cannot hide that bug. */
                ASSERT_TRUE(lockFile(blocker.get(), ltNone, false));
                EXPECT_TRUE(lockFile(observer.get(), ltWrite, false));
                ASSERT_TRUE(lockFile(observer.get(), ltNone, false));
            }
}

TEST(LockFile, everyModeTransitionHasOneMatchingRelease)
{
    AutoDelete dir{createTempDir()};
    for (auto before : {ltNone, ltRead, ltWrite})
        for (auto after : {ltNone, ltRead, ltWrite})
            for (bool wait : {false, true}) {
                SCOPED_TRACE(fmt("before=%d after=%d wait=%d", before, after, wait));
                auto path = dir.path() / "lock";
                auto subject = openLockFile(path, true);
                auto observer = openLockFile(path, false);
                ASSERT_TRUE(subject && observer);
                ASSERT_TRUE(lockFile(subject.get(), before, false));
                ASSERT_TRUE(lockFile(subject.get(), after, wait));

                bool readable = lockFile(observer.get(), ltRead, false);
                EXPECT_EQ(readable, after != ltWrite);
                if (readable) {
                    ASSERT_TRUE(lockFile(observer.get(), ltNone, false));
                }
                bool writable = lockFile(observer.get(), ltWrite, false);
                EXPECT_EQ(writable, after == ltNone);
                if (writable) {
                    ASSERT_TRUE(lockFile(observer.get(), ltNone, false));
                }

                ASSERT_TRUE(lockFile(subject.get(), ltNone, false));
                EXPECT_TRUE(lockFile(observer.get(), ltWrite, false));
            }
}

} // namespace nix
