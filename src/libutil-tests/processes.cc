#include "nix/util/processes.hh"
#include "nix/util/current-process.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/file-system.hh"
#include "nix/util/system-error.hh"
#include "nix/util/signals.hh"
#include "nix/util/file-descriptor.hh"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#ifdef __linux__
#  include <cerrno>
#  include <csignal>
#  include <sys/prctl.h>
#  include <sys/ptrace.h>
#  include <sys/syscall.h>
#  include <unistd.h>
#endif

#ifndef _WIN32
#  include <poll.h>
#  include <sys/wait.h>
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

/* ----------------------------------------------------------------------------
 * runProgram
 * --------------------------------------------------------------------------*/

TEST(runProgram, worksTrivial)
{
    auto self = getSelfExe();
    ASSERT_TRUE(self);

    for (bool isInteractive : {false, true}) {
        std::string output;
        ASSERT_NO_THROW({
            output = runProgram(
                *self,
                /*lookupPath=*/false,
                {
                    OS_STR("__util_test_spawn_trivial"),
                },
                /*isInteractive=*/isInteractive);
        });
        ASSERT_EQ(output, "hello");
    }
}

TEST(runProgram2, mergeStderrToStdout)
{
    auto self = getSelfExe();
    ASSERT_TRUE(self);

    for (bool mergeStdoutToStderr : {false, true}) {
        ::testing::internal::CaptureStdout();
        ::testing::internal::CaptureStderr();

        auto [status, output] = runProgram({
            .spawnOptions =
                {
                    .program = *self,
                    .args = {OS_STR("__util_test_spawn_write_to_stdout_and_stderr")},
                },
            .mergeStderrToStdout = mergeStdoutToStderr,
        });

        ASSERT_TRUE(statusOk(status));
        ASSERT_THAT(output, ::testing::HasSubstr("hi"));
        ASSERT_EQ(::testing::internal::GetCapturedStdout(), "");
        auto capturedErr = ::testing::internal::GetCapturedStderr();
        if (mergeStdoutToStderr)
            ASSERT_THAT(output, ::testing::HasSubstr("there"));
        else
            /* Otherwise stderr is inherited. */
            ASSERT_THAT(capturedErr, ::testing::HasSubstr("there"));
    }
}

#ifdef _WIN32
#  define NIX_EXECUTABLE_EXTENSION ".exe"
/* FIXME: runProgram reports spawn errors as WinError, while other
   platforms conflate everything into ExecError. */
#  define NIX_SPAWN_EXCEPTION windows::WinError
#else
#  define NIX_EXECUTABLE_EXTENSION ""
#  define NIX_SPAWN_EXCEPTION ExecError
#endif

TEST(runProgram, nonexistent)
{
    /* For now spawn failures get reported as ExecError. TODO: Report the proper error that's less
       confusing. */
    ASSERT_THROW(
        runProgram("/this/path/really/should/not/exist/for/real" NIX_EXECUTABLE_EXTENSION), NIX_SPAWN_EXCEPTION);
}

TEST(runProgram2, nonexistent)
{
    ASSERT_THROW(
        {
            runProgram2({{
                .program = "/this/path/really/should/not/exist/for/real" NIX_EXECUTABLE_EXTENSION,
            }});
        },
        NIX_SPAWN_EXCEPTION);
}

#ifndef _WIN32 /* Leaking file descriptors into the child isn't a concern on windows. */

TEST(runProgram2, leakedFDsAreClosed)
{
#  if defined(__linux__) && !defined(__GLIBC__)
    /* Without glibc's closefrom(), doExecChild() only closes leaked FDs via
       close_range, which is best-effort. Skip when it's unavailable, e.g. under
       the enosys test run. */
    if (::syscall(SYS_close_range, ~0u, ~0u, 0) == -1 && errno == ENOSYS)
        GTEST_SKIP() << "close_range is unavailable and there is no closefrom()";
#  endif

    auto self = getSelfExe();
    ASSERT_TRUE(self);
    int fds[2];
    ASSERT_NE(::pipe(fds), -1);

    AutoCloseFD readSide = fds[0];
    AutoCloseFD writeSide = fds[1];

    /* Test that both fds are closed in the child. Just the one isn't always
       enough if the read side is assigned to 3 (also the fd of the relocated pipe in
       the child on linux that gets dup3-ed into). */
    ASSERT_NO_THROW(runProgram2({
        {.program = *self,
         .args = {"__util_test_spawn_leaked_fds"},
         .environment = OsStringMap{{
             "NIX_CHILD_FDS_SHOULD_BE_CLOSED",
             fmt("%d,%d", readSide.get(), writeSide.get()),
         }}},
    }));
}

TEST(spawnProgram, chdirAndArgv0)
{
    auto self = getSelfExe();
    ASSERT_TRUE(self);
    auto tempDir = createTempDir();

    Pid pid = spawnProgram(
        {
            .program = *self,
            .args = {"__util_test_spawn_chdir_and_argv0", tempDir.native()},
            .argv0 = "test",
            .chdir = tempDir,
        },
        {});

    auto status = pid.wait();
    ASSERT_TRUE(statusOk(status)) << "status: " << WEXITSTATUS(status);
}

TEST(spawnProgram, nonexistent)
{
    std::exception_ptr ex;
    Pid pid;

    try {
        pid = spawnProgram(
            {
                .program = "/this/path/really/should/not/exist/for/real",
            },
            {});
    } catch (...) {
        ex = std::current_exception();
    }

    /* Due to platform differences, we either fail with ExecError or actually successfully spawn
       the process but fail later. */
    if (ex) {
        ASSERT_FALSE(pid);
        try {
            std::rethrow_exception(ex);
        } catch (const ExecError & e) {
            ASSERT_TRUE(WIFEXITED(e.status));
            ASSERT_EQ(WEXITSTATUS(e.status), 127);
        } catch (...) {
            FAIL();
        }
    } else if (pid) {
        ASSERT_FALSE(ex);
        int status = pid.wait();
        ASSERT_TRUE(WIFEXITED(status));
        ASSERT_EQ(WEXITSTATUS(status), 127);
    }
}

TEST(spawnProgram, restoresSignalMask)
{
    auto self = getSelfExe();
    ASSERT_TRUE(self);

    /* Do the modifications in the child to not affect the primary test runner process. */

    Pid child = startProcess([&]() {
        /* We usually ignore SIGPIPE, but let's unblock it here and check that the child
           also has it unblocked and does not inherit it from the process after it's modified
           by startSignalHandlerThread. */

        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGPIPE);
        sigprocmask(SIG_UNBLOCK, &set, nullptr);

        unix::startSignalHandlerThread();

        Pid spawned = spawnProgram(
            {
                .program = *self,
                .args = {"__util_test_spawn_sigmask"},
            },
            {});

        if (!spawned)
            _exit(1);

        if (!statusOk(spawned.wait()))
            _exit(2);

        _exit(0);
    });

    ASSERT_TRUE(child);
    ASSERT_TRUE(statusOk(child.wait()));
}

TEST(RootSpawnProgram, setuidAndSetgid)
{
    auto self = getSelfExe();
    ASSERT_TRUE(self);

    Pid spawned = spawnProgram(
        {
            .program = *self,
            .args = {"__util_test_spawn_setuid_setgid"},
            /* Some dummy uid/gid. */
            .uid = uid_t(1),
            .gid = gid_t(1),
            .chdir = "/",
        },
        {});

    ASSERT_TRUE(spawned);
    ASSERT_TRUE(statusOk(spawned.wait()));
}

/* ----------------------------------------------------------------------------
 * openExitDescriptor
 * --------------------------------------------------------------------------*/

#  if defined(__linux__) && defined(SYS_pidfd_open)
static bool pidfdOpenUnavailable()
{
    return ::syscall(SYS_pidfd_open, getpid(), 0) == -1 && errno == ENOSYS;
}
#  endif

TEST(openExitDescriptor, readableOnceExitedAndDoesNotReap)
{
#  if defined(__linux__) && defined(SYS_pidfd_open)
    if (pidfdOpenUnavailable())
        GTEST_SKIP() << "pidfd_open() is unavailable";
#  else
    GTEST_SKIP() << "no exit descriptor on this platform";
#  endif

    /* The child exits with 42 once `go` reaches EOF. */
    Pipe go;
    go.create();
    Pid child = startProcess([&] {
        go.writeSide.close();
        char c;
        [[maybe_unused]] auto n = ::read(go.readSide.get(), &c, 1);
        _exit(42);
    });
    go.readSide.close();

    auto exited = openExitDescriptor(child);
    ASSERT_TRUE(exited);

    struct pollfd pfd = {.fd = exited.get(), .events = POLLIN};
    ASSERT_EQ(::poll(&pfd, 1, 0), 0) << "readable while the child is still running";

    go.writeSide.close();
    ASSERT_EQ(::poll(&pfd, 1, 10000), 1) << "not readable after the child exited";
    ASSERT_TRUE(pfd.revents & POLLIN);

    /* Not reaped: the child's own status is still there to collect. */
    int status = child.wait();
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 42);
}

#  if defined(__linux__) && defined(SYS_pidfd_open)
/* Run by `run-without-new-syscalls`, which makes pidfd_open() fail with
   ENOSYS and sets `_NIX_TEST_NO_PIDFD_OPEN`, so that this test can't be
   skipped there. */
TEST(openExitDescriptor, invalidWithoutPidfdOpen)
{
    if (!pidfdOpenUnavailable()) {
        if (getEnv("_NIX_TEST_NO_PIDFD_OPEN"))
            FAIL() << "pidfd_open() is available, although it should have been disabled";
        GTEST_SKIP() << "pidfd_open() is available";
    }
    Pid child = startProcess([] { _exit(0); });
    ASSERT_FALSE(openExitDescriptor(child));
    ASSERT_TRUE(statusOk(child.wait()));
}
#  endif

#endif

#ifdef __linux__
/* As root: a spawned program that changes its credentials still dies with
   its parent. */
TEST(RootSpawnProgram, dieWithParentAfterSetuid)
{
    ASSERT_EQ(geteuid(), 0) << "Run Root* tests as root";
    auto self = getSelfExe();
    ASSERT_TRUE(self);

    Pipe pipe;
    pipe.create();
    /* A parent that spawns the program as nobody, says its pid, and exits. */
    Pid parent = startProcess([&] {
        pipe.readSide.close();
        Pid child = spawnProgram(
            {
                .program = *self,
                .args = {"__util_test_spawn_sleep"},
                .uid = 65534,
                .gid = 65534,
            },
            {});
        pid_t pid = child.release();
        writeFull(pipe.writeSide.get(), {reinterpret_cast<char *>(&pid), sizeof pid});
        _exit(0);
    });
    pipe.writeSide.close();
    pid_t child;
    readFull(pipe.readSide.get(), reinterpret_cast<char *>(&child), sizeof child);
    parent.wait();

    /* It got SIGKILL, and init reaps it. */
    for (int i = 0; i < 1000 && ::kill(child, 0) == 0; ++i)
        usleep(10000);
    EXPECT_EQ(::kill(child, 0), -1) << "the program outlived its parent";
    ::kill(child, SIGKILL);
}
#endif

#ifdef __linux__
/* The supplementary groups are set before the gid, as setUser() does: for
   a user that can set neither, setgroups() is what fails. */
TEST(spawnProgram, setsGroupsBeforeGid)
{
    if (geteuid() == 0)
        GTEST_SKIP() << "root can set both";
    auto self = getSelfExe();
    ASSERT_TRUE(self);

    /* The vfork() path reports the failure as a trace, the fork() path
       (with ASan) on stderr. */
    ::testing::internal::CaptureStderr();
    std::string message;
    try {
        spawnProgram({.program = *self, .args = {"__util_test_spawn_trivial"}, .gid = getgid() + 1}, {}).wait();
    } catch (Error & e) {
        for (auto & trace : e.info().traces)
            message += trace.hint.str();
    }
    message += ::testing::internal::GetCapturedStderr();
    EXPECT_THAT(message, ::testing::HasSubstr("setgroups failed"));
}

/* A spawned program dies if its parent died before it set its death
   signal: here, while the program was stopped (by ptrace) at its start. */
TEST(spawnProgram, diesIfParentDiedFirst)
{
    auto self = getSelfExe();
    ASSERT_TRUE(self);

    Pid tracer = startProcess([&] {
        /* The orphaned program becomes our child. */
        if (prctl(PR_SET_CHILD_SUBREAPER, 1) == -1)
            _exit(10);
        pid_t parent = fork();
        if (parent == 0) {
            if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) == -1)
                _exit(77);
            raise(SIGSTOP);
            spawnProgram({.program = *self, .args = {"__util_test_spawn_sleep"}}, {});
            _exit(0);
        }
        int status = 0;
        if (waitpid(parent, &status, 0) != parent || !WIFSTOPPED(status))
            _exit(WIFEXITED(status) ? WEXITSTATUS(status) : 11);
        if (ptrace(PTRACE_SETOPTIONS, parent, nullptr, PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK) == -1)
            _exit(12);
        /* Until it forks the program. */
        int sig = 0;
        unsigned long program = 0;
        do {
            if (ptrace(PTRACE_CONT, parent, nullptr, sig) == -1 || waitpid(parent, &status, 0) != parent
                || !WIFSTOPPED(status))
                _exit(13);
            sig = WSTOPSIG(status) == SIGTRAP ? 0 : WSTOPSIG(status);
        } while (!(status >> 16) || ptrace(PTRACE_GETEVENTMSG, parent, nullptr, &program) == -1);
        ::kill(parent, SIGKILL);
        waitpid(parent, &status, 0);
        if (waitpid(program, &status, __WALL) != (pid_t) program
            || ptrace(PTRACE_DETACH, program, nullptr, nullptr) == -1)
            _exit(14);
        for (int i = 0; i < 1000; ++i, usleep(10000))
            if (waitpid(program, &status, WNOHANG | __WALL) == (pid_t) program)
                _exit(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL ? 0 : 15);
        ::kill(program, SIGKILL);
        _exit(1);
    });
    int status = tracer.wait();
    if (WIFEXITED(status) && WEXITSTATUS(status) == 77)
        GTEST_SKIP() << "cannot ptrace";
    ASSERT_TRUE(WIFEXITED(status)) << statusToString(status);
    EXPECT_NE(WEXITSTATUS(status), 1) << "the program outlived its parent";
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

/* killUser()'s child reports why it failed by its exit code, without
   throwing: setuid() refuses an invalid uid. */
TEST(killUser, reportsErrno)
{
    try {
        killUser(uid_t(-1));
        ADD_FAILURE() << "killUser() succeeded";
    } catch (SysError & e) {
        EXPECT_EQ(e.errNo, EINVAL) << e.what();
    }
}
#endif

/* TODO: Test a bunch more things. */

#undef NIX_EXECUTABLE_EXTENSION
#undef NIX_SPAWN_EXCEPTION

} // namespace nix
