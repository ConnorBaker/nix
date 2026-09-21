/* Does the copy walk's parallelism survive the source accessor's lock?
   (doc/lazy-store/05-validation.md section 6, *Does the copy's parallelism
   survive the source accessor's lock?*: `GitSourceAccessor` takes one `Sync`
   for every read, so under any parallel copy the read and the hash stay on
   one thread at a time and only the write side overlaps.)
   Run by linux-measure-floor-parallel-copy.nix.

   The walk's per-file body, in the proportions the real one has:
     under one global mutex:  open the source file, read it, SHA-256 it
     outside the mutex:       open/write/fchmod/futimens/close the
                              destination, then one linkat into
                              blobs/<xx>/<id>, the sharded layout.
   Sequential over 40 directories against T threads over the directories.
   Also the same with no mutex at all, which is the ceiling a per-thread
   accessor handle would reach, so the lock's cost is read off directly. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <errno.h>
#include <openssl/sha.h>

static int NDIR = 40, NPER = 1000;
static char root[512];
static pthread_mutex_t accessorLock = PTHREAD_MUTEX_INITIALIZER;
static int useLock = 1;

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static void hex(const unsigned char * d, char * out)
{
    static const char * h = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2 * i] = h[d[i] >> 4];
        out[2 * i + 1] = h[d[i] & 15];
    }
    out[64] = 0;
}

/* The source tree the walk reads (the repository's blobs). */
static void makeSource(void)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/src", root);
    mkdir(p, 0755);
    for (int d = 0; d < NDIR; d++) {
        snprintf(p, sizeof p, "%s/src/d%d", root, d);
        mkdir(p, 0755);
        for (int i = 0; i < NPER; i++) {
            snprintf(p, sizeof p, "%s/src/d%d/f%d", root, d, i);
            int fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0644);
            char buf[64];
            int n = snprintf(buf, sizeof buf, "content %d %d\n", d, i);
            if (write(fd, buf, n) != n) {
                perror("write src");
                exit(1);
            }
            close(fd);
        }
    }
}

struct Job
{
    int dir;
    const char * dst;
};

static void doDir(int d, const char * dst)
{
    char p[1024], q[1024], h[65];
    unsigned char dig[32];
    char buf[4096];
    snprintf(p, sizeof p, "%s/%s/t/d%d", root, dst, d);
    if (mkdir(p, 0755)) {
        perror("mkdir dst dir");
        exit(1);
    }
    for (int i = 0; i < NPER; i++) {
        /* --- the accessor's share: read and hash, serialised --- */
        ssize_t n;
        if (useLock)
            pthread_mutex_lock(&accessorLock);
        snprintf(p, sizeof p, "%s/src/d%d/f%d", root, d, i);
        int fd = open(p, O_RDONLY);
        if (fd < 0) {
            perror("open src");
            exit(1);
        }
        n = read(fd, buf, sizeof buf);
        close(fd);
        {
            SHA256_CTX c;
            SHA256_Init(&c);
            char hdr[32];
            int hn = snprintf(hdr, sizeof hdr, "blob %zd", n) + 1;
            SHA256_Update(&c, hdr, hn);
            SHA256_Update(&c, buf, n);
            SHA256_Final(dig, &c);
        }
        if (useLock)
            pthread_mutex_unlock(&accessorLock);
        hex(dig, h);

        /* --- the store's share: write the file, then enter it --- */
        snprintf(p, sizeof p, "%s/%s/t/d%d/f%d", root, dst, d, i);
        int wfd = open(p, O_CREAT | O_EXCL | O_WRONLY, 0644);
        if (wfd < 0) {
            perror("open dst");
            exit(1);
        }
        if (write(wfd, buf, n) != n) {
            perror("write dst");
            exit(1);
        }
        if (fchmod(wfd, 0444)) {
            perror("fchmod");
            exit(1);
        }
        struct timespec ts[2] = {{1, 0}, {1, 0}};
        if (futimens(wfd, ts)) {
            perror("futimens");
            exit(1);
        }
        close(wfd);
        snprintf(q, sizeof q, "%s/%s/blobs/%c%c/%s", root, dst, h[0], h[1], h);
        if (link(p, q) && errno != EEXIST) {
            perror("link");
            exit(1);
        }
    }
}

static void * worker(void * arg)
{
    struct Job * js = arg;
    for (int k = 0; js[k].dir >= 0; k++)
        doDir(js[k].dir, js[k].dst);
    return NULL;
}

/* A fresh destination: `t/` for the tree, `blobs/<xx>/` for the objects. */
static void freshDst(const char * dst)
{
    char p[1024];
    snprintf(p, sizeof p, "rm -rf %s/%s", root, dst);
    if (system(p))
        exit(1);
    snprintf(p, sizeof p, "%s/%s", root, dst);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/%s/t", root, dst);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/%s/blobs", root, dst);
    mkdir(p, 0755);
    for (int i = 0; i < 256; i++) {
        snprintf(p, sizeof p, "%s/%s/blobs/%02x", root, dst, i);
        mkdir(p, 0755);
    }
}

static double run(int threads, int lock)
{
    char dst[64];
    snprintf(dst, sizeof dst, "dst%d-%d", threads, lock);
    freshDst(dst);
    useLock = lock;
    double t0 = now();
    if (threads <= 1) {
        for (int d = 0; d < NDIR; d++)
            doDir(d, dst);
    } else {
        pthread_t th[64];
        struct Job * jobs[64];
        for (int t = 0; t < threads; t++) {
            int cap = NDIR / threads + 2;
            jobs[t] = calloc(cap + 1, sizeof(struct Job));
            int k = 0;
            for (int d = t; d < NDIR; d += threads) {
                jobs[t][k].dir = d;
                jobs[t][k].dst = dst;
                k++;
            }
            jobs[t][k].dir = -1;
            pthread_create(&th[t], NULL, worker, jobs[t]);
        }
        for (int t = 0; t < threads; t++)
            pthread_join(th[t], NULL);
    }
    double t1 = now();
    snprintf(dst, sizeof dst, "rm -rf %s/dst%d-%d", root, threads, lock);
    if (system(dst))
        exit(1);
    return t1 - t0;
}

int main(int argc, char ** argv)
{
    snprintf(root, sizeof root, "%s", argc > 1 ? argv[1] : ".");
    makeSource();
    printf("N=%d files in %d directories, blobs sharded 256 ways\n", NDIR * NPER, NDIR);
    for (int trial = 0; trial < 3; trial++) {
        printf("trial %d:", trial);
        printf("  seq %.3f", run(1, 1));
        printf("  |  lock: 4t %.3f  8t %.3f", run(4, 1), run(8, 1));
        printf("  |  no lock: 4t %.3f  8t %.3f", run(4, 0), run(8, 0));
        printf("\n");
        fflush(stdout);
    }
    return 0;
}
