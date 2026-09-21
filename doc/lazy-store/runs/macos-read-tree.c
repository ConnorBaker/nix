/* Open, read and close every regular file under a tree; time the opens.  The
   control for the macOS numbers of doc/lazy-store/06-holder-index.md section
   4: it isolates the per-open tax of this filesystem and its agent from
   anything Nix does.  Build with `cc -Wall -O2 -o readtree macos-read-tree.c`
   and run `./readtree <tree> <label>`. */
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static double topen = 0, tread = 0;
static long nopen = 0;

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void walk(const char * dir)
{
    DIR * d = opendir(dir);
    if (!d)
        return;
    struct dirent * e;
    char p[4096];
    char buf[65536];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strcmp(e->d_name, ".git"))
            continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(p, &st))
            continue;
        if (S_ISDIR(st.st_mode)) {
            walk(p);
            continue;
        }
        if (!S_ISREG(st.st_mode))
            continue;
        double a = now();
        int fd = open(p, O_RDONLY | O_CLOEXEC);
        double b = now();
        topen += b - a;
        nopen++;
        if (fd < 0)
            continue;
        double c = now();
        while (read(fd, buf, sizeof buf) > 0) {
        }
        double e2 = now();
        tread += e2 - c;
        close(fd);
    }
    closedir(d);
}

int main(int argc, char ** argv)
{
    double t0 = now();
    walk(argv[1]);
    double t1 = now();
    printf(
        "%-28s files=%-7ld wall %.3f s  open %.2f us/call (%.2f s)  read %.2f us/call\n",
        argv[2] ? argv[2] : argv[1],
        nopen,
        t1 - t0,
        topen * 1e6 / nopen,
        topen,
        tread * 1e6 / nopen);
    return 0;
}
