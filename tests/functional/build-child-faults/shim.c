/* LD_PRELOAD shim for build-child-faults.sh: makes Nix or its build child
   misbehave at one point, chosen by $NIX_TEST_FAULT. Each fault happens
   once, recorded by a file in $NIX_TEST_FAULT_DIR, which also gets the
   files that the test waits for. A process that Nix started with argv[0]
   `__build-child` is a "build child program"; anything else is Nix (or a
   builder, which none of the faults touch). */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static int buildChildProgram;
static pid_t loadedPid;

static int fault(const char * name)
{
    const char * f = getenv("NIX_TEST_FAULT");
    return f && strcmp(f, name) == 0;
}

/* Whether the fault happens now: only the first time. */
static int once(const char * name)
{
    const char * dir = getenv("NIX_TEST_FAULT_DIR");
    char path[PATH_MAX];
    if (!fault(name) || !dir || snprintf(path, sizeof path, "%s/%s", dir, name) >= (int) sizeof path)
        return 0;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd == -1)
        return 0;
    close(fd);
    return 1;
}

static void mark(const char * name)
{
    const char * dir = getenv("NIX_TEST_FAULT_DIR");
    char path[PATH_MAX];
    if (!dir || snprintf(path, sizeof path, "%s/%s", dir, name) >= (int) sizeof path)
        return;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd == -1)
        return;
    char pid[16];
    int n = snprintf(pid, sizeof pid, "%d\n", (int) getpid());
    if (write(fd, pid, n) < 0) {
    }
    close(fd);
}

__attribute__((constructor)) static void init(int argc, char ** argv)
{
    loadedPid = getpid();
    buildChildProgram = argc > 0 && strcmp(argv[0], "__build-child") == 0;
    /* As a program that does not start, e.g. for a missing library. */
    if (buildChildProgram && once("server-exits-at-start"))
        _exit(1);
    /* The build child server's stdout: /dev/null, or something else. */
    if (buildChildProgram && fault("server-stdout")) {
        struct stat out, null;
        int isNull = fstat(STDOUT_FILENO, &out) == 0 && stat("/dev/null", &null) == 0 && S_ISCHR(out.st_mode)
                     && out.st_rdev == null.st_rdev;
        mark(isNull ? "server-stdout-null" : "server-stdout-other");
    }
}

#define REAL(name)                         \
    static __typeof__(name) * real_##name; \
    if (!real_##name)                      \
        real_##name = (__typeof__(name) *) dlsym(RTLD_NEXT, #name);

/* The build child is about to leave Nix's process group: instead, it
   reports a malformed error to Nix, and waits to be killed. */
pid_t setsid(void)
{
    REAL(setsid);
    if (buildChildProgram && once("child-stops-before-setsid")) {
        /* Mark first: Nix may kill us as soon as it reads the error. */
        mark("child-stopped");
        static const char msg[] = "\1\n\4\0\0\0\0\0\0\0Fail\0\0\0\0";
        if (write(STDOUT_FILENO, msg, sizeof msg - 1) < 0) {
        }
        while (1)
            pause();
    }
    return real_setsid();
}

/* Nix opens the pty slave for the build child: as fd 0, which is the
   target of another fd that Nix passes on. */
static int openSlave(int fd, const char * path)
{
    if (fd != -1 && !buildChildProgram && strncmp(path, "/dev/pts/", 9) == 0 && once("slave-on-stdin")) {
        if (dup3(fd, STDIN_FILENO, O_CLOEXEC) == -1)
            abort();
        close(fd);
        return STDIN_FILENO;
    }
    return fd;
}

int open(const char * path, int flags, ...)
{
    REAL(open);
    va_list ap;
    va_start(ap, flags);
    mode_t mode = va_arg(ap, mode_t);
    va_end(ap);
    return openSlave(real_open(path, flags, mode), path);
}

/* The build child server, about to reply with a build child's pid; or a
   build child that the server did not report, about to send its pid. */
ssize_t write(int fd, const void * buf, size_t count)
{
    REAL(write);
    if (buildChildProgram && getpid() != loadedPid && fd == STDIN_FILENO && count == sizeof(pid_t)
        && fault("unreported-child-hangs")) {
        mark("child-hanging");
        while (1)
            pause();
    }
    if (buildChildProgram && getpid() == loadedPid && fd == STDIN_FILENO && count == sizeof(pid_t)) {
        if (once("server-exits-before-reply") || once("unreported-child-hangs"))
            _exit(1);
        if (once("server-hangs-before-reply")) {
            mark("server-hanging");
            while (1)
                pause();
        }
    }
    return real_write(fd, buf, count);
}

static int isSeqpacket(int fd)
{
    int type;
    socklen_t len = sizeof type;
    return getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) == 0 && type == SOCK_SEQPACKET;
}

/* Nix, about to send a request with fds to the build child server: with
   one byte too many, or failing as on a bad fd. */
ssize_t sendmsg(int fd, const struct msghdr * msg, int flags)
{
    REAL(sendmsg);
    if (!buildChildProgram && msg->msg_controllen && isSeqpacket(fd) && once("request-fails")) {
        errno = EBADF;
        return -1;
    }
    if (!buildChildProgram && msg->msg_controllen && msg->msg_iovlen == 1 && isSeqpacket(fd)
        && once("malformed-request")) {
        char data[256];
        size_t n = msg->msg_iov[0].iov_len;
        if (n + 1 > sizeof data)
            abort();
        memcpy(data, msg->msg_iov[0].iov_base, n);
        data[n] = 0;
        struct iovec iov = {.iov_base = data, .iov_len = n + 1};
        struct msghdr longer = *msg;
        longer.msg_iov = &iov;
        ssize_t sent = real_sendmsg(fd, &longer, flags);
        return sent > (ssize_t) n ? (ssize_t) n : sent;
    }
    return real_sendmsg(fd, msg, flags);
}

/* Nix, about to receive a build child's pid from the build child server:
   failing as on a bad fd. */
ssize_t recv(int fd, void * buf, size_t len, int flags)
{
    REAL(recv);
    if (!buildChildProgram && len == sizeof(pid_t) && isSeqpacket(fd) && once("reply-fails")) {
        errno = EBADF;
        return -1;
    }
    return real_recv(fd, buf, len, flags);
}
