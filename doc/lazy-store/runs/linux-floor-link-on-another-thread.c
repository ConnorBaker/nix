/* Can the link that enters a written file as its blob be taken off the
   walk's thread?  The walk writes the files in its own order, as now, and
   hands each one's name to a single other thread that links it into the
   flat blobs directory: one thread still touches that directory, so there
   is no contention to lose, and the link's kernel time runs beside the
   write's instead of after it.  Compared with doing both on one thread.
   doc/lazy-store/01-specification.md section 10, *A written file is entered as
   its blob on the walk's own thread*; 05-validation.md section 6.  Run by
   linux-measure-floor-link-on-another-thread.nix. */
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
static char root[256], blobs[256];

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

/* The queue of files written and not yet linked. */
struct item
{
    int dirFd;
    char name[32];
    int i;
};

#define QCAP 4096
static struct item queue[QCAP];
static int qHead = 0, qTail = 0, qDone = 0;
static pthread_mutex_t qLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qNotEmpty = PTHREAD_COND_INITIALIZER, qNotFull = PTHREAD_COND_INITIALIZER;

static void push(int dirFd, const char * name, int i)
{
    pthread_mutex_lock(&qLock);
    while ((qTail + 1) % QCAP == qHead)
        pthread_cond_wait(&qNotFull, &qLock);
    queue[qTail].dirFd = dirFd;
    snprintf(queue[qTail].name, sizeof queue[qTail].name, "%s", name);
    queue[qTail].i = i;
    qTail = (qTail + 1) % QCAP;
    pthread_cond_signal(&qNotEmpty);
    pthread_mutex_unlock(&qLock);
}

static void * linker(void * arg)
{
    (void) arg;
    char blob[512];
    for (;;) {
        pthread_mutex_lock(&qLock);
        while (qHead == qTail && !qDone)
            pthread_cond_wait(&qNotEmpty, &qLock);
        if (qHead == qTail && qDone) {
            pthread_mutex_unlock(&qLock);
            return NULL;
        }
        struct item it = queue[qHead];
        qHead = (qHead + 1) % QCAP;
        pthread_cond_signal(&qNotFull);
        pthread_mutex_unlock(&qLock);
        snprintf(blob, sizeof blob, "%s/%064x", blobs, it.i);
        if (linkat(it.dirFd, it.name, AT_FDCWD, blob, 0) == -1)
            die("linkat");
    }
}

/* mode 0: write only.  1: write and link on this thread.  2: write here,
   link on one other thread. */
static double run(int mode)
{
    char path[512], name[32], blob[512];
    pthread_t th;
    qHead = qTail = qDone = 0;
    double t0 = now();
    if (mode == 2 && pthread_create(&th, NULL, linker, NULL))
        die("pthread_create");
    for (int d = 0; d < NDIRS; d++) {
        snprintf(path, sizeof path, "%s/dir%d", root, d);
        int dirFd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dirFd < 0)
            die("opening directory");
        for (int i = d; i < NFILES; i += NDIRS) {
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
            if (mode == 1) {
                snprintf(blob, sizeof blob, "%s/%064x", blobs, i);
                if (linkat(dirFd, name, AT_FDCWD, blob, 0) == -1)
                    die("linkat");
            } else if (mode == 2)
                push(dirFd, name, i);
        }
        /* The directory's descriptor must outlive the links queued from it;
           the walk would hand its own over the same way. */
        if (mode != 2)
            close(dirFd);
    }
    if (mode == 2) {
        pthread_mutex_lock(&qLock);
        qDone = 1;
        pthread_cond_broadcast(&qNotEmpty);
        pthread_mutex_unlock(&qLock);
        pthread_join(th, NULL);
    }
    return now() - t0;
}

int main(int argc, char ** argv)
{
    if (argc < 3)
        return 2;
    const char * label = argv[1];
    int mode = atoi(argv[2]);
    if (getenv("FLOOR_N"))
        NFILES = atoi(getenv("FLOOR_N"));
    snprintf(root, sizeof root, "%s/f3-%s-%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", label, getpid());
    snprintf(blobs, sizeof blobs, "%s.blobs", root);
    char p[512];
    if (mkdir(root, 0777) && errno != EEXIST)
        die("mkdir root");
    for (int d = 0; d < NDIRS; d++) {
        snprintf(p, sizeof p, "%s/dir%d", root, d);
        if (mkdir(p, 0777) && errno != EEXIST)
            die("mkdir dir");
    }
    if (mkdir(blobs, 0777) && errno != EEXIST)
        die("mkdir blobs");
    double t = run(mode);
    printf("%-22s mode=%d  %.3f s  (%.1f us/file)\n", label, mode, t, t * 1e6 / NFILES);
    fflush(stdout);
    return 0;
}
