/* The holder index at a real store's size, against the filesystem directory
   it replaces: doc/lazy-store/06-holder-index.md section 4, *the matched
   probe costs*.  Run by linux-measure-holder-index-scale.nix.

   The question: `.objects/blobs/<xx>/<id>` is a map from blob to file that
   the filesystem maintains; `Blobs(id, exec, …)` is the same map in SQLite.
   At forty thousand entries the two are within noise of each other on the
   probe (section 10).  A real /nix/store holds 10^7 to 10^8 distinct blobs.
   This measures both at that size:

     (a) the probe -- one point query against one `lstat` -- for a miss (what
         a first entry does for every file) and for a hit;
     (a') the batched probe: `WHERE id IN (…)` in chunks, and the prefetch
         bound (one scan into a hash set, then a memory lookup);
     (a'') what it costs to *have* 10^7 entries: the time to create the files
         against the time to insert the rows;
     (b) the insert of one path's forty thousand rows into a table that size;
     (c) the bytes: the database file, with the identifier as 64-char text
         and as a 32-byte blob, against the filesystem's inodes and dirents.

   Random identifiers, so neither side gets a locality it would not have.
   The probes are random over the whole space, so both sides miss their
   caches as they would on a real store; neither can drop caches inside a
   build sandbox, and that is stated rather than worked around. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <errno.h>
#include <sqlite3.h>

static char root[512];

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

/* A deterministic 256-bit identifier from an index: splitmix64 over four
   words, rendered as git renders one. */
static void idOf(unsigned long long n, unsigned char out[32])
{
    for (int w = 0; w < 4; w++) {
        unsigned long long x = n * 4u + w + 0x9e3779b97f4a7c15ULL;
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        for (int b = 0; b < 8; b++)
            out[w * 8 + b] = (unsigned char) (x >> (8 * b));
    }
}

static void hexOf(const unsigned char d[32], char out[65])
{
    static const char * h = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2 * i] = h[d[i] >> 4];
        out[2 * i + 1] = h[d[i] & 15];
    }
    out[64] = 0;
}

static void sh(const char * fmt, ...)
{
    char cmd[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    if (system(cmd))
        fprintf(stderr, "note: failed: %s\n", cmd);
}

static void exec_(sqlite3 * db, const char * sql)
{
    char * err = 0;
    if (sqlite3_exec(db, sql, 0, 0, &err) != SQLITE_OK) {
        fprintf(stderr, "%s: %s\n", sql, err ? err : "?");
        exit(1);
    }
}

/* The local store's own pragmas (`local-store.cc:552-590`). */
static sqlite3 * openDb(const char * file, int fresh, int blobId)
{
    if (fresh)
        sh("rm -f %s %s-wal %s-shm", file, file, file);
    sqlite3 * db;
    if (sqlite3_open(file, &db) != SQLITE_OK) {
        fprintf(stderr, "open db %s\n", file);
        exit(1);
    }
    exec_(db, "pragma foreign_keys = 1");
    exec_(db, "pragma synchronous = normal");
    exec_(db, "pragma main.journal_mode = wal");
    exec_(db, "pragma journal_size_limit = 1099511627776");
    exec_(db, "pragma wal_autocheckpoint = 40000");
    exec_(db, "pragma cache_size = -16000");
    if (fresh) {
        exec_(db, "create table ValidPaths (id integer primary key autoincrement not null, path text unique not null)");
        char sql[600];
        snprintf(
            sql,
            sizeof sql,
            "create table Blobs ("
            " id %s not null, exec integer not null,"
            " ino integer not null, dev integer not null,"
            " path integer not null, name text not null, size integer not null,"
            " primary key (id, exec),"
            " foreign key (path) references ValidPaths(id) on delete cascade)",
            blobId ? "blob" : "text");
        exec_(db, sql);
        exec_(db, "create index IndexBlobsPath on Blobs(path)");
    }
    return db;
}

/* Fill the table to `n` rows: one `ValidPaths` row per 40 000 blobs, as a
   store of 40 000-file paths would have. */
static double fill(sqlite3 * db, unsigned long long n, int blobId)
{
    sqlite3_stmt * insV;
    sqlite3_stmt * ins;
    sqlite3_prepare_v2(db, "insert into ValidPaths (path) values (?)", -1, &insV, 0);
    sqlite3_prepare_v2(
        db,
        "insert or replace into Blobs (id, exec, ino, dev, path, name, size) values (?, ?, ?, ?, ?, ?, ?)",
        -1,
        &ins,
        0);
    double t0 = now();
    long long pathId = 0;
    exec_(db, "begin");
    for (unsigned long long i = 0; i < n; i++) {
        if (i % 40000 == 0) {
            char p[128];
            snprintf(p, sizeof p, "/nix/store/%016llx-path-%llu", i, i);
            sqlite3_bind_text(insV, 1, p, -1, SQLITE_TRANSIENT);
            sqlite3_step(insV);
            sqlite3_reset(insV);
            pathId = sqlite3_last_insert_rowid(db);
        }
        unsigned char d[32];
        char h[65];
        idOf(i, d);
        if (blobId)
            sqlite3_bind_blob(ins, 1, d, 32, SQLITE_TRANSIENT);
        else {
            hexOf(d, h);
            sqlite3_bind_text(ins, 1, h, 64, SQLITE_TRANSIENT);
        }
        sqlite3_bind_int(ins, 2, 0);
        sqlite3_bind_int64(ins, 3, (long long) (1000000 + i));
        sqlite3_bind_int64(ins, 4, 66);
        sqlite3_bind_int64(ins, 5, pathId);
        char name[64];
        snprintf(name, sizeof name, "dir%llu/f%llu", i % 40, i);
        sqlite3_bind_text(ins, 6, name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(ins, 7, 12);
        if (sqlite3_step(ins) != SQLITE_DONE) {
            fprintf(stderr, "insert: %s\n", sqlite3_errmsg(db));
            exit(1);
        }
        sqlite3_reset(ins);
        if ((i + 1) % 16384 == 0) {
            exec_(db, "commit");
            exec_(db, "begin");
        }
    }
    exec_(db, "commit");
    sqlite3_finalize(ins);
    sqlite3_finalize(insV);
    return now() - t0;
}

/* `probes` point queries over the space `[0,n)` (hits) or `[n,2n)` (misses). */
static double pointProbe(sqlite3 * db, unsigned long long n, int hit, int probes, int blobId, int * found)
{
    sqlite3_stmt * sel;
    sqlite3_prepare_v2(db, "select ino, path, name, size from Blobs where id = ? and exec = ?", -1, &sel, 0);
    *found = 0;
    double t0 = now();
    for (int k = 0; k < probes; k++) {
        unsigned long long i = ((unsigned long long) rand() * 2654435761u) % n + (hit ? 0 : n);
        unsigned char d[32];
        char h[65];
        idOf(i, d);
        if (blobId)
            sqlite3_bind_blob(sel, 1, d, 32, SQLITE_TRANSIENT);
        else {
            hexOf(d, h);
            sqlite3_bind_text(sel, 1, h, 64, SQLITE_TRANSIENT);
        }
        sqlite3_bind_int(sel, 2, 0);
        if (sqlite3_step(sel) == SQLITE_ROW)
            (*found)++;
        sqlite3_reset(sel);
    }
    double dt = now() - t0;
    sqlite3_finalize(sel);
    return dt / probes * 1e6; /* microseconds a probe */
}

/* The same in chunks of `chunk` identifiers, one statement a chunk. */
static double batchProbe(sqlite3 * db, unsigned long long n, int hit, int probes, int chunk)
{
    double t0 = now();
    int done = 0;
    while (done < probes) {
        int m = probes - done < chunk ? probes - done : chunk;
    std_again:;
        char sql[64 * 1024];
        int off = snprintf(sql, sizeof sql, "select id from Blobs where exec = 0 and id in (");
        for (int k = 0; k < m; k++) {
            unsigned long long i = ((unsigned long long) (done + k) * 2654435761u) % n + (hit ? 0 : n);
            unsigned char d[32];
            char h[65];
            idOf(i, d);
            hexOf(d, h);
            off += snprintf(sql + off, sizeof sql - off, "%s'%s'", k ? "," : "", h);
        }
        snprintf(sql + off, sizeof sql - off, ")");
        sqlite3_stmt * st;
        if (sqlite3_prepare_v2(db, sql, -1, &st, 0) != SQLITE_OK) {
            fprintf(stderr, "batch prepare: %s\n", sqlite3_errmsg(db));
            exit(1);
        }
        while (sqlite3_step(st) == SQLITE_ROW)
            ;
        sqlite3_finalize(st);
        done += m;
        (void) 0;
        if (0)
            goto std_again;
    }
    return (now() - t0) / probes * 1e6;
}

/* The prefetch bound: one scan of the table.  Reported as the whole scan and
   per row; the probe after it is a hash lookup, a few nanoseconds, and is not
   the question -- the scan is. */
static double scan(sqlite3 * db, unsigned long long * rows)
{
    sqlite3_stmt * st;
    sqlite3_prepare_v2(db, "select id, exec from Blobs", -1, &st, 0);
    double t0 = now();
    unsigned long long n = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        (void) sqlite3_column_text(st, 0);
        n++;
    }
    double dt = now() - t0;
    sqlite3_finalize(st);
    *rows = n;
    return dt;
}

static long long fileSize(const char * p)
{
    struct stat st;
    if (stat(p, &st))
        return -1;
    return (long long) st.st_size;
}

/* The filesystem's side: `n` empty files under `blobs/<xx>/<id>`. */
static double makeFiles(const char * dir, unsigned long long n)
{
    char p[1024];
    sh("rm -rf %s", dir);
    mkdir(dir, 0755);
    for (int i = 0; i < 256; i++) {
        snprintf(p, sizeof p, "%s/%02x", dir, i);
        mkdir(p, 0755);
    }
    double t0 = now();
    for (unsigned long long i = 0; i < n; i++) {
        unsigned char d[32];
        char h[65];
        idOf(i, d);
        hexOf(d, h);
        snprintf(p, sizeof p, "%s/%c%c/%s", dir, h[0], h[1], h);
        int fd = open(p, O_CREAT | O_EXCL | O_WRONLY, 0444);
        if (fd < 0) {
            fprintf(stderr, "creating %s: %s\n", p, strerror(errno));
            exit(1);
        }
        close(fd);
    }
    return now() - t0;
}

static double lstatProbe(const char * dir, unsigned long long n, int hit, int probes, int * found)
{
    char p[1024];
    *found = 0;
    double t0 = now();
    for (int k = 0; k < probes; k++) {
        unsigned long long i = ((unsigned long long) rand() * 2654435761u) % n + (hit ? 0 : n);
        unsigned char d[32];
        char h[65];
        idOf(i, d);
        hexOf(d, h);
        snprintf(p, sizeof p, "%s/%c%c/%s", dir, h[0], h[1], h);
        struct stat st;
        if (!lstat(p, &st))
            (*found)++;
    }
    return (now() - t0) / probes * 1e6;
}

int main(int argc, char ** argv)
{
    snprintf(root, sizeof root, "%s", argc > 1 ? argv[1] : ".");
    unsigned long long dbSizes[3] = {1000000ULL, 10000000ULL, 0};
    unsigned long long fileSizes[2] = {1000000ULL, 0};
    if (argc > 2)
        dbSizes[1] = strtoull(argv[2], 0, 10);
    if (argc > 3)
        fileSizes[0] = strtoull(argv[3], 0, 10);
    /* `trim`: the largest database size alone, the identifier as text alone,
       and no files -- so that one long fill is the whole of the run.  The
       untrimmed run of all four fills plus the files was cancelled by a
       daemon disconnect after about three hours (scale.err). */
    int trim = argc > 4 && !strcmp(argv[4], "trim");
    int filesOnly = argc > 4 && !strcmp(argv[4], "filesonly");
    if (trim) {
        dbSizes[0] = dbSizes[1];
        dbSizes[1] = 0;
        fileSizes[0] = 0;
    }

    struct statvfs vfs;
    if (!statvfs(root, &vfs))
        printf(
            "filesystem at %s: %.1f GiB free, %llu free inodes\n",
            root,
            vfs.f_bavail * (double) vfs.f_frsize / (1 << 30),
            (unsigned long long) vfs.f_favail);

    for (int s = 0; dbSizes[s] && !filesOnly; s++) {
        unsigned long long n = dbSizes[s];
        for (int blobId = 0; blobId < (trim ? 1 : 2); blobId++) {
            char file[600];
            snprintf(file, sizeof file, "%s/scale-%llu-%s.sqlite", root, n, blobId ? "blob" : "text");
            sqlite3 * db = openDb(file, 1, blobId);
            double tf = fill(db, n, blobId);
            long long bytes = fileSize(file);
            printf(
                "\n== %llu rows, id as %-4s : fill %.1f s (%.2f us/row), file %.2f GiB (%.0f bytes/row)\n",
                n,
                blobId ? "blob" : "text",
                tf,
                tf / n * 1e6,
                bytes / (double) (1 << 30),
                bytes / (double) n);
            int found;
            for (int trial = 0; trial < 3; trial++) {
                double miss = pointProbe(db, n, 0, 5000, blobId, &found);
                double hits = pointProbe(db, n, 1, 5000, blobId, &found);
                printf("   trial %d  point probe: miss %.2f us   hit %.2f us\n", trial, miss, hits);
            }
            if (!blobId) {
                for (int trial = 0; trial < 3; trial++)
                    printf(
                        "   trial %d  batched IN(500): miss %.2f us/id   hit %.2f us/id\n",
                        trial,
                        batchProbe(db, n, 0, 5000, 500),
                        batchProbe(db, n, 1, 5000, 500));
                unsigned long long rows;
                for (int trial = 0; trial < 3; trial++) {
                    double t = scan(db, &rows);
                    printf(
                        "   trial %d  prefetch scan of %llu rows: %.2f s (%.2f us/row)\n",
                        trial,
                        rows,
                        t,
                        t / rows * 1e6);
                }
                /* (b) one path's rows inserted into a table this size. */
                for (int trial = 0; trial < 3; trial++) {
                    sqlite3_stmt * insV;
                    sqlite3_prepare_v2(db, "insert into ValidPaths (path) values (?)", -1, &insV, 0);
                    char p[128];
                    snprintf(p, sizeof p, "/nix/store/%016x-new-%d", rand(), trial);
                    sqlite3_bind_text(insV, 1, p, -1, SQLITE_TRANSIENT);
                    sqlite3_step(insV);
                    long long pathId = sqlite3_last_insert_rowid(db);
                    sqlite3_finalize(insV);
                    sqlite3_stmt * ins;
                    sqlite3_prepare_v2(
                        db,
                        "insert or replace into Blobs (id, exec, ino, dev, path, name, size) values (?, ?, ?, ?, ?, ?, ?)",
                        -1,
                        &ins,
                        0);
                    double t0 = now();
                    exec_(db, "begin");
                    for (int i = 0; i < 40000; i++) {
                        unsigned char d[32];
                        char h[65];
                        idOf(n + 1000000ULL * (trial + 1) + i, d);
                        hexOf(d, h);
                        sqlite3_bind_text(ins, 1, h, 64, SQLITE_TRANSIENT);
                        sqlite3_bind_int(ins, 2, 0);
                        sqlite3_bind_int64(ins, 3, 9000000 + i);
                        sqlite3_bind_int64(ins, 4, 66);
                        sqlite3_bind_int64(ins, 5, pathId);
                        char name[64];
                        snprintf(name, sizeof name, "dir%d/f%d", i % 40, i);
                        sqlite3_bind_text(ins, 6, name, -1, SQLITE_TRANSIENT);
                        sqlite3_bind_int64(ins, 7, 12);
                        sqlite3_step(ins);
                        sqlite3_reset(ins);
                        if ((i + 1) % 16384 == 0) {
                            exec_(db, "commit");
                            exec_(db, "begin");
                        }
                    }
                    exec_(db, "commit");
                    sqlite3_finalize(ins);
                    double t1 = now();
                    /* and the cascade that takes them away again */
                    double t2 = now();
                    sqlite3_stmt * del;
                    sqlite3_prepare_v2(db, "delete from ValidPaths where id = ?", -1, &del, 0);
                    sqlite3_bind_int64(del, 1, pathId);
                    exec_(db, "begin");
                    sqlite3_step(del);
                    exec_(db, "commit");
                    sqlite3_finalize(del);
                    printf(
                        "   trial %d  40000 rows inserted: %.3f s   the cascade removing them: %.3f s\n",
                        trial,
                        t1 - t0,
                        now() - t2);
                }
            }
            sqlite3_close(db);
            if (blobId)
                sh("rm -f %s %s-wal %s-shm", file, file, file);
        }
    }

    for (int s = 0; fileSizes[s]; s++) {
        unsigned long long n = fileSizes[s];
        char dir[600];
        snprintf(dir, sizeof dir, "%s/blobs-%llu", root, n);
        if (!statvfs(root, &vfs) && (unsigned long long) vfs.f_favail < n + 100000) {
            printf("\n== %llu blob files: skipped, only %llu free inodes\n", n, (unsigned long long) vfs.f_favail);
            continue;
        }
        double tc = makeFiles(dir, n);
        printf("\n== %llu blob files under blobs/<xx>/: created in %.1f s (%.2f us/file)\n", n, tc, tc / n * 1e6);
        int found;
        for (int trial = 0; trial < 3; trial++) {
            double miss = lstatProbe(dir, n, 0, 5000, &found);
            double hits = lstatProbe(dir, n, 1, 5000, &found);
            printf("   trial %d  lstat probe: miss %.2f us   hit %.2f us\n", trial, miss, hits);
        }
        double t0 = now();
        sh("rm -rf %s", dir);
        printf("   removing them all (the sweep's floor): %.1f s\n", now() - t0);
    }
    return 0;
}
