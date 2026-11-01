// SPDX-License-Identifier: GPL-3.0-or-later
// Benchmark primitives for Bench/run.sh (plan Step 11). Each mode prints elapsed seconds.
//
//   io write <file> <MiB>        sequential 4 MiB writes, then fsync
//   io read <file> [nocache]     sequential 4 MiB reads (F_NOCACHE: bypass the page cache)
//   io meta <dir> <count>        create, rename, then delete <count> empty files
//   io small <dir> <count> <seed> create <count> files of 1-16 KiB in 100 subdirectories
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BLOCK (4u << 20)

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static int fail(const char *what, const char *path)
{
    fprintf(stderr, "io: %s %s: %s\n", what, path, strerror(errno));
    return 1;
}

static int do_write(const char *path, long mib)
{
    char *buf = malloc(BLOCK);
    if (!buf)
        return 1;
    for (size_t i = 0; i < BLOCK; i++)
        buf[i] = (char)(i * 2654435761u >> 24);   // not zeros: no shortcut for zero pages
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return fail("open", path);
    double t = now();
    for (long done = 0; done < mib << 20; done += BLOCK)
        if (write(fd, buf, BLOCK) != BLOCK)
            return fail("write", path);
    if (fsync(fd) < 0)
        return fail("fsync", path);
    t = now() - t;
    close(fd);
    printf("%.6f\n", t);
    return 0;
}

static int do_read(const char *path, int nocache)
{
    char *buf = malloc(BLOCK);
    int fd = open(path, O_RDONLY);
    ssize_t n;
    if (!buf || fd < 0)
        return fail("open", path);
    if (nocache && fcntl(fd, F_NOCACHE, 1) < 0)
        return fail("F_NOCACHE", path);
    double t = now();
    while ((n = read(fd, buf, BLOCK)) > 0)
        ;
    if (n < 0)
        return fail("read", path);
    t = now() - t;
    close(fd);
    printf("%.6f\n", t);
    return 0;
}

static int do_meta(const char *dir, long count)
{
    char a[4096], b[4096];
    double t = now();
    for (long i = 0; i < count; i++) {
        snprintf(a, sizeof(a), "%s/m%06ld", dir, i);
        int fd = open(a, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0)
            return fail("create", a);
        close(fd);
    }
    for (long i = 0; i < count; i++) {
        snprintf(a, sizeof(a), "%s/m%06ld", dir, i);
        snprintf(b, sizeof(b), "%s/r%06ld", dir, i);
        if (rename(a, b) < 0)
            return fail("rename", a);
    }
    for (long i = 0; i < count; i++) {
        snprintf(b, sizeof(b), "%s/r%06ld", dir, i);
        if (unlink(b) < 0)
            return fail("unlink", b);
    }
    printf("%.6f\n", now() - t);
    return 0;
}

static int do_small(const char *dir, long count, unsigned seed)
{
    static char buf[16384];
    char path[4096];
    for (size_t i = 0; i < sizeof(buf); i++)
        buf[i] = (char)(i * 40503u >> 8);
    srand(seed);
    double t = now();
    for (int d = 0; d < 100; d++) {
        snprintf(path, sizeof(path), "%s/d%02d", dir, d);
        if (mkdir(path, 0755) < 0 && errno != EEXIST)
            return fail("mkdir", path);
    }
    for (long i = 0; i < count; i++) {
        snprintf(path, sizeof(path), "%s/d%02ld/f%06ld", dir, i % 100, i);
        size_t len = 1024 + (size_t)(rand() % 15360);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0 || write(fd, buf, len) != (ssize_t)len)
            return fail("small", path);
        close(fd);
    }
    printf("%.6f\n", now() - t);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 4 && !strcmp(argv[1], "write"))
        return do_write(argv[2], atol(argv[3]));
    if (argc >= 3 && !strcmp(argv[1], "read"))
        return do_read(argv[2], argc > 3 && !strcmp(argv[3], "nocache"));
    if (argc >= 4 && !strcmp(argv[1], "meta"))
        return do_meta(argv[2], atol(argv[3]));
    if (argc >= 5 && !strcmp(argv[1], "small"))
        return do_small(argv[2], atol(argv[3]), (unsigned)atoi(argv[4]));
    fprintf(stderr, "usage: io write <file> <MiB> | read <file> [nocache] | meta <dir> <n> | small <dir> <n> <seed>\n");
    return 64;
}
