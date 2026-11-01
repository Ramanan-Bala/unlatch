// SPDX-License-Identifier: GPL-3.0-or-later
// Engine tests (Step 03). Run through Scripts/test-engine.sh, which prepares the images.
//   test_engine <image-dir>        run all tests
//   test_engine leakloop <img>     open/close 100 times (run under `leaks --atExit`)
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "file_io.h"
#include "unlatch_engine.h"

static int failures;
static char dir[1024];

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                                   \
            fprintf(stderr, "\n");                                          \
            failures++;                                                     \
            return;                                                         \
        }                                                                   \
    } while (0)

static const char *img(const char *name)
{
    static char path[2048];
    snprintf(path, sizeof(path), "%s/%s.img", dir, name);
    return path;
}

static int probe_file(const char *path, ue_probe_t *p)
{
    file_io_t f;
    int err = file_io_open(&f, path, false);
    if (err)
        return err;
    err = ue_probe(&f.io, p);
    file_io_close(&f);
    return err;
}

static void test_probe_clean(void)
{
    ue_probe_t p;
    int err = probe_file(img("clean"), &p);

    CHECK(err == 0, "ue_probe: %s", strerror(err));
    CHECK(p.state == UE_VOL_CLEAN, "state %s (%s)", ue_vol_state_name(p.state), p.reason);
    CHECK(strcmp(p.label, "T1") == 0, "label '%s'", p.label);
    CHECK(p.cluster_size == 4096, "cluster_size %u", p.cluster_size);
    CHECK(p.total_bytes > 500ull << 20 && p.total_bytes <= 512ull << 20, "total %llu",
          (unsigned long long)p.total_bytes);
    CHECK(p.free_bytes > 0 && p.free_bytes < p.total_bytes, "free %llu",
          (unsigned long long)p.free_bytes);
    CHECK(p.major_ver == 3 && p.minor_ver == 1, "version %u.%u", p.major_ver, p.minor_ver);
    CHECK(p.serial != 0, "serial 0");
}

static void test_probe_dirty(void)
{
    ue_probe_t p;
    int err = probe_file(img("dirty"), &p);

    CHECK(err == 0, "ue_probe: %s", strerror(err));
    CHECK(p.state == UE_VOL_DIRTY, "state %s (%s)", ue_vol_state_name(p.state), p.reason);
    CHECK(strcmp(p.label, "T1") == 0, "label '%s'", p.label);
}

static void test_probe_not_ntfs(void)
{
    ue_probe_t p;
    int err = probe_file(img("zeroboot"), &p);

    CHECK(err == 0, "ue_probe: %s", strerror(err));
    CHECK(p.state == UE_VOL_NOT_NTFS, "state %s (%s)", ue_vol_state_name(p.state), p.reason);
}

static void test_probe_truncated(void)
{
    ue_probe_t p;
    int err = probe_file(img("truncated"), &p);

    CHECK(err == 0, "ue_probe: %s", strerror(err));
    CHECK(p.state == UE_VOL_CORRUPT, "state %s (%s)", ue_vol_state_name(p.state), p.reason);
}

static void test_hibernated(void)
{
    ue_probe_t p;
    ue_engine_t *e;
    file_io_t f;
    int err = probe_file(img("hiber"), &p);

    CHECK(err == 0, "ue_probe: %s", strerror(err));
    CHECK(p.state == UE_VOL_HIBERNATED, "state %s (%s)", ue_vol_state_name(p.state), p.reason);

    // Read-write open must refuse, not silently degrade to read-only.
    CHECK(file_io_open(&f, img("hiber"), true) == 0, "open image");
    err = ue_open(&f.io, 0, &e, NULL);
    file_io_close(&f);
    CHECK(err == EPERM, "rw open of hibernated volume: %s", strerror(err));

    CHECK(file_io_open(&f, img("hiber"), false) == 0, "open image");
    err = ue_open(&f.io, UE_OPEN_RDONLY, &e, NULL);
    CHECK(err == 0, "ro open of hibernated volume: %s", strerror(err));
    CHECK(ue_is_readonly(e), "not read-only");
    CHECK(ue_close(e) == 0, "close");
    file_io_close(&f);
}

static void test_rw_open_on_ro_io(void)
{
    ue_engine_t *e = (ue_engine_t *)1;
    file_io_t f;
    int err;

    CHECK(file_io_open(&f, img("clean"), false) == 0, "open image");
    err = ue_open(&f.io, 0, &e, NULL);
    CHECK(err == EROFS, "ue_open: %s", strerror(err));
    CHECK(e == NULL, "out not cleared");

    // A writable flag with no pwrite callback is still read-only.
    f.io.writable = true;
    err = ue_open(&f.io, 0, &e, NULL);
    CHECK(err == EROFS, "ue_open without pwrite: %s", strerror(err));
    file_io_close(&f);
}

static void test_bad_args(void)
{
    ue_probe_t p;
    ue_engine_t *e;
    file_io_t f;

    CHECK(ue_probe(NULL, &p) == EINVAL, "NULL io");
    CHECK(ue_open(NULL, 0, &e, NULL) == EINVAL, "NULL io");
    CHECK(ue_close(NULL) == EINVAL, "NULL engine");
    CHECK(file_io_open(&f, img("clean"), false) == 0, "open image");
    f.io.block_size = 1000;
    CHECK(ue_probe(&f.io, &p) == EINVAL, "block size 1000");
    f.io.block_size = 512;
    f.io.size_bytes = 0;
    CHECK(ue_probe(&f.io, &p) == EINVAL, "size 0");
    file_io_close(&f);
}

static int open_close_loop(const char *path, bool rw, int n)
{
    for (int i = 0; i < n; i++) {
        file_io_t f;
        ue_engine_t *e;
        ue_probe_t p;
        int err = file_io_open(&f, path, rw);

        if (err)
            return err;
        err = ue_open(&f.io, rw ? 0 : UE_OPEN_RDONLY, &e, &p);
        if (!err)
            err = ue_close(e);
        file_io_close(&f);
        if (err)
            return err;
    }
    return 0;
}

static void test_open_close_repeat(void)
{
    ue_probe_t p;
    int err;

    err = open_close_loop(img("rw"), true, 100);
    CHECK(err == 0, "rw open/close loop: %s", strerror(err));
    err = open_close_loop(img("rw"), false, 100);
    CHECK(err == 0, "ro open/close loop: %s", strerror(err));
    // A read-write open and close leaves the volume clean.
    err = probe_file(img("rw"), &p);
    CHECK(err == 0 && p.state == UE_VOL_CLEAN, "after loop: %s (%s)",
          ue_vol_state_name(p.state), p.reason);
}

#define RUN(t)                                     \
    do {                                           \
        int before = failures;                     \
        t();                                       \
        printf("%s %s\n", failures == before ? "ok  " : "FAIL", #t); \
    } while (0)

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "leakloop") == 0) {
        int err = open_close_loop(argv[2], true, 100);
        if (err)
            fprintf(stderr, "leakloop: %s\n", strerror(err));
        return err ? 1 : 0;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: test_engine <image-dir> | leakloop <img>\n");
        return 64;
    }
    snprintf(dir, sizeof(dir), "%s", argv[1]);

    RUN(test_probe_clean);
    RUN(test_probe_dirty);
    RUN(test_probe_not_ntfs);
    RUN(test_probe_truncated);
    RUN(test_hibernated);
    RUN(test_rw_open_on_ro_io);
    RUN(test_bad_args);
    RUN(test_open_close_repeat);

    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
