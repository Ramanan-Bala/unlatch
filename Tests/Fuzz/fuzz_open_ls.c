// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer target: read-only ue_open on a mutated image, then a bounded walk: readdir,
// lookup of every entry, getattr, reads at the start and end of files, statfs, close.
#include "mem_io.h"

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n);

typedef struct {
    ue_engine_t *e;
    ue_ino_t dir;
    int depth;
    int budget;                         // total entries visited, bounds the work per input
} walk_t;

static void walk_dir(walk_t *w, ue_ino_t dir, int depth);

static bool visit(void *ctx, const char *name, const ue_attr_t *a, uint64_t next)
{
    walk_t *w = ctx;
    ue_attr_t b;
    unsigned char buf[8192];
    size_t done;

    (void)next;
    if (--w->budget <= 0)
        return false;
    if (ue_lookup(w->e, w->dir, name, &b) != 0)
        return true;
    if (b.type == UE_T_DIR && w->depth < 3)
        walk_dir(w, b.ino, w->depth + 1);
    else if (b.type == UE_T_REG) {
        ue_read(w->e, b.ino, 0, buf, sizeof(buf), &done);
        if (a->size > sizeof(buf))
            ue_read(w->e, b.ino, a->size - 100, buf, 200, &done);
    }
    ue_release(w->e, b.ino);
    return true;
}

static void walk_dir(walk_t *w, ue_ino_t dir, int depth)
{
    walk_t sub = *w;
    uint64_t v;

    sub.dir = dir;
    sub.depth = depth;
    ue_readdir(w->e, dir, 0, visit, &sub, &v);
    w->budget = sub.budget;
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    mem_io_t m;
    ue_engine_t *e;
    ue_attr_t root;
    uint64_t total, free_bytes;
    uint32_t cluster;

    if (mem_from_input(&m, d, n, 0))
        return 0;
    if (ue_open(&m.io, UE_OPEN_RDONLY, &e, NULL) == 0) {
        walk_t w = { e, 0, 0, 200 };
        if (ue_getattr(e, UE_ROOT_INO, &root) == 0)
            walk_dir(&w, root.ino, 0);
        ue_statfs(e, &total, &free_bytes, &cluster);
        ue_close(e);
    }
    mem_free(&m);
    return 0;
}
