/* LD_PRELOAD shim for fork-alloc.sh: reports to fd 9 an allocator call that
   a forked child makes before it execs. In a multithreaded parent, such a
   call can wait forever for a lock that another thread held at the fork.
   The shim removes itself from LD_PRELOAD, so what the child execs runs
   without it. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static pid_t loadedPid, reportedPid;
static int ready, resolving;
static __thread int inside;

/* dlsym() may allocate before the real functions are known. */
static char early[1 << 16];
static size_t earlyUsed;

static void * earlyAlloc(size_t n)
{
    n = (n + 15) & ~(size_t) 15;
    if (earlyUsed + n > sizeof(early))
        return NULL;
    earlyUsed += n;
    return early + earlyUsed - n;
}

static void report(const char * fn)
{
    pid_t pid = syscall(SYS_getpid);
    if (!ready || pid == loadedPid || pid == reportedPid || fcntl(9, F_GETFD) == -1)
        return;
    reportedPid = pid;
    const char * msg = "fork-alloc: allocator call in a forked child before exec: ";
    if (write(9, msg, strlen(msg)) < 0 || write(9, fn, strlen(fn)) < 0 || write(9, "\n", 1) < 0)
        return;
    void * frames[48];
    backtrace_symbols_fd(frames, backtrace(frames, 48), 9);
}

#define WRAP(ret, name, params, args, early)       \
    static ret(*real_##name) params;               \
    ret name params                                \
    {                                              \
        if (!real_##name) {                        \
            if (resolving)                         \
                early;                             \
            resolving = 1;                         \
            real_##name = dlsym(RTLD_NEXT, #name); \
            resolving = 0;                         \
        }                                          \
        if (!inside) {                             \
            inside = 1;                            \
            report(#name);                         \
            inside = 0;                            \
        }                                          \
        return real_##name args;                   \
    }

WRAP(void *, malloc, (size_t n), (n), return earlyAlloc(n))
WRAP(void *, calloc, (size_t a, size_t b), (a, b), return earlyAlloc(a * b))
WRAP(void *, realloc, (void * p, size_t n), (p, n), return NULL)
WRAP(int, posix_memalign, (void ** p, size_t a, size_t n), (p, a, n), return 12)
WRAP(void *, aligned_alloc, (size_t a, size_t n), (a, n), return NULL)
WRAP(void *, memalign, (size_t a, size_t n), (a, n), return NULL)
WRAP(void *, _Znwm, (size_t n), (n), return NULL)
WRAP(void *, _Znam, (size_t n), (n), return NULL)
WRAP(void *, _ZnwmRKSt9nothrow_t, (size_t n, const void * t), (n, t), return NULL)
WRAP(void *, _ZnamRKSt9nothrow_t, (size_t n, const void * t), (n, t), return NULL)
WRAP(void *, _ZnwmSt11align_val_t, (size_t n, size_t a), (n, a), return NULL)
WRAP(void *, _ZnamSt11align_val_t, (size_t n, size_t a), (n, a), return NULL)

void free(void * p)
{
    static void (*real_free)(void *);
    if (!p || ((char *) p >= early && (char *) p < early + sizeof(early)))
        return;
    if (!real_free)
        real_free = dlsym(RTLD_NEXT, "free");
    if (!inside) {
        inside = 1;
        report("free");
        inside = 0;
    }
    real_free(p);
}

__attribute__((constructor)) static void init(void)
{
    inside = 1;
    void * frames[4];
    backtrace(frames, 4); /* may dlopen libgcc_s the first time */
    unsetenv("LD_PRELOAD");
    loadedPid = syscall(SYS_getpid);
    ready = 1;
    inside = 0;
}
