/* Does concurrency pay for the copy's per-file work on this filesystem?
   Two questions the first floor left open: whether siblings of ONE directory
   may be created concurrently (what a pool over `each` would do), and whether
   the link may be deferred out of the write's path and issued after it.
   doc/lazy-store/05-validation.md section 6, *The kernel time is the link*.
   Run by linux-measure-floor-directory-concurrency.nix. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int NFILES = 40000, NDIRS = 40;

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
    const char * root;
    const char * blobs;
    int ndirs;    /* how many directories the files are spread over */
    int link;     /* 1: link as each file is written */
    int from, to; /* the file numbers modulo ndirs this thread does */
};

static void one(int dirFd, const char * root, const char * blobs, int i, int link)
{
    char name[64], blob[512];
    snprintf(name, sizeof name, "f%d", i);
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
    if (link) {
        snprintf(blob, sizeof blob, "%s/%064x", blobs, i);
        if (linkat(dirFd, name, AT_FDCWD, blob, 0) == -1)
            die("linkat");
    }
}

static void * run(void * arg)
{
    struct job * j = arg;
    char path[512];
    for (int d = j->from; d < j->to; d++) {
        snprintf(path, sizeof path, "%s/dir%d", j->root, d % j->ndirs);
        int dirFd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dirFd < 0)
            die("opening directory");
        for (int i = d; i < NFILES; i += NDIRS)
            one(dirFd, j->root, j->blobs, i, j->link);
        close(dirFd);
    }
    return NULL;
}

/* Threads share ONE directory: each takes every Tth file of it. */
struct sharejob
{
    const char * root;
    const char * blobs;
    int link, t, threads;
};

static void * runShared(void * arg)
{
    struct sharejob * j = arg;
    char path[512];
    snprintf(path, sizeof path, "%s/dir0", j->root);
    int dirFd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirFd < 0)
        die("opening directory");
    for (int i = j->t; i < NFILES; i += j->threads)
        one(dirFd, j->root, j->blobs, i, j->link);
    close(dirFd);
    return NULL;
}

static void mkdirs(const char * root, const char * blobs, int ndirs)
{
    char p[512];
    if (mkdir(root, 0777) && errno != EEXIST)
        die("mkdir root");
    for (int d = 0; d < ndirs; d++) {
        snprintf(p, sizeof p, "%s/dir%d", root, d);
        if (mkdir(p, 0777) && errno != EEXIST)
            die("mkdir dir");
    }
    if (blobs && mkdir(blobs, 0777) && errno != EEXIST)
        die("mkdir blobs");
}

/* Link every file of `root`'s directories into `blobs`, sequentially, after
   they are all written: the deferred link. */
static double linkAfter(const char * root, const char * blobs, int ndirs)
{
    char path[512], name[64], blob[512];
    double t0 = now();
    for (int d = 0; d < ndirs; d++) {
        snprintf(path, sizeof path, "%s/dir%d", root, d);
        int dirFd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dirFd < 0)
            die("opening directory");
        for (int i = d; i < NFILES; i += ndirs) {
            snprintf(name, sizeof name, "f%d", i);
            snprintf(blob, sizeof blob, "%s/%064x", blobs, i);
            if (linkat(dirFd, name, AT_FDCWD, blob, 0) == -1)
                die("linkat");
        }
        close(dirFd);
    }
    return now() - t0;
}

/* <label> <mode> <threads> <link>
   mode: spread  -- the files over 40 directories, a thread per group
         shared  -- all files in one directory, threads interleaved
         defer   -- spread, written by <threads> threads, then linked in one pass */
int main(int argc, char ** argv)
{
    if (argc < 5)
        return 2;
    const char * label = argv[1];
    const char * mode = argv[2];
    int threads = atoi(argv[3]), link = atoi(argv[4]);
    if (getenv("FLOOR_N"))
        NFILES = atoi(getenv("FLOOR_N"));
    char root[256], blobs[256];
    snprintf(root, sizeof root, "%s/f2-%s-%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", label, getpid());
    snprintf(blobs, sizeof blobs, "%s.blobs", root);
    int shared = strcmp(mode, "shared") == 0;
    int ndirs = shared ? 1 : NDIRS;
    mkdirs(root, blobs, ndirs);
    pthread_t th[64];
    double t0 = now(), tl = 0;
    if (shared) {
        static struct sharejob sj[64];
        for (int t = 0; t < threads; t++) {
            sj[t] = (struct sharejob) {root, blobs, link, t, threads};
            if (pthread_create(&th[t], NULL, runShared, &sj[t]))
                die("pthread_create");
        }
        for (int t = 0; t < threads; t++)
            pthread_join(th[t], NULL);
    } else {
        static struct job jobs[64];
        if (threads > NDIRS)
            threads = NDIRS;
        for (int t = 0; t < threads; t++) {
            jobs[t] = (struct job) {root,
                                    blobs,
                                    NDIRS,
                                    strcmp(mode, "defer") == 0 ? 0 : link,
                                    t * NDIRS / threads,
                                    (t + 1) * NDIRS / threads};
            if (pthread_create(&th[t], NULL, run, &jobs[t]))
                die("pthread_create");
        }
        for (int t = 0; t < threads; t++)
            pthread_join(th[t], NULL);
        if (strcmp(mode, "defer") == 0 && link)
            tl = linkAfter(root, blobs, NDIRS);
    }
    double t1 = now();
    printf(
        "%-24s mode=%-7s threads=%-3d link=%d  %.3f s  (%.1f us/file)%s\n",
        label,
        mode,
        threads,
        link,
        t1 - t0,
        (t1 - t0) * 1e6 / NFILES,
        tl > 0 ? "" : "");
    if (tl > 0)
        printf("%-24s   of which the deferred link pass: %.3f s\n", label, tl);
    fflush(stdout);
    return 0;
}
