// SPDX-License-Identifier: GPL-3.0-or-later
// Held-inode table: MFT record -> what the engine must remember while the host holds an item
// (from ue_lookup/ue_create until ue_release). The engine never keeps an ntfs_inode open
// between calls (D-24); this table only tracks identity and deferred deletes.
#include "internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define HELD_MIN 64

static size_t slot_of(const ue_held_map_t *m, uint64_t rec)
{
    uint64_t h = rec * 0x9E3779B97F4A7C15ull;
    return (size_t)(h >> 32) & (m->cap - 1);
}

int ue_held_init(ue_held_map_t *m)
{
    m->cap = HELD_MIN;
    m->count = 0;
    m->slots = calloc(m->cap, sizeof(*m->slots));
    return m->slots ? 0 : ENOMEM;
}

void ue_held_free(ue_held_map_t *m)
{
    free(m->slots);
    m->slots = NULL;
    m->cap = m->count = 0;
}

// Linear probing with backward-shift deletion; load factor <= 1/2.
static int grow(ue_held_map_t *m)
{
    ue_held_t *old = m->slots;
    size_t old_cap = m->cap;

    m->slots = calloc(old_cap * 2, sizeof(*m->slots));
    if (!m->slots) {
        m->slots = old;
        return ENOMEM;
    }
    m->cap = old_cap * 2;
    for (size_t i = 0; i < old_cap; i++) {
        if (!old[i].used)
            continue;
        size_t s = slot_of(m, old[i].rec);
        while (m->slots[s].used)
            s = (s + 1) & (m->cap - 1);
        m->slots[s] = old[i];
    }
    free(old);
    return 0;
}

ue_held_t *ue_held_find(const ue_held_map_t *m, uint64_t rec)
{
    for (size_t s = slot_of(m, rec);; s = (s + 1) & (m->cap - 1)) {
        if (!m->slots[s].used)
            return NULL;
        if (m->slots[s].rec == rec)
            return &m->slots[s];
    }
}

int ue_held_add(ue_held_map_t *m, uint64_t rec, uint16_t seq)
{
    ue_held_t *h = ue_held_find(m, rec);
    size_t s;
    int err;

    if (h) {
        if (h->seq != seq) {           // the record was reused: forget the old identity
            memset(h, 0, sizeof(*h));
            h->used = true;
            h->rec = rec;
            h->seq = seq;
        }
        return 0;
    }
    if ((m->count + 1) * 2 > m->cap && (err = grow(m)))
        return err;
    for (s = slot_of(m, rec); m->slots[s].used; s = (s + 1) & (m->cap - 1))
        ;
    memset(&m->slots[s], 0, sizeof(m->slots[s]));
    m->slots[s].used = true;
    m->slots[s].rec = rec;
    m->slots[s].seq = seq;
    m->count++;
    return 0;
}

void ue_held_remove(ue_held_map_t *m, uint64_t rec)
{
    size_t s, next, home;

    for (s = slot_of(m, rec);; s = (s + 1) & (m->cap - 1)) {
        if (!m->slots[s].used)
            return;
        if (m->slots[s].rec == rec)
            break;
    }
    // Backward-shift the cluster after s so lookups never hit a premature hole.
    for (next = (s + 1) & (m->cap - 1); m->slots[next].used; next = (next + 1) & (m->cap - 1)) {
        home = slot_of(m, m->slots[next].rec);
        if (((next - home) & (m->cap - 1)) >= ((next - s) & (m->cap - 1))) {
            m->slots[s] = m->slots[next];
            s = next;
        }
    }
    memset(&m->slots[s], 0, sizeof(m->slots[s]));
    m->count--;
}
