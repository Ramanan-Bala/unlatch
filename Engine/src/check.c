// SPDX-License-Identifier: GPL-3.0-or-later
// Check Disk (D-93): a read-only check of the MFT, the two bitmaps and the directory indexes,
// and the one repair that cannot overwrite anything: setting bitmap bits of what is in use.
//
// The MFT is read raw, record by record, so a damaged record is reported instead of being
// opened. Pass 1 records which records are in use, marks every cluster their runlists use and
// compares both with the bitmaps on the disk. Pass 2 (only when clusters are shared) finds the
// first owner of each shared run. Then every directory is listed with ntfs_readdir, which
// reads every index block its $I30 bitmap marks in use, and every entry is compared with
// pass 1. Last, named files no directory listed are orphans.
#include "internal.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ntfs-3g/attrib.h>
#include <ntfs-3g/bitmap.h>
#include <ntfs-3g/bootsect.h>
#include <ntfs-3g/dir.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/layout.h>
#include <ntfs-3g/logging.h>
#include <ntfs-3g/mft.h>
#include <ntfs-3g/mst.h>
#include <ntfs-3g/runlist.h>
#include <ntfs-3g/unistr.h>

#define CHUNK_RECORDS 256
#define MAX_SHARED 1024               // shared runs whose first owner pass 2 looks for
#define MAX_DEPTH 256                 // path components

typedef struct {
    uint64_t rec, lcn, count, other;
} shared_t;

typedef struct {
    ue_engine_t *e;
    ntfs_volume *vol;
    ue_check_fn fn;                   // NULL: count only (no paths are built)
    void *ctx;
    ue_check_result_t *res;
    uint64_t nrec, ncl;
    u32 recsize;
    u8 *rbuf, *pbuf;                  // a chunk of records; one record for paths
    u8 *mftbmp, *lcnbmp;              // the bitmaps on the disk
    uint64_t mftbmp_bits, lcnbmp_bits;
    u8 *inuse, *base, *isdir, *named, *refd, *bad, *used;   // one bit per record (used: per cluster)
    u16 *seq;
    shared_t *shared;
    size_t nshared;
} chk_t;

static inline bool has(const u8 *m, uint64_t i) { return (m[i >> 3] >> (i & 7)) & 1; }
static inline void mark(u8 *m, uint64_t i) { m[i >> 3] |= (u8)(1u << (i & 7)); }

const char *ue_check_kind_name(ue_check_kind_t kind)
{
    switch (kind) {
    case UE_CHK_RECORD_UNMARKED: return "record_unmarked";
    case UE_CHK_CLUSTERS_UNMARKED: return "clusters_unmarked";
    case UE_CHK_CLUSTERS_SHARED: return "clusters_shared";
    case UE_CHK_BAD_RUNLIST: return "bad_runlist";
    case UE_CHK_BAD_RECORD: return "bad_record";
    case UE_CHK_INDEX_UNREADABLE: return "index_unreadable";
    case UE_CHK_ENTRY_FREE_RECORD: return "entry_free_record";
    case UE_CHK_ENTRY_WRONG_SEQ: return "entry_wrong_seq";
    case UE_CHK_ORPHAN: return "orphan";
    case UE_CHK_MFT_MIRROR: return "mft_mirror";
    case UE_CHK_KINDS: break;
    }
    return "unknown";
}

// Reads record rec raw into buf and applies the fixups. 0, or EIO for a torn or unreadable one.
static int read_record(chk_t *c, uint64_t rec, u8 *buf)
{
    if (ntfs_attr_pread(c->vol->mft_na, (s64)(rec * c->recsize), c->recsize, buf) != c->recsize)
        return EIO;
    if (!ntfs_is_file_record(((MFT_RECORD *)buf)->magic))
        return EINVAL;
    return ntfs_mst_post_read_fixup_warn((NTFS_RECORD *)buf, c->recsize, FALSE) ? EIO : 0;
}

// The next attribute of a record, or NULL at its end. Bounds are checked here as well, since
// paths are built from records ntfs_mft_record_check has not seen.
static ATTR_RECORD *next_attr(const chk_t *c, MFT_RECORD *m, ATTR_RECORD *a)
{
    u32 used = le32_to_cpu(m->bytes_in_use), off, len;

    if (used > c->recsize)
        used = c->recsize;
    off = a ? (u32)((u8 *)a - (u8 *)m) + le32_to_cpu(a->length) : le16_to_cpu(m->attrs_offset);
    if ((uint64_t)off + 16 > used)
        return NULL;
    a = (ATTR_RECORD *)((u8 *)m + off);
    len = le32_to_cpu(a->length);
    if (a->type == AT_END || len < 16 || (len & 7) || (uint64_t)off + len > used)
        return NULL;
    return a;
}

// A resident $FILE_NAME's value, or NULL when it does not fit its attribute.
static FILE_NAME_ATTR *file_name(ATTR_RECORD *a)
{
    u32 len = le32_to_cpu(a->length), voff, vlen;
    FILE_NAME_ATTR *fn;

    if (a->type != AT_FILE_NAME || a->non_resident || len < offsetof(ATTR_RECORD, resident_end))
        return NULL;
    voff = le16_to_cpu(a->value_offset);
    vlen = le32_to_cpu(a->value_length);
    if ((uint64_t)voff + vlen > len || vlen < sizeof(FILE_NAME_ATTR))
        return NULL;
    fn = (FILE_NAME_ATTR *)((u8 *)a + voff);
    return sizeof(FILE_NAME_ATTR) + 2u * fn->file_name_length <= vlen ? fn : NULL;
}

// A record's name (UTF-8, malloc'd; a Win32 or POSIX name before a DOS one) and its parent.
static char *record_name(chk_t *c, uint64_t rec, uint64_t *parent)
{
    MFT_RECORD *m = (MFT_RECORD *)c->pbuf;
    FILE_NAME_ATTR *best = NULL;
    char *out = NULL;

    if (rec >= c->nrec || read_record(c, rec, c->pbuf) || !(m->flags & MFT_RECORD_IN_USE))
        return NULL;
    for (ATTR_RECORD *a = next_attr(c, m, NULL); a; a = next_attr(c, m, a)) {
        FILE_NAME_ATTR *fn = file_name(a);
        if (fn && (!best || best->file_name_type == FILE_NAME_DOS))
            best = fn;
    }
    if (!best || ntfs_ucstombs(best->file_name, best->file_name_length, &out, 0) < 0)
        return NULL;
    *parent = MREF_LE(best->parent_directory);
    return out;
}

// Prepends s to the string that ends at buf + cap - 1 and starts at *pos. False if it did not fit.
static bool prepend(char *buf, size_t *pos, const char *s)
{
    size_t n = strlen(s);

    if (n > *pos)
        return false;
    *pos -= n;
    memcpy(buf + *pos, s, n);
    return true;
}

// "/dir/name" from the records' $FILE_NAMEs; "<record N>" stands for a part that cannot be read.
static void rec_path(chk_t *c, uint64_t rec, char *out, size_t cap)
{
    size_t pos = cap - 1;
    char part[64];

    out[pos] = 0;
    for (int depth = 0; rec != FILE_root; depth++) {
        uint64_t parent = 0;
        char *name = depth < MAX_DEPTH ? record_name(c, rec, &parent) : NULL;
        bool ok = name && prepend(out, &pos, name) && prepend(out, &pos, "/");
        free(name);
        if (!ok) {
            snprintf(part, sizeof(part), "<record %llu>", (unsigned long long)rec);
            if (!prepend(out, &pos, part))
                prepend(out, &pos, "...");
            break;
        }
        rec = parent;
    }
    if (pos == cap - 1)
        prepend(out, &pos, "/");
    memmove(out, out + pos, cap - pos);
}

static void report(chk_t *c, ue_check_kind_t kind, bool repairable, uint64_t rec, uint64_t other, uint64_t lcn,
                   uint64_t count, const char *path, const char *fmt, ...) __attribute__((format(printf, 9, 10)));

static void report(chk_t *c, ue_check_kind_t kind, bool repairable, uint64_t rec, uint64_t other, uint64_t lcn,
                   uint64_t count, const char *path, const char *fmt, ...)
{
    ue_check_problem_t p;
    va_list ap;

    c->res->problems++;
    c->res->by_kind[kind]++;
    if (repairable)
        c->res->repairable++;
    if (!c->fn)
        return;
    memset(&p, 0, sizeof(p));
    p.kind = kind;
    p.repairable = repairable;
    p.rec = rec;
    p.other = other;
    p.lcn = lcn;
    p.count = count;
    if (path)
        snprintf(p.path, sizeof(p.path), "%s", path);
    else
        rec_path(c, rec, p.path, sizeof(p.path));
    va_start(ap, fmt);
    vsnprintf(p.detail, sizeof(p.detail), fmt, ap);
    va_end(ap);
    c->fn(c->ctx, &p);
}

static int read_bitmap(ntfs_attr *na, uint64_t bits, u8 **out, uint64_t *have_bits)
{
    size_t bytes = (size_t)((bits + 7) / 8);
    s64 n = na->data_size < (s64)bytes ? na->data_size : (s64)bytes;
    u8 *m = calloc(bytes ? bytes : 1, 1);

    if (!m)
        return ENOMEM;
    if (n > 0 && ntfs_attr_pread(na, 0, n, m) != n) {
        free(m);
        return EIO;
    }
    *out = m;
    *have_bits = n > 0 ? (uint64_t)n * 8 : 0;
    return 0;
}

static void chk_free(chk_t *c)
{
    free(c->rbuf);
    free(c->pbuf);
    free(c->mftbmp);
    free(c->lcnbmp);
    free(c->inuse);
    free(c->base);
    free(c->isdir);
    free(c->named);
    free(c->refd);
    free(c->bad);
    free(c->used);
    free(c->seq);
    free(c->shared);
    memset(c, 0, sizeof(*c));
}

static int chk_init(chk_t *c, ue_engine_t *e, ue_check_fn fn, void *ctx, ue_check_result_t *res)
{
    ntfs_volume *vol = e->vol;
    size_t rbytes;
    int err;

    memset(c, 0, sizeof(*c));
    memset(res, 0, sizeof(*res));
    if (!ue_bitmaps_plausible(vol) || vol->nr_clusters <= 0)
        return EIO;
    c->e = e;
    c->vol = vol;
    c->fn = fn;
    c->ctx = ctx;
    c->res = res;
    c->recsize = vol->mft_record_size;
    c->nrec = (uint64_t)vol->mft_na->initialized_size >> vol->mft_record_size_bits;
    c->ncl = (uint64_t)vol->nr_clusters;
    res->records = c->nrec;
    res->clusters = c->ncl;
    rbytes = (size_t)((c->nrec + 7) / 8) + 1;
    c->rbuf = malloc((size_t)c->recsize * CHUNK_RECORDS);
    c->pbuf = malloc(c->recsize);
    c->inuse = calloc(rbytes, 1);
    c->base = calloc(rbytes, 1);
    c->isdir = calloc(rbytes, 1);
    c->named = calloc(rbytes, 1);
    c->refd = calloc(rbytes, 1);
    c->bad = calloc(rbytes, 1);
    c->used = calloc((size_t)((c->ncl + 7) / 8) + 1, 1);
    c->seq = calloc((size_t)c->nrec + 1, sizeof(*c->seq));
    c->shared = calloc(MAX_SHARED, sizeof(*c->shared));
    if (!c->rbuf || !c->pbuf || !c->inuse || !c->base || !c->isdir || !c->named || !c->refd || !c->bad || !c->used ||
        !c->seq || !c->shared) {
        chk_free(c);
        return ENOMEM;
    }
    if ((err = read_bitmap(vol->mftbmp_na, c->nrec, &c->mftbmp, &c->mftbmp_bits)) ||
        (err = read_bitmap(vol->lcnbmp_na, c->ncl, &c->lcnbmp, &c->lcnbmp_bits))) {
        chk_free(c);
        return err;
    }
    // Every entry, metafiles and hidden files included (ue_open hides the metafiles).
    ntfs_set_shown_files(vol, TRUE, TRUE, FALSE);
    return 0;
}

// Pass 1 for one in-use record that passed ntfs_mft_record_check: its names and clusters.
static void scan_runs(chk_t *c, uint64_t rec, MFT_RECORD *m)
{
    uint64_t owner = MREF_LE(m->base_mft_record) ? MREF_LE(m->base_mft_record) : rec;
    uint64_t um_first = 0, um_count = 0, sh_first = 0, sh_count = 0;
    bool um_fixable = true;

    for (ATTR_RECORD *a = next_attr(c, m, NULL); a; a = next_attr(c, m, a)) {
        if (a->type == AT_FILE_NAME && owner < c->nrec && file_name(a))
            mark(c->named, owner);
        if (!a->non_resident)
            continue;
        runlist_element *rl = ntfs_mapping_pairs_decompress(c->vol, a, NULL);
        if (!rl) {
            report(c, UE_CHK_BAD_RUNLIST, false, owner, rec, 0, 0, NULL,
                   "the runlist of attribute 0x%x in record %llu cannot be read", le32_to_cpu(a->type),
                   (unsigned long long)rec);
            continue;
        }
        for (runlist_element *r = rl; r->length; r++) {
            if (r->lcn < 0)
                continue;             // a hole, or not mapped in this extent
            uint64_t lcn = (uint64_t)r->lcn, end = lcn + (uint64_t)r->length;
            if (r->length < 0 || end > c->ncl || end < lcn) {
                report(c, UE_CHK_BAD_RUNLIST, false, owner, rec, lcn, (uint64_t)r->length, NULL,
                       "attribute 0x%x has clusters %llu-%llu, past the end of the volume (%llu clusters)",
                       le32_to_cpu(a->type), (unsigned long long)lcn, (unsigned long long)(end - 1),
                       (unsigned long long)c->ncl);
                continue;
            }
            for (uint64_t l = lcn; l < end; l++) {
                if (has(c->used, l)) {
                    if (!sh_count++)
                        sh_first = l;
                } else {
                    mark(c->used, l);
                    c->res->clusters_in_use++;
                }
                if (!(l < c->lcnbmp_bits && has(c->lcnbmp, l))) {
                    if (!um_count++)
                        um_first = l;
                    um_fixable &= l < c->lcnbmp_bits;
                }
            }
        }
        free(rl);
    }
    if (um_count)
        report(c, UE_CHK_CLUSTERS_UNMARKED, um_fixable, owner, 0, um_first, um_count, NULL,
               "%llu cluster%s in use (from %llu) %s free in $Bitmap", (unsigned long long)um_count,
               um_count == 1 ? "" : "s", (unsigned long long)um_first, um_count == 1 ? "is" : "are");
    if (sh_count && c->nshared < MAX_SHARED)
        c->shared[c->nshared++] = (shared_t){ .rec = owner, .lcn = sh_first, .count = sh_count, .other = UINT64_MAX };
    else if (sh_count)
        report(c, UE_CHK_CLUSTERS_SHARED, false, owner, UINT64_MAX, sh_first, sh_count, NULL,
               "%llu cluster%s (from %llu) also belong%s to another file", (unsigned long long)sh_count,
               sh_count == 1 ? "" : "s", (unsigned long long)sh_first, sh_count == 1 ? "s" : "");
}

static void pass1(chk_t *c, uint64_t rec, MFT_RECORD *m, int rerr)
{
    bool marked = rec < c->mftbmp_bits && has(c->mftbmp, rec);
    bool in_use = !rerr || rerr == EIO ? (m->flags & MFT_RECORD_IN_USE) != 0 : false;

    // Reported by the directory entry that points at it, with its path, or after the
    // directories (bad_records) when none does.
    if ((rerr == EIO && (in_use || marked)) || (rerr && marked && ntfs_is_baad_record(m->magic))) {
        mark(c->bad, rec);
        return;
    }
    if (rerr || !in_use) {
        if (marked)
            c->res->lost_records++;
        return;
    }
    mark(c->inuse, rec);
    c->seq[rec] = le16_to_cpu(m->sequence_number);
    c->res->records_in_use++;
    if (!MREF_LE(m->base_mft_record))
        mark(c->base, rec);
    if (!marked)
        report(c, UE_CHK_RECORD_UNMARKED, rec < c->mftbmp_bits, rec, 0, 0, 0, NULL,
               "record %llu is in use but free in $MFT's bitmap", (unsigned long long)rec);
    if (ntfs_mft_record_check(c->vol, rec, m)) {
        report(c, UE_CHK_BAD_RECORD, false, rec, 0, 0, 0, NULL, "record %llu has damaged attributes",
               (unsigned long long)rec);
        return;
    }
    if (has(c->base, rec) && (m->flags & MFT_RECORD_IS_DIRECTORY))
        mark(c->isdir, rec);
    scan_runs(c, rec, m);
}

// Pass 2: the first owner of each shared run is a record other than the one that found it.
static void pass2(chk_t *c, uint64_t rec, MFT_RECORD *m, int rerr)
{
    uint64_t owner;

    if (rerr || !(m->flags & MFT_RECORD_IN_USE) || ntfs_mft_record_check(c->vol, rec, m))
        return;
    owner = MREF_LE(m->base_mft_record) ? MREF_LE(m->base_mft_record) : rec;
    for (ATTR_RECORD *a = next_attr(c, m, NULL); a; a = next_attr(c, m, a)) {
        runlist_element *rl = a->non_resident ? ntfs_mapping_pairs_decompress(c->vol, a, NULL) : NULL;
        for (runlist_element *r = rl; r && r->length; r++) {
            if (r->lcn < 0)
                continue;
            for (size_t i = 0; i < c->nshared; i++) {
                shared_t *s = &c->shared[i];
                if (s->other == UINT64_MAX && s->rec != owner && s->lcn >= (uint64_t)r->lcn &&
                    s->lcn < (uint64_t)r->lcn + (uint64_t)r->length)
                    s->other = owner;
            }
        }
        free(rl);
    }
}

// Calls fn for every record of the MFT. A chunk that cannot be read is read record by record.
static int each_record(chk_t *c, void (*fn)(chk_t *, uint64_t, MFT_RECORD *, int))
{
    for (uint64_t first = 0; first < c->nrec; first += CHUNK_RECORDS) {
        uint64_t n = c->nrec - first < CHUNK_RECORDS ? c->nrec - first : CHUNK_RECORDS;
        s64 bytes = (s64)(n * c->recsize);
        bool chunk_ok = ntfs_attr_pread(c->vol->mft_na, (s64)(first * c->recsize), bytes, c->rbuf) == bytes;
        if (c->e->dev_ctx->failed)
            return EIO;
        for (uint64_t i = 0; i < n; i++) {
            u8 *b = c->rbuf + i * c->recsize;
            MFT_RECORD *m = (MFT_RECORD *)b;
            int rerr = 0;
            if (!chunk_ok && ntfs_attr_pread(c->vol->mft_na, (s64)((first + i) * c->recsize), c->recsize, b) !=
                                 c->recsize) {
                memset(b, 0, c->recsize);     // unreadable: reported if the bitmap marks it
                rerr = EIO;
            } else if (!ntfs_is_file_record(m->magic))
                rerr = EINVAL;
            else if (ntfs_mst_post_read_fixup_warn((NTFS_RECORD *)b, c->recsize, FALSE))
                rerr = EIO;
            fn(c, first + i, m, rerr);
        }
    }
    return c->e->dev_ctx->failed ? EIO : 0;
}

static void bad_record(chk_t *c, uint64_t rec, const char *path)
{
    c->bad[rec >> 3] &= (u8)~(1u << (rec & 7));
    report(c, UE_CHK_BAD_RECORD, false, rec, 0, 0, 0, path,
           "record %llu cannot be read (a torn or failed write, or marked bad)", (unsigned long long)rec);
}

typedef struct {
    chk_t *c;
    uint64_t dir;
    char path[1024];                  // the directory's, built at its first problem
} dir_ctx_t;

static const char *entry_path(dir_ctx_t *d, char *buf, size_t cap, const ntfschar *name, int name_len)
{
    char *utf8 = NULL;

    if (!d->c->fn)
        return "";
    if (!d->path[0])
        rec_path(d->c, d->dir, d->path, sizeof(d->path));
    if (ntfs_ucstombs(name, name_len, &utf8, 0) < 0)
        utf8 = NULL;
    snprintf(buf, cap, "%s%s%s", d->path, strcmp(d->path, "/") ? "/" : "", utf8 ? utf8 : "?");
    free(utf8);
    return buf;
}

static int check_entry(void *ctx, const ntfschar *name, const int name_len, const int name_type, const s64 pos,
                       const MFT_REF mref, const unsigned dt_type)
{
    dir_ctx_t *d = ctx;
    chk_t *c = d->c;
    uint64_t rec = MREF(mref);
    char path[1024];

    (void)dt_type;
    if (pos < 2)
        return 0;                     // ntfs_readdir's own "." and ".."
    c->res->entries++;
    if (rec < c->nrec && has(c->bad, rec)) {
        if (name_type != FILE_NAME_DOS)
            bad_record(c, rec, entry_path(d, path, sizeof(path), name, name_len));
        return 0;
    }
    if (rec >= c->nrec || !has(c->inuse, rec) || !has(c->base, rec)) {
        if (name_type != FILE_NAME_DOS)
            report(c, UE_CHK_ENTRY_FREE_RECORD, false, d->dir, rec, 0, 0,
                   entry_path(d, path, sizeof(path), name, name_len), "the entry points at record %llu, %s",
                   (unsigned long long)rec,
                   rec >= c->nrec ? "past the end of the MFT"
                   : has(c->inuse, rec) ? "an extension record" : "which is not in use");
        return 0;
    }
    if (MSEQNO(mref) && MSEQNO(mref) != c->seq[rec]) {
        if (name_type != FILE_NAME_DOS)
            report(c, UE_CHK_ENTRY_WRONG_SEQ, false, d->dir, rec, 0, 0,
                   entry_path(d, path, sizeof(path), name, name_len),
                   "the entry points at record %llu with sequence %u; the record has %u", (unsigned long long)rec,
                   (unsigned)MSEQNO(mref), (unsigned)c->seq[rec]);
        return 0;
    }
    mark(c->refd, rec);
    return 0;
}

static int check_dirs(chk_t *c)
{
    for (uint64_t rec = 0; rec < c->nrec; rec++) {
        if (!has(c->isdir, rec))
            continue;
        dir_ctx_t d = { .c = c, .dir = rec };
        ntfs_inode *ni = ue_iopen_rec(c->e, rec);
        c->res->directories++;
        if (!ni) {
            report(c, UE_CHK_INDEX_UNREADABLE, false, rec, 0, 0, 0, NULL, "the directory cannot be opened: %s",
                   strerror(errno ? errno : EIO));
        } else {
            // Muted: ntfs_readdir logs an error for each system entry outside the metafiles
            // ($Extend's $Quota, $ObjId, ...) whose type it cannot tell, and a read failure is
            // reported here anyway.
            u32 muted = ntfs_log_clear_levels(NTFS_LOG_LEVEL_ERROR | NTFS_LOG_LEVEL_PERROR);
            s64 pos = 0;
            int rerr = ntfs_readdir(ni, &pos, &d, check_entry) ? (errno ? errno : EIO) : 0;
            ntfs_log_set_levels(muted & (NTFS_LOG_LEVEL_ERROR | NTFS_LOG_LEVEL_PERROR));
            if (rerr)
                report(c, UE_CHK_INDEX_UNREADABLE, false, rec, 0, 0, 0, NULL,
                       "the directory's index cannot be read (%s); files listed in it may be missing",
                       strerror(rerr));
            ue_iput(ni);
        }
        if (c->e->dev_ctx->failed)
            return EIO;
    }
    return 0;
}

static int chk_run(chk_t *c)
{
    int err;

    if ((err = each_record(c, pass1)))
        return err;
    if (c->nshared && (err = each_record(c, pass2)))
        return err;
    for (size_t i = 0; i < c->nshared; i++) {
        shared_t *s = &c->shared[i];
        report(c, UE_CHK_CLUSTERS_SHARED, false, s->rec, s->other, s->lcn, s->count, NULL,
               "%llu cluster%s (from %llu) also belong%s to record %llu", (unsigned long long)s->count,
               s->count == 1 ? "" : "s", (unsigned long long)s->lcn, s->count == 1 ? "s" : "",
               (unsigned long long)s->other);
    }
    for (uint64_t l = 0; l < c->ncl && l < c->lcnbmp_bits; l++)
        if (has(c->lcnbmp, l) && !has(c->used, l))
            c->res->lost_clusters++;
    if ((err = check_dirs(c)))
        return err;
    for (uint64_t rec = 0; rec < c->nrec; rec++)
        if (has(c->bad, rec))
            bad_record(c, rec, "");
    for (uint64_t rec = 0; rec < c->nrec; rec++)
        if (rec != FILE_root && has(c->named, rec) && has(c->inuse, rec) && has(c->base, rec) && !has(c->refd, rec))
            report(c, UE_CHK_ORPHAN, false, rec, 0, 0, 0, NULL,
                   "record %llu has a name but no directory lists it", (unsigned long long)rec);
    return 0;
}

// Sets the bits of na that want has and have lacks, in runs.
static int set_missing(ntfs_attr *na, const u8 *want, const u8 *have, uint64_t have_bits, uint64_t n, uint64_t *done)
{
    for (uint64_t i = 0; i < n && i < have_bits;) {
        if (!has(want, i) || has(have, i)) {
            i++;
            continue;
        }
        uint64_t j = i;
        while (j < n && j < have_bits && has(want, j) && !has(have, j))
            j++;
        if (ntfs_bitmap_set_run(na, (s64)i, (s64)(j - i)))
            return errno ? errno : EIO;
        *done += j - i;
        i = j;
    }
    return 0;
}

static int chk_repair(chk_t *c, uint64_t *done)
{
    uint64_t clusters = 0;
    int err;

    if ((err = set_missing(c->vol->mftbmp_na, c->inuse, c->mftbmp, c->mftbmp_bits, c->nrec, done)) ||
        (err = set_missing(c->vol->lcnbmp_na, c->used, c->lcnbmp, c->lcnbmp_bits, c->ncl, &clusters)))
        return err;
    *done += clusters;
    if (c->vol->free_clusters >= (s64)clusters)
        c->vol->free_clusters -= (s64)clusters;
    return c->e->dev_ctx->failed ? EIO : 0;
}

// Checks an open volume; with repair (read-write), repairs when every problem is repairable,
// then checks again: res->remaining is what that last check found.
static int check_volume(ue_engine_t *e, ue_check_fn fn, void *ctx, ue_check_result_t *res, bool repair)
{
    ue_check_result_t again;
    chk_t c;
    int err;

    if ((err = chk_init(&c, e, fn, ctx, res)))
        return err;
    err = chk_run(&c);
    res->remaining = res->problems;
    if (!err && repair && res->problems && res->problems == res->repairable) {
        res->repair_ran = true;
        err = chk_repair(&c, &res->repaired);
        chk_free(&c);
        if (!err && !(err = chk_init(&c, e, NULL, NULL, &again))) {
            err = chk_run(&c);
            res->remaining = again.problems;
        }
    }
    chk_free(&c);
    return err;
}

// libntfs-3g's mount compares $MFTMirr with $MFT's first records and refuses the volume when
// they differ, so a write cut off between the two leaves a volume nothing on the Mac can open.
// Returns the first record that differs plus one, or 0.
static uint64_t mirror_differs(const ue_io_t *io)
{
    NTFS_BOOT_SECTOR bs;
    uint64_t found = 0;

    if (io->pread(io->ctx, &bs, sizeof(bs), 0) || !ntfs_boot_sector_is_ntfs(&bs))
        return 0;
    u32 ss = le16_to_cpu(bs.bpb.bytes_per_sector), spc = bs.bpb.sectors_per_cluster;
    spc = spc > 0x80 ? 1u << (256 - spc) : spc;
    uint64_t cs = (uint64_t)ss * spc;
    s8 c = bs.clusters_per_mft_record;
    uint64_t rs = c < 0 ? (c > -31 ? 1ull << -c : 0) : (uint64_t)c * cs;
    if (!rs || rs > 65536 || (rs & (rs - 1)) || rs < NTFS_BLOCK_SIZE || !cs || cs > (2u << 20))
        return 0;
    uint64_t n = cs <= 4 * rs ? 4 : cs / rs;
    s64 mft = sle64_to_cpu(bs.mft_lcn), mirr = sle64_to_cpu(bs.mftmirr_lcn);
    if (mft < 0 || mirr < 0 || (uint64_t)mft > io->size_bytes / cs || (uint64_t)mirr > io->size_bytes / cs ||
        ((uint64_t)mft + 1) * cs + n * rs > io->size_bytes || ((uint64_t)mirr + 1) * cs + n * rs > io->size_bytes)
        return 0;
    u8 *a = malloc(n * rs), *b = malloc(n * rs);
    if (a && b && !io->pread(io->ctx, a, n * rs, (uint64_t)mft * cs) &&
        !io->pread(io->ctx, b, n * rs, (uint64_t)mirr * cs)) {
        for (uint64_t i = 0; i < n && !found; i++) {
            MFT_RECORD *ma = (MFT_RECORD *)(a + i * rs), *mb = (MFT_RECORD *)(b + i * rs);
            bool ta = ntfs_mst_post_read_fixup_warn((NTFS_RECORD *)ma, (u32)rs, FALSE) != 0;
            bool tb = ntfs_mst_post_read_fixup_warn((NTFS_RECORD *)mb, (u32)rs, FALSE) != 0;
            u32 used = le32_to_cpu(ma->bytes_in_use);
            if (ta || tb || used <= sizeof(MFT_RECORD) || used > rs || memcmp(ma, mb, used))
                found = i + 1;
        }
    }
    free(a);
    free(b);
    return found;
}

int ue_check(const ue_io_t *io, uint32_t flags, ue_check_fn fn, void *ctx, ue_check_result_t *out)
{
    bool repair = (flags & UE_CHECK_REPAIR) != 0, dirty;
    ue_check_result_t rw;
    ue_engine_t *e;
    ue_probe_t p;
    int err, cerr;

    if (!io || !out)
        return EINVAL;
    memset(out, 0, sizeof(*out));
    if ((err = ue_probe(io, &p)))
        return err;
    if (p.state == UE_VOL_NOT_NTFS)
        return EINVAL;
    if (p.state == UE_VOL_CORRUPT) {
        uint64_t rec = mirror_differs(io);
        if (!rec)
            return EIO;
        ue_check_problem_t mp = { .kind = UE_CHK_MFT_MIRROR, .rec = rec - 1 };
        snprintf(mp.path, sizeof(mp.path), "/$MFTMirr");
        snprintf(mp.detail, sizeof(mp.detail),
                 "$MFTMirr does not match $MFT at record %llu (a write cut off between the two): the volume "
                 "cannot be opened until Windows' chkdsk repairs it", (unsigned long long)(rec - 1));
        out->state = p.state;
        out->problems = out->remaining = out->by_kind[UE_CHK_MFT_MIRROR] = 1;
        if (fn)
            fn(ctx, &mp);
        return 0;                     // nothing is repaired here
    }
    if (repair && (p.state == UE_VOL_HIBERNATED || p.state == UE_VOL_UNCLEAN_LOG))
        return EPERM;
    if (repair && (!io->writable || !io->pwrite))
        return EROFS;
    dirty = p.state == UE_VOL_DIRTY;

    // The check itself writes nothing.
    if ((err = ue_open(io, UE_OPEN_RDONLY, &e, NULL)))
        return err;
    err = check_volume(e, fn, ctx, out, false);
    cerr = ue_close(e);
    out->state = p.state;
    if (err || cerr)
        return err ? err : cerr;
    if (!repair || (out->problems ? out->problems != out->repairable : !dirty))
        return 0;

    // Only now, with nothing found that a repair could make worse, is the volume opened for
    // writing. It is checked again there (opening it read-write may sweep crash leftovers in
    // the root, D-65), repaired, and checked once more.
    if ((err = ue_open(io, 0, &e, NULL)))
        return err;
    err = check_volume(e, NULL, NULL, &rw, true);
    // Clear the mark only when the last check found nothing; otherwise it stays, or is left
    // set if this open set it (D-86).
    e->marked_dirty = !err && rw.remaining == 0;
    cerr = ue_close(e);
    out->repair_ran = rw.repair_ran;
    out->repaired = rw.repaired;
    out->remaining = err ? out->problems : rw.remaining;
    out->dirty_cleared = dirty && !err && !cerr && rw.remaining == 0;
    return err ? err : cerr;
}
