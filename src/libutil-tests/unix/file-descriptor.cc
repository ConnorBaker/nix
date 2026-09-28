#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "nix/util/file-descriptor.hh"
#include "nix/util/processes.hh"

#include <fcntl.h>
#include <unistd.h>

namespace nix {

TEST(closeExtraFDs, works)
{
    Pipe pipe;
    pipe.create();
    Pid pid = startProcess([&]() {
        unix::closeExtraFDs();

        /* File descriptors should already be closed by unix::closeExtraFDs(). */
        for (int fd : {pipe.readSide.get(), pipe.writeSide.get()}) {
            if (::close(fd) == 0 || errno != EBADF)
                _exit(1);
        }

        /* stdin, stdout and stderr should not be closed. */
        for (int fd : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) {
            if (::close(fd) == -1)
                _exit(2);
        }

        _exit(0);
    });

    ASSERT_NE(pid_t(pid), -1);
    ASSERT_TRUE(statusOk(pid.wait()));
}

TEST(AutoCloseFD, closeErrorConsumesOwnershipBeforeDescriptorReuse)
{
    Pipe pipe;
    pipe.create();
    Pid pid = startProcess([&]() {
        int reused;
        {
            AutoCloseFD owner{::dup(pipe.readSide.get())};
            if (!owner)
                _exit(1);
            reused = owner.get();
            /* Force a real EBADF without a production fault hook. */
            if (::close(reused) != 0)
                _exit(2);
            try {
                owner.close();
                _exit(3);
            } catch (const SysError & error) {
                if (error.errNo != EBADF)
                    _exit(4);
            }
            if (owner)
                _exit(5);
            if (::dup2(pipe.readSide.get(), reused) != reused)
                _exit(6);
            owner.close();
        }
        /* Neither another close nor destruction may touch the new owner. */
        if (::fcntl(reused, F_GETFD) == -1)
            _exit(7);
        if (::close(reused) != 0)
            _exit(8);
        _exit(0);
    });
    ASSERT_NE(pid_t(pid), -1);
    ASSERT_TRUE(statusOk(pid.wait()));
}

} // namespace nix
