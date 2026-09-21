/* The floor of the copy's per-file I/O on this filesystem: what creating a
   file costs, what entering it as a shared blob adds, and whether doing it
   from several threads at once costs less wall time.  This is where the
   `linkat`'s 0.19-0.21 s per 40 000 files is measured, which is the whole of
   what the holder design proposes to remove: doc/lazy-store/05-validation.md
   section 6, *The kernel time is the link*, and 06-holder-index.md section 1.
   Run by linux-measure-floor-write-and-link.nix. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static int NFILES = 40000, NDIRS = 40, SHARDS = 0;

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

struct job
{
    const char * root;  /* where the files are created */
    const char * blobs; /* where they are linked, or NULL */
    int statFirst;      /* an fstatat of the blob name before the write */
    int from, to;       /* the directories this thread does */
};

static void * run(void * arg)
{
    struct job * j = arg;
    char path[512], blob[512], name[64];
    for (int d = j->from; d < j->to; d++) {
        snprintf(path, sizeof path, "%s/dir%d", j->root, d);
        int dirFd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dirFd < 0)
            die("opening directory");
        for (int i = d; i < NFILES; i += NDIRS) {
            snprintf(name, sizeof name, "f%d", i);
            /* A blob name of the shape the object store writes: 64 hex. */
            snprintf(blob, sizeof blob, "%s/%s%064x", j->blobs ? j->blobs : "", SHARDS ? "" : "", i);
            if (SHARDS && j->blobs)
                snprintf(blob, sizeof blob, "%s/%02x/%064x", j->blobs, i & 0xff, i);
            if (j->blobs && j->statFirst) {
                struct stat st;
                fstatat(AT_FDCWD, blob, &st, AT_SYMLINK_NOFOLLOW); /* a miss */
            }
            int fd = openat(dirFd, name, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0666);
            if (fd < 0)
                die("creating file");
            char buf[32];
            int n = snprintf(buf, sizeof buf, "content %d\n", i);
            if (write(fd, buf, n) != n)
                die("writing file");
            if (fchmod(fd, 0444) == -1)
                die("chmod");
            struct timespec times[2] = {{1, 0}, {1, 0}};
            if (futimens(fd, times) == -1)
                die("utimens");
            close(fd);
            if (j->blobs)
                if (linkat(dirFd, name, AT_FDCWD, blob, 0) == -1)
                    die("linkat");
        }
        close(dirFd);
    }
    return NULL;
}

static void mkdirs(const char * root, const char * blobs)
{
    char p[512];
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

/* <label> <threads> <link 0|1> <statFirst 0|1> <shards 0|1> */
int main(int argc, char ** argv)
{
    if (argc < 6) {
        fprintf(stderr, "usage: floor <label> <threads> <link> <statFirst> <shards>\n");
        return 2;
    }
    const char * label = argv[1];
    int threads = atoi(argv[2]), link = atoi(argv[3]), statFirst = atoi(argv[4]);
    SHARDS = atoi(argv[5]);
    if (getenv("FLOOR_N"))
        NFILES = atoi(getenv("FLOOR_N"));
    char root[256], blobs[256];
    static int seq = 0;
    snprintf(
        root, sizeof root, "%s/floor-%s-%d-%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", label, getpid(), seq);
    snprintf(blobs, sizeof blobs, "%s.blobs", root);
    mkdirs(root, link ? blobs : NULL);
    if (threads > NDIRS)
        threads = NDIRS;
    pthread_t th[64];
    struct job jobs[64];
    double t0 = now();
    for (int t = 0; t < threads; t++) {
        jobs[t] = (struct job) {
            .root = root,
            .blobs = link ? blobs : NULL,
            .statFirst = statFirst,
            .from = t * NDIRS / threads,
            .to = (t + 1) * NDIRS / threads,
        };
        if (pthread_create(&th[t], NULL, run, &jobs[t]))
            die("pthread_create");
    }
    for (int t = 0; t < threads; t++)
        pthread_join(th[t], NULL);
    double t1 = now();
    printf(
        "%-26s threads=%-3d link=%d stat=%d shards=%d  %.3f s  (%.1f us/file)\n",
        label,
        threads,
        link,
        statFirst,
        SHARDS,
        t1 - t0,
        (t1 - t0) * 1e6 / NFILES);
    fflush(stdout);
    return 0;
}
