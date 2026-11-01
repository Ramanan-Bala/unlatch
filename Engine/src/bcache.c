// SPDX-License-Identifier: GPL-3.0-or-later
// Write-back page cache between libntfs-3g and the host's ue_io_t (Step 15, D-62).
//
// libntfs-3g does its metadata I/O in small pieces: 1 KiB MFT records, 4 KiB index blocks,
// bitmap bytes. On a raw device (/dev/rdiskN) each of those is a separate transfer, and on an
// attached image every transfer is a round trip to the disk-image helper. The cache keeps
// 4 KiB pages, reads ahead up to 64 KiB on a miss, and writes dirty pages back sorted and
// coalesced into large transfers. Large requests (64 KiB and more of whole pages) bypass it:
// they go straight to the device, and any cached copy of the pages they cover is updated, so
// the cache never holds data older than the device.
//
// Write-back happens when the cache fills, on bc_writeback (ue_sync, ue_writeback, close).
// A failed write-back is reported to the caller, which marks the device failed (D-46).
#include "internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BC_PAGE_MIN 4096u
#define BC_READAHEAD 16u              // pages read on a miss, within an aligned 64 KiB window
#define BC_DIRECT_MIN (64u << 10)     // whole-page spans this long or longer bypass the cache
#define BC_STAGE (1u << 20)           // largest coalesced write-back transfer
#define NIL (-1)

typedef struct {
    uint64_t idx;                     // page number on the device
    int32_t hnext;                    // hash chain
    int32_t prev, next;               // LRU list (head = most recent), or the free list
    bool dirty;
} bc_ent_t;

struct ue_bcache {
    ue_io_t io;
    uint32_t page;
    uint64_t dev_pages;               // pages on the device (the last may be short)
    size_t n;                         // entries
    uint8_t *mem;                     // n pages
    bc_ent_t *ent;
    int32_t *hash;
    size_t hmask;
    int32_t head, tail, free_list;
    size_t ndirty;
    uint64_t dirty_since;             // monotonic ns when the oldest dirty page was dirtied
    uint8_t *stage;                   // BC_STAGE bytes for coalesced write-back
    uint8_t *ahead;                   // BC_READAHEAD pages; separate, since filling can evict and write back
    bool cold;                        // file data: new pages go to the LRU tail, hits stay put
    int32_t *sorted;                  // n entries, for write-back
    ue_cache_stats_t stats;
};

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static size_t hslot(const ue_bcache_t *bc, uint64_t idx)
{
    return (size_t)((idx * 0x9E3779B97F4A7C15ull) >> 20) & bc->hmask;
}

static uint8_t *page_of(const ue_bcache_t *bc, int32_t i)
{
    return bc->mem + (size_t)i * bc->page;
}

// Bytes of page idx that lie on the device (the last page may be short).
static size_t page_len(const ue_bcache_t *bc, uint64_t idx)
{
    uint64_t start = idx * bc->page;

    return bc->io.size_bytes - start < bc->page ? (size_t)(bc->io.size_bytes - start) : bc->page;
}

static int32_t find(const ue_bcache_t *bc, uint64_t idx)
{
    for (int32_t i = bc->hash[hslot(bc, idx)]; i != NIL; i = bc->ent[i].hnext)
        if (bc->ent[i].idx == idx)
            return i;
    return NIL;
}

static void lru_unlink(ue_bcache_t *bc, int32_t i)
{
    bc_ent_t *e = &bc->ent[i];

    if (e->prev != NIL)
        bc->ent[e->prev].next = e->next;
    else
        bc->head = e->next;
    if (e->next != NIL)
        bc->ent[e->next].prev = e->prev;
    else
        bc->tail = e->prev;
}

static void lru_push(ue_bcache_t *bc, int32_t i)
{
    bc->ent[i].prev = NIL;
    bc->ent[i].next = bc->head;
    if (bc->head != NIL)
        bc->ent[bc->head].prev = i;
    bc->head = i;
    if (bc->tail == NIL)
        bc->tail = i;
}

static void lru_push_tail(ue_bcache_t *bc, int32_t i)
{
    bc->ent[i].next = NIL;
    bc->ent[i].prev = bc->tail;
    if (bc->tail != NIL)
        bc->ent[bc->tail].next = i;
    bc->tail = i;
    if (bc->head == NIL)
        bc->head = i;
}

static void touch(ue_bcache_t *bc, int32_t i)
{
    if (bc->cold)
        return;                       // file data does not push metadata out
    if (bc->head != i) {
        lru_unlink(bc, i);
        lru_push(bc, i);
    }
}

static void hash_remove(ue_bcache_t *bc, int32_t i)
{
    int32_t *p = &bc->hash[hslot(bc, bc->ent[i].idx)];

    while (*p != i)
        p = &bc->ent[*p].hnext;
    *p = bc->ent[i].hnext;
}

static void mark_dirty(ue_bcache_t *bc, int32_t i)
{
    if (!bc->ent[i].dirty) {
        if (bc->ndirty++ == 0)
            bc->dirty_since = now_ns();
        bc->ent[i].dirty = true;
    }
}

static void mark_clean(ue_bcache_t *bc, int32_t i)
{
    if (bc->ent[i].dirty) {
        bc->ent[i].dirty = false;
        bc->ndirty--;
    }
}

static int cmp_idx(void *ctx, const void *a, const void *b)   // Darwin qsort_r order
{
    const bc_ent_t *ent = ctx;
    uint64_t x = ent[*(const int32_t *)a].idx, y = ent[*(const int32_t *)b].idx;

    return x < y ? -1 : x > y;
}

// Writes every dirty page, in device order, adjacent pages joined into one transfer.
int bc_writeback(ue_bcache_t *bc)
{
    size_t m = 0;

    if (!bc || bc->ndirty == 0)
        return 0;
    uint64_t t0 = now_ns();
    int err = 0;
    for (int32_t i = bc->head; i != NIL; i = bc->ent[i].next)
        if (bc->ent[i].dirty)
            bc->sorted[m++] = i;
    qsort_r(bc->sorted, m, sizeof(*bc->sorted), bc->ent, cmp_idx);
    for (size_t a = 0; a < m;) {
        size_t b = a + 1, used = page_len(bc, bc->ent[bc->sorted[a]].idx);
        uint64_t first = bc->ent[bc->sorted[a]].idx;
        while (b < m && bc->ent[bc->sorted[b]].idx == first + (b - a) && used + bc->page <= BC_STAGE)
            used += page_len(bc, bc->ent[bc->sorted[b++]].idx);
        const void *src;
        if (b - a == 1) {
            src = page_of(bc, bc->sorted[a]);
        } else {
            for (size_t k = a; k < b; k++)
                memcpy(bc->stage + (k - a) * bc->page, page_of(bc, bc->sorted[k]),
                       page_len(bc, bc->ent[bc->sorted[k]].idx));
            src = bc->stage;
        }
        err = bc->io.pwrite(bc->io.ctx, src, used, first * bc->page);
        bc->stats.writebacks++;
        bc->stats.write_bytes += used;
        if (err)
            break;
        for (size_t k = a; k < b; k++)
            mark_clean(bc, bc->sorted[k]);
        a = b;
    }
    bc->stats.writeback_us += (now_ns() - t0) / 1000;
    return err;
}

// A free entry: the free list, else the least recently used clean page among the last
// BC_EVICT_SCAN; only when those are all dirty is everything written back (coalesced), which
// also keeps write-backs in the middle of an engine call rare.
#define BC_EVICT_SCAN 1024

static int take_entry(ue_bcache_t *bc, int32_t *out)
{
    int32_t i = bc->free_list;
    int err;

    if (i != NIL) {
        bc->free_list = bc->ent[i].next;
        *out = i;
        return 0;
    }
    i = bc->tail;
    for (int k = 0; k < BC_EVICT_SCAN && i != NIL && bc->ent[i].dirty; k++)
        i = bc->ent[i].prev;
    if (i == NIL || bc->ent[i].dirty) {
        if ((err = bc_writeback(bc)))
            return err;
        i = bc->tail;
    }
    lru_unlink(bc, i);
    hash_remove(bc, i);
    bc->stats.evictions++;
    *out = i;
    return 0;
}

static int insert(ue_bcache_t *bc, uint64_t idx, const void *data, int32_t *out)
{
    int32_t i;
    int err;

    if ((err = take_entry(bc, &i)))
        return err;
    bc->ent[i].idx = idx;
    bc->ent[i].dirty = false;
    memcpy(page_of(bc, i), data, page_len(bc, idx));
    size_t h = hslot(bc, idx);
    bc->ent[i].hnext = bc->hash[h];
    bc->hash[h] = i;
    if (bc->cold)
        lru_push_tail(bc, i);
    else
        lru_push(bc, i);
    *out = i;
    return 0;
}

// Brings page idx in, reading ahead over the uncached pages around it in its 64 KiB window.
// With ahead false, only that page is read (a partial write is about to change it).
static int fill(ue_bcache_t *bc, uint64_t idx, bool ahead, int32_t *out)
{
    uint64_t lo = idx, hi = idx + 1;
    int err;

    // No read-ahead for cold pages: each is inserted at the LRU tail, where the next insert
    // of the same window could evict it.
    if (ahead && !bc->cold) {
        uint64_t wlo = idx - idx % BC_READAHEAD, whi = wlo + BC_READAHEAD;
        if (whi > bc->dev_pages)
            whi = bc->dev_pages;
        while (lo > wlo && find(bc, lo - 1) == NIL)
            lo--;
        while (hi < whi && find(bc, hi) == NIL)
            hi++;
    }
    size_t len = (size_t)(hi - lo - 1) * bc->page + page_len(bc, hi - 1);
    if ((err = bc->io.pread(bc->io.ctx, bc->ahead, len, lo * bc->page)))
        return err;
    bc->stats.misses++;
    bc->stats.read_bytes += len;
    for (uint64_t p = lo; p < hi; p++) {
        int32_t i;
        if ((err = insert(bc, p, bc->ahead + (p - lo) * bc->page, &i)))
            return err;
        if (p == idx)
            *out = i;
    }
    // Inserting cannot evict a page of this window: the cache holds at least four windows.
    return 0;
}

static int read_cached(ue_bcache_t *bc, uint8_t *buf, size_t len, uint64_t off)
{
    while (len > 0) {
        uint64_t idx = off / bc->page;
        size_t in = (size_t)(off % bc->page), n = bc->page - in;
        int32_t i = find(bc, idx);
        int err;

        if (n > len)
            n = len;
        if (i == NIL) {
            if ((err = fill(bc, idx, true, &i)))
                return err;
        } else {
            bc->stats.hits++;
        }
        touch(bc, i);
        memcpy(buf, page_of(bc, i) + in, n);
        buf += n;
        off += n;
        len -= n;
    }
    return 0;
}

static int write_cached(ue_bcache_t *bc, const uint8_t *buf, size_t len, uint64_t off)
{
    while (len > 0) {
        uint64_t idx = off / bc->page;
        size_t in = (size_t)(off % bc->page), n = bc->page - in;
        int32_t i = find(bc, idx);
        int err;

        if (n > len)
            n = len;
        if (i == NIL) {
            if (in == 0 && n == page_len(bc, idx))
                err = insert(bc, idx, buf, &i);     // the whole page: nothing to read
            else
                err = fill(bc, idx, false, &i);
            if (err)
                return err;
        }
        touch(bc, i);
        memcpy(page_of(bc, i) + in, buf, n);
        mark_dirty(bc, i);
        buf += n;
        off += n;
        len -= n;
    }
    return 0;
}

// Splits [off, off+len) into a cached head, a direct middle of whole pages, and a cached tail.
// The middle is direct only when it is at least BC_DIRECT_MIN long.
static void split(const ue_bcache_t *bc, uint64_t off, size_t len, uint64_t *mid_off, size_t *mid_len)
{
    uint64_t a = (off + bc->page - 1) / bc->page * bc->page, b = (off + len) / bc->page * bc->page;

    if (off + len == bc->io.size_bytes)
        b = off + len;                // the short last page counts as whole
    *mid_off = a;
    *mid_len = b > a && b - a >= BC_DIRECT_MIN ? (size_t)(b - a) : 0;
}

int bc_read(ue_bcache_t *bc, void *buf, size_t len, uint64_t off)
{
    uint64_t mo;
    size_t ml;
    int err;

    split(bc, off, len, &mo, &ml);
    if (ml == 0)
        return read_cached(bc, buf, len, off);
    uint8_t *p = buf;
    if ((err = read_cached(bc, p, (size_t)(mo - off), off)))
        return err;
    if ((err = bc->io.pread(bc->io.ctx, p + (mo - off), ml, mo)))
        return err;
    bc->stats.direct_reads++;
    bc->stats.read_bytes += ml;
    // Cached pages are at least as new as the device: lay them over what was read.
    if (bc->n * (uint64_t)bc->page < ml) {
        for (int32_t i = bc->head; i != NIL; i = bc->ent[i].next) {
            uint64_t s = bc->ent[i].idx * bc->page;
            if (s >= mo && s < mo + ml)
                memcpy(p + (s - off), page_of(bc, i), page_len(bc, bc->ent[i].idx));
        }
    } else {
        for (uint64_t s = mo; s < mo + ml; s += bc->page) {
            int32_t i = find(bc, s / bc->page);
            if (i != NIL)
                memcpy(p + (s - off), page_of(bc, i), page_len(bc, bc->ent[i].idx));
        }
    }
    return read_cached(bc, p + (mo - off) + ml, (size_t)(off + len - mo - ml), mo + ml);
}

int bc_write(ue_bcache_t *bc, const void *buf, size_t len, uint64_t off)
{
    uint64_t mo;
    size_t ml;
    int err;

    split(bc, off, len, &mo, &ml);
    if (ml == 0)
        return write_cached(bc, buf, len, off);
    const uint8_t *p = buf;
    if ((err = write_cached(bc, p, (size_t)(mo - off), off)))
        return err;
    if ((err = bc->io.pwrite(bc->io.ctx, p + (mo - off), ml, mo)))
        return err;
    bc->stats.direct_writes++;
    bc->stats.write_bytes += ml;
    // Pages the direct write covered now match the device: refresh them and mark them clean.
    for (uint64_t s = mo; s < mo + ml; s += bc->page) {
        int32_t i = find(bc, s / bc->page);
        if (i != NIL) {
            memcpy(page_of(bc, i), p + (s - off), page_len(bc, bc->ent[i].idx));
            mark_clean(bc, i);
        }
    }
    return write_cached(bc, p + (mo - off) + ml, (size_t)(off + len - mo - ml), mo + ml);
}

void bc_set_cold(ue_bcache_t *bc, bool cold)
{
    if (bc)
        bc->cold = cold;
}

size_t bc_dirty(const ue_bcache_t *bc)
{
    return bc ? bc->ndirty : 0;
}

uint64_t bc_dirty_age_ms(const ue_bcache_t *bc)
{
    return bc && bc->ndirty ? (now_ns() - bc->dirty_since) / 1000000u : 0;
}

void bc_get_stats(const ue_bcache_t *bc, ue_cache_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    if (bc) {
        *out = bc->stats;
        out->dirty_pages = bc->ndirty;
        out->page_size = bc->page;
    }
}

int bc_new(const ue_io_t *io, size_t bytes, ue_bcache_t **out)
{
    ue_bcache_t *bc = calloc(1, sizeof(*bc));
    size_t hsize = 1;

    if (!bc)
        return ENOMEM;
    bc->io = *io;
    bc->page = io->block_size > BC_PAGE_MIN ? io->block_size : BC_PAGE_MIN;
    bc->dev_pages = (io->size_bytes + bc->page - 1) / bc->page;
    bc->n = bytes / bc->page;
    if (bc->n < 4 * BC_READAHEAD)
        bc->n = 4 * BC_READAHEAD;
    while (hsize < bc->n)
        hsize <<= 1;
    bc->hmask = hsize - 1;
    bc->mem = malloc(bc->n * bc->page);
    bc->ent = calloc(bc->n, sizeof(*bc->ent));
    bc->hash = malloc(hsize * sizeof(*bc->hash));
    bc->sorted = malloc(bc->n * sizeof(*bc->sorted));
    bc->stage = malloc(BC_STAGE > 2 * bc->page ? BC_STAGE : 2 * bc->page);
    bc->ahead = malloc(BC_READAHEAD * bc->page);
    if (!bc->mem || !bc->ent || !bc->hash || !bc->sorted || !bc->stage || !bc->ahead) {
        bc_free(bc);
        return ENOMEM;
    }
    for (size_t h = 0; h < hsize; h++)
        bc->hash[h] = NIL;
    bc->head = bc->tail = NIL;
    for (size_t i = 0; i < bc->n; i++)
        bc->ent[i].next = i + 1 < bc->n ? (int32_t)(i + 1) : NIL;
    bc->free_list = 0;
    *out = bc;
    return 0;
}

void bc_free(ue_bcache_t *bc)
{
    if (!bc)
        return;
    free(bc->mem);
    free(bc->ent);
    free(bc->hash);
    free(bc->sorted);
    free(bc->stage);
    free(bc->ahead);
    free(bc);
}
