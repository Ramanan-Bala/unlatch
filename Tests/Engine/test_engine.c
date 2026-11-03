// SPDX-License-Identifier: GPL-3.0-or-later
// Engine tests (Step 03). Run through Scripts/test-engine.sh, which prepares the images.
//   test_engine <image-dir>        run all tests
//   test_engine leakloop <img>     open/close 100 times (run under `leaks --atExit`)
//   test_engine readloop <img>     open, walk, read, readdir, close 50 times (same)
//   test_engine writeloop <dir>    create, write, rename, remove on a fresh clone, 30 times
#include <copyfile.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "file_io.h"
#include "gen.h"
#include "unlatch_engine.h"
#include "walk.h"

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


// ---- Read path (Step 04), on the "read" fixture built by Scripts/test-engine.sh ----

static ue_engine_t *rd;               // read fixture, open read-only for all read tests
static file_io_t rd_file;

static int read_all(ue_ino_t ino, uint64_t off, void *buf, size_t len, size_t *got)
{
    size_t total = 0, done;
    int err;

    while (total < len) {
        if ((err = ue_read(rd, ino, off + total, (char *)buf + total, len - total, &done)))
            return err;
        if (done == 0)
            break;
        total += done;
    }
    *got = total;
    return 0;
}

static bool matches_gen(const unsigned char *buf, size_t len, uint64_t seed, uint64_t off)
{
    unsigned char exp[4096];

    for (size_t i = 0; i < len; i += sizeof(exp)) {
        size_t n = len - i < sizeof(exp) ? len - i : sizeof(exp);
        gen_fill(seed, off + i, exp, n);
        if (memcmp(buf + i, exp, n))
            return false;
    }
    return true;
}

static bool all_zero(const unsigned char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++)
        if (buf[i])
            return false;
    return true;
}

static void test_lookup_nested(void)
{
    static unsigned char buf[100000 + 10];
    ue_attr_t a, d;
    size_t got;
    int err;

    CHECK(ue_walk(rd, "/dir1/dir2", &d) == 0 && d.type == UE_T_DIR, "dir2");
    err = ue_walk(rd, "/dir1/dir2/deep.bin", &a);
    CHECK(err == 0, "walk: %s", strerror(err));
    CHECK(a.type == UE_T_REG && a.size == 100000 && a.init_size == 100000, "size %llu",
          (unsigned long long)a.size);
    CHECK(a.nlink == 1 && a.mode == 0644, "nlink %u mode %o", a.nlink, a.mode);
    CHECK(a.mtime.tv_sec > 1700000000, "mtime %ld", (long)a.mtime.tv_sec);
    err = read_all(a.ino, 0, buf, sizeof(buf), &got);
    CHECK(err == 0 && got == 100000, "read %zu: %s", got, strerror(err));
    CHECK(matches_gen(buf, got, 7, 0), "content mismatch");
    err = read_all(a.ino, 99990, buf, 100, &got);
    CHECK(err == 0 && got == 10 && matches_gen(buf, 10, 7, 99990), "tail read %zu", got);
}

typedef struct {
    char names[64][64];
    int n;
} names_t;

static bool collect_names(void *ctx, const char *name, const ue_attr_t *a, uint64_t next)
{
    names_t *l = ctx;

    (void)a;
    (void)next;
    if (l->n < 64)
        snprintf(l->names[l->n++], sizeof(l->names[0]), "%s", name);
    return true;
}

static bool has_name(const names_t *l, const char *name)
{
    for (int i = 0; i < l->n; i++)
        if (!strcmp(l->names[i], name))
            return true;
    return false;
}

static void test_case_and_root(void)
{
    ue_attr_t a, b, root;
    names_t l = { .n = 0 };
    uint64_t v;
    int err;

    CHECK(ue_walk(rd, "/ReadMe.TXT", &a) == 0, "exact case");
    CHECK(ue_walk(rd, "/readme.txt", &b) == 0 && b.ino == a.ino, "lower case");
    CHECK(ue_walk(rd, "/README.TXT", &b) == 0 && b.ino == a.ino, "upper case");
    CHECK(ue_getattr(rd, UE_ROOT_INO, &root) == 0 && root.type == UE_T_DIR, "root");
    CHECK((root.ino >> 48) != 0, "root ino has no sequence number");
    err = ue_readdir(rd, root.ino, 0, collect_names, &l, &v);
    CHECK(err == 0, "readdir: %s", strerror(err));
    CHECK(l.n == 7, "root has %d entries", l.n);
    CHECK(has_name(&l, "ReadMe.TXT"), "readdir lost the case of ReadMe.TXT");
    CHECK(!has_name(&l, "readme.txt"), "readdir lowercased a name");
    CHECK(!has_name(&l, "$MFT") && !has_name(&l, "$Extend") && !has_name(&l, "."), "metafiles shown");
    CHECK(ue_lookup(rd, root.ino, "$MFT", &a) == ENOENT, "$MFT visible to lookup");
    CHECK(ue_lookup(rd, root.ino, "$Extend", &a) == ENOENT, "$Extend visible to lookup");
}

static void test_unicode(void)
{
    unsigned char buf[16];
    ue_attr_t a;
    size_t got;

    CHECK(ue_walk(rd, "/caf\xC3\xA9.txt", &a) == 0, "NFC lookup");
    CHECK(ue_walk(rd, "/CAF\xC3\x89.TXT", &a) == 0, "case-insensitive non-ASCII lookup");
    CHECK(read_all(a.ino, 0, buf, sizeof(buf), &got) == 0 && got == 12 && matches_gen(buf, 12, 2, 0),
          "content");
    // The engine does not normalize: NFD input is the host's job (seam contract).
    CHECK(ue_walk(rd, "/cafe\xCC\x81.txt", &a) == ENOENT, "NFD lookup should miss");
}

typedef struct {
    unsigned char *seen;              // 10000 flags
    unsigned count, dups, bad;
    unsigned limit;                   // accept this many per call, then refuse (0: all)
    unsigned taken;
    uint64_t last_cookie;
} bigdir_t;

static bool bigdir_entry(void *ctx, const char *name, const ue_attr_t *a, uint64_t next)
{
    bigdir_t *b = ctx;
    unsigned i;

    if (b->limit && b->taken == b->limit)
        return false;                 // refuse: the host resumes after last_cookie
    b->taken++;
    b->last_cookie = next;
    if (sscanf(name, "f%5u", &i) != 1 || i >= 10000 || a->type != UE_T_REG || a->size != 0) {
        b->bad++;
        return true;
    }
    if (b->seen[i])
        b->dups++;
    b->seen[i] = 1;
    b->count++;
    return true;
}

static void test_big_dir(void)
{
    unsigned char seen[10000];
    bigdir_t b = { seen, 0, 0, 0, 0, 0, 0 };
    ue_stats_t s0, s1;
    ue_attr_t d;
    uint64_t v, v2;
    int err, calls = 0;

    CHECK(ue_walk(rd, "/big", &d) == 0, "walk /big");
    memset(seen, 0, sizeof(seen));
    ue_get_stats(rd, &s0);
    err = ue_readdir(rd, d.ino, 0, bigdir_entry, &b, &v);
    ue_get_stats(rd, &s1);
    CHECK(err == 0, "readdir: %s", strerror(err));
    CHECK(b.count == 10000 && b.dups == 0 && b.bad == 0, "count %u dups %u bad %u", b.count, b.dups, b.bad);
    // Each MFT record is opened exactly once: the directory, then every child.
    CHECK(s1.inode_opens - s0.inode_opens == 10001, "inode opens %llu",
          (unsigned long long)(s1.inode_opens - s0.inode_opens));
    CHECK(s1.held_inodes == s0.held_inodes, "readdir left %llu inodes held",
          (unsigned long long)(s1.held_inodes - s0.held_inodes));

    // Paged: take 97 per call, refuse the 98th, resume from the last cookie taken.
    memset(seen, 0, sizeof(seen));
    b = (bigdir_t){ seen, 0, 0, 0, 97, 0, 0 };
    for (;;) {
        unsigned before = b.count;
        b.taken = 0;
        err = ue_readdir(rd, d.ino, b.last_cookie, bigdir_entry, &b, &v2);
        CHECK(err == 0, "paged readdir call %d: %s", calls, strerror(err));
        CHECK(v2 == v, "verifier changed");
        calls++;
        if (b.count == before || calls > 200)
            break;
    }
    CHECK(b.count == 10000 && b.dups == 0 && b.bad == 0, "paged: count %u dups %u bad %u calls %d",
          b.count, b.dups, b.bad, calls);
}

static void test_sparse(void)
{
    enum { CHUNK = 8 << 20 };
    const uint64_t size = 3ull << 30, data_off = 2ull << 30, data_len = 1 << 20;
    unsigned char *buf = malloc(CHUNK);
    ue_attr_t a;
    uint64_t off;
    size_t got;
    int err = 0;

    CHECK(buf, "malloc");
    if (ue_walk(rd, "/sparse.bin", &a)) {
        free(buf);
        CHECK(0, "walk");
    }
    CHECK(a.size == size && (a.flags & UE_F_SPARSE) && a.alloc_size < (64u << 20),
          "size %llu alloc %llu flags %x", (unsigned long long)a.size,
          (unsigned long long)a.alloc_size, a.flags);
    // Read the whole 3 GiB and check every byte: zeros except the 1 MiB written at 2 GiB.
    for (off = 0; off < size && !err; off += got) {
        err = read_all(a.ino, off, buf, CHUNK, &got);
        if (err || got == 0)
            break;
        for (uint64_t p = 0; p < got; p += 4096) {
            uint64_t at = off + p, n = got - p < 4096 ? got - p : 4096;
            bool in_data = at >= data_off && at < data_off + data_len;
            if (in_data ? !matches_gen(buf + p, n, 3, at) : !all_zero(buf + p, n)) {
                err = -1;
                break;
            }
        }
    }
    free(buf);
    CHECK(err == 0, "content mismatch near %llu (%d)", (unsigned long long)off, err);
    CHECK(off == size, "read %llu of %llu", (unsigned long long)off, (unsigned long long)size);
}

static void test_uninit(void)
{
    static unsigned char buf[1 << 20];
    ue_attr_t a;
    size_t got;

    CHECK(ue_walk(rd, "/uninit.bin", &a) == 0, "walk");
    CHECK(a.size == (1 << 20) && a.init_size == 4096, "size %llu init %llu",
          (unsigned long long)a.size, (unsigned long long)a.init_size);
    CHECK(read_all(a.ino, 0, buf, sizeof(buf), &got) == 0 && got == sizeof(buf), "read %zu", got);
    CHECK(matches_gen(buf, 4096, 4, 0), "initialized part");
    CHECK(all_zero(buf + 4096, sizeof(buf) - 4096), "past init_size is not zero");
}

static void test_resident_and_flags(void)
{
    unsigned char buf[200];
    ue_attr_t a;
    size_t got;

    CHECK(ue_walk(rd, "/small.txt", &a) == 0, "walk");
    CHECK(a.size == 100 && (a.flags & UE_F_RESIDENT) && (a.flags & UE_F_HIDDEN), "size %llu flags %x",
          (unsigned long long)a.size, a.flags);
    CHECK(read_all(a.ino, 0, buf, sizeof(buf), &got) == 0 && got == 100 && matches_gen(buf, 100, 5, 0),
          "content (%zu)", got);
    CHECK(ue_walk(rd, "/dir1", &a) == 0 && !(a.flags & UE_F_RESIDENT), "dir flags %x", a.flags);
}

static void test_read_errors(void)
{
    unsigned char buf[16];
    ue_attr_t a, d, f;
    size_t done = 99;

    CHECK(ue_walk(rd, "/dir1", &d) == 0, "walk dir1");
    CHECK(ue_read(rd, d.ino, 0, buf, sizeof(buf), &done) == EISDIR, "read dir");
    CHECK(ue_walk(rd, "/ReadMe.TXT", &f) == 0, "walk file");
    CHECK(ue_lookup(rd, f.ino, "x", &a) == ENOTDIR, "lookup in file");
    CHECK(ue_readdir(rd, f.ino, 0, collect_names, NULL, NULL) == ENOTDIR, "readdir of file");
    CHECK(ue_lookup(rd, d.ino, "missing", &a) == ENOENT, "missing");
    CHECK(ue_lookup(rd, d.ino, "a/b", &a) == EINVAL, "slash in name");
    CHECK(ue_lookup(rd, d.ino, "..", &a) == EINVAL, "dot-dot");
    CHECK(ue_lookup(rd, d.ino, "", &a) == EINVAL, "empty name");
    CHECK(ue_read(rd, f.ino, 5000, buf, sizeof(buf), &done) == 0 && done == 0, "read at EOF");
    CHECK(ue_read(rd, f.ino, 1ull << 40, buf, sizeof(buf), &done) == 0 && done == 0, "read past EOF");
    CHECK(ue_getattr(rd, f.ino ^ (1ull << 48), &a) == ESTALE, "wrong sequence number");
}

static void test_release(void)
{
    ue_stats_t s0, s1, s2;
    ue_attr_t a, b;

    ue_get_stats(rd, &s0);
    CHECK(ue_walk(rd, "/dir1/dir2/deep.bin", &a) == 0, "walk");
    ue_get_stats(rd, &s1);
    CHECK(s1.held_inodes >= s0.held_inodes, "cache shrank");
    ue_release(rd, a.ino);
    ue_get_stats(rd, &s2);
    CHECK(s2.held_inodes == s1.held_inodes - 1, "release did not drop the entry");
    CHECK(ue_getattr(rd, a.ino, &b) == 0 && b.size == a.size, "getattr after release");
    ue_release(rd, a.ino);
    ue_release(rd, a.ino);            // releasing twice is harmless
    ue_release(rd, UE_ROOT_INO);      // the root is never released
    CHECK(ue_getattr(rd, UE_ROOT_INO, &b) == 0, "root after release");
}

static void test_statfs(void)
{
    uint64_t total, free_bytes;
    uint32_t cluster;
    ue_probe_t p;

    CHECK(probe_file(img("read"), &p) == 0, "probe");
    CHECK(ue_statfs(rd, &total, &free_bytes, &cluster) == 0, "statfs");
    CHECK(total == p.total_bytes && free_bytes == p.free_bytes && cluster == p.cluster_size,
          "statfs %llu/%llu vs probe %llu/%llu", (unsigned long long)total,
          (unsigned long long)free_bytes, (unsigned long long)p.total_bytes,
          (unsigned long long)p.free_bytes);
}

static int read_loop(const char *path, int n)
{
    for (int i = 0; i < n; i++) {
        file_io_t f;
        ue_engine_t *e;
        ue_attr_t a;
        names_t l = { .n = 0 };
        unsigned char buf[4096];
        size_t done;
        int err = file_io_open(&f, path, false);

        if (err)
            return err;
        if (!(err = ue_open(&f.io, UE_OPEN_RDONLY, &e, NULL))) {
            if (!(err = ue_walk(e, "/dir1/dir2/deep.bin", &a)))
                err = ue_read(e, a.ino, 0, buf, sizeof(buf), &done);
            if (!err && !(err = ue_walk(e, "/", &a)))
                err = ue_readdir(e, a.ino, 0, collect_names, &l, NULL);
            int cerr = ue_close(e);
            if (!err)
                err = cerr;
        }
        file_io_close(&f);
        if (err)
            return err;
    }
    return 0;
}


// ---- Write path (Step 05). Each test works on its own clone of a fresh image and must
// leave it CLEAN; Scripts/test-engine.sh then checks every w-*.img with ntfsinfo/ntfsfix.

typedef struct {
    file_io_t f;
    ue_engine_t *e;
    char path[2048];
} wvol_t;

// The write tests run twice: without and then with the page cache (Step 15, D-62).
static uint32_t wflags;
static const char *wtag = "";

static int wopen(wvol_t *w, const char *base, const char *name)
{
    int err;

    snprintf(w->path, sizeof(w->path), "%s/w-%s%s.img", dir, wtag, name);
    unlink(w->path);
    if (copyfile(img(base), w->path, NULL, COPYFILE_CLONE | COPYFILE_DATA) < 0)
        return errno;
    if ((err = file_io_open(&w->f, w->path, true)))
        return err;
    if ((err = ue_open(&w->f.io, wflags, &w->e, NULL))) {
        file_io_close(&w->f);
        return err;
    }
    return 0;
}

// Every write test: nothing reached the device while the dirty mark was off (D-95).
static int unmarked_writes(const wvol_t *w)
{
    ue_stats_t st;

    ue_get_stats(w->e, &st);
    if (st.unmarked_writes)
        fprintf(stderr, "  %s: %llu writes while not marked dirty\n", w->path, (unsigned long long)st.unmarked_writes);
    return st.unmarked_writes ? EIO : 0;
}

static int wreopen(wvol_t *w, bool rw)
{
    int uerr = unmarked_writes(w);
    int err = ue_close(w->e);

    if (!err)
        err = uerr;
    file_io_close(&w->f);
    if (err)
        return err;
    if ((err = file_io_open(&w->f, w->path, rw)))
        return err;
    return ue_open(&w->f.io, (rw ? 0 : UE_OPEN_RDONLY) | wflags, &w->e, NULL);
}

// Closes and re-probes: every write test must leave the volume CLEAN.
static int wclose(wvol_t *w)
{
    ue_probe_t p;
    int uerr = unmarked_writes(w);
    int err = ue_close(w->e);

    file_io_close(&w->f);
    if (err || (err = uerr))
        return err;
    if ((err = probe_file(w->path, &p)))
        return err;
    if (p.state != UE_VOL_CLEAN) {
        fprintf(stderr, "  %s probes %s: %s\n", w->path, ue_vol_state_name(p.state), p.reason);
        return EIO;
    }
    return 0;
}

#define WCHECK(cond, ...)                                                   \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                                   \
            fprintf(stderr, "\n");                                          \
            failures++;                                                     \
            ue_close(w.e);                                                  \
            file_io_close(&w.f);                                            \
            return;                                                         \
        }                                                                   \
    } while (0)

static int write_gen(ue_engine_t *e, ue_ino_t ino, uint64_t off, uint64_t len, uint64_t seed)
{
    unsigned char buf[65536];

    while (len > 0) {
        size_t n = len > sizeof(buf) ? sizeof(buf) : (size_t)len, done;
        int err;
        gen_fill(seed, off, buf, n);
        if ((err = ue_write(e, ino, off, buf, n, &done)))
            return err;
        if (done != n)
            return EIO;
        off += n;
        len -= n;
    }
    return 0;
}

static int check_gen(ue_engine_t *e, ue_ino_t ino, uint64_t off, uint64_t len, uint64_t seed)
{
    unsigned char buf[65536];

    while (len > 0) {
        size_t n = len > sizeof(buf) ? sizeof(buf) : (size_t)len, done;
        int err = ue_read(e, ino, off, buf, n, &done);
        if (err)
            return err;
        if (done != n || !matches_gen(buf, n, seed, off))
            return EIO;
        off += n;
        len -= n;
    }
    return 0;
}

static int check_zero(ue_engine_t *e, ue_ino_t ino, uint64_t off, uint64_t len)
{
    unsigned char buf[65536];

    while (len > 0) {
        size_t n = len > sizeof(buf) ? sizeof(buf) : (size_t)len, done;
        int err = ue_read(e, ino, off, buf, n, &done);
        if (err)
            return err;
        if (done != n || !all_zero(buf, n))
            return EIO;
        off += n;
        len -= n;
    }
    return 0;
}

static ue_ino_t root_of(ue_engine_t *e)
{
    ue_attr_t r;

    return ue_getattr(e, UE_ROOT_INO, &r) ? 0 : r.ino;
}

static void test_write_roundtrip(void)
{
    wvol_t w;
    ue_attr_t a, b;
    ue_ino_t root;
    int err = wopen(&w, "clean", "roundtrip");

    CHECK(err == 0, "open: %s", strerror(err));
    root = root_of(w.e);
    WCHECK(ue_create(w.e, root, "data.bin", UE_T_REG, 0644, &a) == 0, "create");
    WCHECK(a.type == UE_T_REG && a.size == 0 && a.nlink == 1, "fresh attr");
    // Two writes, the second past a gap, so the file has a zero-filled middle.
    WCHECK(write_gen(w.e, a.ino, 0, 3 << 20, 11) == 0, "write 1");
    WCHECK(write_gen(w.e, a.ino, 5 << 20, 1 << 20, 12) == 0, "write 2");
    WCHECK(ue_getattr(w.e, a.ino, &b) == 0 && b.size == (6u << 20), "size %llu", (unsigned long long)b.size);
    WCHECK(b.mtime.tv_sec >= a.mtime.tv_sec && (b.flags & UE_F_ARCHIVE), "mtime/archive");
    // Overwrite in the middle of the first range.
    WCHECK(write_gen(w.e, a.ino, 1 << 20, 4096, 13) == 0, "overwrite");
    WCHECK(ue_sync(w.e) == 0, "sync");
    WCHECK(wreopen(&w, false) == 0, "reopen read-only");
    WCHECK(ue_walk(w.e, "/data.bin", &b) == 0 && b.ino == a.ino && b.size == (6u << 20), "after reopen");
    WCHECK(check_gen(w.e, b.ino, 0, 1 << 20, 11) == 0, "range 1");
    WCHECK(check_gen(w.e, b.ino, 1 << 20, 4096, 13) == 0, "overwritten range");
    WCHECK(check_gen(w.e, b.ino, (1 << 20) + 4096, (2 << 20) - 4096, 11) == 0, "range 1 tail");
    WCHECK(check_zero(w.e, b.ino, 3 << 20, 2 << 20) == 0, "gap is zero");
    WCHECK(check_gen(w.e, b.ino, 5 << 20, 1 << 20, 12) == 0, "range 2");
    CHECK(wclose(&w) == 0, "not clean");
}

static void test_create_rules(void)
{
    static const char *bad[] = {
        "a\\b", "a:b", "a*b", "a?b", "a\"b", "a<b", "a>b", "a|b", "tab\tname", "dot.", "space ",
        "CON", "con.txt", "PRN", "AUX", "NUL", "COM1", "lpt9.log", ".unlatch-ghost-1",
    };
    char longname[300];
    wvol_t w;
    ue_attr_t a, d, f;
    ue_ino_t root;
    int err = wopen(&w, "clean", "create");

    CHECK(err == 0, "open: %s", strerror(err));
    root = root_of(w.e);
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        err = ue_create(w.e, root, bad[i], UE_T_REG, 0644, &a);
        WCHECK(err == EINVAL, "'%s' gave %s", bad[i], strerror(err));
    }
    memset(longname, 'x', 256);
    longname[256] = '\0';
    WCHECK(ue_create(w.e, root, longname, UE_T_REG, 0644, &a) == ENAMETOOLONG, "256 chars");
    longname[255] = '\0';
    WCHECK(ue_create(w.e, root, longname, UE_T_REG, 0644, &a) == 0, "255 chars");
    WCHECK(ue_create(w.e, root, "COM10", UE_T_REG, 0644, &a) == 0, "COM10 is allowed");
    WCHECK(ue_create(w.e, root, "x.txt", UE_T_REG, 0644, &f) == 0, "x.txt");
    WCHECK(ue_create(w.e, root, "X.TXT", UE_T_REG, 0644, &a) == EEXIST, "case-insensitive EEXIST");
    WCHECK(ue_create(w.e, f.ino, "y", UE_T_REG, 0644, &a) == ENOTDIR, "create in a file");
    WCHECK(ue_create(w.e, root, "ro.txt", UE_T_REG, 0444, &a) == 0 && (a.flags & UE_F_READONLY), "0444 -> readonly");
    WCHECK(ue_create(w.e, root, "dir", UE_T_DIR, 0755, &d) == 0 && d.type == UE_T_DIR, "mkdir");
    WCHECK(ue_create(w.e, d.ino, "nested", UE_T_DIR, 0755, &a) == 0, "nested mkdir");
    WCHECK(ue_walk(w.e, "/dir/nested", &a) == 0 && a.type == UE_T_DIR, "walk nested");
    WCHECK(ue_create(w.e, root, "link", UE_T_LNK, 0755, &a) == EOPNOTSUPP, "symlink");
    WCHECK(ue_create(w.e, root, "caf\xC3\xA9", UE_T_REG, 0644, &a) == 0, "unicode");
    CHECK(wclose(&w) == 0, "not clean");
}

static void test_readonly_engine(void)
{
    unsigned char buf[4] = { 1, 2, 3, 4 };
    ue_attr_t a, f;
    file_io_t fio;
    ue_engine_t *e;
    size_t done;
    ue_ino_t root;

    CHECK(file_io_open(&fio, img("read"), false) == 0, "open");
    CHECK(ue_open(&fio.io, UE_OPEN_RDONLY, &e, NULL) == 0, "ue_open");
    root = root_of(e);
    if (ue_walk(e, "/ReadMe.TXT", &f) == 0) {
        ue_attr_t in = { .size = 1 };
        int r[] = {
            ue_write(e, f.ino, 0, buf, sizeof(buf), &done),
            ue_create(e, root, "new", UE_T_REG, 0644, &a),
            ue_remove(e, root, "ReadMe.TXT", false),
            ue_rename(e, root, "ReadMe.TXT", root, "x", 0),
            ue_setattr(e, f.ino, UE_SET_SIZE, &in, &a),
        };
        for (size_t i = 0; i < sizeof(r) / sizeof(r[0]); i++)
            if (r[i] != EROFS)
                failures++, fprintf(stderr, "  FAIL mutating call %zu on a read-only engine: %s\n", i, strerror(r[i]));
    } else {
        failures++;
    }
    CHECK(ue_close(e) == 0, "close");
    file_io_close(&fio);
}

static void test_remove(void)
{
    wvol_t w;
    ue_attr_t a, d, g;
    ue_ino_t root;
    uint64_t free0, free1, free2;
    int err = wopen(&w, "clean", "remove");

    CHECK(err == 0, "open: %s", strerror(err));
    root = root_of(w.e);
    WCHECK(ue_create(w.e, root, "d", UE_T_DIR, 0755, &d) == 0, "mkdir");
    WCHECK(ue_create(w.e, d.ino, "f", UE_T_REG, 0644, &a) == 0, "create");
    WCHECK(ue_remove(w.e, root, "d", true) == ENOTEMPTY, "rmdir non-empty");
    WCHECK(ue_remove(w.e, root, "d", false) == EISDIR, "unlink a dir");
    WCHECK(ue_remove(w.e, d.ino, "f", true) == ENOTDIR, "rmdir a file");
    WCHECK(ue_remove(w.e, root, "missing", false) == ENOENT, "missing");
    WCHECK(ue_remove(w.e, root, "$MFT", false) == ENOENT, "$MFT");
    WCHECK(ue_remove(w.e, root, "$Extend", true) == ENOENT, "$Extend");
    ue_release(w.e, a.ino);                 // not held: removal is immediate
    WCHECK(ue_remove(w.e, d.ino, "F", false) == 0, "unlink (case-insensitive)");
    WCHECK(ue_getattr(w.e, a.ino, &g) == ESTALE, "removed record is stale");
    WCHECK(ue_remove(w.e, root, "d", true) == 0, "rmdir");
    WCHECK(ue_walk(w.e, "/d", &g) == ENOENT, "dir gone");

    // Held file: removing its last name keeps the data until ue_release (D-25).
    WCHECK(ue_statfs(w.e, NULL, &free0, NULL) == 0, "statfs");
    WCHECK(ue_create(w.e, root, "held.bin", UE_T_REG, 0644, &a) == 0, "create held");
    WCHECK(write_gen(w.e, a.ino, 0, 2 << 20, 21) == 0, "write held");
    WCHECK(ue_remove(w.e, root, "held.bin", false) == 0, "unlink held");
    WCHECK(ue_walk(w.e, "/held.bin", &g) == ENOENT, "name gone");
    names_t l = { .n = 0 };
    WCHECK(ue_readdir(w.e, root, 0, collect_names, &l, NULL) == 0 && l.n == 0, "root shows %d entries (%s)",
           l.n, l.n ? l.names[0] : "");
    WCHECK(check_gen(w.e, a.ino, 0, 2 << 20, 21) == 0, "data readable after unlink");
    WCHECK(ue_create(w.e, root, "held.bin", UE_T_REG, 0644, &g) == 0 && g.ino != a.ino, "name reusable");
    WCHECK(ue_statfs(w.e, NULL, &free1, NULL) == 0 && free1 < free0, "space still in use");
    ue_release(w.e, a.ino);
    WCHECK(ue_getattr(w.e, a.ino, &g) == ESTALE, "released ghost is gone");
    WCHECK(ue_statfs(w.e, NULL, &free2, NULL) == 0 && free2 > free1, "space returned");

    // A ghost left at close is deleted by ue_close.
    WCHECK(ue_create(w.e, root, "late.bin", UE_T_REG, 0644, &a) == 0, "create late");
    WCHECK(write_gen(w.e, a.ino, 0, 1 << 20, 22) == 0, "write late");
    WCHECK(ue_remove(w.e, root, "late.bin", false) == 0, "unlink late");
    CHECK(wclose(&w) == 0, "not clean");
}

// UE_OPEN_GHOST_OPEN_ONLY (D-82): a held file without open handles is deleted at once; one
// removed while open lives until its last close.
static void test_ghost_open_only(void)
{
    wvol_t w;
    ue_attr_t a, b, g;
    ue_ino_t root;
    uint64_t free0, free1, free2;
    uint32_t saved = wflags;
    int err;

    wflags |= UE_OPEN_GHOST_OPEN_ONLY;
    err = wopen(&w, "clean", "ghost-open");
    wflags = saved;
    CHECK(err == 0, "open: %s", strerror(err));
    root = root_of(w.e);

    // Held (looked up by the host) but not open: removal frees the space now.
    WCHECK(ue_create(w.e, root, "held.bin", UE_T_REG, 0644, &a) == 0, "create");
    WCHECK(write_gen(w.e, a.ino, 0, 2 << 20, 31) == 0, "write");
    WCHECK(ue_statfs(w.e, NULL, &free0, NULL) == 0, "statfs after write");
    WCHECK(ue_remove(w.e, root, "held.bin", false) == 0, "unlink held");
    WCHECK(ue_getattr(w.e, a.ino, &g) == ESTALE, "held, not open: deleted at once");
    WCHECK(ue_statfs(w.e, NULL, &free1, NULL) == 0 && free1 >= free0 + (2 << 20), "space returned at once");
    WCHECK(ue_file_opened(w.e, a.ino) == ESTALE, "open of a removed file");
    ue_release(w.e, a.ino);

    // Open: kept, readable, until the last of two handles closes.
    WCHECK(ue_create(w.e, root, "open.bin", UE_T_REG, 0644, &b) == 0, "create open");
    WCHECK(ue_file_opened(w.e, b.ino) == 0 && ue_file_opened(w.e, b.ino) == 0, "two opens");
    WCHECK(write_gen(w.e, b.ino, 0, 2 << 20, 32) == 0, "write open");
    WCHECK(ue_remove(w.e, root, "open.bin", false) == 0, "unlink open");
    WCHECK(ue_walk(w.e, "/open.bin", &g) == ENOENT, "name gone");
    WCHECK(check_gen(w.e, b.ino, 0, 2 << 20, 32) == 0, "data readable while open");
    ue_file_closed(w.e, b.ino);
    WCHECK(check_gen(w.e, b.ino, 0, 2 << 20, 32) == 0, "still readable with one handle");
    WCHECK(ue_statfs(w.e, NULL, &free1, NULL) == 0, "statfs while open");
    ue_file_closed(w.e, b.ino);
    WCHECK(ue_getattr(w.e, b.ino, &g) == ESTALE, "deleted at last close");
    WCHECK(ue_statfs(w.e, NULL, &free2, NULL) == 0 && free2 >= free1 + (2 << 20), "space returned at last close");
    ue_file_closed(w.e, b.ino);             // extra close of a stale item: ignored
    ue_release(w.e, b.ino);

    // Removed while open and never closed: ue_close deletes it.
    WCHECK(ue_create(w.e, root, "late.bin", UE_T_REG, 0644, &a) == 0, "create late");
    WCHECK(ue_file_opened(w.e, a.ino) == 0, "open late");
    WCHECK(ue_remove(w.e, root, "late.bin", false) == 0, "unlink late");
    CHECK(wclose(&w) == 0, "not clean");
}

static void test_rename_matrix(void)
{
    wvol_t w;
    ue_attr_t a, b, d1, d2, sub, t, x;
    ue_ino_t root;
    names_t l;
    int err = wopen(&w, "clean", "rename");

    CHECK(err == 0, "open: %s", strerror(err));
    root = root_of(w.e);
    WCHECK(ue_create(w.e, root, "d1", UE_T_DIR, 0755, &d1) == 0, "d1");
    WCHECK(ue_create(w.e, root, "d2", UE_T_DIR, 0755, &d2) == 0, "d2");
    WCHECK(ue_create(w.e, d1.ino, "a", UE_T_REG, 0644, &a) == 0, "a");
    WCHECK(write_gen(w.e, a.ino, 0, 100000, 31) == 0, "write a");

    // Same directory.
    WCHECK(ue_rename(w.e, d1.ino, "a", d1.ino, "b", 0) == 0, "a -> b");
    WCHECK(ue_lookup(w.e, d1.ino, "a", &x) == ENOENT, "old name gone");
    WCHECK(ue_lookup(w.e, d1.ino, "b", &b) == 0 && b.ino == a.ino && b.nlink == 1, "b is a");
    WCHECK(check_gen(w.e, b.ino, 0, 100000, 31) == 0, "content kept");
    // Across directories.
    WCHECK(ue_rename(w.e, d1.ino, "b", d2.ino, "c", 0) == 0, "d1/b -> d2/c");
    WCHECK(ue_walk(w.e, "/d2/c", &b) == 0 && b.ino == a.ino, "moved");
    WCHECK(ue_lookup(w.e, d1.ino, "b", &x) == ENOENT, "gone from d1");
    // Case-only rename.
    WCHECK(ue_rename(w.e, d2.ino, "c", d2.ino, "C", 0) == 0, "c -> C");
    l.n = 0;
    WCHECK(ue_readdir(w.e, d2.ino, 0, collect_names, &l, NULL) == 0 && l.n == 1 && !strcmp(l.names[0], "C"),
           "d2 lists %d: %s", l.n, l.n ? l.names[0] : "");
    WCHECK(ue_lookup(w.e, d2.ino, "C", &b) == 0 && b.ino == a.ino && b.nlink == 1, "same item, one link");
    WCHECK(ue_rename(w.e, d2.ino, "C", d2.ino, "C", 0) == 0, "rename to itself");
    // Replace an existing file (held: it becomes a ghost until released).
    WCHECK(ue_create(w.e, d2.ino, "t", UE_T_REG, 0644, &t) == 0, "t");
    WCHECK(write_gen(w.e, t.ino, 0, 5000, 32) == 0, "write t");
    WCHECK(ue_rename(w.e, d2.ino, "C", d2.ino, "t", UE_RENAME_EXCL) == EEXIST, "EXCL");
    WCHECK(ue_rename(w.e, d2.ino, "C", d2.ino, "t", 0) == 0, "C replaces t");
    WCHECK(ue_lookup(w.e, d2.ino, "t", &b) == 0 && b.ino == a.ino && b.size == 100000, "t is now a");
    WCHECK(check_gen(w.e, t.ino, 0, 5000, 32) == 0, "replaced file readable while held");
    l.n = 0;
    WCHECK(ue_readdir(w.e, d2.ino, 0, collect_names, &l, NULL) == 0 && l.n == 1, "d2 lists %d", l.n);
    ue_release(w.e, t.ino);
    WCHECK(ue_getattr(w.e, t.ino, &x) == ESTALE, "replaced file deleted on release");
    // Directories: move with contents, type rules, subtree rule.
    WCHECK(ue_create(w.e, d1.ino, "sub", UE_T_DIR, 0755, &sub) == 0, "d1/sub");
    WCHECK(ue_create(w.e, sub.ino, "deep", UE_T_REG, 0644, &x) == 0, "d1/sub/deep");
    WCHECK(ue_rename(w.e, root, "d1", sub.ino, "d1", 0) == EINVAL, "into own subtree");
    WCHECK(ue_rename(w.e, root, "d1", d1.ino, "self", 0) == EINVAL, "into itself");
    WCHECK(ue_rename(w.e, d1.ino, "sub", d2.ino, "t", 0) == ENOTDIR, "dir over file");
    WCHECK(ue_rename(w.e, d2.ino, "t", root, "d1", 0) == EISDIR, "file over dir");
    WCHECK(ue_create(w.e, root, "empty", UE_T_DIR, 0755, &x) == 0, "empty dir");
    WCHECK(ue_rename(w.e, root, "empty", root, "d1", 0) == ENOTEMPTY, "over non-empty dir");
    WCHECK(ue_rename(w.e, d1.ino, "sub", root, "empty", 0) == 0, "dir replaces empty dir");
    WCHECK(ue_walk(w.e, "/empty/deep", &b) == 0, "contents moved");
    WCHECK(ue_walk(w.e, "/d1/sub", &x) == ENOENT, "old dir name gone");
    WCHECK(ue_rename(w.e, root, "d2", d1.ino, "d2", 0) == 0, "move d2 into d1");
    WCHECK(ue_walk(w.e, "/d1/d2/t", &b) == 0 && b.ino == a.ino, "nested move");
    WCHECK(ue_rename(w.e, root, "missing", root, "x", 0) == ENOENT, "missing source");
    WCHECK(ue_rename(w.e, root, "empty", root, "bad:name", 0) == EINVAL, "bad target name");
    WCHECK(wreopen(&w, false) == 0, "reopen");
    WCHECK(ue_walk(w.e, "/d1/d2/t", &b) == 0 && check_gen(w.e, b.ino, 0, 100000, 31) == 0, "after reopen");
    WCHECK(ue_walk(w.e, "/empty/deep", &b) == 0, "after reopen 2");
    CHECK(wclose(&w) == 0, "not clean");
}

static void test_setattr(void)
{
    wvol_t w;
    ue_attr_t a, in, out, d;
    ue_ino_t root;
    int err = wopen(&w, "clean", "setattr");

    CHECK(err == 0, "open: %s", strerror(err));
    root = root_of(w.e);
    WCHECK(ue_create(w.e, root, "f", UE_T_REG, 0644, &a) == 0, "create");
    WCHECK(write_gen(w.e, a.ino, 0, 1 << 20, 41) == 0, "write");
    memset(&in, 0, sizeof(in));
    in.size = 3 << 20;                  // grow: the new tail reads as zeros
    WCHECK(ue_setattr(w.e, a.ino, UE_SET_SIZE, &in, &out) == 0 && out.size == in.size, "grow");
    WCHECK(check_gen(w.e, a.ino, 0, 1 << 20, 41) == 0 && check_zero(w.e, a.ino, 1 << 20, 2 << 20) == 0, "grown");
    in.size = 4096;
    WCHECK(ue_setattr(w.e, a.ino, UE_SET_SIZE, &in, &out) == 0 && out.size == 4096, "shrink");
    WCHECK(check_gen(w.e, a.ino, 0, 4096, 41) == 0, "shrunk content");
    in.mtime = (struct timespec){ 1000000000, 123456700 };
    in.atime = (struct timespec){ 1100000000, 0 };
    in.birth = (struct timespec){ 900000000, 500 * 100 };
    WCHECK(ue_setattr(w.e, a.ino, UE_SET_MTIME | UE_SET_ATIME | UE_SET_BIRTH, &in, &out) == 0, "times");
    WCHECK(out.mtime.tv_sec == 1000000000 && out.mtime.tv_nsec == 123456700, "mtime %ld.%ld",
           (long)out.mtime.tv_sec, out.mtime.tv_nsec);
    WCHECK(out.atime.tv_sec == 1100000000 && out.birth.tv_sec == 900000000 && out.birth.tv_nsec == 50000, "atime/birth");
    WCHECK(out.ctime.tv_sec > 1700000000, "ctime moves to now");
    in.flags = UE_F_HIDDEN | UE_F_READONLY;
    WCHECK(ue_setattr(w.e, a.ino, UE_SET_FLAGS, &in, &out) == 0, "flags");
    WCHECK((out.flags & UE_F_HIDDEN) && (out.flags & UE_F_READONLY) && !(out.flags & UE_F_ARCHIVE) &&
           out.mode == 0444, "flags %x mode %o", out.flags, out.mode);
    in.mode = 0644;
    WCHECK(ue_setattr(w.e, a.ino, UE_SET_MODE, &in, &out) == 0 && !(out.flags & UE_F_READONLY) &&
           (out.flags & UE_F_HIDDEN), "mode 0644 clears readonly only");
    WCHECK(ue_create(w.e, root, "d", UE_T_DIR, 0755, &d) == 0, "mkdir");
    in.size = 1;
    WCHECK(ue_setattr(w.e, d.ino, UE_SET_SIZE, &in, &out) == EISDIR, "size of a dir");
    WCHECK(wreopen(&w, false) == 0, "reopen");
    WCHECK(ue_walk(w.e, "/f", &out) == 0 && out.size == 4096 && out.mtime.tv_nsec == 123456700 &&
           (out.flags & UE_F_HIDDEN), "after reopen");
    CHECK(wclose(&w) == 0, "not clean");
}

static bool count_entries(void *ctx, const char *name, const ue_attr_t *a, uint64_t next)
{
    (void)name;
    (void)a;
    (void)next;
    (*(unsigned *)ctx)++;
    return true;
}

static void test_verifier(void)
{
    wvol_t w;
    ue_attr_t a;
    uint64_t v1, v2, v3;
    unsigned n = 0;
    ue_ino_t root;
    int err = wopen(&w, "clean", "verifier");

    CHECK(err == 0, "open: %s", strerror(err));
    root = root_of(w.e);
    WCHECK(ue_readdir(w.e, root, 0, count_entries, &n, &v1) == 0, "readdir");
    WCHECK(ue_create(w.e, root, "x", UE_T_REG, 0644, &a) == 0, "create");
    WCHECK(ue_readdir(w.e, root, 0, count_entries, &n, &v2) == 0 && v2 != v1, "verifier after create");
    WCHECK(ue_rename(w.e, root, "x", root, "y", 0) == 0, "rename");
    WCHECK(ue_readdir(w.e, root, 0, count_entries, &n, &v3) == 0 && v3 != v2, "verifier after rename");
    CHECK(wclose(&w) == 0, "not clean");
}

static void test_enospc(void)
{
    enum { CHUNK = 1 << 20 };
    unsigned char *buf = malloc(CHUNK);
    wvol_t w;
    ue_attr_t a, g;
    ue_ino_t root;
    uint64_t off = 0, free0, free1;
    int err = wopen(&w, "small", "enospc");

    CHECK(buf && err == 0, "open: %s", strerror(err));
    memset(buf, 0xA5, CHUNK);
    root = root_of(w.e);
    WCHECK(ue_statfs(w.e, NULL, &free0, NULL) == 0, "statfs");
    WCHECK(ue_create(w.e, root, "fill", UE_T_REG, 0644, &a) == 0, "create");
    for (;;) {
        size_t done;
        err = ue_write(w.e, a.ino, off, buf, CHUNK, &done);
        off += done;
        if (err || done < CHUNK)
            break;
    }
    if (!err) {                         // the short write: the next one must fail
        size_t done;
        err = ue_write(w.e, a.ino, off, buf, CHUNK, &done);
    }
    free(buf);
    WCHECK(err == ENOSPC, "filling gave %s after %llu bytes", strerror(err), (unsigned long long)off);
    WCHECK(off > free0 / 2, "only %llu of %llu bytes written", (unsigned long long)off, (unsigned long long)free0);
    WCHECK(ue_getattr(w.e, a.ino, &g) == 0 && g.size == off, "size %llu vs %llu written",
           (unsigned long long)g.size, (unsigned long long)off);
    ue_release(w.e, a.ino);
    WCHECK(ue_remove(w.e, root, "fill", false) == 0, "remove");
    WCHECK(ue_statfs(w.e, NULL, &free1, NULL) == 0 && free1 + (64u << 10) >= free0, "free %llu vs %llu",
           (unsigned long long)free1, (unsigned long long)free0);
    CHECK(wclose(&w) == 0, "not clean after ENOSPC");
}

static int write_loop(const char *base, int n)
{
    for (int i = 0; i < n; i++) {
        wvol_t w;
        ue_attr_t a, d;
        int err = wopen(&w, base, "leak");
        ue_ino_t root;

        if (err)
            return err;
        root = root_of(w.e);
        if (!(err = ue_create(w.e, root, "d", UE_T_DIR, 0755, &d)) &&
            !(err = ue_create(w.e, d.ino, "f", UE_T_REG, 0644, &a)) &&
            !(err = write_gen(w.e, a.ino, 0, 70000, 51)) &&
            !(err = ue_rename(w.e, d.ino, "f", root, "g", 0)) &&
            !(err = ue_remove(w.e, root, "g", false)))
            ue_release(w.e, a.ino);
        int cerr = ue_close(w.e);
        file_io_close(&w.f);
        if (err || cerr)
            return err ? err : cerr;
    }
    return 0;
}

static bool count_entry(void *ctx, const char *name, const ue_attr_t *attr, uint64_t next);

// Crash leftovers (Step 17, D-65): a read-write open deletes root ghosts and turns a rename
// temp name that is its item's only name into a visible "Unlatch recovered N". Temp names in
// other directories stay hidden, and new temp names never collide with them.
static void test_crash_leftovers(void)
{
    wvol_t w;
    ue_attr_t a;
    ue_ino_t root;

    // Read-only: nothing is touched.
    file_io_t f;
    ue_engine_t *e;
    CHECK(file_io_open(&f, img("leftover"), false) == 0, "open ro");
    CHECK(ue_open(&f.io, UE_OPEN_RDONLY, &e, NULL) == 0, "ue_open ro");
    CHECK(ue_lookup(e, root_of(e), "Unlatch recovered 1", &a) == ENOENT, "recovered on a read-only open");
    ue_close(e);
    file_io_close(&f);

    int err = wopen(&w, "leftover", "leftover");
    CHECK(err == 0, "open: %s", strerror(err));
    root = root_of(w.e);
    WCHECK(ue_lookup(w.e, root, "Unlatch recovered 1", &a) == 0, "rename temp not recovered");
    WCHECK(check_gen(w.e, a.ino, 0, 3000, 3) == 0, "recovered content");
    WCHECK(ue_lookup(w.e, root, "keep.txt", &a) == 0, "keep.txt");
    // Only keep.txt, sub and the recovered file are listed; the ghost is gone.
    uint64_t v;
    int count = 0;
    WCHECK(ue_readdir(w.e, root, 0, count_entry, &count, &v) == 0, "readdir");
    WCHECK(count == 3, "root lists %d entries, want 3", count);
    // New temp names (a case-only rename uses one) do not collide with sub's leftover.
    WCHECK(ue_lookup(w.e, root, "sub", &a) == 0, "sub");
    ue_attr_t b;
    WCHECK(ue_create(w.e, a.ino, "x", UE_T_REG, 0644, &b) == 0, "create in sub");
    WCHECK(ue_rename(w.e, a.ino, "x", a.ino, "X", 0) == 0, "case-only rename in sub");
    ue_release(w.e, b.ino);
    CHECK(wclose(&w) == 0, "close");
    // Opening again finds nothing more to do.
    err = wopen(&w, "leftover", "leftover2");
    CHECK(err == 0, "reopen");
    CHECK(wclose(&w) == 0, "close 2");
}

// ---- Page cache (Step 15, D-62): a model test against an in-memory device. The engine's
// cache functions are internal; their prototypes are repeated here.
typedef struct ue_bcache ue_bcache_t;
int bc_new(const ue_io_t *io, size_t bytes, ue_bcache_t **out);
void bc_free(ue_bcache_t *bc);
int bc_read(ue_bcache_t *bc, void *buf, size_t len, uint64_t off);
int bc_write(ue_bcache_t *bc, const void *buf, size_t len, uint64_t off);
int bc_writeback(ue_bcache_t *bc);
size_t bc_dirty(const ue_bcache_t *bc);
void bc_set_cold(ue_bcache_t *bc, bool cold);
void bc_get_stats(const ue_bcache_t *bc, ue_cache_stats_t *out);

typedef struct {
    uint8_t *mem;
    uint64_t size;
    uint32_t bs;
    uint64_t ops;
    bool misaligned;                  // a transfer was not block-aligned
} memdev_t;

static int mem_pread(void *ctx, void *buf, size_t len, uint64_t off)
{
    memdev_t *d = ctx;
    if (off % d->bs || len % d->bs)
        d->misaligned = true;
    if (off + len > d->size)
        return EIO;
    memcpy(buf, d->mem + off, len);
    d->ops++;
    return 0;
}

static int mem_pwrite(void *ctx, const void *buf, size_t len, uint64_t off)
{
    memdev_t *d = ctx;
    if (off % d->bs || len % d->bs)
        d->misaligned = true;
    if (off + len > d->size)
        return EIO;
    memcpy(d->mem + off, buf, len);
    d->ops++;
    return 0;
}

static void test_cache_model(void)
{
    // 8 MiB less three sectors: the last 4 KiB page is short. A cache of 64 pages forces
    // evictions and write-backs from inside reads and writes.
    memdev_t d = { .size = (8u << 20) - 1536, .bs = 512 };
    ue_io_t io = { .ctx = &d, .pread = mem_pread, .pwrite = mem_pwrite, .size_bytes = d.size,
                   .block_size = 512, .writable = true };
    uint8_t *shadow = malloc(d.size), *buf = malloc(1u << 20);
    ue_bcache_t *bc = NULL;
    ue_cache_stats_t st;
    uint64_t rng = 0x5eed;

    d.mem = malloc(d.size);
    CHECK(shadow && buf && d.mem, "malloc");
    for (uint64_t i = 0; i < d.size; i++)
        shadow[i] = d.mem[i] = (uint8_t)(i * 7 + (i >> 9));
    CHECK(bc_new(&io, 64 * 4096, &bc) == 0, "bc_new");
    for (int op = 0; op < 20000; op++) {
        rng = rng * 6364136223846793005ull + 1442695040888963407ull;
        uint64_t r = rng >> 11;
        // Mostly sector-aligned metadata-sized transfers, some large (direct), some at the end.
        size_t len = (r % 4 == 0) ? (size_t)((r >> 3) % 2048 + 1) * 512 : (size_t)((r >> 3) % 8 + 1) * 512;
        uint64_t off = ((r >> 20) % (d.size / 512)) * 512;
        if ((r >> 40) % 16 == 0)
            off = d.size - len < d.size ? d.size - len : 0;
        if (off + len > d.size)
            len = (size_t)(d.size - off);
        int kind = (int)((r >> 50) % 10);
        bc_set_cold(bc, (r >> 60) % 3 == 0);     // file data enters cold (evicted first)
        if (kind < 4) {
            for (size_t k = 0; k < len; k++)
                buf[k] = (uint8_t)(rng >> (k % 56)) ^ (uint8_t)k;
            CHECK(bc_write(bc, buf, len, off) == 0, "write");
            memcpy(shadow + off, buf, len);
        } else if (kind < 9) {
            CHECK(bc_read(bc, buf, len, off) == 0, "read");
            CHECK(memcmp(buf, shadow + off, len) == 0, "op %d: read of %zu at %llu differs", op, len,
                  (unsigned long long)off);
        } else {
            CHECK(bc_writeback(bc) == 0 && bc_dirty(bc) == 0, "writeback");
            CHECK(memcmp(d.mem, shadow, d.size) == 0, "op %d: device differs after write-back", op);
        }
    }
    CHECK(bc_writeback(bc) == 0, "final writeback");
    CHECK(memcmp(d.mem, shadow, d.size) == 0, "device differs at the end");
    CHECK(!d.misaligned, "a device transfer was not sector-aligned");
    bc_get_stats(bc, &st);
    CHECK(st.hits > 0 && st.misses > 0 && st.evictions > 0 && st.direct_reads > 0 && st.direct_writes > 0,
          "not every path ran: hits %llu misses %llu evictions %llu direct %llu/%llu",
          (unsigned long long)st.hits, (unsigned long long)st.misses, (unsigned long long)st.evictions,
          (unsigned long long)st.direct_reads, (unsigned long long)st.direct_writes);

    // Coalescing: 256 scattered-then-adjacent 1 KiB writes become one transfer.
    CHECK(bc_writeback(bc) == 0, "writeback");
    for (int i = 255; i >= 0; i--)
        CHECK(bc_write(bc, buf, 1024, (uint64_t)i * 1024) == 0, "small write");
    uint64_t before = d.ops;
    CHECK(bc_writeback(bc) == 0, "coalesced writeback");
    CHECK(d.ops - before == 1, "256 KiB of adjacent dirty pages took %llu transfers",
          (unsigned long long)(d.ops - before));
    bc_free(bc);
    free(d.mem);
    free(shadow);
    free(buf);
}

// Engine level: dirty metadata stays in the cache until written back, and the volume is
// complete on the device after ue_writeback alone.
static void test_cache_engine(void)
{
    wvol_t w;
    ue_attr_t a;
    ue_cache_stats_t st;
    ue_probe_t p;
    uint32_t saved = wflags;

    wflags = UE_OPEN_CACHE;
    int err = wopen(&w, "clean", "cache-engine");
    wflags = saved;
    CHECK(err == 0, "open: %s", strerror(err));
    ue_ino_t root = root_of(w.e);
    for (int i = 0; i < 200; i++) {
        char name[32];
        snprintf(name, sizeof(name), "f%03d", i);
        WCHECK(ue_create(w.e, root, name, UE_T_REG, 0644, &a) == 0, "create %s", name);
        WCHECK(write_gen(w.e, a.ino, 0, 3000 + (uint64_t)i * 100, (uint64_t)i) == 0, "write %s", name);
        ue_release(w.e, a.ino);
    }
    ue_get_cache_stats(w.e, &st);
    WCHECK(st.page_size == 4096 && st.dirty_pages > 0, "nothing dirty in the cache (%llu)",
           (unsigned long long)st.dirty_pages);
    WCHECK(ue_writeback(w.e, 60000) == 0, "young writeback");
    ue_get_cache_stats(w.e, &st);
    WCHECK(st.dirty_pages > 0, "written back before its age");
    WCHECK(ue_writeback(w.e, 0) == 0, "writeback");
    ue_get_cache_stats(w.e, &st);
    WCHECK(st.dirty_pages == 0, "dirty after writeback");
    // Open for writing, the volume is marked dirty on the device; a clean close clears it (D-86).
    WCHECK(probe_file(w.path, &p) == 0 && p.state == UE_VOL_DIRTY, "not dirty while open: %s",
           ue_vol_state_name(p.state));
    // A second, uncached reader of the same image sees every file now.
    file_io_t f2;
    ue_engine_t *e2;
    WCHECK(file_io_open(&f2, w.path, false) == 0, "open copy");
    WCHECK(ue_open(&f2.io, UE_OPEN_RDONLY, &e2, NULL) == 0, "open second engine");
    for (int i = 0; i < 200; i++) {
        char name[32];
        snprintf(name, sizeof(name), "f%03d", i);
        err = ue_lookup(e2, root_of(e2), name, &a);
        if (!err)
            err = check_gen(e2, a.ino, 0, 3000 + (uint64_t)i * 100, (uint64_t)i);
        if (err)
            break;
    }
    ue_close(e2);
    file_io_close(&f2);
    WCHECK(err == 0, "second reader: %s", strerror(err));
    CHECK(wclose(&w) == 0, "close");
    CHECK(probe_file(w.path, &p) == 0 && p.state == UE_VOL_CLEAN, "probe");
}

// D-86: a dirty volume keeps its mark through a read-write open and close, unless the caller
// says it has checked it (UE_OPEN_CLEAR_DIRTY).
static void test_dirty_mark(void)
{
    wvol_t w;
    ue_probe_t p;
    uint32_t saved = wflags;

    int err = wopen(&w, "dirty", "dirty-mark");
    CHECK(err == 0, "open: %s", strerror(err));
    CHECK(ue_close(w.e) == 0, "close");
    file_io_close(&w.f);
    CHECK(probe_file(w.path, &p) == 0 && p.state == UE_VOL_DIRTY, "lost the mark: %s",
          ue_vol_state_name(p.state));
    unlink(w.path);                    // the written-image checks expect every image clean
    wflags = UE_OPEN_CLEAR_DIRTY;
    err = wopen(&w, "dirty", "dirty-clear");
    wflags = saved;
    CHECK(err == 0, "open with CLEAR_DIRTY: %s", strerror(err));
    CHECK(wclose(&w) == 0, "close");
    CHECK(probe_file(w.path, &p) == 0 && p.state == UE_VOL_CLEAN, "still marked: %s",
          ue_vol_state_name(p.state));
}

#define RUN(t)                                     \
    do {                                           \
        int before = failures;                     \
        t();                                       \
        printf("%s %s\n", failures == before ? "ok  " : "FAIL", #t); \
    } while (0)

// Step 16: xattrs are named streams; list/get/set/remove, flags, positions, ':' names.
static void test_xattr(void)
{
    wvol_t w;
    ue_attr_t a;
    ue_ino_t root;
    char buf[256];
    size_t n;
    int err = wopen(&w, "clean", "xattr");

    CHECK(err == 0, "open: %s", strerror(err));
    root = root_of(w.e);
    WCHECK(ue_create(w.e, root, "x.txt", UE_T_REG, 0644, &a) == 0, "create");
    WCHECK(write_gen(w.e, a.ino, 0, 5000, 31) == 0, "data");
    WCHECK(ue_xattr_list(w.e, a.ino, NULL, 0, &n) == 0 && n == 0, "empty list: %zu", n);
    WCHECK(ue_xattr_get(w.e, a.ino, "nope", 0, buf, sizeof(buf), &n) == 93, "ENOATTR");
    WCHECK(ue_xattr_set(w.e, a.ino, "com.example.a", 0, "hello", 5, 0) == 0, "set a");
    WCHECK(ue_xattr_set(w.e, a.ino, "com.apple.metadata:_kMDItemUserTags", 0, "tags", 4, 0) == 0, "set colon");
    WCHECK(ue_xattr_set(w.e, a.ino, "com.example.a", 0, "x", 1, UE_XATTR_CREATE) == EEXIST, "CREATE on existing");
    WCHECK(ue_xattr_set(w.e, a.ino, "com.example.b", 0, "x", 1, UE_XATTR_REPLACE) == 93, "REPLACE on missing");
    WCHECK(ue_xattr_set(w.e, a.ino, "com.example.a", 0, "hi", 2, UE_XATTR_REPLACE) == 0, "replace shrinks");
    WCHECK(ue_xattr_set(w.e, a.ino, "com.apple.ResourceFork", 0, "abcdef", 6, 0) == 0, "fork");
    WCHECK(ue_xattr_set(w.e, a.ino, "com.apple.ResourceFork", 4, "XYZ", 3, 0) == 0, "fork at 4");
    WCHECK(ue_xattr_list(w.e, a.ino, NULL, 0, &n) == 0, "size query");
    WCHECK(ue_xattr_list(w.e, a.ino, buf, 4, &n) == ERANGE, "small buffer");
    WCHECK(wreopen(&w, true) == 0, "reopen");
    WCHECK(ue_walk(w.e, "/x.txt", &a) == 0, "walk");
    WCHECK(ue_xattr_list(w.e, a.ino, buf, sizeof(buf), &n) == 0, "list");
    bool sa = false, sc = false, sf = false;
    for (size_t i = 0; i < n; i += strlen(buf + i) + 1) {
        sa |= !strcmp(buf + i, "com.example.a");
        sc |= !strcmp(buf + i, "com.apple.metadata:_kMDItemUserTags");
        sf |= !strcmp(buf + i, "com.apple.ResourceFork");
    }
    WCHECK(sa && sc && sf, "names after reopen");
    WCHECK(ue_xattr_get(w.e, a.ino, "com.example.a", 0, buf, sizeof(buf), &n) == 0 && n == 2 && !memcmp(buf, "hi", 2), "value a");
    WCHECK(ue_xattr_get(w.e, a.ino, "com.apple.ResourceFork", 0, buf, sizeof(buf), &n) == 0 && n == 7 &&
           !memcmp(buf, "abcdXYZ", 7), "fork value");
    WCHECK(ue_xattr_get(w.e, a.ino, "com.apple.ResourceFork", 0, NULL, 0, &n) == 0 && n == 7, "fork size");
    WCHECK(ue_xattr_get(w.e, a.ino, "com.example.a", 0, buf, 1, &n) == ERANGE, "get ERANGE");
    WCHECK(ue_xattr_remove(w.e, a.ino, "com.example.a") == 0, "remove");
    WCHECK(ue_xattr_remove(w.e, a.ino, "com.example.a") == 93, "remove again");
    WCHECK(ue_xattr_set(w.e, a.ino, "bad/name", 0, "x", 1, 0) == EINVAL, "slash");
    WCHECK(check_gen(w.e, a.ino, 0, 5000, 31) == 0, "file data untouched");
    WCHECK(ue_getattr(w.e, a.ino, &a) == 0 && a.size == 5000, "size is the unnamed stream");
    CHECK(wclose(&w) == 0, "not clean");
}

// Step 16: WSL symlinks, reading them back, hard links.
static void test_links(void)
{
    wvol_t w;
    ue_attr_t f, l, h, b;
    ue_ino_t root;
    char buf[512];
    int err = wopen(&w, "clean", "links");

    CHECK(err == 0, "open: %s", strerror(err));
    root = root_of(w.e);
    WCHECK(ue_create(w.e, root, "target.txt", UE_T_REG, 0644, &f) == 0, "create");
    WCHECK(ue_symlink(w.e, root, "rel", "target.txt", &l) == 0 && l.type == UE_T_LNK, "symlink");
    WCHECK(ue_symlink(w.e, root, "abs", "/Volumes/x/y", &b) == 0, "absolute symlink");
    WCHECK(ue_symlink(w.e, root, "rel", "x", &b) == EEXIST, "symlink over a name");
    WCHECK(ue_readlink(w.e, l.ino, "/mnt", buf, sizeof(buf)) == 0 && !strcmp(buf, "target.txt"), "readlink %s", buf);
    WCHECK(ue_readlink(w.e, l.ino, "/mnt", buf, 4) == ENAMETOOLONG, "small buffer");
    WCHECK(ue_readlink(w.e, f.ino, "/mnt", buf, sizeof(buf)) == EINVAL, "not a link");
    WCHECK(ue_link(w.e, f.ino, root, "hard.txt", &h) == 0 && h.ino == f.ino && h.nlink == 2, "link nlink %u", h.nlink);
    WCHECK(ue_link(w.e, root, root, "rootlink", &b) == EPERM, "directory link");
    WCHECK(ue_link(w.e, f.ino, root, "hard.txt", &b) == EEXIST, "link over a name");
    WCHECK(wreopen(&w, true) == 0, "reopen");
    WCHECK(ue_walk(w.e, "/rel", &l) == 0 && l.type == UE_T_LNK, "type after reopen");
    WCHECK(ue_readlink(w.e, l.ino, "/mnt", buf, sizeof(buf)) == 0 && !strcmp(buf, "target.txt"), "readlink after reopen");
    WCHECK(ue_walk(w.e, "/abs", &b) == 0 && ue_readlink(w.e, b.ino, "/mnt", buf, sizeof(buf)) == 0 &&
           !strcmp(buf, "/Volumes/x/y"), "absolute target %s", buf);
    WCHECK(ue_remove(w.e, root, "hard.txt", false) == 0, "unlink one name");
    WCHECK(ue_walk(w.e, "/target.txt", &f) == 0 && f.nlink == 1, "nlink back to 1");
    WCHECK(ue_remove(w.e, root, "rel", false) == 0, "remove symlink");
    CHECK(wclose(&w) == 0, "not clean");
}

// Step 16: reparse points written the way Windows writes them (mkfixture reparse).
// Native Windows symlinks for relative on-volume targets (Step 17, D-67); the others stay WSL
// links. Every target reads back exactly as written. Scripts/test-engine.sh checks the tags.
static void test_native_links(void)
{
    static const struct { const char *dir, *name, *target; } links[] = {
        { "", "s-file", "t.txt" },              // native, file
        { "", "s-dir", "d" },                   // native, directory
        { "d", "s-up", "../t.txt" },            // native, through ..
        { "d", "s-dot", "./../d/inner.txt" },   // native
        { "", "s-escape", "../t.txt" },         // WSL: leaves the volume
        { "", "s-abs", "/Volumes/X/t.txt" },    // WSL: absolute
        { "", "s-missing", "nothing" },         // WSL: dangling
        { "", "s-colon", "a:b" },               // WSL: not a Windows name
    };
    wvol_t w;
    ue_attr_t a, d, l;
    char buf[512];
    int err = wopen(&w, "clean", "native-links");

    CHECK(err == 0, "open: %s", strerror(err));
    ue_ino_t root = root_of(w.e);
    WCHECK(ue_create(w.e, root, "t.txt", UE_T_REG, 0644, &a) == 0, "create");
    WCHECK(ue_create(w.e, root, "d", UE_T_DIR, 0755, &d) == 0, "mkdir");
    WCHECK(ue_create(w.e, d.ino, "inner.txt", UE_T_REG, 0644, &a) == 0, "create inner");
    for (size_t i = 0; i < sizeof(links) / sizeof(*links); i++) {
        ue_ino_t dir = *links[i].dir ? d.ino : root;
        WCHECK(ue_symlink(w.e, dir, links[i].name, links[i].target, &l) == 0 && l.type == UE_T_LNK, "symlink %s",
               links[i].name);
        ue_release(w.e, l.ino);
    }
    WCHECK(wreopen(&w, true) == 0, "reopen");
    for (size_t i = 0; i < sizeof(links) / sizeof(*links); i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s%s/%s", *links[i].dir ? "/" : "", links[i].dir, links[i].name);
        WCHECK(ue_walk(w.e, path, &l) == 0 && l.type == UE_T_LNK, "%s is not a link", path);
        WCHECK(ue_readlink(w.e, l.ino, "/mnt", buf, sizeof(buf)) == 0 && !strcmp(buf, links[i].target),
               "%s reads back %s", path, buf);
    }
    // A directory symlink is removed with unlink, like any link; rename moves it.
    root = root_of(w.e);
    WCHECK(ue_rename(w.e, root, "s-dir", root, "s-dir2", 0) == 0, "rename a directory symlink");
    WCHECK(ue_remove(w.e, root, "s-dir2", true) == ENOTDIR, "rmdir of a directory symlink");
    WCHECK(ue_remove(w.e, root, "s-dir2", false) == 0, "unlink a directory symlink");
    WCHECK(ue_symlink(w.e, root, "s-dir", "d", &l) == 0, "symlink again");
    ue_release(w.e, l.ino);
    CHECK(wclose(&w) == 0, "close");
}

static void test_windows_links(void)
{
    static const struct { const char *path, *target; } want[] = {
        { "/rel.lnk", "users/public/hello.txt" },
        { "/abs.lnk", "/mnt/users/public/hello.txt" },
        { "/Docs", "/mnt/users/public" },
        { "/other.lnk", "/mnt/.NTFS-3G/D:/elsewhere/x.txt" },
    };
    file_io_t f;
    ue_engine_t *e;
    ue_attr_t a;
    char buf[512];

    CHECK(file_io_open(&f, img("reparse"), false) == 0, "open image");
    CHECK(ue_open(&f.io, UE_OPEN_RDONLY, &e, NULL) == 0, "ue_open");
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        int err = ue_walk(e, want[i].path, &a);
        if (!err && a.type != UE_T_LNK)
            err = EFTYPE;
        if (!err)
            err = ue_readlink(e, a.ino, "/mnt", buf, sizeof(buf));
        if (err || strcmp(buf, want[i].target)) {
            fprintf(stderr, "  FAIL %s: %s, got \"%s\"\n", want[i].path, strerror(err), err ? "" : buf);
            failures++;
        }
    }
    ue_close(e);
    file_io_close(&f);
}

// A directory entry whose MFT record is zeros (seen on a real disk) hides only itself.
static bool count_entry(void *ctx, const char *name, const ue_attr_t *attr, uint64_t next)
{
    (void)name;
    (void)attr;
    (void)next;
    (*(int *)ctx)++;
    return true;
}

static void test_damaged_entry(void)
{
    file_io_t f;
    ue_engine_t *e;
    ue_attr_t d, a;
    uint64_t v;
    int n = 0;

    CHECK(file_io_open(&f, img("reparse"), false) == 0, "open image");
    CHECK(ue_open(&f.io, UE_OPEN_RDONLY, &e, NULL) == 0, "ue_open");
    int err = ue_walk(e, "/damaged", &d);
    if (!err)
        err = ue_readdir(e, d.ino, 0, count_entry, &n, &v);
    if (err || n != 2) {
        fprintf(stderr, "  FAIL readdir of /damaged: %s, %d entries (want 2)\n", strerror(err), n);
        failures++;
    }
    if (ue_walk(e, "/damaged/c.txt", &a) != 0) {
        fprintf(stderr, "  FAIL sibling after the damaged entry\n");
        failures++;
    }
    ue_close(e);
    file_io_close(&f);
}

// Surprise removal / failed write (Step 10): after the first failed device write the engine
// returns EIO for everything, writes nothing more, and close reports the failure.
static void test_device_failure(void)
{
    wvol_t w;
    ue_attr_t a, b;
    ue_ino_t root;
    ue_probe_t p;
    size_t done;
    char buf[4096] = { 1 };
    int err = wopen(&w, "clean", "failure");

    CHECK(err == 0, "open: %s", strerror(err));
    root = root_of(w.e);
    WCHECK(ue_create(w.e, root, "kept.bin", UE_T_REG, 0644, &a) == 0, "create");
    WCHECK(write_gen(w.e, a.ino, 0, 1 << 20, 21) == 0, "write");
    WCHECK(ue_sync(w.e) == 0, "sync");
    WCHECK(!ue_has_failed(w.e), "failed too early");
    // The device turns read-only underneath the engine: every pwrite now fails.
    int ro = open(w.path, O_RDONLY);
    WCHECK(ro >= 0 && dup2(ro, w.f.fd) >= 0, "swap fd");
    close(ro);
    err = 0;
    for (int i = 0; i < 64 && !err; i++)   // libntfs-3g may buffer a few writes in memory
        err = ue_write(w.e, a.ino, (uint64_t)i << 20, buf, sizeof(buf), &done);
    if (!err)
        err = ue_sync(w.e);
    WCHECK(err != 0, "writes kept succeeding");
    WCHECK(ue_has_failed(w.e), "not failed after a failed write");
    WCHECK(ue_getattr(w.e, root, &b) == EIO, "getattr after failure");
    WCHECK(ue_lookup(w.e, root, "kept.bin", &b) == EIO, "lookup after failure");
    WCHECK(ue_create(w.e, root, "new.bin", UE_T_REG, 0644, &b) == EIO, "create after failure");
    WCHECK(ue_sync(w.e) == EIO, "sync after failure");
    err = ue_close(w.e);
    file_io_close(&w.f);
    CHECK(err != 0, "close after failure claimed success");
    // What was synced before the failure is still there and the volume still parses.
    CHECK(probe_file(w.path, &p) == 0 && p.state != UE_VOL_CORRUPT && p.state != UE_VOL_NOT_NTFS,
          "probe after failure: %s", ue_vol_state_name(p.state));
    unlink(w.path);                    // not a clean-unmount image; keep it out of the w-*.img checks
}

// Check Disk (D-93). The chk-*.img fixtures (Scripts/test-engine.sh) each carry one kind of
// planted damage; clean images and the read fixture must check clean.
typedef struct {
    ue_check_problem_t first[UE_CHK_KINDS];   // the first problem of each kind
} check_seen_t;

static void on_check(void *ctx, const ue_check_problem_t *p)
{
    check_seen_t *s = ctx;
    if (!s->first[p->kind].kind)
        s->first[p->kind] = *p;
}

static int check_path(const char *path, uint32_t flags, ue_check_result_t *r, check_seen_t *seen)
{
    file_io_t f;
    int err = file_io_open(&f, path, (flags & UE_CHECK_REPAIR) != 0);
    if (err)
        return err;
    memset(seen, 0, sizeof(*seen));
    err = ue_check(&f.io, flags, on_check, seen, r);
    file_io_close(&f);
    return err;
}

static void test_check_clean(void)
{
    const char *clean[] = { "clean", "read", "small", "leftover", "dirty" };
    ue_check_result_t r;
    check_seen_t seen;

    for (size_t i = 0; i < sizeof(clean) / sizeof(clean[0]); i++) {
        int err = check_path(img(clean[i]), 0, &r, &seen);
        CHECK(err == 0 && r.problems == 0 && r.lost_records == 0 && r.lost_clusters == 0,
              "%s: %s, %llu problems, lost %llu/%llu", clean[i], strerror(err), (unsigned long long)r.problems,
              (unsigned long long)r.lost_records, (unsigned long long)r.lost_clusters);
        CHECK(r.records_in_use > 16 && r.directories >= 2 && r.clusters_in_use > 0, "%s: counted nothing", clean[i]);
    }
    CHECK(check_path(img("read"), 0, &r, &seen) == 0 && r.entries > 10000, "read: %llu entries",
          (unsigned long long)r.entries);
    // The fixture made from a real disk's damage (zero-record): one entry left behind.
    CHECK(check_path(img("reparse"), 0, &r, &seen) == 0 && r.problems == 1 &&
          r.by_kind[UE_CHK_ENTRY_FREE_RECORD] == 1, "reparse: %llu problems", (unsigned long long)r.problems);
    CHECK(!strcmp(seen.first[UE_CHK_ENTRY_FREE_RECORD].path, "/damaged/broken.txt"), "path %s",
          seen.first[UE_CHK_ENTRY_FREE_RECORD].path);
    CHECK(check_path(img("zeroboot"), 0, &r, &seen) == EINVAL, "not NTFS");
    CHECK(check_path(img("hiber"), UE_CHECK_REPAIR, &r, &seen) == EPERM, "repair of a hibernated volume");
}

static void test_check_kinds(void)
{
    static const struct {
        const char *name;
        ue_check_kind_t kind, also;   // also: a kind that follows from the same damage
        const char *path;
        bool repairable;
    } cases[] = {
        { "chk-record-bit", UE_CHK_RECORD_UNMARKED, 0, "/d/a.bin", true },
        { "chk-cluster-bits", UE_CHK_CLUSTERS_UNMARKED, 0, "/d/c.bin", true },
        { "chk-shared", UE_CHK_CLUSTERS_SHARED, 0, "/d/b.bin", false },
        { "chk-run-past", UE_CHK_BAD_RUNLIST, 0, "/d/a.bin", false },
        { "chk-seq", UE_CHK_ENTRY_WRONG_SEQ, UE_CHK_ORPHAN, "/d/a.bin", true },
        { "chk-torn", UE_CHK_BAD_RECORD, 0, "/d/a.bin", false },
        { "chk-index", UE_CHK_INDEX_UNREADABLE, UE_CHK_ORPHAN, "/big", false },
    };
    ue_check_result_t r;
    check_seen_t seen;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int err = check_path(img(cases[i].name), 0, &r, &seen);
        CHECK(err == 0, "%s: %s", cases[i].name, strerror(err));
        CHECK(r.by_kind[cases[i].kind] == 1, "%s: %llu of %s", cases[i].name,
              (unsigned long long)r.by_kind[cases[i].kind], ue_check_kind_name(cases[i].kind));
        CHECK(r.problems == 1 + (cases[i].also ? r.by_kind[cases[i].also] : 0), "%s: %llu problems",
              cases[i].name, (unsigned long long)r.problems);
        CHECK(!cases[i].also || r.by_kind[cases[i].also] > 0, "%s: no %s", cases[i].name,
              ue_check_kind_name(cases[i].also));
        const ue_check_problem_t *p = &seen.first[cases[i].kind];
        CHECK(!strcmp(p->path, cases[i].path), "%s: path %s", cases[i].name, p->path);
        // Orphans of whole records are repairable (D-99).
        uint64_t orphans = cases[i].also == UE_CHK_ORPHAN ? r.by_kind[UE_CHK_ORPHAN] : 0;
        CHECK(p->repairable == cases[i].repairable && r.repairable == (cases[i].repairable ? 1u : 0u) + orphans,
              "%s: repairable %d", cases[i].name, p->repairable);
        CHECK(r.state == UE_VOL_CLEAN && r.remaining == r.problems && !r.repair_ran, "%s: state", cases[i].name);
    }
    // Shared clusters name both files.
    CHECK(check_path(img("chk-shared"), 0, &r, &seen) == 0 && seen.first[UE_CHK_CLUSTERS_SHARED].other > 0 &&
          seen.first[UE_CHK_CLUSTERS_SHARED].other != UINT64_MAX, "no first owner");
}

static uint64_t file_sum(const char *path)
{
    uint64_t h = 1469598103934665603ull;
    unsigned char b[65536];
    ssize_t n;
    int fd = open(path, O_RDONLY);
    while (fd >= 0 && (n = read(fd, b, sizeof(b))) > 0)
        for (ssize_t i = 0; i < n; i++)
            h = (h ^ b[i]) * 1099511628211ull;
    if (fd >= 0)
        close(fd);
    return h;
}

static void test_check_repair(void)
{
    char path[2048];
    ue_check_result_t r;
    check_seen_t seen;
    ue_probe_t p;

    // Bitmap bits only, on a volume marked dirty: repaired, checked clean, mark cleared.
    snprintf(path, sizeof(path), "%s/x-chk-repair.img", dir);
    CHECK(copyfile(img("chk-repairable"), path, NULL, COPYFILE_CLONE | COPYFILE_DATA) == 0, "copy");
    int err = check_path(path, UE_CHECK_REPAIR, &r, &seen);
    CHECK(err == 0, "repair: %s", strerror(err));
    CHECK(r.state == UE_VOL_DIRTY && r.problems == 3 && r.repairable == 3 && r.repair_ran && r.repaired > 3 &&
          r.remaining == 0 && r.dirty_cleared, "problems %llu repaired %llu remaining %llu cleared %d",
          (unsigned long long)r.problems, (unsigned long long)r.repaired, (unsigned long long)r.remaining,
          r.dirty_cleared);
    CHECK(probe_file(path, &p) == 0 && p.state == UE_VOL_CLEAN, "after repair: %s", ue_vol_state_name(p.state));
    CHECK(check_path(path, 0, &r, &seen) == 0 && r.problems == 0, "%llu problems after repair",
          (unsigned long long)r.problems);
    unlink(path);

    // One problem that cannot be repaired (shared clusters): nothing is written, the mark stays.
    snprintf(path, sizeof(path), "%s/x-chk-mix.img", dir);
    CHECK(copyfile(img("chk-mix"), path, NULL, COPYFILE_CLONE | COPYFILE_DATA) == 0, "copy");
    uint64_t before = file_sum(path);
    CHECK(check_path(path, UE_CHECK_REPAIR, &r, &seen) == 0, "check of the mix");
    CHECK(!r.repair_ran && r.repaired == 0 && r.remaining == r.problems && r.problems > r.repairable &&
          r.repairable > 0 && !r.dirty_cleared, "mix: ran %d repaired %llu", r.repair_ran,
          (unsigned long long)r.repaired);
    CHECK(file_sum(path) == before, "the mix was written to");
    CHECK(probe_file(path, &p) == 0 && p.state == UE_VOL_DIRTY, "mix: %s", ue_vol_state_name(p.state));
    unlink(path);

    // Folder entries that lead to no file (D-96): free records in /d and in /big's index
    // blocks, a DOS+Win32 pair (one problem, two entries), /big/f00100's record reused by /e
    // (another sequence number), and an entry for a record whose name is in another folder.
    // All are removed; the records are not touched.
    snprintf(path, sizeof(path), "%s/x-chk-dangling.img", dir);
    CHECK(copyfile(img("chk-dangling"), path, NULL, COPYFILE_CLONE | COPYFILE_DATA) == 0, "copy");
    CHECK(check_path(path, 0, &r, &seen) == 0, "check of the dangling entries");
    CHECK(r.problems == 5 && r.repairable == 5 && r.by_kind[UE_CHK_ENTRY_FREE_RECORD] == 3 &&
          r.by_kind[UE_CHK_ENTRY_WRONG_SEQ] == 1 && r.by_kind[UE_CHK_ENTRY_WRONG_PARENT] == 1,
          "dangling: %llu problems, %llu repairable, %llu free, %llu seq, %llu parent", (unsigned long long)r.problems,
          (unsigned long long)r.repairable, (unsigned long long)r.by_kind[UE_CHK_ENTRY_FREE_RECORD],
          (unsigned long long)r.by_kind[UE_CHK_ENTRY_WRONG_SEQ],
          (unsigned long long)r.by_kind[UE_CHK_ENTRY_WRONG_PARENT]);
    CHECK(!strcmp(seen.first[UE_CHK_ENTRY_WRONG_SEQ].path, "/big/f00100"), "path %s",
          seen.first[UE_CHK_ENTRY_WRONG_SEQ].path);
    CHECK(!strcmp(seen.first[UE_CHK_ENTRY_WRONG_PARENT].path, "/e/ghost.bin"), "path %s",
          seen.first[UE_CHK_ENTRY_WRONG_PARENT].path);
    CHECK(!strcmp(seen.first[UE_CHK_ENTRY_FREE_RECORD].path, "/d/a.bin"), "path %s",
          seen.first[UE_CHK_ENTRY_FREE_RECORD].path);
    err = check_path(path, UE_CHECK_REPAIR, &r, &seen);
    CHECK(err == 0 && r.repair_ran && r.entries_removed == 5 && r.remaining == 0 && r.dirty_cleared,
          "dangling repair: %s, ran %d, removed %llu, remaining %llu", strerror(err), r.repair_ran,
          (unsigned long long)r.entries_removed, (unsigned long long)r.remaining);
    CHECK(check_path(path, 0, &r, &seen) == 0 && r.problems == 0 && r.entries > 300, "%llu problems after repair",
          (unsigned long long)r.problems);
    CHECK(probe_file(path, &p) == 0 && p.state == UE_VOL_CLEAN, "after repair: %s", ue_vol_state_name(p.state));
    unlink(path);

    // Nothing found on a dirty volume: only the mark is cleared. Read-only io: EROFS.
    snprintf(path, sizeof(path), "%s/x-chk-dirty.img", dir);
    CHECK(copyfile(img("dirty"), path, NULL, COPYFILE_CLONE | COPYFILE_DATA) == 0, "copy");
    file_io_t f;
    CHECK(file_io_open(&f, path, false) == 0, "open");
    err = ue_check(&f.io, UE_CHECK_REPAIR, NULL, NULL, &r);
    file_io_close(&f);
    CHECK(err == EROFS, "repair on a read-only device: %s", strerror(err));
    CHECK(check_path(path, UE_CHECK_REPAIR, &r, &seen) == 0 && r.problems == 0 && !r.repair_ran && r.dirty_cleared,
          "dirty, nothing found: cleared %d", r.dirty_cleared);
    CHECK(probe_file(path, &p) == 0 && p.state == UE_VOL_CLEAN, "still %s", ue_vol_state_name(p.state));
    unlink(path);
}

// A copy of fixture name in the image directory, for a repair.
static int fixture_copy(const char *name, char *path, size_t cap)
{
    snprintf(path, cap, "%s/x-%s.img", dir, name);
    return copyfile(img(name), path, NULL, COPYFILE_CLONE | COPYFILE_DATA);
}

// Opens image path read-only and checks that file `file` holds len bytes of stream seed.
static int image_has_gen(const char *path, const char *file, uint64_t len, uint64_t seed)
{
    file_io_t f;
    ue_engine_t *e;
    ue_attr_t a;
    int err = file_io_open(&f, path, false);

    if (err)
        return err;
    if (!(err = ue_open(&f.io, UE_OPEN_RDONLY, &e, NULL))) {
        if (!(err = ue_walk(e, file, &a)))
            err = a.size != len ? EIO : check_gen(e, a.ino, 0, len, seed);
        int cerr = ue_close(e);
        err = err ? err : cerr;
    }
    file_io_close(&f);
    return err;
}

// The names in folder `folder` of image path.
static int image_names(const char *path, const char *folder, names_t *out)
{
    file_io_t f;
    ue_engine_t *e;
    ue_attr_t a;
    int err = file_io_open(&f, path, false);

    memset(out, 0, sizeof(*out));
    if (err)
        return err;
    if (!(err = ue_open(&f.io, UE_OPEN_RDONLY, &e, NULL))) {
        if (!(err = ue_walk(e, folder, &a)))
            err = ue_readdir(e, a.ino, 0, collect_names, out, NULL);
        int cerr = ue_close(e);
        err = err ? err : cerr;
    }
    file_io_close(&f);
    return err;
}

// Orphans (D-99): records whose folder entries are gone go back where their names say; the
// ones whose folder is gone, or whose name another file took, go to /found.000.
static void test_check_orphans(void)
{
    char path[2048];
    ue_check_result_t r;
    check_seen_t seen;
    names_t names;

    // Back in place: two files and a DOS+Win32 pair (one orphan, two entries).
    CHECK(fixture_copy("chk-orphan", path, sizeof(path)) == 0, "copy");
    CHECK(check_path(path, 0, &r, &seen) == 0 && r.problems == 3 && r.repairable == 3 &&
          r.by_kind[UE_CHK_ORPHAN] == 3, "orphan: %llu problems, %llu repairable", (unsigned long long)r.problems,
          (unsigned long long)r.repairable);
    CHECK(seen.first[UE_CHK_ORPHAN].repairable && strstr(seen.first[UE_CHK_ORPHAN].path, "/"), "orphan path %s",
          seen.first[UE_CHK_ORPHAN].path);
    int err = check_path(path, UE_CHECK_REPAIR, &r, &seen);
    CHECK(err == 0 && r.repair_ran && r.orphans_relinked == 3 && r.orphans_found == 0 && r.remaining == 0,
          "orphan repair: %s, relinked %llu, found %llu, remaining %llu", strerror(err),
          (unsigned long long)r.orphans_relinked, (unsigned long long)r.orphans_found, (unsigned long long)r.remaining);
    CHECK(check_path(path, 0, &r, &seen) == 0 && r.problems == 0, "%llu problems after repair",
          (unsigned long long)r.problems);
    CHECK(image_has_gen(path, "/d/a.bin", 20000, 1) == 0, "/d/a.bin after relink");
    CHECK(image_has_gen(path, "/d/Orphan-Long-Name.txt", 10, 6) == 0, "the long name after relink");
    CHECK(image_names(path, "/d", &names) == 0 && has_name(&names, "a.bin") && has_name(&names, "Orphan-Long-Name.txt") &&
          !has_name(&names, "ORPHAN~1.TXT"), "/d lists %d names", names.n);
    CHECK(image_names(path, "/", &names) == 0 && !has_name(&names, "found.000"), "found.000 made for nothing");
    unlink(path);

    // The folder is gone: its entry is removed (D-96) and the file goes to /found.000.
    CHECK(fixture_copy("chk-orphan-gone", path, sizeof(path)) == 0, "copy");
    CHECK(check_path(path, 0, &r, &seen) == 0 && r.problems == 2 && r.repairable == 2 &&
          r.by_kind[UE_CHK_ORPHAN] == 1 && r.by_kind[UE_CHK_ENTRY_FREE_RECORD] == 1,
          "gone: %llu problems, %llu repairable", (unsigned long long)r.problems, (unsigned long long)r.repairable);
    uint64_t rec = seen.first[UE_CHK_ORPHAN].rec;
    err = check_path(path, UE_CHECK_REPAIR, &r, &seen);
    CHECK(err == 0 && r.entries_removed == 1 && r.orphans_relinked == 1 && r.orphans_found == 1 && r.remaining == 0,
          "gone repair: %s, removed %llu, relinked %llu, found %llu, remaining %llu", strerror(err),
          (unsigned long long)r.entries_removed, (unsigned long long)r.orphans_relinked,
          (unsigned long long)r.orphans_found, (unsigned long long)r.remaining);
    CHECK(check_path(path, 0, &r, &seen) == 0 && r.problems == 0, "%llu problems after repair",
          (unsigned long long)r.problems);
    char moved[64];
    snprintf(moved, sizeof(moved), "/found.000/file%llu", (unsigned long long)rec);
    CHECK(image_has_gen(path, moved, 5000, 7) == 0, "%s after relink", moved);
    CHECK(image_names(path, "/", &names) == 0 && !has_name(&names, "p"), "/p still listed");
    unlink(path);

    // The name is taken by a newer file: the orphan goes to /found.000, the newer file stays.
    CHECK(fixture_copy("chk-orphan-taken", path, sizeof(path)) == 0, "copy");
    CHECK(check_path(path, 0, &r, &seen) == 0 && r.problems == 1 && r.repairable == 1, "taken: %llu problems",
          (unsigned long long)r.problems);
    rec = seen.first[UE_CHK_ORPHAN].rec;
    err = check_path(path, UE_CHECK_REPAIR, &r, &seen);
    CHECK(err == 0 && r.orphans_relinked == 1 && r.orphans_found == 1 && r.remaining == 0,
          "taken repair: %s, found %llu, remaining %llu", strerror(err), (unsigned long long)r.orphans_found,
          (unsigned long long)r.remaining);
    snprintf(moved, sizeof(moved), "/found.000/file%llu", (unsigned long long)rec);
    CHECK(image_has_gen(path, moved, 20000, 2) == 0, "%s after relink", moved);
    CHECK(image_has_gen(path, "/d/b.bin", 100, 8) == 0, "the newer /d/b.bin");
    CHECK(check_path(path, 0, &r, &seen) == 0 && r.problems == 0, "%llu problems after repair",
          (unsigned long long)r.problems);
    unlink(path);

    // A damaged orphan is left alone, and blocks the repair: chk-seq's orphan has a reused
    // sequence number but is whole, so it is repairable; chk-index's are behind an unreadable index.
    CHECK(check_path(img("chk-index"), 0, &r, &seen) == 0 && r.problems > r.repairable, "chk-index repairable");
}

// $MFTMirr apart from $MFT (D-98): copied from $MFT when $MFT's records are whole, and the
// rest of the volume is then checked and repaired in the same call.
static void test_check_mirror(void)
{
    char path[2048];
    ue_check_result_t r;
    check_seen_t seen;
    ue_probe_t p;

    CHECK(fixture_copy("chk-mirror", path, sizeof(path)) == 0, "copy");
    CHECK(probe_file(path, &p) == 0 && p.state == UE_VOL_CORRUPT, "mirror fixture: %s", ue_vol_state_name(p.state));
    CHECK(check_path(path, 0, &r, &seen) == 0 && r.problems == 1 && r.repairable == 1 &&
          r.by_kind[UE_CHK_MFT_MIRROR] == 1 && seen.first[UE_CHK_MFT_MIRROR].rec == 3,
          "mirror: %llu problems, %llu repairable, record %llu", (unsigned long long)r.problems,
          (unsigned long long)r.repairable, (unsigned long long)seen.first[UE_CHK_MFT_MIRROR].rec);
    file_io_t f;
    CHECK(file_io_open(&f, path, false) == 0, "open");
    int err = ue_check(&f.io, UE_CHECK_REPAIR, NULL, NULL, &r);
    file_io_close(&f);
    CHECK(err == EROFS, "mirror repair on a read-only device: %s", strerror(err));
    err = check_path(path, UE_CHECK_REPAIR, &r, &seen);
    CHECK(err == 0 && r.mirror_repaired && r.repair_ran && r.state == UE_VOL_CORRUPT && r.problems == 1 &&
          r.remaining == 0, "mirror repair: %s, repaired %d, %llu problems, remaining %llu", strerror(err),
          r.mirror_repaired, (unsigned long long)r.problems, (unsigned long long)r.remaining);
    CHECK(probe_file(path, &p) == 0 && p.state == UE_VOL_CLEAN, "after repair: %s", ue_vol_state_name(p.state));
    CHECK(check_path(path, 0, &r, &seen) == 0 && r.problems == 0, "%llu problems after repair",
          (unsigned long long)r.problems);
    unlink(path);

    // A torn $MFT record: reported, not repairable, nothing written.
    CHECK(fixture_copy("chk-mirror-torn", path, sizeof(path)) == 0, "copy");
    uint64_t before = file_sum(path);
    CHECK(check_path(path, UE_CHECK_REPAIR, &r, &seen) == 0 && r.problems == 1 && r.repairable == 0 &&
          r.by_kind[UE_CHK_MFT_MIRROR] == 1 && !r.mirror_repaired && !r.repair_ran && r.remaining == 1,
          "torn: %llu problems, %llu repairable, repaired %d", (unsigned long long)r.problems,
          (unsigned long long)r.repairable, r.mirror_repaired);
    CHECK(strstr(seen.first[UE_CHK_MFT_MIRROR].detail, "torn"), "detail: %s", seen.first[UE_CHK_MFT_MIRROR].detail);
    CHECK(file_sum(path) == before, "the torn image was written to");
    unlink(path);
}

// All three at once: $MFTMirr apart, an orphan, a folder entry for a free record and an
// unmarked record. One check --repair handles everything.
static void test_check_combined(void)
{
    char path[2048];
    ue_check_result_t r;
    check_seen_t seen;
    ue_probe_t p;

    CHECK(fixture_copy("chk-combined", path, sizeof(path)) == 0, "copy");
    CHECK(check_path(path, 0, &r, &seen) == 0 && r.problems == 1 && r.by_kind[UE_CHK_MFT_MIRROR] == 1,
          "combined, before: %llu problems", (unsigned long long)r.problems);
    int err = check_path(path, UE_CHECK_REPAIR, &r, &seen);
    CHECK(err == 0 && r.mirror_repaired && r.by_kind[UE_CHK_MFT_MIRROR] == 1 && r.by_kind[UE_CHK_ORPHAN] == 1 &&
          r.by_kind[UE_CHK_ENTRY_FREE_RECORD] == 1 && r.by_kind[UE_CHK_RECORD_UNMARKED] == 1 && r.problems == 4 &&
          r.repairable == 4 && r.entries_removed == 1 && r.orphans_relinked == 1 && r.orphans_found == 0 &&
          r.repaired >= 1 && r.remaining == 0,
          "combined repair: %s, %llu problems, %llu repairable, removed %llu, relinked %llu, remaining %llu",
          strerror(err), (unsigned long long)r.problems, (unsigned long long)r.repairable,
          (unsigned long long)r.entries_removed, (unsigned long long)r.orphans_relinked,
          (unsigned long long)r.remaining);
    CHECK(probe_file(path, &p) == 0 && p.state == UE_VOL_CLEAN, "after repair: %s", ue_vol_state_name(p.state));
    CHECK(check_path(path, 0, &r, &seen) == 0 && r.problems == 0, "%llu problems after repair",
          (unsigned long long)r.problems);
    CHECK(image_has_gen(path, "/d/c.bin", 20000, 3) == 0, "/d/c.bin after relink");
    unlink(path);
}

// D-102: space marked used that nothing uses (a free record, one past $MFT's end, four
// clusters) is freed by a repair, which runs for that alone; a damaged security descriptor
// is replaced. Windows' chkdsk reports both.
static void test_check_lost_security(void)
{
    char path[2048];
    ue_check_result_t r;
    check_seen_t seen;
    ue_probe_t p;

    CHECK(fixture_copy("chk-lost", path, sizeof(path)) == 0, "copy");
    CHECK(check_path(path, 0, &r, &seen) == 0 && r.problems == 0 && r.lost_records == 2 && r.lost_clusters == 4,
          "lost: %llu problems, lost %llu/%llu", (unsigned long long)r.problems, (unsigned long long)r.lost_records,
          (unsigned long long)r.lost_clusters);
    int err = check_path(path, UE_CHECK_REPAIR, &r, &seen);
    CHECK(err == 0 && r.repair_ran && r.records_freed == 2 && r.clusters_freed == 4 && r.remaining == 0,
          "lost repair: %s, freed %llu/%llu, remaining %llu", strerror(err), (unsigned long long)r.records_freed,
          (unsigned long long)r.clusters_freed, (unsigned long long)r.remaining);
    CHECK(check_path(path, 0, &r, &seen) == 0 && r.problems == 0 && r.lost_records == 0 && r.lost_clusters == 0,
          "after: lost %llu/%llu", (unsigned long long)r.lost_records, (unsigned long long)r.lost_clusters);
    CHECK(image_has_gen(path, "/d/a.bin", 20000, 1) == 0 && image_has_gen(path, "/d/c.bin", 20000, 3) == 0,
          "files after freeing");
    CHECK(probe_file(path, &p) == 0 && p.state == UE_VOL_CLEAN, "after repair: %s", ue_vol_state_name(p.state));
    unlink(path);

    CHECK(fixture_copy("chk-sd", path, sizeof(path)) == 0, "copy");
    CHECK(check_path(path, 0, &r, &seen) == 0 && r.problems == 1 && r.repairable == 1 &&
          r.by_kind[UE_CHK_BAD_SECURITY] == 1 && !strcmp(seen.first[UE_CHK_BAD_SECURITY].path, "/d/b.bin"),
          "sd: %llu problems, path %s", (unsigned long long)r.problems, seen.first[UE_CHK_BAD_SECURITY].path);
    err = check_path(path, UE_CHECK_REPAIR, &r, &seen);
    CHECK(err == 0 && r.security_replaced == 1 && r.remaining == 0, "sd repair: %s, replaced %llu, remaining %llu",
          strerror(err), (unsigned long long)r.security_replaced, (unsigned long long)r.remaining);
    CHECK(check_path(path, 0, &r, &seen) == 0 && r.problems == 0, "%llu problems after repair",
          (unsigned long long)r.problems);
    CHECK(image_has_gen(path, "/d/b.bin", 20000, 2) == 0, "/d/b.bin after repair");
    unlink(path);
}

// A copy cut off mid-way, as a kill or a pull leaves it (Tests/Fuse/interrupt.sh): the
// engine writes through its page cache to a device that stops taking writes after `limit`
// of them, at many cut points. The check must run on every result, must find damage on some,
// and its repair must leave a clean volume whenever it runs.
typedef struct {
    uint8_t *mem;
    uint64_t size;
    long writes, limit;               // limit < 0: no limit; after it every write fails (gone)
    bool deletes;                     // cut_tree: also remove a folder's files
} cutdev_t;

static int cut_pread(void *ctx, void *buf, size_t len, uint64_t off)
{
    cutdev_t *d = ctx;
    if (off + len > d->size)
        return EIO;
    memcpy(buf, d->mem + off, len);
    return 0;
}

static int cut_pwrite(void *ctx, const void *buf, size_t len, uint64_t off)
{
    cutdev_t *d = ctx;
    if (off + len > d->size)
        return EIO;
    if (d->limit >= 0 && d->writes >= d->limit)
        return ENXIO;
    d->writes++;
    memcpy(d->mem + off, buf, len);
    return 0;
}

// cp -R of a tree with a write-back every few files, as the daemon's 1 s timer does.
// Returns the writes that reached the device.
static long cut_copy(cutdev_t *d)
{
    ue_io_t io = { .ctx = d, .pread = cut_pread, .pwrite = cut_pwrite, .size_bytes = d->size, .block_size = 512,
                   .writable = true };
    ue_engine_t *e;
    ue_attr_t root, sub, a;

    d->writes = 0;
    if (ue_open(&io, UE_OPEN_CACHE | UE_OPEN_GHOST_OPEN_ONLY, &e, NULL))
        return -1;
    if (ue_getattr(e, UE_ROOT_INO, &root) == 0 && ue_create(e, root.ino, "copy", UE_T_DIR, 0755, &sub) == 0) {
        for (int i = 0; i < 4; i++) {
            char name[32];
            ue_attr_t dd;
            snprintf(name, sizeof(name), "dir%d", i);
            if (ue_create(e, sub.ino, name, UE_T_DIR, 0755, &dd))
                break;
            for (int j = 0; j < 120; j++) {
                snprintf(name, sizeof(name), "file-with-a-long-name-%03d.bin", j);
                if (ue_create(e, dd.ino, name, UE_T_REG, 0644, &a) ||
                    write_gen(e, a.ino, 0, 1000 + (uint64_t)((i * 120 + j) * 997) % 30000, (uint64_t)j))
                    break;
                if (j % 25 == 24)
                    ue_writeback(e, 0);
            }
        }
    }
    ue_close(e);
    return d->writes;
}

static void test_check_cut_copy(void)
{
    cutdev_t d = { 0 };
    int fd = open(img("small"), O_RDONLY);
    CHECK(fd >= 0, "open small");
    d.size = (uint64_t)lseek(fd, 0, SEEK_END);
    uint8_t *orig = malloc(d.size), *orig_cut = malloc(d.size);
    d.mem = malloc(d.size);
    CHECK(orig && orig_cut && d.mem && pread(fd, orig, d.size, 0) == (ssize_t)d.size, "read small");
    close(fd);

    memcpy(d.mem, orig, d.size);
    d.limit = -1;
    long total = cut_copy(&d);
    CHECK(total > 20, "the copy made %ld writes", total);
    int cuts = 0, damaged = 0, repaired = 0, mirror = 0;
    for (long n = 3; n < total; n += total / 23 + 1, cuts++) {
        memcpy(d.mem, orig, d.size);
        d.limit = n;
        cut_copy(&d);
        d.limit = -1;
        ue_io_t io = { .ctx = &d, .pread = cut_pread, .pwrite = cut_pwrite, .size_bytes = d.size, .block_size = 512,
                       .writable = true };
        ue_check_result_t r, r2;
        int err = ue_check(&io, 0, NULL, NULL, &r);
        if (err) {                    // kept for a look: unlatch-engine probe/check
            char path[2048];
            snprintf(path, sizeof(path), "%s/x-cut-%ld.img", dir, n);
            int out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (out >= 0 && write(out, d.mem, d.size) != (ssize_t)d.size)
                fprintf(stderr, "  cannot save %s\n", path);
            if (out >= 0)
                close(out);
        }
        CHECK(err == 0, "cut at %ld of %ld: check failed: %s", n, total, strerror(err));
        if (r.state == UE_VOL_CORRUPT) {   // cut between $MFT and $MFTMirr: chkdsk's to repair
            CHECK(r.by_kind[UE_CHK_MFT_MIRROR] == 1 && r.problems == 1, "cut at %ld: corrupt, not the mirror", n);
            mirror++;
        } else
            CHECK(r.state == UE_VOL_DIRTY, "cut at %ld: %s, not dirty", n, ue_vol_state_name(r.state));
        if (r.problems)
            damaged++;
        if (r.problems && r.problems == r.repairable) {
            memcpy(orig_cut, d.mem, d.size);
            if (ue_check(&io, UE_CHECK_REPAIR, NULL, NULL, &r2) || r2.remaining) {   // kept for a look
                char path[2048];
                snprintf(path, sizeof(path), "%s/x-cut-repair-%ld.img", dir, n);
                int out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (out >= 0 && write(out, orig_cut, d.size) != (ssize_t)d.size)
                    fprintf(stderr, "  cannot save %s\n", path);
                if (out >= 0)
                    close(out);
            }
            memcpy(d.mem, orig_cut, d.size);
            CHECK(ue_check(&io, UE_CHECK_REPAIR, NULL, NULL, &r) == 0 && r.remaining == 0 && r.dirty_cleared,
                  "cut at %ld: repair left %llu", n, (unsigned long long)r.remaining);
            CHECK(ue_check(&io, 0, NULL, NULL, &r2) == 0 && r2.problems == 0 && r2.state == UE_VOL_CLEAN,
                  "cut at %ld: %llu problems after repair", n, (unsigned long long)r2.problems);
            repaired++;
        }
    }
    printf("     %d cuts of %ld writes: %d damaged (%d $MFTMirr), %d repaired by Check Disk\n", cuts, total,
           damaged, mirror, repaired);
    CHECK(damaged > 0, "no cut left damage the check found");
    free(orig_cut);
    free(orig);
    free(d.mem);
}

// Ordered write-back (D-97): a create-heavy workload (nested folders, small and large files,
// a write-back every few creates as the 1 s timer does, then a folder emptied) cut off after
// every other device write. Whatever the cut, no folder entry may point at a record that is
// free or reused, no index may be unreadable and no record torn: what is pointed to lands
// before what points at it. New records may be orphans, space may be lost, and a delete may
// leave bits cleared in a bitmap before its record is freed (repairable).
static long cut_tree(cutdev_t *d)
{
    ue_io_t io = { .ctx = d, .pread = cut_pread, .pwrite = cut_pwrite, .size_bytes = d->size, .block_size = 512,
                   .writable = true };
    ue_engine_t *e;
    ue_attr_t root, top, sub, a;
    int made = 0;

    d->writes = 0;
    if (ue_open(&io, UE_OPEN_CACHE | UE_OPEN_GHOST_OPEN_ONLY, &e, NULL))
        return -1;
    if (ue_getattr(e, UE_ROOT_INO, &root))
        goto out;
    for (int t = 0; t < 3; t++) {
        char name[48];
        snprintf(name, sizeof(name), "top-%d", t);
        if (ue_create(e, root.ino, name, UE_T_DIR, 0755, &top))
            goto out;
        for (int s = 0; s < 4; s++) {
            snprintf(name, sizeof(name), "sub-%d", s);
            if (ue_create(e, top.ino, name, UE_T_DIR, 0755, &sub))
                goto out;
            for (int f = 0; f < 30; f++) {
                snprintf(name, sizeof(name), "a-file-with-a-longer-name-%02d.txt", f);
                if (ue_create(e, sub.ino, name, UE_T_REG, 0644, &a) ||
                    write_gen(e, a.ino, 0, f % 10 == 9 ? 20000 : 200, (uint64_t)(t * 1000 + s * 100 + f)))
                    goto out;
                if (++made % 7 == 0)
                    ue_writeback(e, 0);
            }
        }
    }
    // Then deletes: a written folder emptied, a few files at a time (rm -r).
    ue_writeback(e, 0);
    if (d->deletes && ue_lookup(e, root.ino, "top-0", &top) == 0 && ue_lookup(e, top.ino, "sub-1", &sub) == 0) {
        for (int f = 0; f < 30; f++) {
            char name[48];
            snprintf(name, sizeof(name), "a-file-with-a-longer-name-%02d.txt", f);
            if (ue_remove(e, sub.ino, name, false))
                goto out;
            if (f % 7 == 6)
                ue_writeback(e, 0);
        }
        ue_remove(e, top.ino, "sub-1", true);
    }
out:
    ue_close(e);
    return d->writes;
}

static void test_cut_order(void)
{
    cutdev_t d = { .deletes = true };
    int fd = open(img("small"), O_RDONLY);
    CHECK(fd >= 0, "open small");
    d.size = (uint64_t)lseek(fd, 0, SEEK_END);
    uint8_t *orig = malloc(d.size);
    d.mem = malloc(d.size);
    CHECK(orig && d.mem && pread(fd, orig, d.size, 0) == (ssize_t)d.size, "read small");
    close(fd);

    memcpy(d.mem, orig, d.size);
    d.limit = -1;
    long total = cut_tree(&d);
    CHECK(total > 20, "the workload made %ld writes", total);
    static const ue_check_kind_t bad[] = { UE_CHK_ENTRY_FREE_RECORD, UE_CHK_ENTRY_WRONG_SEQ, UE_CHK_INDEX_UNREADABLE,
                                           UE_CHK_BAD_RECORD, UE_CHK_BAD_RUNLIST, UE_CHK_CLUSTERS_SHARED };
    uint64_t kinds[UE_CHK_KINDS] = { 0 };
    int cuts = 0, mirror = 0, broken = 0, repaired = 0;
    uint64_t relinked = 0, to_found = 0, freed_rec = 0, freed_cl = 0;
    uint8_t *pre = malloc(d.size);    // the cut image, before its repair
    CHECK(pre, "alloc");
    long step = total / 400 + 1;
    for (long n = 1; n < total; n += step, cuts++) {
        memcpy(d.mem, orig, d.size);
        d.limit = n;
        cut_tree(&d);
        d.limit = -1;
        ue_io_t io = { .ctx = &d, .pread = cut_pread, .pwrite = cut_pwrite, .size_bytes = d.size, .block_size = 512,
                       .writable = true };
        ue_check_result_t r, r2;
        int err = ue_check(&io, 0, NULL, NULL, &r);
        CHECK(err == 0, "cut at %ld of %ld: check failed: %s", n, total, strerror(err));
        if (r.state == UE_VOL_CORRUPT) {
            CHECK(r.by_kind[UE_CHK_MFT_MIRROR] == 1 && r.problems == 1, "cut at %ld: corrupt, not the mirror", n);
            mirror++;
        }
        // Every cut is repaired by Check Disk itself ($MFTMirr D-98, orphans D-99); the repair's
        // result counts everything, the rest of a corrupt volume included.
        memcpy(pre, d.mem, d.size);
        err = ue_check(&io, UE_CHECK_REPAIR, NULL, NULL, &r);
        CHECK(err == 0 && r.remaining == 0, "cut at %ld: repair: %s, %llu left", n, strerror(err),
              (unsigned long long)r.remaining);
        // Nothing left that chkdsk would fix either: no space marked used by nothing (D-102).
        CHECK(ue_check(&io, 0, NULL, NULL, &r2) == 0 && r2.problems == 0 && r2.state == UE_VOL_CLEAN &&
                  r2.lost_records == 0 && r2.lost_clusters == 0,
              "cut at %ld: %llu problems after repair, %s, lost %llu/%llu", n, (unsigned long long)r2.problems,
              ue_vol_state_name(r2.state), (unsigned long long)r2.lost_records, (unsigned long long)r2.lost_clusters);
        freed_rec += r.records_freed;
        freed_cl += r.clusters_freed;
        relinked += r.orphans_relinked;
        to_found += r.orphans_found;
        repaired += r.repair_ran;
        bool worse = false;
        for (int k = 1; k < UE_CHK_KINDS; k++)
            kinds[k] += r.by_kind[k];
        for (size_t k = 0; k < sizeof(bad) / sizeof(bad[0]); k++)
            worse |= r.by_kind[bad[k]] != 0;
        if (worse && broken++ == 0) {   // kept for a look: unlatch-engine check
            char path[2048];
            snprintf(path, sizeof(path), "%s/x-order-%ld.img", dir, n);
            int out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (out >= 0 && write(out, pre, d.size) != (ssize_t)d.size)
                fprintf(stderr, "  cannot save %s\n", path);
            if (out >= 0)
                close(out);
        }
    }
    printf("     %d cuts repaired by Check Disk (%d ran a repair): %llu orphans put back, %llu of them in "
           "found.000; %llu records and %llu clusters marked used by nothing freed; %llu bad security\n", cuts,
           repaired, (unsigned long long)relinked, (unsigned long long)to_found, (unsigned long long)freed_rec,
           (unsigned long long)freed_cl, (unsigned long long)kinds[UE_CHK_BAD_SECURITY]);
    printf("     %d cuts of %ld writes: %d with entries or indexes broken, %d $MFTMirr; found: "
           "%llu free-record entries, %llu wrong-seq entries, %llu unreadable indexes, %llu bad records, "
           "%llu orphans, %llu unmarked records, %llu unmarked clusters\n",
           cuts, total, broken, mirror, (unsigned long long)kinds[UE_CHK_ENTRY_FREE_RECORD],
           (unsigned long long)kinds[UE_CHK_ENTRY_WRONG_SEQ], (unsigned long long)kinds[UE_CHK_INDEX_UNREADABLE],
           (unsigned long long)kinds[UE_CHK_BAD_RECORD], (unsigned long long)kinds[UE_CHK_ORPHAN],
           (unsigned long long)kinds[UE_CHK_RECORD_UNMARKED], (unsigned long long)kinds[UE_CHK_CLUSTERS_UNMARKED]);
    CHECK(broken == 0, "%d of %d cuts left an entry or index broken", broken, cuts);
    free(pre);
    free(orig);
    free(d.mem);
}

// D-95: the mark is on the disk only while the volume is being changed. Opening writes
// nothing; the first change sets it; ue_settle clears it once the volume has been idle,
// leaving a complete image; the next change sets it again.
static void test_settle(void)
{
    wvol_t w;
    ue_attr_t a;
    ue_probe_t p;
    uint32_t saved = wflags;

    wflags = UE_OPEN_CACHE;
    int err = wopen(&w, "clean", "settle");
    wflags = saved;
    CHECK(err == 0, "open: %s", strerror(err));
    WCHECK(!ue_is_marked_dirty(w.e) && probe_file(w.path, &p) == 0 && p.state == UE_VOL_CLEAN,
           "marked before any change: %s", ue_vol_state_name(p.state));
    WCHECK(ue_settle(w.e, 0) == 0, "settle with nothing to do");
    ue_ino_t root = root_of(w.e);
    for (int round = 0; round < 3; round++) {
        char name[32];
        snprintf(name, sizeof(name), "s%d", round);
        WCHECK(ue_create(w.e, root, name, UE_T_REG, 0644, &a) == 0, "create %s", name);
        WCHECK(write_gen(w.e, a.ino, 0, 50000, (uint64_t)round) == 0, "write %s", name);
        ue_release(w.e, a.ino);
        WCHECK(ue_is_marked_dirty(w.e) && probe_file(w.path, &p) == 0 && p.state == UE_VOL_DIRTY,
               "round %d: not marked while changing: %s", round, ue_vol_state_name(p.state));
        WCHECK(ue_settle(w.e, 60000) == EAGAIN, "settled before idle");
        WCHECK(ue_settle(w.e, 0) == 0, "settle");
        WCHECK(!ue_is_marked_dirty(w.e) && probe_file(w.path, &p) == 0 && p.state == UE_VOL_CLEAN,
               "round %d: still marked after settle: %s", round, ue_vol_state_name(p.state));
        // As if unplugged now: the image alone has every file, and checks clean.
        ue_check_result_t r;
        check_seen_t seen;
        WCHECK(check_path(w.path, 0, &r, &seen) == 0 && r.problems == 0, "round %d: %llu problems after settle", round,
               (unsigned long long)r.problems);
        file_io_t f2;
        ue_engine_t *e2;
        WCHECK(file_io_open(&f2, w.path, false) == 0 && ue_open(&f2.io, UE_OPEN_RDONLY, &e2, NULL) == 0, "second reader");
        for (int i = 0; i <= round && !err; i++) {
            snprintf(name, sizeof(name), "s%d", i);
            err = ue_lookup(e2, root_of(e2), name, &a);
            if (!err)
                err = check_gen(e2, a.ino, 0, 50000, (uint64_t)i);
        }
        ue_close(e2);
        file_io_close(&f2);
        WCHECK(err == 0, "round %d: second reader: %s", round, strerror(err));
    }
    // A volume dirty at open keeps its mark: settle leaves it, unless UE_OPEN_CLEAR_DIRTY.
    CHECK(wclose(&w) == 0, "close");
    err = wopen(&w, "dirty", "settle-dirty");
    CHECK(err == 0, "open dirty: %s", strerror(err));
    WCHECK(ue_settle(w.e, 0) == 0 && ue_is_marked_dirty(w.e), "a dirty volume lost its mark");
    ue_close(w.e);
    file_io_close(&w.f);
    unlink(w.path);
    wflags = UE_OPEN_CLEAR_DIRTY;
    err = wopen(&w, "dirty", "settle-clear");
    wflags = saved;
    CHECK(err == 0, "open with CLEAR_DIRTY: %s", strerror(err));
    WCHECK(ue_settle(w.e, 0) == 0 && !ue_is_marked_dirty(w.e) && probe_file(w.path, &p) == 0 &&
           p.state == UE_VOL_CLEAN, "CLEAR_DIRTY: not cleared by settle: %s", ue_vol_state_name(p.state));
    CHECK(wclose(&w) == 0, "close");
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "readloop") == 0) {
        int err = read_loop(argv[2], 50);
        if (err)
            fprintf(stderr, "readloop: %s\n", strerror(err));
        return err ? 1 : 0;
    }
    if (argc == 3 && strcmp(argv[1], "writeloop") == 0) {
        snprintf(dir, sizeof(dir), "%s", argv[2]);
        int err = write_loop("clean", 15);
        wflags = UE_OPEN_CACHE;
        if (!err)
            err = write_loop("clean", 15);
        if (err)
            fprintf(stderr, "writeloop: %s\n", strerror(err));
        return err ? 1 : 0;
    }
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

    if (file_io_open(&rd_file, img("read"), false) || ue_open(&rd_file.io, UE_OPEN_RDONLY, &rd, NULL)) {
        printf("FAIL cannot open the read fixture\n");
        return 1;
    }
    RUN(test_lookup_nested);
    RUN(test_case_and_root);
    RUN(test_unicode);
    RUN(test_big_dir);
    RUN(test_sparse);
    RUN(test_uninit);
    RUN(test_resident_and_flags);
    RUN(test_read_errors);
    RUN(test_release);
    RUN(test_statfs);
    if (ue_close(rd)) {
        printf("FAIL close of the read fixture\n");
        failures++;
    }
    file_io_close(&rd_file);

    RUN(test_check_clean);
    RUN(test_check_kinds);
    RUN(test_check_repair);
    RUN(test_check_orphans);
    RUN(test_check_mirror);
    RUN(test_check_combined);
    RUN(test_check_lost_security);
    RUN(test_check_cut_copy);
    RUN(test_cut_order);
    RUN(test_cache_model);
    RUN(test_cache_engine);
    RUN(test_dirty_mark);
    RUN(test_settle);
    for (int pass = 0; pass < 2; pass++) {
        // Second pass: the same write tests with the page cache on (D-62).
        wflags = pass ? UE_OPEN_CACHE : 0;
        wtag = pass ? "c-" : "";
        if (pass)
            printf("-- write tests again, with UE_OPEN_CACHE\n");
        RUN(test_write_roundtrip);
        RUN(test_create_rules);
        RUN(test_readonly_engine);
        RUN(test_remove);
        RUN(test_ghost_open_only);
        RUN(test_rename_matrix);
        RUN(test_setattr);
        RUN(test_verifier);
        RUN(test_enospc);
        RUN(test_xattr);
        RUN(test_links);
        RUN(test_native_links);
        RUN(test_windows_links);
        RUN(test_damaged_entry);
        RUN(test_device_failure);
        RUN(test_crash_leftovers);
    }

    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
