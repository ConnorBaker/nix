/* Per-call cost of the copy walk's file-system operations on this machine.
   Each named op is timed individually and accumulated, so the table gives
   microseconds per call as well as the phase wall time.  This is where APFS
   with an endpoint-security agent charges 248-436 us for one `linkat` at one
   thread against ext4's ~5 us: doc/lazy-store/06-holder-index.md section 1.
   Build with `cc -Wall -O2 -o macfloor macos-floor-write-and-link.c
   -lpthread` and run it from macos-first-entry-phases.sh's directory; there
   is no derivation, the measurement being of this machine. */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int NFILES = 4000, NDIRS = 40, SHARDS = 0;

enum {
    OP_LSTAT_MISS,
    OP_OPEN,
    OP_WRITE,
    OP_FCHMOD,
    OP_FUTIMENS,
    OP_FSYNC,
    OP_CLOSE,
    OP_FSTATAT,
    OP_LINK,
    OP_RENAME,
    OP_UNLINK,
    OP_LSTAT_HIT,
    OP_READ,
    OP_N
};

static const char * opName[OP_N] = {
    "lstat-miss",
    "open-create",
    "write",
    "fchmod",
    "futimens",
    "fsync",
    "close",
    "fstatat",
    "linkat",
    "rename",
    "unlink",
    "lstat-hit",
    "read"};

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void die(const char * what)
{
    fprintf(stderr, "%s: %s\n", what, strerror(errno));
    exit(1);
}

struct acc
{
    double t[OP_N];
    long n[OP_N];
};

struct job
{
    const char * root;
    const char * blobs;
    int doLstatMiss, doChmod, doTimes, doFsync, doStatIn, doLink, doRename, doReadBack;
    int from, to;
    struct acc acc;
};

#define fl(c) (strchr(flags, (c)) != NULL)
#define TIME(j, op, expr)          \
    do {                           \
        double _a = now();         \
        expr;                      \
        double _b = now();         \
        (j)->acc.t[op] += _b - _a; \
        (j)->acc.n[op]++;          \
    } while (0)

static void * run(void * arg)
{
    struct job * j = arg;
    char path[1024], blob[1024], name[64], tmpname[64];
    for (int d = j->from; d < j->to; d++) {
        snprintf(path, sizeof path, "%s/dir%d", j->root, d);
        int dirFd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dirFd < 0)
            die("opening directory");
        for (int i = d; i < NFILES; i += NDIRS) {
            snprintf(name, sizeof name, "f%d", i);
            snprintf(tmpname, sizeof tmpname, ".tmp-f%d", i);
            if (SHARDS)
                snprintf(blob, sizeof blob, "%s/%02x/%062x", j->blobs ? j->blobs : "", i & 0xff, i);
            else
                snprintf(blob, sizeof blob, "%s/%064x", j->blobs ? j->blobs : "", i);
            struct stat st;
            if (j->doLstatMiss)
                TIME(j, OP_LSTAT_MISS, fstatat(AT_FDCWD, blob, &st, AT_SYMLINK_NOFOLLOW));
            const char * created = j->doRename ? tmpname : name;
            int fd = -1;
            TIME(j, OP_OPEN, fd = openat(dirFd, created, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0666));
            if (fd < 0)
                die("creating file");
            char buf[64];
            int n = snprintf(buf, sizeof buf, "content %d\n", i);
            ssize_t w;
            TIME(j, OP_WRITE, w = write(fd, buf, n));
            if (w != n)
                die("writing file");
            if (j->doChmod)
                TIME(j, OP_FCHMOD, if (fchmod(fd, 0444) == -1) die("fchmod"));
            if (j->doTimes) {
                struct timespec tv[2] = {{1, 0}, {1, 0}};
                TIME(j, OP_FUTIMENS, if (futimens(fd, tv) == -1) die("futimens"));
            }
            if (j->doFsync)
                TIME(j, OP_FSYNC, if (fsync(fd) == -1) die("fsync"));
            TIME(j, OP_CLOSE, close(fd));
            if (j->doRename)
                TIME(j, OP_RENAME, if (renameat(dirFd, tmpname, dirFd, name) == -1) die("rename"));
            if (j->doStatIn)
                TIME(j, OP_FSTATAT, fstatat(dirFd, name, &st, AT_SYMLINK_NOFOLLOW));
            if (j->doLink)
                TIME(j, OP_LINK, if (linkat(dirFd, name, AT_FDCWD, blob, 0) == -1) die("linkat"));
            if (j->doReadBack) {
                int rfd;
                char rb[64];
                TIME(j, OP_READ, {
                    rfd = openat(dirFd, name, O_RDONLY | O_CLOEXEC);
                    if (rfd >= 0) {
                        read(rfd, rb, sizeof rb);
                        close(rfd);
                    }
                });
            }
        }
        close(dirFd);
    }
    return NULL;
}

static void mkdirs(const char * root, const char * blobs)
{
    char p[1024];
    if (mkdir(root, 0777) && errno != EEXIST)
        die("mkdir root");
    for (int d = 0; d < NDIRS; d++) {
        snprintf(p, sizeof p, "%s/dir%d", root, d);
        if (mkdir(p, 0777) && errno != EEXIST)
            die("mkdir dir");
    }
    if (blobs) {
        if (mkdir(blobs, 0777) && errno != EEXIST)
            die("mkdir blobs");
        if (SHARDS)
            for (int s = 0; s < 256; s++) {
                snprintf(p, sizeof p, "%s/%02x", blobs, s);
                if (mkdir(p, 0777) && errno != EEXIST)
                    die("mkdir shard");
            }
    }
}

/* floor <label> <threads> <flags string> <shards>
   flags: l=lstat-miss c=chmod t=times s=fsync i=statIn k=link r=rename b=readback */
int main(int argc, char ** argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: macfloor <label> <threads> <flags> <shards>\n");
        return 2;
    }
    const char * label = argv[1];
    int threads = atoi(argv[2]);
    const char * flags = argv[3];
    SHARDS = atoi(argv[4]);
    if (getenv("FLOOR_N"))
        NFILES = atoi(getenv("FLOOR_N"));
    char root[512], blobs[600];
    snprintf(root, sizeof root, "%s/floor-%s-%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", label, getpid());
    snprintf(blobs, sizeof blobs, "%s.blobs", root);
    int link = fl('k');
    mkdirs(root, link ? blobs : NULL);
    if (threads > NDIRS)
        threads = NDIRS;
    pthread_t th[64];
    static struct job jobs[64];
    double t0 = now();
    for (int t = 0; t < threads; t++) {
        jobs[t] = (struct job) {.root = root,
                                .blobs = link ? blobs : NULL,
                                .doLstatMiss = fl('l'),
                                .doChmod = fl('c'),
                                .doTimes = fl('t'),
                                .doFsync = fl('s'),
                                .doStatIn = fl('i'),
                                .doLink = link,
                                .doRename = fl('r'),
                                .doReadBack = fl('b'),
                                .from = t * NDIRS / threads,
                                .to = (t + 1) * NDIRS / threads};
        if (pthread_create(&th[t], NULL, run, &jobs[t]))
            die("pthread_create");
    }
    for (int t = 0; t < threads; t++)
        pthread_join(th[t], NULL);
    double t1 = now();
    struct acc a = {{0}};
    for (int t = 0; t < threads; t++)
        for (int o = 0; o < OP_N; o++) {
            a.t[o] += jobs[t].acc.t[o];
            a.n[o] += jobs[t].acc.n[o];
        }
    printf(
        "%-22s threads=%-3d flags=%-8s shards=%d  wall %.3f s  (%.1f us/file)\n",
        label,
        threads,
        flags,
        SHARDS,
        t1 - t0,
        (t1 - t0) * 1e6 / NFILES);
    for (int o = 0; o < OP_N; o++)
        if (a.n[o])
            printf(
                "    %-12s n=%-8ld  %8.2f us/call  %7.3f s total\n", opName[o], a.n[o], a.t[o] * 1e6 / a.n[o], a.t[o]);
    fflush(stdout);
    return 0;
}
