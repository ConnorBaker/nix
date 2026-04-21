#include "nix/util/processes.hh"

#include <gtest/gtest.h>

#ifdef __linux__
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace nix {

/* ----------------------------------------------------------------------------
 * statusOk
 * --------------------------------------------------------------------------*/

TEST(statusOk, zeroIsOk)
{
    ASSERT_EQ(statusOk(0), true);
    ASSERT_EQ(statusOk(1), false);
}

#ifdef __linux__

/* ----------------------------------------------------------------------------
 * openPidfd / pidfdAlive
 * --------------------------------------------------------------------------*/

TEST(pidfd, aliveChildReportsAlive)
{
    // Fork a child that blocks on a pipe read. While it's alive
    // `pidfdAlive` must report true; after we close the pipe, read
    // returns 0, the child exits, and once reaped the pidfd's task
    // reports ESRCH.
    int pipefd[2];
    ASSERT_EQ(::pipe(pipefd), 0);

    pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        ::close(pipefd[1]);
        char buf;
        (void) ::read(pipefd[0], &buf, 1);
        ::_exit(0);
    }
    ::close(pipefd[0]);

    int fd = openPidfd(child);
    // pidfd_open is ENOSYS on Linux < 5.3. Skip gracefully so the
    // test stays portable across test hosts.
    if (fd < 0) {
        ::close(pipefd[1]);
        int status = 0;
        ::waitpid(child, &status, 0);
        GTEST_SKIP() << "pidfd_open not available on this kernel";
    }
    EXPECT_TRUE(pidfdAlive(fd));

    ::close(pipefd[1]);
    int status = 0;
    ::waitpid(child, &status, 0);

    EXPECT_FALSE(pidfdAlive(fd));
    ::close(fd);
}

TEST(pidfd, negativeFdIsNotAlive)
{
    EXPECT_FALSE(pidfdAlive(-1));
}

TEST(pidfd, nonexistentPidReturnsMinusOne)
{
    // 999999999 is reliably above pid_max on any normal Linux host,
    // so `pidfd_open` returns -1 (ESRCH). Our wrapper must surface
    // that as -1 rather than throwing.
    int fd = openPidfd(999999999);
    EXPECT_EQ(fd, -1);
}

#endif // __linux__

} // namespace nix
