/* The cost of the object store's directories, either way
   (doc/lazy-store/01-specification.md section 9.10, *Where the objects live*;
   section 10, *The shards are made lazily*).  Run by
   linux-measure-floor-object-store-directories.nix:
   (a) the 772 mkdir that making every shard at store creation would cost,
       once;
   (b) the 772 lstat that `createDirectories` would then cost at every
       writable store's open, against
   (c) the 4 lstat it costs when only the four kind directories are made;
   (d) the lazy alternative's whole-lifetime price: per shard one linkat
       that fails with ENOENT and one mkdir, 256 times a kind. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static const char * kinds[3] = {"blobs", "blobs-x", "trees"};

int main(int argc, char ** argv)
{
    const char * root = argc > 1 ? argv[1] : ".";
    char p[4096];
    for (int trial = 0; trial < 3; trial++) {
        snprintf(p, sizeof p, "rm -rf %s/o%d", root, trial);
        if (system(p))
            return 1;
        snprintf(p, sizeof p, "%s/o%d", root, trial);
        if (mkdir(p, 0755)) {
            perror("mkdir root");
            return 1;
        }

        /* (c) the four kind directories: lstat then mkdir. */
        double t0 = now();
        for (int k = 0; k < 3; k++) {
            snprintf(p, sizeof p, "%s/o%d/%s", root, trial, kinds[k]);
            struct stat st;
            lstat(p, &st);
            mkdir(p, 0755);
        }
        snprintf(p, sizeof p, "%s/o%d/tmp", root, trial);
        {
            struct stat st;
            lstat(p, &st);
            mkdir(p, 0755);
        }
        double t1 = now();

        /* (a) the 768 shards. */
        for (int k = 0; k < 3; k++)
            for (int i = 0; i < 256; i++) {
                snprintf(p, sizeof p, "%s/o%d/%s/%02x", root, trial, kinds[k], i);
                if (mkdir(p, 0755)) {
                    perror("mkdir shard");
                    return 1;
                }
            }
        double t2 = now();

        /* (b) the 772 lstat of an object store whose shards all exist. */
        for (int k = 0; k < 3; k++)
            for (int i = 0; i < 256; i++) {
                snprintf(p, sizeof p, "%s/o%d/%s/%02x", root, trial, kinds[k], i);
                struct stat st;
                lstat(p, &st);
            }
        for (int k = 0; k < 3; k++) {
            snprintf(p, sizeof p, "%s/o%d/%s", root, trial, kinds[k]);
            struct stat st;
            lstat(p, &st);
        }
        snprintf(p, sizeof p, "%s/o%d/tmp", root, trial);
        {
            struct stat st;
            lstat(p, &st);
        }
        double t3 = now();

        /* (d) the lazy price: a failed linkat then a mkdir, per shard.
           A fresh tree so the shards are absent again. */
        snprintf(p, sizeof p, "rm -rf %s/l%d", root, trial);
        if (system(p))
            return 1;
        snprintf(p, sizeof p, "%s/l%d", root, trial);
        mkdir(p, 0755);
        for (int k = 0; k < 3; k++) {
            snprintf(p, sizeof p, "%s/l%d/%s", root, trial, kinds[k]);
            mkdir(p, 0755);
        }
        char src[4096];
        snprintf(src, sizeof src, "%s/l%d/src", root, trial);
        {
            int fd = open(src, O_CREAT | O_WRONLY, 0644);
            if (fd < 0) {
                perror("src");
                return 1;
            }
            close(fd);
        }
        double t4 = now();
        for (int k = 0; k < 3; k++)
            for (int i = 0; i < 256; i++) {
                snprintf(p, sizeof p, "%s/l%d/%s/%02x/f", root, trial, kinds[k], i);
                if (link(src, p) == 0) {
                    fprintf(stderr, "unexpected success\n");
                    return 1;
                }
                char d[4096];
                snprintf(d, sizeof d, "%s/l%d/%s/%02x", root, trial, kinds[k], i);
                if (mkdir(d, 0755)) {
                    perror("mkdir lazy");
                    return 1;
                }
                if (link(src, p)) {
                    perror("link");
                    return 1;
                }
            }
        double t5 = now();

        printf(
            "trial %d: four-mkdir %.6f  768-mkdir %.6f  772-lstat %.6f  lazy-768(enoent+mkdir+link) %.6f\n",
            trial,
            t1 - t0,
            t2 - t1,
            t3 - t2,
            t5 - t4);
        fflush(stdout);
    }
    return 0;
}
