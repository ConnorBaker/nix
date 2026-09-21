/* Inject one interrupted waiting flock call, independently for each mode:
 *
 * cc -shared -fPIC -O2 linux-flock-eintr.c -ldl -o /tmp/linux-flock-eintr.so
 * for mode in SH EX; do
 *   env NIX_FLOCK_EINTR="$mode" LD_PRELOAD=/tmp/linux-flock-eintr.so \
 *     /absolute/path/to/nix-store-tests \
 *     --gtest_filter=FdLock.ownershipAndReleaseForEveryAcquisitionCase
 * done
 *
 * Each run must print the injection marker and pass the test. The logger in
 * that test deterministically releases the competing lock before the waiting
 * call. This checks handling of EINTR, not actual signal delivery. An unused
 * injector exits 125, preventing a vacuous pass. No production hook is needed.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

static int (*real_flock)(int, int);
static int requested, injected;

__attribute__((constructor)) static void initialize(void)
{
    const char * mode = getenv("NIX_FLOCK_EINTR");
    if (!mode || (strcmp(mode, "SH") && strcmp(mode, "EX")))
        _exit(125);
    requested = !strcmp(mode, "SH") ? LOCK_SH : LOCK_EX;
    real_flock = dlsym(RTLD_NEXT, "flock");
    if (!real_flock)
        _exit(125);
}

int flock(int fd, int operation)
{
    if (operation == requested && !__atomic_exchange_n(&injected, 1, __ATOMIC_RELAXED)) {
        static const char marker[] = "injected EINTR into waiting flock\n";
        (void) write(STDERR_FILENO, marker, sizeof(marker) - 1);
        errno = EINTR;
        return -1;
    }
    return real_flock(fd, operation);
}

__attribute__((destructor)) static void require_injection(void)
{
    if (!__atomic_load_n(&injected, __ATOMIC_RELAXED))
        _exit(125);
}
