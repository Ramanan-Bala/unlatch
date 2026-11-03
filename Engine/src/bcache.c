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
//
// NTFS through libntfs-3g has no journal, so a write-back cut off by a pull leaves whatever
// part of it reached the disk. Write-back is ordered so that what is pointed to lands, and
// is flushed, before what points at it (D-97, D-100):
//   1. file data and the bitmaps, then $MFT's first page with $MFTMirr right after it
//   2. new index blocks (pages that were not index blocks on the disk)
//   3. new extension records, then other metadata (attribute lists, $I30 bitmaps, stream
//      values: pages that are neither records nor index blocks; $Secure's record and its
//      $SII/$SDH blocks, which a new record's security id points into), then new base
//      records, highest first (a new folder's record after those of the files created in it)
//   flush; until here everything new is unreachable: only what follows points at it
//   4. changed MFT records, highest first (index roots, runlists, $FILE_NAMEs of links and
//      renames): an index root lands before the blocks it points at, a file's new name
//      before the entry that carries it
//   flush
//   5. the other index blocks: nodes before leaves (a split's parent before the cut-down block)
//   flush
//   6. records freed since the last write-back (after the entries naming them are gone)
// MFT pages are written record by record when their records fall in different phases. A cut
// then leaves at worst lost space, orphans or entries that lead to no file, and in a folder
// whose index changed: a new block it marks in use before a node points at it, a split's old
// tail listed twice, or a renamed file's old entry (no order closes those windows, D-100).
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
#define BC_SLOTS 4                    // MFT records per page tracked one by one (1 KiB in 4 KiB)

typedef struct {
    uint64_t idx;                     // page number on the device
    int32_t hnext;                    // hash chain
    int32_t prev, next;               // LRU list (head = most recent), or the free list
    bool dirty;
    bool data;                        // dirtied as file data (cold), see bc_set_cold
    bool was_index;                   // an index block on the disk (when it was last clean)
    bool slots_known;                 // was_used and was_seq hold the disk's records (D-100)
    uint8_t fresh;                    // record slots created since the page was last written back
    uint8_t was_used;                 // record slots in use on the disk
    uint8_t parts, left;              // write-back: pieces of this page, pieces not yet written
    uint16_t was_seq[BC_SLOTS];       // their sequence numbers
} bc_ent_t;

typedef struct {
    uint64_t lo, hi;                  // pages [lo, hi)
    int kind;
} bc_range_t;

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
    int32_t *sorted;                  // n * BC_SLOTS pieces (entry * BC_SLOTS + part), for write-back
    uint64_t *key;                    // n * BC_SLOTS: write-back order of each piece (D-97)
    size_t *runs;                     // n * BC_SLOTS + 1: starts of one phase's transfers in sorted
    bc_order_fn order;                // fills the ranges at each write-back; NULL: device order
    void *order_ctx;
    bc_range_t *ranges;
    size_t nranges, cap_ranges;
    uint64_t mirror_after;            // $MFTMirr pages are written right after this page
    uint32_t rec;                     // MFT record size; 0: unknown, records not told apart
    uint64_t secure;                  // device offset of $Secure's record; UINT64_MAX: unknown
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
    if (bc->cold)
        bc->ent[i].data = true;
}

static void mark_clean(ue_bcache_t *bc, int32_t i)
{
    if (bc->ent[i].dirty) {
        bc->ent[i].dirty = false;
        bc->ndirty--;
    }
    bc->ent[i].data = false;
    bc->ent[i].fresh = 0;
}

void bc_set_order(ue_bcache_t *bc, bc_order_fn fn, void *ctx)
{
    if (bc) {
        bc->order = fn;
        bc->order_ctx = ctx;
    }
}

// A range that cannot be stored is left out: its pages are then written with the rest.
void bc_add_range(ue_bcache_t *bc, uint64_t off, uint64_t len, int kind)
{
    if (len == 0 || off >= bc->io.size_bytes)
        return;
    if (bc->nranges == bc->cap_ranges) {
        size_t cap = bc->cap_ranges ? 2 * bc->cap_ranges : 16;
        bc_range_t *r = realloc(bc->ranges, cap * sizeof(*r));
        if (!r)
            return;
        bc->ranges = r;
        bc->cap_ranges = cap;
    }
    bc->ranges[bc->nranges++] = (bc_range_t){ off / bc->page, (off + len + bc->page - 1) / bc->page, kind };
}

void bc_set_mirror_after(ue_bcache_t *bc, uint64_t off)
{
    bc->mirror_after = off / bc->page;
}

void bc_set_record_size(ue_bcache_t *bc, uint32_t size)
{
    if (bc)
        bc->rec = size && size <= bc->page && bc->page % size == 0 && bc->page / size <= BC_SLOTS ? size : 0;
}

void bc_set_secure(ue_bcache_t *bc, uint64_t off)
{
    bc->secure = off;
}

void bc_hint_new(ue_bcache_t *bc, uint64_t off)
{
    int32_t i = bc ? find(bc, off / bc->page) : NIL;

    if (i != NIL)
        bc->ent[i].fresh |= (uint8_t)(1u << (bc->rec ? off % bc->page / bc->rec : 0));
}

// A record slot's header: FILE, in use, sequence number, base record (0 for a base record).
// The header lies in the first sector, which update sequence fixups leave alone.
static bool slot_in_use(const uint8_t *r, uint16_t *seq, bool *ext)
{
    if (memcmp(r, "FILE", 4) != 0)
        return false;
    *seq = (uint16_t)(r[16] | r[17] << 8);
    *ext = (r[32] | r[33] | r[34] | r[35] | r[36] | r[37]) != 0;
    return (r[22] & 1) != 0;          // MFT_RECORD_IN_USE
}

// Called as a clean page is first changed: what the disk holds in each record slot. A page
// written whole without being read (it then holds the new data) is unknown, except when no
// record in it is in use: libntfs-3g zero-filling $MFT past its initialized size.
static void note_slots(ue_bcache_t *bc, int32_t i, bool whole)
{
    bc_ent_t *e = &bc->ent[i];

    e->slots_known = bc->rec != 0;
    e->was_used = 0;
    for (uint32_t k = 0; e->slots_known && k < bc->page / bc->rec; k++) {
        bool ext;
        if (slot_in_use(page_of(bc, i) + k * bc->rec, &e->was_seq[k], &ext))
            e->was_used |= (uint8_t)(1u << k);
    }
    if (whole) {
        e->slots_known = e->slots_known && e->was_used == 0;
        e->was_used = 0;
    }
}

static int cmp_range(const void *a, const void *b)
{
    uint64_t x = ((const bc_range_t *)a)->lo, y = ((const bc_range_t *)b)->lo;

    return x < y ? -1 : x > y;
}

static int range_kind(const ue_bcache_t *bc, uint64_t idx)
{
    size_t lo = 0, hi = bc->nranges;

    while (lo < hi) {                 // the last range starting at or before idx
        size_t mid = (lo + hi) / 2;
        if (bc->ranges[mid].lo <= idx)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo > 0 && idx < bc->ranges[lo - 1].hi ? bc->ranges[lo - 1].kind : BC_RANGE_NONE;
}

static bool is_index(const uint8_t *page)
{
    return memcmp(page, "INDX", 4) == 0;
}

// Write-back phases (D-97, D-100), in order. Pieces in phases of different groups are
// separated by a device flush; BC_PH_NEW and BC_PH_RECORDS go highest first.
enum { BC_PH_FIRST, BC_PH_MFT_HEAD, BC_PH_NEW_INDEX, BC_PH_NEW_EXT, BC_PH_NEW_META, BC_PH_NEW,
       BC_PH_RECORDS, BC_PH_NODES, BC_PH_REST, BC_PH_FREED };
static const uint8_t phase_group[] = { 0, 0, 0, 0, 0, 0, 1, 2, 2, 3 };

static bool index_node(const uint8_t *page)
{
    return page[0x24] & 1;            // INDEX_HEADER.ih_flags & LARGE_INDEX: entries point at blocks
}

// A block of $Secure's $SII (4-byte keys) or $SDH (8-byte keys), not a folder's $I30 (file
// name keys, 66 bytes and more): told by the first entry's key length. An empty block is a folder's.
static bool index_security(const uint8_t *page, size_t len)
{
    uint32_t first = 0x18 + (uint32_t)(page[0x18] | page[0x19] << 8 | page[0x1a] << 16 | (uint32_t)page[0x1b] << 24);

    if (first + 16 > len || page[first + 12] & 2)   // INDEX_ENTRY_END
        return false;
    uint16_t key = (uint16_t)(page[first + 10] | page[first + 11] << 8);
    return key == 4 || key == 8;
}

// The phase of one record slot of an MFT page.
static int slot_phase(const ue_bcache_t *bc, int32_t i, uint32_t k)
{
    const bc_ent_t *e = &bc->ent[i];
    uint16_t seq;
    bool ext, used = slot_in_use(page_of(bc, i) + k * bc->rec, &seq, &ext);
    bool was = e->slots_known && (e->was_used >> k & 1);

    if (used && (e->fresh >> k & 1 || (e->slots_known && (!was || e->was_seq[k] != seq))))
        return ext ? BC_PH_NEW_EXT : BC_PH_NEW;
    return was && !used ? BC_PH_FREED : BC_PH_RECORDS;   // unknown: as D-97, a changed record
}

// Sort key of piece `part` of entry i: phase in the top byte, then the position within it
// (in 256-byte units, so a $MFTMirr page can sit right after $MFT's first page).
static uint64_t order_key(const ue_bcache_t *bc, int32_t i, uint32_t part)
{
    const bc_ent_t *e = &bc->ent[i];
    uint64_t pos = (e->idx * bc->page + (uint64_t)part * (bc->page / e->parts)) >> 8;
    int ph;

    switch (bc->order ? range_kind(bc, e->idx) : BC_RANGE_NONE) {
    case BC_RANGE_BITMAP:
        ph = BC_PH_FIRST;
        break;
    case BC_RANGE_MFT:
        // $MFT's own record is an allocation like the bitmaps: when the MFT grows, entries
        // point at records past its old end.
        if (e->idx == bc->mirror_after)
            ph = BC_PH_MFT_HEAD;
        else if (bc->rec && e->idx * bc->page + (uint64_t)part * (bc->page / e->parts) == bc->secure)
            ph = BC_PH_NEW_META;      // in pieces only when the page is (1 KiB records)
        else if (bc->rec)
            ph = slot_phase(bc, i, part);
        else
            ph = e->fresh ? BC_PH_NEW : BC_PH_RECORDS;
        break;
    case BC_RANGE_MIRROR:
        ph = BC_PH_MFT_HEAD;
        pos = (bc->mirror_after * bc->page >> 8) | 1;
        break;
    default:
        if (!bc->order)
            ph = BC_PH_REST;
        else if (e->data)
            ph = BC_PH_FIRST;
        else if (!is_index(page_of(bc, i)))
            ph = BC_PH_NEW_META;
        else if (!e->was_index)
            ph = BC_PH_NEW_INDEX;
        else if (index_security(page_of(bc, i), page_len(bc, e->idx)) && !index_node(page_of(bc, i)))
            ph = BC_PH_NEW_META;
        else
            ph = index_node(page_of(bc, i)) ? BC_PH_NODES : BC_PH_REST;
        break;
    }
    return (uint64_t)ph << 56 | (pos & ((1ull << 56) - 1));
}

static uint64_t piece_off(const ue_bcache_t *bc, int32_t p)
{
    const bc_ent_t *e = &bc->ent[p / BC_SLOTS];

    return e->idx * bc->page + (uint64_t)(p % BC_SLOTS) * (bc->page / e->parts);
}

static size_t piece_len(const ue_bcache_t *bc, int32_t p)
{
    const bc_ent_t *e = &bc->ent[p / BC_SLOTS];

    return e->parts > 1 ? bc->page / e->parts : page_len(bc, e->idx);
}

static const uint8_t *piece_data(const ue_bcache_t *bc, int32_t p)
{
    return page_of(bc, p / BC_SLOTS) + (size_t)(p % BC_SLOTS) * (bc->page / bc->ent[p / BC_SLOTS].parts);
}

static int cmp_key(void *ctx, const void *a, const void *b)   // Darwin qsort_r order
{
    const ue_bcache_t *bc = ctx;
    int32_t i = *(const int32_t *)a, j = *(const int32_t *)b;
    uint64_t x = bc->key[i], y = bc->key[j];

    if (x == y) {
        x = piece_off(bc, i);
        y = piece_off(bc, j);
    }
    return x < y ? -1 : x > y;
}

// One transfer: sorted[a, b), adjacent pieces. A page is clean once all its pieces are written.
static int write_run(ue_bcache_t *bc, size_t a, size_t b)
{
    size_t used = 0;
    const void *src;
    int err;

    if (b - a == 1) {
        src = piece_data(bc, bc->sorted[a]);
        used = piece_len(bc, bc->sorted[a]);
    } else {
        for (size_t k = a; k < b; k++) {
            size_t len = piece_len(bc, bc->sorted[k]);
            memcpy(bc->stage + used, piece_data(bc, bc->sorted[k]), len);
            used += len;
        }
        src = bc->stage;
    }
    err = bc->io.pwrite(bc->io.ctx, src, used, piece_off(bc, bc->sorted[a]));
    bc->stats.writebacks++;
    bc->stats.write_bytes += used;
    if (err)
        return err;
    for (size_t k = a; k < b; k++)
        if (--bc->ent[bc->sorted[k] / BC_SLOTS].left == 0)
            mark_clean(bc, bc->sorted[k] / BC_SLOTS);
    return 0;
}

// Writes every dirty page, phase by phase (D-97, D-100); within a phase in device order,
// adjacent pieces joined into one transfer. New and changed records go highest first: a new
// folder's record, with the entries of the files created in it, lands after theirs.
int bc_writeback(ue_bcache_t *bc)
{
    size_t m = 0;

    if (!bc || bc->ndirty == 0)
        return 0;
    uint64_t t0 = now_ns();
    int err = 0;
    bc->nranges = 0;
    if (bc->order) {
        bc->order(bc->order_ctx, bc);
        qsort(bc->ranges, bc->nranges, sizeof(*bc->ranges), cmp_range);
    }
    for (int32_t i = bc->head; i != NIL; i = bc->ent[i].next) {
        bc_ent_t *e = &bc->ent[i];
        if (!e->dirty)
            continue;
        // MFT records one by one, so that each lands in its own phase.
        e->parts = bc->order && bc->rec && e->idx != bc->mirror_after && range_kind(bc, e->idx) == BC_RANGE_MFT
                       ? (uint8_t)(bc->page / bc->rec) : 1;
        e->left = e->parts;
        for (uint32_t k = 0; k < e->parts; k++) {
            int32_t p = i * BC_SLOTS + (int32_t)k;
            bc->sorted[m++] = p;
            bc->key[p] = order_key(bc, i, k);
        }
    }
    qsort_r(bc->sorted, m, sizeof(*bc->sorted), bc, cmp_key);
    bool unflushed = false;
    int group = 0;
    for (size_t a = 0; a < m && !err;) {
        int ph = (int)(bc->key[bc->sorted[a]] >> 56);
        size_t nr = 0, b = a;
        while (b < m && (int)(bc->key[bc->sorted[b]] >> 56) == ph) {
            size_t c = b + 1, used = piece_len(bc, bc->sorted[b]);
            while (c < m && (int)(bc->key[bc->sorted[c]] >> 56) == ph &&
                   piece_off(bc, bc->sorted[c]) == piece_off(bc, bc->sorted[b]) + used &&
                   used + piece_len(bc, bc->sorted[c]) <= BC_STAGE)
                used += piece_len(bc, bc->sorted[c++]);
            bc->runs[nr++] = b;
            b = c;
        }
        bc->runs[nr] = b;
        if (unflushed && phase_group[ph] != group && bc->io.flush) {
            if ((err = bc->io.flush(bc->io.ctx)))
                break;
            bc->stats.order_flushes++;
            unflushed = false;
        }
        group = phase_group[ph];
        for (size_t r = 0; r < nr && !err; r++) {
            size_t k = ph == BC_PH_NEW || ph == BC_PH_RECORDS ? nr - 1 - r : r;
            err = write_run(bc, bc->runs[k], bc->runs[k + 1]);
        }
        unflushed = true;
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
    bc->ent[i].dirty = bc->ent[i].data = bc->ent[i].was_index = bc->ent[i].slots_known = false;
    bc->ent[i].fresh = 0;
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
        bool whole = false;
        int err;

        if (n > len)
            n = len;
        if (i == NIL) {
            whole = in == 0 && n == page_len(bc, idx);
            if (whole)
                err = insert(bc, idx, buf, &i);     // the whole page: nothing to read
            else
                err = fill(bc, idx, false, &i);
            if (err)
                return err;
        }
        if (!bc->ent[i].dirty) {      // a clean page is what the disk holds (unknown if whole)
            bc->ent[i].was_index = !whole && is_index(page_of(bc, i));
            note_slots(bc, i, whole);
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
    bc->sorted = malloc(bc->n * BC_SLOTS * sizeof(*bc->sorted));
    bc->key = malloc(bc->n * BC_SLOTS * sizeof(*bc->key));
    bc->runs = malloc((bc->n * BC_SLOTS + 1) * sizeof(*bc->runs));
    bc->stage = malloc(BC_STAGE > 2 * bc->page ? BC_STAGE : 2 * bc->page);
    bc->ahead = malloc(BC_READAHEAD * bc->page);
    if (!bc->mem || !bc->ent || !bc->hash || !bc->sorted || !bc->key || !bc->runs || !bc->stage || !bc->ahead) {
        bc_free(bc);
        return ENOMEM;
    }
    for (size_t h = 0; h < hsize; h++)
        bc->hash[h] = NIL;
    bc->head = bc->tail = NIL;
    for (size_t i = 0; i < bc->n; i++)
        bc->ent[i].next = i + 1 < bc->n ? (int32_t)(i + 1) : NIL;
    bc->free_list = 0;
    bc->secure = UINT64_MAX;
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
    free(bc->key);
    free(bc->runs);
    free(bc->ranges);
    free(bc->stage);
    free(bc->ahead);
    free(bc);
}
