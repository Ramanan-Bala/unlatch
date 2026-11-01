// SPDX-License-Identifier: GPL-3.0-or-later
// fsx: randomized model test for the engine's write path (Step 06).
//
// Random operations run against an image and an in-memory model of the expected tree and
// file contents. The model predicts the exact result (0 or errno) of every operation. After
// every op the whole directory structure is compared (names, types, sizes, inode numbers)
// and the file the op touched is read back in full; after every remount, and at the end,
// every file's contents are compared. The first mismatch prints the seed and op and fails.
#include "fsx.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "file_io.h"
#include "unlatch_engine.h"

#define MAX_NODES 96
#define MAX_FILE (2u << 20)           // keeps 96 files inside a 512 MB image
#define MAX_IO (256u << 10)

typedef struct node {
    char name[64];
    bool dir;
    ue_ino_t ino;
    struct node *parent;
    struct node **kids;
    int nkids, capk;
    unsigned char *data;
    uint64_t size;
} node_t;

typedef struct {
    const char *img;
    uint64_t seed, rng;
    unsigned long ops, op;
    bool verbose;
    file_io_t f;
    ue_engine_t *e;
    node_t *root;
    int nodes;
    char desc[512];
    unsigned char *buf;               // MAX_FILE scratch
    unsigned long remounts, ghosts_made, errors_expected;
} fsx_t;

// Name pool: ASCII with case variants, Latin-1 letters with case pairs, CJK, long names.
static const char *pool[] = {
    "a", "A", "b", "B", "file", "File", "FILE", "dir", "Dir", "x.txt", "X.TXT", "data.bin",
    "caf\xC3\xA9", "CAF\xC3\x89", "\xC3\xBC-\xC3\xB1", "\xC3\x9C-\xC3\x91", "\xE6\x97\xA5\xE6\x9C\xAC",
    "long_name_with_some_length_0123456789", "LONG_NAME_WITH_SOME_LENGTH_0123456789", "z", "q.dat",
};
#define NPOOL (sizeof(pool) / sizeof(pool[0]))

static uint64_t rnd(fsx_t *x)
{
    x->rng ^= x->rng >> 12;
    x->rng ^= x->rng << 25;
    x->rng ^= x->rng >> 27;
    return x->rng * 0x2545F4914F6CDD1Dull;
}

static uint64_t below(fsx_t *x, uint64_t n)
{
    return n ? rnd(x) % n : 0;
}

static __attribute__((format(printf, 2, 3))) int fail(fsx_t *x, const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "fsx: FAIL seed %" PRIu64 " op %lu (%s): ", x->seed, x->op, x->desc);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\nfsx: rerun: unlatch-engine fsx <fresh-img> --seed %" PRIu64 " --ops %lu --verbose\n",
            x->seed, x->ops);
    return 1;
}

// Case folding as NTFS $UpCase does it for the names in the pool: ASCII and Latin-1.
static void fold(const char *in, char *out, size_t cap)
{
    size_t o = 0;

    for (const unsigned char *p = (const unsigned char *)in; *p && o + 1 < cap; p++) {
        unsigned char c = *p;
        if (c >= 'a' && c <= 'z')
            c -= 32;
        else if (c == 0xC3 && p[1] >= 0xA0 && p[1] <= 0xBE && p[1] != 0xB7) {
            out[o++] = (char)c;
            c = p[1] - 0x20;           // à-þ -> À-Þ (skipping ÷)
            p++;
        }
        out[o++] = (char)c;
    }
    out[o] = '\0';
}

static bool same_name(const char *a, const char *b)
{
    char fa[128], fb[128];

    fold(a, fa, sizeof(fa));
    fold(b, fb, sizeof(fb));
    return strcmp(fa, fb) == 0;
}

static node_t *find_kid(node_t *d, const char *name)
{
    for (int i = 0; i < d->nkids; i++)
        if (same_name(d->kids[i]->name, name))
            return d->kids[i];
    return NULL;
}

static void add_kid(node_t *d, node_t *n)
{
    if (d->nkids == d->capk) {
        d->capk = d->capk ? d->capk * 2 : 8;
        d->kids = realloc(d->kids, (size_t)d->capk * sizeof(*d->kids));
    }
    d->kids[d->nkids++] = n;
    n->parent = d;
}

static void drop_kid(node_t *d, node_t *n)
{
    for (int i = 0; i < d->nkids; i++)
        if (d->kids[i] == n) {
            d->kids[i] = d->kids[--d->nkids];
            return;
        }
}

static void free_node(fsx_t *x, node_t *n)
{
    for (int i = 0; i < n->nkids; i++)
        free_node(x, n->kids[i]);
    free(n->kids);
    free(n->data);
    free(n);
    x->nodes--;
}

// Collects every node (dirs_only: directories) into out; returns the count.
static int collect(node_t *n, node_t **out, int cap, int count, bool dirs_only, bool files_only)
{
    if (count < cap && (!dirs_only || n->dir) && (!files_only || !n->dir))
        out[count++] = n;
    for (int i = 0; i < n->nkids; i++)
        count = collect(n->kids[i], out, cap, count, dirs_only, files_only);
    return count;
}

static node_t *pick(fsx_t *x, bool dirs_only, bool files_only, bool allow_root)
{
    node_t *all[MAX_NODES + 1];
    int n = collect(x->root, all, MAX_NODES + 1, 0, dirs_only, files_only);
    int start = allow_root ? 0 : (all[0] == x->root ? 1 : 0);

    if (n <= start)
        return NULL;
    return all[start + (int)below(x, (uint64_t)(n - start))];
}

static bool inside(const node_t *n, const node_t *anc)
{
    for (; n; n = n->parent)
        if (n == anc)
            return true;
    return false;
}

static void path_of(const node_t *n, char *out, size_t cap)
{
    if (!n->parent) {
        snprintf(out, cap, "/");
        return;
    }
    char parent[1024];
    path_of(n->parent, parent, sizeof(parent));
    snprintf(out, cap, "%s%s%s", parent, strcmp(parent, "/") ? "/" : "", n->name);
}

static void fill(uint64_t seed, unsigned char *p, size_t len)
{
    uint64_t s = seed * 0x9E3779B97F4A7C15ull + 1;

    for (size_t i = 0; i < len; i++) {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        p[i] = (unsigned char)((s * 0x2545F4914F6CDD1Dull) >> 56);
    }
}

// ---- comparison --------------------------------------------------------------------

typedef struct {
    fsx_t *x;
    node_t *d;
    int seen;
    int err;
    char what[256];
} cmp_t;

static bool cmp_entry(void *ctx, const char *name, const ue_attr_t *a, uint64_t next)
{
    cmp_t *c = ctx;
    node_t *n = NULL;

    (void)next;
    for (int i = 0; i < c->d->nkids; i++)
        if (!strcmp(c->d->kids[i]->name, name))
            n = c->d->kids[i];
    if (!n) {
        snprintf(c->what, sizeof(c->what), "unexpected entry '%s'", name);
        c->err = 1;
        return false;
    }
    if (n->ino != a->ino || n->dir != (a->type == UE_T_DIR) || (!n->dir && n->size != a->size) ||
        a->nlink != 1) {
        snprintf(c->what, sizeof(c->what),
                 "'%s': model ino %" PRIx64 " dir %d size %" PRIu64 ", engine ino %" PRIx64
                 " type %u size %" PRIu64 " nlink %u",
                 name, n->ino, n->dir, n->size, a->ino, a->type, a->size, a->nlink);
        c->err = 1;
        return false;
    }
    c->seen++;
    return true;
}

static int compare_tree(fsx_t *x, node_t *d)
{
    cmp_t c = { x, d, 0, 0, "" };
    char path[1024];
    int err = ue_readdir(x->e, d->ino, 0, cmp_entry, &c, NULL);

    path_of(d, path, sizeof(path));
    if (err)
        return fail(x, "readdir %s: %s", path, strerror(err));
    if (c.err)
        return fail(x, "in %s: %s", path, c.what);
    if (c.seen != d->nkids)
        return fail(x, "%s lists %d entries, model has %d", path, c.seen, d->nkids);
    for (int i = 0; i < d->nkids; i++)
        if (d->kids[i]->dir && compare_tree(x, d->kids[i]))
            return 1;
    return 0;
}

static int compare_file(fsx_t *x, node_t *f)
{
    uint64_t off = 0;
    char path[1024];

    path_of(f, path, sizeof(path));
    while (off < f->size) {
        size_t done;
        size_t want = (size_t)(f->size - off < MAX_IO ? f->size - off : MAX_IO);
        int err = ue_read(x->e, f->ino, off, x->buf, want, &done);
        if (err)
            return fail(x, "read %s at %" PRIu64 ": %s", path, off, strerror(err));
        if (done != want)
            return fail(x, "read %s at %" PRIu64 ": %zu of %zu bytes", path, off, done, want);
        if (memcmp(x->buf, f->data + off, want)) {
            size_t i = 0;
            while (x->buf[i] == f->data[off + i])
                i++;
            return fail(x, "%s differs at byte %" PRIu64 " (engine %02x, model %02x)", path, off + i,
                        x->buf[i], f->data[off + i]);
        }
        off += want;
    }
    size_t done;
    if (ue_read(x->e, f->ino, f->size, x->buf, 16, &done) || done)
        return fail(x, "%s reads past its size %" PRIu64, path, f->size);
    return 0;
}

static int compare_all(fsx_t *x, node_t *n)
{
    if (!n->dir)
        return compare_file(x, n);
    for (int i = 0; i < n->nkids; i++)
        if (compare_all(x, n->kids[i]))
            return 1;
    return 0;
}

// ---- operations ----------------------------------------------------------------------

static int expect(fsx_t *x, int got, int want)
{
    if (got != want)
        return fail(x, "engine returned %s, model expected %s", got ? strerror(got) : "success",
                    want ? strerror(want) : "success");
    if (want)
        x->errors_expected++;
    return 0;
}

static int op_create(fsx_t *x, node_t **touched)
{
    node_t *d = pick(x, true, false, true);
    const char *name = pool[below(x, NPOOL)];
    bool dir = below(x, 4) == 0;
    node_t *exists = find_kid(d, name);
    ue_attr_t a;
    char path[1024];
    int err;

    path_of(d, path, sizeof(path));
    snprintf(x->desc, sizeof(x->desc), "create %s %s in %s", dir ? "dir" : "file", name, path);
    if (!exists && x->nodes >= MAX_NODES)
        return 0;
    err = ue_create(x->e, d->ino, name, dir ? UE_T_DIR : UE_T_REG, 0644, &a);
    if (expect(x, err, exists ? EEXIST : 0))
        return 1;
    if (err)
        return 0;
    node_t *n = calloc(1, sizeof(*n));
    snprintf(n->name, sizeof(n->name), "%s", name);
    n->dir = dir;
    n->ino = a.ino;
    add_kid(d, n);
    x->nodes++;
    *touched = n;
    return 0;
}

static int op_write(fsx_t *x, node_t **touched)
{
    node_t *f = pick(x, false, true, false);
    uint64_t off, len, end;
    size_t done;
    char path[1024];
    int err;

    if (!f)
        return 0;
    off = below(x, f->size + MAX_IO < MAX_FILE ? f->size + MAX_IO : MAX_FILE);
    len = 1 + below(x, MAX_IO);
    if (below(x, 3) == 0)
        len = 1 + below(x, 600);        // small writes stay resident or cross the boundary
    if (off + len > MAX_FILE)
        len = MAX_FILE - off;
    if (len == 0)
        return 0;
    end = off + len;
    path_of(f, path, sizeof(path));
    snprintf(x->desc, sizeof(x->desc), "write %s off %" PRIu64 " len %" PRIu64, path, off, len);
    fill(x->seed ^ (x->op << 20), x->buf, (size_t)len);
    err = ue_write(x->e, f->ino, off, x->buf, (size_t)len, &done);
    if (expect(x, err, 0))
        return 1;
    if (done != len)
        return fail(x, "short write %zu of %" PRIu64, done, len);
    if (end > f->size) {
        f->data = realloc(f->data, (size_t)end);
        memset(f->data + f->size, 0, (size_t)(end - f->size));
        f->size = end;
    }
    memcpy(f->data + off, x->buf, (size_t)len);
    *touched = f;
    return 0;
}

static int op_truncate(fsx_t *x, node_t **touched)
{
    node_t *f = pick(x, false, true, false);
    ue_attr_t in = { 0 }, out;
    char path[1024];
    uint64_t size;
    int err;

    if (!f)
        return 0;
    size = below(x, 2) ? below(x, f->size + 1) : below(x, MAX_FILE + 1);
    path_of(f, path, sizeof(path));
    snprintf(x->desc, sizeof(x->desc), "truncate %s %" PRIu64 " -> %" PRIu64, path, f->size, size);
    in.size = size;
    err = ue_setattr(x->e, f->ino, UE_SET_SIZE, &in, &out);
    if (expect(x, err, 0))
        return 1;
    if (out.size != size)
        return fail(x, "size after truncate %" PRIu64, out.size);
    if (size > f->size) {
        f->data = realloc(f->data, (size_t)size);
        memset(f->data + f->size, 0, (size_t)(size - f->size));
    }
    f->size = size;
    *touched = f;
    return 0;
}

static int op_remove(fsx_t *x)
{
    node_t *n = pick(x, false, false, false);
    bool want_dir;
    char path[1024], name[64];
    int want, err;

    if (!n)
        return 0;
    want_dir = below(x, 10) == 0 ? !n->dir : n->dir;
    // Refer to it by a case variant sometimes: lookups are case-insensitive.
    snprintf(name, sizeof(name), "%s", n->name);
    if (below(x, 4) == 0)
        for (char *p = name; *p; p++)
            if (*p >= 'a' && *p <= 'z')
                *p -= 32;
    path_of(n, path, sizeof(path));
    snprintf(x->desc, sizeof(x->desc), "remove %s as %s (%s)", path, want_dir ? "dir" : "file", name);
    if (want_dir != n->dir)
        want = want_dir ? ENOTDIR : EISDIR;
    else if (n->dir && n->nkids)
        want = ENOTEMPTY;
    else
        want = 0;
    err = ue_remove(x->e, n->parent->ino, name, want_dir);
    if (expect(x, err, want))
        return 1;
    if (!err) {
        drop_kid(n->parent, n);
        free_node(x, n);
    }
    return 0;
}

static int op_rename(fsx_t *x, node_t **touched)
{
    node_t *src = pick(x, false, false, false);
    node_t *dst = pick(x, true, false, true), *tgt;
    const char *name;
    char spath[1024], dpath[1024], case_name[64];
    uint32_t flags = below(x, 10) == 0 ? UE_RENAME_EXCL : 0;
    int want, err;

    if (!src)
        return 0;
    switch (below(x, 4)) {
    case 0:                             // same name, maybe another case
        snprintf(case_name, sizeof(case_name), "%s", src->name);
        for (char *p = case_name; *p; p++)
            if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))
                *p ^= 0x20;
        name = case_name;
        dst = below(x, 2) ? src->parent : dst;
        break;
    default:
        name = pool[below(x, NPOOL)];
    }
    path_of(src, spath, sizeof(spath));
    path_of(dst, dpath, sizeof(dpath));
    snprintf(x->desc, sizeof(x->desc), "rename %s -> %s/%s%s", spath, dpath, name, flags ? " EXCL" : "");
    tgt = find_kid(dst, name);
    if (src->dir && inside(dst, src))
        want = EINVAL;
    else if (!tgt)
        want = 0;
    else if (tgt == src)
        want = 0;
    else if (flags & UE_RENAME_EXCL)
        want = EEXIST;
    else if (src->dir && !tgt->dir)
        want = ENOTDIR;
    else if (!src->dir && tgt->dir)
        want = EISDIR;
    else if (tgt->dir && tgt->nkids)
        want = ENOTEMPTY;
    else
        want = 0;
    err = ue_rename(x->e, src->parent->ino, src->name, dst->ino, name, flags);
    if (expect(x, err, want))
        return 1;
    if (err)
        return 0;
    if (tgt && tgt != src) {
        drop_kid(dst, tgt);
        free_node(x, tgt);
    }
    if (tgt == src && dst == src->parent && !strcmp(src->name, name))
        return 0;                       // rename to itself
    drop_kid(src->parent, src);
    snprintf(src->name, sizeof(src->name), "%s", name);
    add_kid(dst, src);
    *touched = src;
    return 0;
}

static int remount(fsx_t *x)
{
    int err = ue_close(x->e);

    file_io_close(&x->f);
    if (err)
        return fail(x, "close: %s", strerror(err));
    if ((err = file_io_open(&x->f, x->img, true)) || (err = ue_open(&x->f.io, 0, &x->e, NULL)))
        return fail(x, "reopen: %s", strerror(err));
    x->remounts++;
    return 0;
}

static int run(fsx_t *x)
{
    for (x->op = 1; x->op <= x->ops; x->op++) {
        node_t *touched = NULL;
        uint64_t r = below(x, 100);
        int rc;

        if (r < 18)
            rc = op_create(x, &touched);
        else if (r < 50)
            rc = op_write(x, &touched);
        else if (r < 60)
            rc = op_truncate(x, &touched);
        else if (r < 75)
            rc = op_rename(x, &touched);
        else if (r < 87)
            rc = op_remove(x);
        else if (r < 95) {
            // Host lets go of a random item (FSKit reclaim): releases ghosts too.
            node_t *n = pick(x, false, false, false);
            snprintf(x->desc, sizeof(x->desc), "release");
            if (n)
                ue_release(x->e, n->ino);
            rc = 0;
        } else if (r < 98) {
            snprintf(x->desc, sizeof(x->desc), "sync");
            rc = expect(x, ue_sync(x->e), 0);
        } else {
            snprintf(x->desc, sizeof(x->desc), "remount");
            rc = remount(x) || compare_all(x, x->root);
        }
        if (x->verbose)
            fprintf(stderr, "%5lu %s\n", x->op, x->desc);
        if (rc)
            return 1;
        if (compare_tree(x, x->root) || (touched && !touched->dir && compare_file(x, touched)))
            return 1;
    }
    return 0;
}

int fsx_main(const char *img, uint64_t seed, unsigned long ops, bool verbose)
{
    fsx_t x = { .img = img, .seed = seed, .rng = seed * 0x9E3779B97F4A7C15ull | 1, .ops = ops,
                .verbose = verbose };
    ue_attr_t root;
    ue_probe_t p;
    int rc, err;

    snprintf(x.desc, sizeof(x.desc), "open");
    if ((err = file_io_open(&x.f, img, true)) || (err = ue_open(&x.f.io, 0, &x.e, NULL)))
        return fail(&x, "open %s: %s", img, strerror(err));
    if ((err = ue_getattr(x.e, UE_ROOT_INO, &root))) {
        ue_close(x.e);
        return fail(&x, "root: %s", strerror(err));
    }
    x.root = calloc(1, sizeof(*x.root));
    x.root->dir = true;
    x.root->ino = root.ino;
    x.buf = malloc(MAX_FILE);
    // The image must start empty: the model starts empty.
    rc = compare_tree(&x, x.root) || run(&x);
    if (!rc) {
        snprintf(x.desc, sizeof(x.desc), "final compare");
        rc = compare_all(&x, x.root);
    }
    snprintf(x.desc, sizeof(x.desc), "close");
    err = ue_close(x.e);
    file_io_close(&x.f);
    if (!rc && err)
        rc = fail(&x, "close: %s", strerror(err));
    if (!rc && ((err = file_io_open(&x.f, img, false)) || (err = ue_probe(&x.f.io, &p))))
        rc = fail(&x, "probe: %s", strerror(err));
    else if (!rc) {
        file_io_close(&x.f);
        if (p.state != UE_VOL_CLEAN)
            rc = fail(&x, "volume is %s after the run: %s", ue_vol_state_name(p.state), p.reason);
    }
    if (!rc)
        printf("fsx: seed %" PRIu64 ": %lu ops ok (%d nodes, %lu remounts, %lu expected errors)\n", seed,
               ops, x.nodes, x.remounts, x.errors_expected);
    free_node(&x, x.root);
    free(x.buf);
    return rc;
}
