// SPDX-License-Identifier: GPL-3.0-or-later
// Open-inode cache: MFT record number -> ntfs_inode*. One ntfs_inode per record, ever,
// so libntfs-3g never sees the same record opened twice.
#include "internal.h"

#include <errno.h>
#include <stdlib.h>

#include <ntfs-3g/inode.h>
#include <ntfs-3g/layout.h>

#define ICACHE_MIN 64

static size_t slot_of(const ue_icache_t *c, uint64_t rec)
{
    uint64_t h = rec * 0x9E3779B97F4A7C15ull;
    return (size_t)(h >> 32) & (c->cap - 1);
}

int ue_icache_init(ue_icache_t *c)
{
    c->cap = ICACHE_MIN;
    c->count = 0;
    c->slots = calloc(c->cap, sizeof(*c->slots));
    return c->slots ? 0 : ENOMEM;
}

// Linear probing with backward-shift deletion; load factor <= 1/2.
static int grow(ue_icache_t *c)
{
    ue_icache_ent_t *old = c->slots;
    size_t old_cap = c->cap;

    c->slots = calloc(old_cap * 2, sizeof(*c->slots));
    if (!c->slots) {
        c->slots = old;
        return ENOMEM;
    }
    c->cap = old_cap * 2;
    for (size_t i = 0; i < old_cap; i++) {
        if (!old[i].ni)
            continue;
        size_t s = slot_of(c, old[i].rec);
        while (c->slots[s].ni)
            s = (s + 1) & (c->cap - 1);
        c->slots[s] = old[i];
    }
    free(old);
    return 0;
}

ntfs_inode *ue_icache_find(const ue_icache_t *c, uint64_t rec)
{
    for (size_t s = slot_of(c, rec);; s = (s + 1) & (c->cap - 1)) {
        if (!c->slots[s].ni)
            return NULL;
        if (c->slots[s].rec == rec)
            return c->slots[s].ni;
    }
}

int ue_icache_insert(ue_icache_t *c, uint64_t rec, ntfs_inode *ni)
{
    size_t s;
    int err;

    if ((c->count + 1) * 2 > c->cap && (err = grow(c)))
        return err;
    for (s = slot_of(c, rec); c->slots[s].ni; s = (s + 1) & (c->cap - 1))
        ;
    c->slots[s].rec = rec;
    c->slots[s].ni = ni;
    c->count++;
    return 0;
}

ntfs_inode *ue_icache_remove(ue_icache_t *c, uint64_t rec)
{
    size_t s, next, home;
    ntfs_inode *ni;

    for (s = slot_of(c, rec);; s = (s + 1) & (c->cap - 1)) {
        if (!c->slots[s].ni)
            return NULL;
        if (c->slots[s].rec == rec)
            break;
    }
    ni = c->slots[s].ni;
    // Backward-shift the cluster after s so lookups never hit a premature hole.
    for (next = (s + 1) & (c->cap - 1); c->slots[next].ni; next = (next + 1) & (c->cap - 1)) {
        home = slot_of(c, c->slots[next].rec);
        if (((next - home) & (c->cap - 1)) >= ((next - s) & (c->cap - 1))) {
            c->slots[s] = c->slots[next];
            s = next;
        }
    }
    c->slots[s].ni = NULL;
    c->slots[s].rec = 0;
    c->count--;
    return ni;
}

int ue_icache_close_all(ue_icache_t *c)
{
    int err = 0;

    for (size_t i = 0; i < c->cap; i++) {
        if (c->slots[i].ni && ntfs_inode_close(c->slots[i].ni) && !err)
            err = errno ? errno : EIO;
    }
    free(c->slots);
    c->slots = NULL;
    c->cap = c->count = 0;
    return err;
}
