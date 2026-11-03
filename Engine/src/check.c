// SPDX-License-Identifier: GPL-3.0-or-later
// Check Disk (D-93): a read-only check of the MFT, the two bitmaps and the directory indexes,
// and the repairs that cannot overwrite anything: setting bitmap bits of what is in use,
// removing folder entries that lead to no file (D-96), putting files no folder lists back in
// a folder (D-99), and copying $MFT's first records over a $MFTMirr a cut left behind (D-98).
//
// The MFT is read raw, record by record, so a damaged record is reported instead of being
// opened. Pass 1 records which records are in use, marks every cluster their runlists use and
// compares both with the bitmaps on the disk. Pass 2 (only when clusters are shared) finds the
// first owner of each shared run. Then every directory is listed with ntfs_readdir, which
// reads every index block its $I30 bitmap marks in use, and every entry is compared with
// pass 1. Last, named files no directory listed are orphans.
//
// A folder entry leads to no file when its record is not in use, has another sequence number,
// or has no $FILE_NAME in that folder: what a write cut off between a folder's index and the
// new file's record leaves. Its removal touches only the folder's index, as chkdsk's does.
//
// An orphan, a whole record with a name that no folder lists, is what a write cut off between
// a new file's record and its folder's index leaves (D-97 writes the record first). It goes
// back where its $FILE_NAMEs say, when every one of those folders is in use, reachable from the
// root and has none of its names; otherwise its names are replaced by one in /found.NNN, as
// chkdsk does. Its data is never touched.
//
// Space marked used that nothing uses (records past $MFT's end included) is freed by a repair
// once the volume checks clean (D-102), and a file whose own $SECURITY_DESCRIPTOR is not a
// valid descriptor gets the one Unlatch gives every new file.
#include "internal.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ntfs-3g/acls.h>
#include <ntfs-3g/attrib.h>
#include <ntfs-3g/bitmap.h>
#include <ntfs-3g/bootsect.h>
#include <ntfs-3g/dir.h>
#include <ntfs-3g/index.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/layout.h>
#include <ntfs-3g/logging.h>
#include <ntfs-3g/mft.h>
#include <ntfs-3g/mst.h>
#include <ntfs-3g/runlist.h>
#include <ntfs-3g/security.h>
#include <ntfs-3g/unistr.h>

#define CHUNK_RECORDS 256
#define MAX_SHARED 1024               // shared runs whose first owner pass 2 looks for
#define MAX_DEPTH 256                 // path components

typedef struct {
    uint64_t rec, lcn, count, other;
} shared_t;

// A folder entry that leads to no file, kept for the repair (D-96): the entry is looked up
// again by its exact name and removed only if it still points at mref.
typedef struct {
    uint64_t dir;
    MFT_REF mref;
    u8 name_type, name_len;
    ntfschar name[NTFS_MAX_NAME_LEN];
} dangling_t;

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
    u8 *multi;                        // names in more than one folder, or unknown: read the record
    u16 *seq;
    uint64_t *parent;                 // the folder of a record's first $FILE_NAME; UINT64_MAX: none
    u8 *flawed;                       // damaged, or shares clusters: an orphan left alone (D-99)
    dangling_t *dangling;
    size_t ndangling, cap_dangling;
    uint64_t *orphans;                // repairable orphans, for the repair
    size_t norphans;
    u8 *sd_late;                      // a non-resident $SECURITY_DESCRIPTOR, read after pass 1
    uint64_t *bad_sd;                 // records whose $SECURITY_DESCRIPTOR is not valid
    size_t nbad_sd;
    struct sid_use { u32 id; uint64_t rec; } *sids;   // security_ids in use, and a record that has each
    size_t nsids;
    bool nomem;                       // keep_dangling ran out of memory (ntfs_readdir stops quietly)
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
    case UE_CHK_ENTRY_WRONG_PARENT: return "entry_wrong_parent";
    case UE_CHK_BAD_SECURITY: return "bad_security";
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
    free(c->multi);
    free(c->seq);
    free(c->parent);
    free(c->flawed);
    free(c->dangling);
    free(c->orphans);
    free(c->sd_late);
    free(c->bad_sd);
    free(c->sids);
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
    c->multi = calloc(rbytes, 1);
    c->flawed = calloc(rbytes, 1);
    c->sd_late = calloc(rbytes, 1);
    c->seq = calloc((size_t)c->nrec + 1, sizeof(*c->seq));
    c->parent = malloc(((size_t)c->nrec + 1) * sizeof(*c->parent));
    c->shared = calloc(MAX_SHARED, sizeof(*c->shared));
    if (!c->rbuf || !c->pbuf || !c->inuse || !c->base || !c->isdir || !c->named || !c->refd || !c->bad || !c->used ||
        !c->multi || !c->flawed || !c->sd_late || !c->seq || !c->parent || !c->shared) {
        chk_free(c);
        return ENOMEM;
    }
    memset(c->parent, 0xff, ((size_t)c->nrec + 1) * sizeof(*c->parent));
    // All of $MFT's bitmap: bits past its last record count as lost (D-102).
    uint64_t mbits = (uint64_t)vol->mftbmp_na->data_size * 8;
    if ((err = read_bitmap(vol->mftbmp_na, mbits > c->nrec ? mbits : c->nrec, &c->mftbmp, &c->mftbmp_bits)) ||
        (err = read_bitmap(vol->lcnbmp_na, c->ncl, &c->lcnbmp, &c->lcnbmp_bits))) {
        chk_free(c);
        return err;
    }
    // Every entry, metafiles and hidden files included (ue_open hides the metafiles).
    ntfs_set_shown_files(vol, TRUE, TRUE, FALSE);
    return 0;
}

// An extent record belongs to its base record. Test the whole reference: $MFT's own extents
// (records 16-23, written when $MFT fragments on a nearly full volume) point at record 0, and
// the record number alone, 0, read as "no base" made them nameless orphans.
static bool is_extent(const MFT_RECORD *m)
{
    return m->base_mft_record != const_cpu_to_le64(0);
}

// Appends to a list that grows at 0, 1, 3, 7, ... entries.
static int grow(void **list, size_t n, size_t size)
{
    if (n & (n + 1) && *list)
        return 0;
    void *g = realloc(*list, 2 * (n + 1) * size);
    if (!g)
        return ENOMEM;
    *list = g;
    return 0;
}

static void bad_security(chk_t *c, uint64_t rec)
{
    if (!grow((void **)&c->bad_sd, c->nbad_sd, sizeof(*c->bad_sd)))
        c->bad_sd[c->nbad_sd++] = rec;
    else
        c->nomem = true;
    report(c, UE_CHK_BAD_SECURITY, true, rec, 0, 0, 0, NULL,
           "the file's security descriptor is not valid (written after its record, or damaged)");
}

// Security, from pass 1: a resident $SECURITY_DESCRIPTOR is checked now, a non-resident one
// after pass 1, and each security_id in $STANDARD_INFORMATION against $Secure's $SII.
static void note_security(chk_t *c, uint64_t owner, ATTR_RECORD *a)
{
    u32 len = le32_to_cpu(a->length);

    if (a->type == AT_SECURITY_DESCRIPTOR && a->non_resident) {
        mark(c->sd_late, owner);
    } else if (a->type == AT_SECURITY_DESCRIPTOR) {
        u32 voff = le16_to_cpu(a->value_offset), vlen = le32_to_cpu(a->value_length);
        if ((uint64_t)voff + vlen > len || !ntfs_valid_descr((const char *)a + voff, vlen))
            bad_security(c, owner);
    } else if (a->type == AT_STANDARD_INFORMATION && !a->non_resident) {
        u32 voff = le16_to_cpu(a->value_offset), vlen = le32_to_cpu(a->value_length);
        if (vlen < offsetof(STANDARD_INFORMATION, v3_end) || (uint64_t)voff + vlen > len)
            return;                   // NTFS 1.2 layout: no security_id
        u32 id = le32_to_cpu(((STANDARD_INFORMATION *)((u8 *)a + voff))->security_id);
        if (!id)
            return;
        for (size_t i = 0; i < c->nsids; i++)
            if (c->sids[i].id == id)
                return;
        if (!grow((void **)&c->sids, c->nsids, sizeof(*c->sids)))
            c->sids[c->nsids++] = (struct sid_use){ .id = id, .rec = owner };
        else
            c->nomem = true;
    }
}

// A $SECURITY_DESCRIPTOR value read through libntfs-3g; true when it is a valid descriptor
// (or cannot be read: that is reported as a bad runlist or record instead).
static bool sd_valid(ntfs_inode *ni)
{
    ntfs_attr *na = ntfs_attr_open(ni, AT_SECURITY_DESCRIPTOR, AT_UNNAMED, 0);
    bool ok = true;

    if (!na)
        return true;
    if (na->data_size <= 0)
        ok = false;
    else if (na->data_size <= 65536) {
        char *b = malloc((size_t)na->data_size);
        if (b && ntfs_attr_pread(na, 0, na->data_size, b) == na->data_size)
            ok = ntfs_valid_descr(b, (unsigned)na->data_size);
        free(b);
    }
    ntfs_attr_close(na);
    return ok;
}

// After the directories: the non-resident descriptors, and the security_ids $SII must hold.
static int check_security(chk_t *c)
{
    for (uint64_t rec = 0; rec < c->nrec; rec++) {
        if (!has(c->sd_late, rec) || !has(c->inuse, rec) || !has(c->base, rec) || has(c->flawed, rec))
            continue;
        ntfs_inode *ni = ue_iopen_rec(c->e, rec);
        if (!ni)
            continue;                 // reported by the directory walk or pass 1
        bool ok = sd_valid(ni);
        ue_iput(ni);
        if (!ok)
            bad_security(c, rec);
    }
    if (!c->nsids)
        return c->nomem ? ENOMEM : 0;
    ntfs_inode *sec = ue_iopen_rec(c->e, FILE_Secure);
    ntfs_index_context *icx = sec ? ntfs_index_ctx_get(sec, NTFS_INDEX_SII, 4) : NULL;
    for (size_t i = 0; icx && i < c->nsids; i++) {
        le32 key = cpu_to_le32(c->sids[i].id);
        if (ntfs_index_lookup(&key, sizeof(key), icx))
            report(c, UE_CHK_BAD_SECURITY, false, c->sids[i].rec, 0, 0, 0, NULL,
                   "security_id %u is not in $Secure (files with it have no valid security descriptor)",
                   (unsigned)c->sids[i].id);
        ntfs_index_ctx_reinit(icx);
    }
    if (icx)
        ntfs_index_ctx_put(icx);
    if (sec)
        ue_iput(sec);
    return c->nomem ? ENOMEM : 0;
}

// Pass 1 for one in-use record that passed ntfs_mft_record_check: its names and clusters.
static void scan_runs(chk_t *c, uint64_t rec, MFT_RECORD *m)
{
    uint64_t owner = is_extent(m) ? MREF_LE(m->base_mft_record) : rec;
    uint64_t um_first = 0, um_count = 0, sh_first = 0, sh_count = 0;
    bool um_fixable = true;

    for (ATTR_RECORD *a = next_attr(c, m, NULL); a; a = next_attr(c, m, a)) {
        FILE_NAME_ATTR *fn = a->type == AT_FILE_NAME && owner < c->nrec ? file_name(a) : NULL;
        if (fn) {
            uint64_t dir = MREF_LE(fn->parent_directory);
            mark(c->named, owner);
            if (c->parent[owner] == UINT64_MAX)
                c->parent[owner] = dir;
            else if (c->parent[owner] != dir)
                mark(c->multi, owner);
        }
        if (owner < c->nrec)
            note_security(c, owner, a);
        if (!a->non_resident)
            continue;
        runlist_element *rl = ntfs_mapping_pairs_decompress(c->vol, a, NULL);
        if (!rl) {
            mark(c->flawed, owner);
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
                mark(c->flawed, owner);
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
    if (sh_count)
        mark(c->flawed, owner);
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
    if (!is_extent(m))
        mark(c->base, rec);
    if (!marked)
        report(c, UE_CHK_RECORD_UNMARKED, rec < c->mftbmp_bits, rec, 0, 0, 0, NULL,
               "record %llu is in use but free in $MFT's bitmap", (unsigned long long)rec);
    if (ntfs_mft_record_check(c->vol, rec, m)) {
        report(c, UE_CHK_BAD_RECORD, false, rec, 0, 0, 0, NULL, "record %llu has damaged attributes",
               (unsigned long long)rec);
        uint64_t owner = is_extent(m) ? MREF_LE(m->base_mft_record) : rec;
        mark(c->multi, rec);          // its names are unknown: names_dir must not judge by them
        mark(c->flawed, rec);
        if (owner < c->nrec) {
            mark(c->multi, owner);
            mark(c->flawed, owner);
        }
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
    owner = is_extent(m) ? MREF_LE(m->base_mft_record) : rec;
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

// True when record rec (in use, a base record) has a $FILE_NAME in folder dir, or when that
// cannot be told from the record alone (a damaged record, or names in an extent record).
static bool names_dir(chk_t *c, uint64_t rec, uint64_t dir)
{
    MFT_RECORD *m = (MFT_RECORD *)c->pbuf;

    if (!has(c->multi, rec))
        return c->parent[rec] == dir;
    if (read_record(c, rec, c->pbuf) || ntfs_mft_record_check(c->vol, rec, m))
        return true;
    for (ATTR_RECORD *a = next_attr(c, m, NULL); a; a = next_attr(c, m, a)) {
        FILE_NAME_ATTR *fn = file_name(a);
        if (a->type == AT_ATTRIBUTE_LIST || (fn && MREF_LE(fn->parent_directory) == dir))
            return true;
    }
    return false;
}

// Keeps an entry that leads to no file for the repair. A DOS name is kept too (and not
// reported): it is the other half of a Win32 name, and both entries go.
static int keep_dangling(chk_t *c, uint64_t dir, const ntfschar *name, int name_len, int name_type, MFT_REF mref)
{
    if (name_len <= 0 || name_len > NTFS_MAX_NAME_LEN)
        return 0;
    if (c->ndangling == c->cap_dangling) {
        size_t cap = c->cap_dangling ? 2 * c->cap_dangling : 16;
        dangling_t *n = realloc(c->dangling, cap * sizeof(*n));
        if (!n) {
            c->nomem = true;
            return -1;                // stops ntfs_readdir
        }
        c->dangling = n;
        c->cap_dangling = cap;
    }
    dangling_t *x = &c->dangling[c->ndangling++];
    x->dir = dir;
    x->mref = mref;
    x->name_type = (u8)name_type;
    x->name_len = (u8)name_len;
    memcpy(x->name, name, (size_t)name_len * sizeof(ntfschar));
    return 0;
}

static int keep_orphan(chk_t *c, uint64_t rec)
{
    if (grow((void **)&c->orphans, c->norphans, sizeof(*c->orphans)))
        return ENOMEM;
    c->orphans[c->norphans++] = rec;
    return 0;
}

static int check_entry(void *ctx, const ntfschar *name, const int name_len, const int name_type, const s64 pos,
                       const MFT_REF mref, const unsigned dt_type)
{
    dir_ctx_t *d = ctx;
    chk_t *c = d->c;
    uint64_t rec = MREF(mref);
    bool shown = name_type != FILE_NAME_DOS;
    char path[1024];

    (void)dt_type;
    if (pos < 2)
        return 0;                     // ntfs_readdir's own "." and ".."
    c->res->entries++;
    if (rec < c->nrec && has(c->bad, rec)) {
        if (shown)
            bad_record(c, rec, entry_path(d, path, sizeof(path), name, name_len));
        return 0;
    }
    // Past the end of the MFT ($MFT may have lost an extent) and extension records are left
    // for chkdsk; a record not in use is free, or was freed and never written again.
    if (rec >= c->nrec || !has(c->inuse, rec) || !has(c->base, rec)) {
        bool free_rec = rec < c->nrec && !has(c->inuse, rec);
        if (shown)
            report(c, UE_CHK_ENTRY_FREE_RECORD, free_rec, d->dir, rec, 0, 0,
                   entry_path(d, path, sizeof(path), name, name_len), "the entry points at record %llu, %s",
                   (unsigned long long)rec,
                   rec >= c->nrec ? "past the end of the MFT"
                   : has(c->inuse, rec) ? "an extension record" : "which is not in use");
        return free_rec ? keep_dangling(c, d->dir, name, name_len, name_type, mref) : 0;
    }
    // The record was reused: by another file (listed in its own folder, or an orphan, which
    // keeps the repair from running), or by this one (then also an orphan).
    if (MSEQNO(mref) && MSEQNO(mref) != c->seq[rec]) {
        if (shown)
            report(c, UE_CHK_ENTRY_WRONG_SEQ, true, d->dir, rec, 0, 0,
                   entry_path(d, path, sizeof(path), name, name_len),
                   "the entry points at record %llu with sequence %u; the record has %u", (unsigned long long)rec,
                   (unsigned)MSEQNO(mref), (unsigned)c->seq[rec]);
        return keep_dangling(c, d->dir, name, name_len, name_type, mref);
    }
    if (!names_dir(c, rec, d->dir)) {
        if (shown)
            report(c, UE_CHK_ENTRY_WRONG_PARENT, true, d->dir, rec, 0, 0,
                   entry_path(d, path, sizeof(path), name, name_len),
                   "the entry points at record %llu, %s", (unsigned long long)rec,
                   has(c->named, rec) ? "a file in another folder" : "which has no name");
        return keep_dangling(c, d->dir, name, name_len, name_type, mref);
    }
    mark(c->refd, rec);
    return 0;
}

// Checks the child pointers of a folder's index, which ntfs_readdir does not follow (it reads
// the blocks the $I30 bitmap marks): each must name a block inside $INDEX_ALLOCATION that the
// bitmap marks in use. A cut can leave a node pointing at a block whose record never grew to
// hold it; looking up any name past that pointer then fails. 0, or the bad VCN plus one.
static int64_t index_bad_child(chk_t *c, ntfs_inode *ni)
{
    ntfs_attr *ra = ntfs_attr_open(ni, AT_INDEX_ROOT, NTFS_INDEX_I30, 4), *ia = NULL;
    u8 *root = NULL, *blk = NULL, *bmp = NULL;
    int64_t bad = 0;
    s64 bmp_len = 0;
    u32 bs = 0, vbits = 0;
    uint64_t nblocks = 0;

    if (!ra)
        return 0;                     // readdir has reported it
    if (ra->data_size < (s64)sizeof(INDEX_ROOT) || ra->data_size > c->recsize || !(root = malloc((size_t)ra->data_size)) ||
        ntfs_attr_pread(ra, 0, ra->data_size, root) != ra->data_size)
        goto out;
    INDEX_ROOT *ir = (INDEX_ROOT *)root;
    bs = le32_to_cpu(ir->index_block_size);
    if (bs < NTFS_BLOCK_SIZE || (bs & (bs - 1)) || bs > (1u << 20))
        goto out;
    vbits = c->vol->cluster_size <= bs ? c->vol->cluster_size_bits : NTFS_BLOCK_SIZE_BITS;
    if ((ia = ntfs_attr_open(ni, AT_INDEX_ALLOCATION, NTFS_INDEX_I30, 4))) {
        nblocks = (uint64_t)ia->data_size / bs;
        ntfs_attr *ba = ntfs_attr_open(ni, AT_BITMAP, NTFS_INDEX_I30, 4);
        if (ba) {
            bmp_len = ba->data_size;
            if (bmp_len > 0 && (!(bmp = calloc(1, (size_t)bmp_len)) || ntfs_attr_pread(ba, 0, bmp_len, bmp) != bmp_len))
                bmp_len = 0;
            ntfs_attr_close(ba);
        }
    }
    if (!(blk = malloc(bs)))
        goto out;
    // The root's entries, then every block in use: each entry with a child names a block.
    for (int64_t b = -1; !bad && b < (int64_t)nblocks; b++) {
        INDEX_HEADER *ih;
        u8 *end;
        if (b < 0) {
            ih = &ir->index;
            end = root + ra->data_size;
        } else {
            if ((uint64_t)b >= (uint64_t)bmp_len * 8 || !has(bmp, (uint64_t)b))
                continue;
            if (ntfs_attr_mst_pread(ia, (s64)b * bs, 1, bs, blk) != 1 ||
                !ntfs_is_indx_record(((INDEX_BLOCK *)blk)->magic))
                continue;             // readdir has reported it
            ih = &((INDEX_BLOCK *)blk)->index;
            end = blk + bs;
        }
        u8 *p = (u8 *)ih + le32_to_cpu(ih->entries_offset);
        u8 *stop = (u8 *)ih + le32_to_cpu(ih->index_length);
        if (stop > end)
            stop = end;
        while (p + sizeof(INDEX_ENTRY_HEADER) <= stop) {
            INDEX_ENTRY *ie = (INDEX_ENTRY *)p;
            u16 len = le16_to_cpu(ie->length);
            if (len < sizeof(INDEX_ENTRY_HEADER) + 8 || p + len > stop)
                break;
            if (ie->ie_flags & INDEX_ENTRY_NODE) {
                s64 vcn = sle64_to_cpu(*(sle64 *)(p + len - 8));
                uint64_t blkno = vcn < 0 ? UINT64_MAX : ((uint64_t)vcn << vbits) / bs;
                if (blkno >= nblocks || blkno >= (uint64_t)bmp_len * 8 || !has(bmp, blkno)) {
                    bad = vcn + 1;
                    break;
                }
            }
            if (ie->ie_flags & INDEX_ENTRY_END)
                break;
            p += len;
        }
    }
out:
    free(blk);
    free(bmp);
    free(root);
    if (ia)
        ntfs_attr_close(ia);
    ntfs_attr_close(ra);
    return bad;
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
            // Case-sensitive, so names come as they are on the disk (D-19): the repair looks
            // entries up by their exact names.
            u32 muted = ntfs_log_clear_levels(NTFS_LOG_LEVEL_ERROR | NTFS_LOG_LEVEL_PERROR);
            s64 pos = 0;
            NVolSetCaseSensitive(c->vol);
            int rerr = ntfs_readdir(ni, &pos, &d, check_entry) ? (errno ? errno : EIO) : 0;
            NVolClearCaseSensitive(c->vol);
            ntfs_log_set_levels(muted & (NTFS_LOG_LEVEL_ERROR | NTFS_LOG_LEVEL_PERROR));
            if (c->nomem) {
                ue_iput(ni);
                return ENOMEM;
            }
            int64_t bad = rerr ? 0 : index_bad_child(c, ni);
            if (rerr)
                report(c, UE_CHK_INDEX_UNREADABLE, false, rec, 0, 0, 0, NULL,
                       "the directory's index cannot be read (%s); files listed in it may be missing",
                       strerror(rerr));
            else if (bad)
                report(c, UE_CHK_INDEX_UNREADABLE, false, rec, 0, 0, 0, NULL,
                       "the directory's index points at index block %lld, which it does not hold; names "
                       "after it cannot be looked up", (long long)(bad - 1));
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
        if (s->other < c->nrec)
            mark(c->flawed, s->other);
        report(c, UE_CHK_CLUSTERS_SHARED, false, s->rec, s->other, s->lcn, s->count, NULL,
               "%llu cluster%s (from %llu) also belong%s to record %llu", (unsigned long long)s->count,
               s->count == 1 ? "" : "s", (unsigned long long)s->lcn, s->count == 1 ? "s" : "",
               (unsigned long long)s->other);
    }
    for (uint64_t l = 0; l < c->ncl && l < c->lcnbmp_bits; l++)
        if (has(c->lcnbmp, l) && !has(c->used, l))
            c->res->lost_clusters++;
    for (uint64_t rec = c->nrec; rec < c->mftbmp_bits; rec++)
        if (has(c->mftbmp, rec))
            c->res->lost_records++;   // past $MFT's end: an extension that never landed
    if ((err = check_dirs(c)) || (err = check_security(c)))
        return err;
    for (uint64_t rec = 0; rec < c->nrec; rec++)
        if (has(c->bad, rec))
            bad_record(c, rec, "");
    for (uint64_t rec = 0; rec < c->nrec; rec++) {
        if (rec == FILE_root || !has(c->named, rec) || !has(c->inuse, rec) || !has(c->base, rec) ||
            has(c->refd, rec))
            continue;
        // Only a record that passed pass 1 whole is put back (D-99); metafiles are chkdsk's.
        bool fixable = rec >= FILE_first_user && !has(c->flawed, rec);
        if (fixable && (err = keep_orphan(c, rec)))
            return err;
        report(c, UE_CHK_ORPHAN, fixable, rec, 0, 0, 0, NULL, "record %llu has a name but no directory lists it",
               (unsigned long long)rec);
    }
    return 0;
}

// True when folder dir is listed in a folder, and that one in another, up to the root.
static bool reaches_root(const chk_t *c, uint64_t dir)
{
    for (int depth = 0; depth < MAX_DEPTH; depth++) {
        if (dir == FILE_root)
            return true;
        if (dir >= c->nrec || !has(c->isdir, dir) || !has(c->refd, dir) || has(c->flawed, dir))
            return false;
        dir = c->parent[dir];
    }
    return false;
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

// Removes one entry from its folder's index, as ntfs_index_remove does, but only while the
// entry found by its exact name still points at the record it pointed at. The record itself
// is not touched (D-96).
static int remove_entry(chk_t *c, const dangling_t *x)
{
    size_t klen = sizeof(FILE_NAME_ATTR) + (size_t)x->name_len * sizeof(ntfschar);
    FILE_NAME_ATTR *key = calloc(1, klen);
    ntfs_index_context *icx = NULL;
    ntfs_inode *dir = NULL;
    int err = 0;

    if (!key)
        return ENOMEM;
    key->file_name_length = x->name_len;
    key->file_name_type = x->name_type;
    memcpy(key->file_name, x->name, (size_t)x->name_len * sizeof(ntfschar));
    if (!(dir = ue_iopen_rec(c->e, x->dir)) || !(icx = ntfs_index_ctx_get(dir, NTFS_INDEX_I30, 4))) {
        err = errno ? errno : EIO;
        goto out;
    }
    for (;;) {
        if (ntfs_index_lookup(key, (int)klen, icx)) {
            err = errno ? errno : EIO;
            break;
        }
        if (le64_to_cpu(icx->entry->indexed_file) != x->mref) {
            err = ESTALE;
            break;
        }
        int rc = ntfs_index_rm(icx);
        if (rc == STATUS_ERROR) {
            err = errno ? errno : EIO;
            break;
        }
        ntfs_inode_mark_dirty(icx->actx->ntfs_ino);
        if (rc == STATUS_OK)
            break;
        ntfs_index_ctx_reinit(icx);
    }
out:
    if (icx)
        ntfs_index_ctx_put(icx);
    if (dir && ue_iput(dir) && !err)
        err = errno ? errno : EIO;
    free(key);
    return err;
}

#define MAX_NAMES 16                  // names an orphan is put back under; more go to found

// The folder orphans go to when their own cannot take them: /found.000, or found.001, ...
// when that name is a file. Made when first needed.
static int found_dir(chk_t *c, uint64_t *dir)
{
    ue_attr_t a;
    char name[16];
    int err;

    for (int k = 0; k < 1000; k++) {
        snprintf(name, sizeof(name), "found.%03d", k);
        err = ue_lookup(c->e, UE_ROOT_INO, name, &a);
        if (err == ENOENT) {
            if ((err = ue_create(c->e, UE_ROOT_INO, name, UE_T_DIR, 0755, &a)))
                return err;
            ue_release(c->e, a.ino);
        } else if (err)
            return err;
        else if (a.type != UE_T_DIR)
            continue;
        *dir = MREF(a.ino);
        return 0;
    }
    return EEXIST;
}

// The $FILE_NAMEs of an open record, copied (they may be in extension records).
static int record_names(ntfs_inode *ni, FILE_NAME_ATTR **names, size_t *n)
{
    ntfs_attr_search_ctx *ctx = ntfs_attr_get_search_ctx(ni, NULL);
    int err = 0;

    *n = 0;
    if (!ctx)
        return errno ? errno : ENOMEM;
    while (!ntfs_attr_lookup(AT_FILE_NAME, AT_UNNAMED, 0, CASE_SENSITIVE, 0, NULL, 0, ctx)) {
        ATTR_RECORD *a = ctx->attr;
        FILE_NAME_ATTR *fn = file_name(a);
        if (!fn || *n == MAX_NAMES) {
            err = EINVAL;             // damaged, or more names than this puts back
            break;
        }
        size_t len = sizeof(FILE_NAME_ATTR) + (size_t)fn->file_name_length * sizeof(ntfschar);
        if (!(names[*n] = malloc(len))) {
            err = ENOMEM;
            break;
        }
        memcpy(names[(*n)++], fn, len);
    }
    if (!err && errno != ENOENT)
        err = errno ? errno : EIO;
    ntfs_attr_put_search_ctx(ctx);
    return err;
}

// Where an orphan's name lives now. 0: the folder can take it back (the name is free there),
// EEXIST: another file has the name, ESTALE: the folder lists this record after all, EINVAL:
// the folder is not one to put it in.
static int name_free(chk_t *c, uint64_t rec, const FILE_NAME_ATTR *fn)
{
    uint64_t dir = MREF_LE(fn->parent_directory);
    u16 seq = MSEQNO_LE(fn->parent_directory);
    ntfs_inode *dni;
    u64 found;

    if (dir == rec || dir >= c->nrec || !has(c->inuse, dir) || !has(c->base, dir) || !has(c->isdir, dir) ||
        (seq && seq != c->seq[dir]) || !reaches_root(c, dir))
        return EINVAL;
    if (!(dni = ue_iopen_rec(c->e, dir)))
        return errno ? errno : EIO;
    // Case-insensitive for Win32 and DOS names, as Windows looks them up.
    found = ntfs_inode_lookup_by_name(dni, fn->file_name, fn->file_name_length);
    int lerr = found == (u64)-1 ? errno : 0;
    int err = ue_iput(dni);
    if (err)
        return err;
    if (found != (u64)-1)
        return MREF(found) == rec ? ESTALE : EEXIST;
    return lerr == ENOENT || !lerr ? 0 : lerr;
}

// Adds the entry for name fn of record mref to its folder (as ntfs_link does, with the
// record's own $FILE_NAME as the key).
static int add_entry(chk_t *c, const FILE_NAME_ATTR *fn, MFT_REF mref)
{
    ntfs_inode *dni = ue_iopen_rec(c->e, MREF_LE(fn->parent_directory));
    int err = 0;

    if (!dni)
        return errno ? errno : EIO;
    if (ntfs_index_add_filename(dni, (FILE_NAME_ATTR *)fn, mref))
        err = errno ? errno : EIO;
    else
        ntfs_inode_mark_dirty(dni);
    int cerr = ue_iput(dni);
    return err ? err : cerr;
}

// Replaces the names of open record ni with one, "file<rec>" or "dir<rec>", in folder dir.
static int move_to_found(chk_t *c, ntfs_inode *ni, uint64_t dir)
{
    ntfs_attr_search_ctx *ctx = NULL;
    ntfs_inode *dni;
    ntfschar *u = NULL;
    char name[48];
    int len = 0, err = 0;

    if (!(dni = ue_iopen_rec(c->e, dir)))
        return errno ? errno : EIO;
    for (int k = 0; k < 1000 && !err; k++) {
        int n = snprintf(name, sizeof(name), "%s%llu", (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) ? "dir" : "file",
                         (unsigned long long)ni->mft_no);
        if (k)
            snprintf(name + n, sizeof(name) - (size_t)n, ".%d", k);
        free(u);
        u = NULL;
        if ((len = ntfs_mbstoucs(name, &u)) <= 0) {
            err = errno ? errno : EINVAL;
            break;
        }
        if (ntfs_inode_lookup_by_name(dni, u, len) == (u64)-1) {
            if (errno && errno != ENOENT)
                err = errno;
            break;
        }
        if (k == 999)
            err = EEXIST;
    }
    if (!err && !(ctx = ntfs_attr_get_search_ctx(ni, NULL)))
        err = errno ? errno : ENOMEM;
    else if (!err) {
        // Every old name goes, then the new one is linked: ntfs_link adds the entry and the
        // $FILE_NAME and counts the link.
        while (!err && !ntfs_attr_lookup(AT_FILE_NAME, AT_UNNAMED, 0, CASE_SENSITIVE, 0, NULL, 0, ctx)) {
            if (ntfs_attr_record_rm(ctx))
                err = errno ? errno : EIO;
            ntfs_attr_reinit_search_ctx(ctx);
        }
        ntfs_attr_put_search_ctx(ctx);
        if (!err) {
            ni->mrec->link_count = const_cpu_to_le16(0);
            if (ntfs_link(ni, dni, u, (u8)len))
                err = errno ? errno : EIO;
            ntfs_inode_mark_dirty(ni);
        }
    }
    free(u);
    int cerr = ue_iput(dni);
    return err ? err : cerr;
}

// Puts orphan rec back (D-99), but only while it is still one: in use, the sequence number
// pass 1 saw, a base record, and listed by none of its folders. Without found: EAGAIN when
// it cannot go back in place.
static int relink(chk_t *c, uint64_t rec, bool found, uint64_t *found_rec, bool *to_found)
{
    FILE_NAME_ATTR *names[MAX_NAMES];
    size_t n = 0;
    bool in_place = true;
    ntfs_inode *ni;
    int err = 0;

    *to_found = false;
    if (!(ni = ue_iopen_rec(c->e, rec)))
        return ENOENT;
    if (!(ni->mrec->flags & MFT_RECORD_IN_USE) || le16_to_cpu(ni->mrec->sequence_number) != c->seq[rec] ||
        is_extent(ni->mrec))
        err = ESTALE;
    else if ((err = record_names(ni, names, &n)) == EINVAL) {
        err = 0;                      // the names cannot all be put back: found
        in_place = false;
    } else if (!err && !n)
        err = ENOENT;
    for (size_t i = 0; !err && in_place && i < n; i++) {
        int r = name_free(c, rec, names[i]);
        if (r == EEXIST || r == EINVAL)
            in_place = false;
        else
            err = r;                  // ESTALE: listed after all
    }
    MFT_REF mref = MK_MREF(rec, c->seq[rec]);
    if (!err && !in_place && !found)
        err = EAGAIN;
    if (!err && in_place) {
        for (size_t i = 0; !err && i < n; i++)
            err = add_entry(c, names[i], mref);
    } else if (!err) {
        if (!*found_rec)
            err = found_dir(c, found_rec);
        if (!err)
            err = move_to_found(c, ni, *found_rec);
        *to_found = !err;
    }
    for (size_t i = 0; i < n; i++)
        free(names[i]);
    int cerr = ue_iput(ni);
    return err ? err : cerr;
}

// Gives record rec the descriptor ntfs_create gives a new file (everyone, full control, as
// every file Unlatch makes has), in place of a $SECURITY_DESCRIPTOR that is not valid, while
// it is still not valid (D-102). A file with a security_id keeps that and only loses the attribute.
static int fix_security(chk_t *c, uint64_t rec)
{
    ntfs_inode *ni = ue_iopen_rec(c->e, rec);
    ntfs_attr *na;
    int err = 0;

    if (!ni)
        return ENOENT;
    if (!(ni->mrec->flags & MFT_RECORD_IN_USE) || le16_to_cpu(ni->mrec->sequence_number) != c->seq[rec] ||
        sd_valid(ni))
        err = ESTALE;
    else if (!(na = ntfs_attr_open(ni, AT_SECURITY_DESCRIPTOR, AT_UNNAMED, 0)))
        err = errno ? errno : EIO;
    else {
        if (ntfs_attr_rm(na))         // frees its clusters too
            err = errno ? errno : EIO;
        ntfs_attr_close(na);
        if (!err && !ni->security_id && ntfs_sd_add_everyone(ni))
            err = errno ? errno : EIO;
        ntfs_inode_mark_dirty(ni);
    }
    int cerr = ue_iput(ni);
    return err ? err : cerr;
}

// Clears the bits of records and clusters marked used that nothing uses (D-102). Only on a
// volume the check just found nothing wrong with, so every record in use was read whole and
// every runlist decoded: nothing in use can be freed.
static int clear_lost(chk_t *c, uint64_t *records, uint64_t *clusters)
{
    int err;

    if ((err = ue_mark_busy(c->e)))
        return err;
    for (int pass = 0; pass < 2; pass++) {
        ntfs_attr *na = pass ? c->vol->lcnbmp_na : c->vol->mftbmp_na;
        const u8 *have = pass ? c->lcnbmp : c->mftbmp, *want = pass ? c->used : c->inuse;
        uint64_t bits = pass ? (c->lcnbmp_bits < c->ncl ? c->lcnbmp_bits : c->ncl) : c->mftbmp_bits;
        uint64_t limit = pass ? c->ncl : c->nrec;   // beyond it nothing is in use
        for (uint64_t i = 0; i < bits;) {
            if (!has(have, i) || (i < limit && has(want, i))) {
                i++;
                continue;
            }
            uint64_t j = i;
            while (j < bits && has(have, j) && !(j < limit && has(want, j)))
                j++;
            if (ntfs_bitmap_clear_run(na, (s64)i, (s64)(j - i)))
                return errno ? errno : EIO;
            *(pass ? clusters : records) += j - i;
            if (pass)
                c->vol->free_clusters += (s64)(j - i);
            else if (c->vol->free_mft_records >= 0)
                c->vol->free_mft_records += (s64)(j - i);
            i = j;
        }
    }
    return c->e->dev_ctx->failed ? EIO : 0;
}

static int chk_repair(chk_t *c, ue_check_result_t *out)
{
    uint64_t *done = &out->repaired, *removed = &out->entries_removed, *relinked = &out->orphans_relinked;
    uint64_t *found = &out->orphans_found;
    uint64_t found_rec = 0;
    uint64_t clusters = 0;
    int err;

    if ((err = ue_mark_busy(c->e)))   // D-95
        return err;
    // The bitmaps first: removing an entry can allocate an index block.
    if ((err = set_missing(c->vol->mftbmp_na, c->inuse, c->mftbmp, c->mftbmp_bits, c->nrec, done)) ||
        (err = set_missing(c->vol->lcnbmp_na, c->used, c->lcnbmp, c->lcnbmp_bits, c->ncl, &clusters)))
        return err;
    *done += clusters;
    if (c->vol->free_clusters >= (s64)clusters)
        c->vol->free_clusters -= (s64)clusters;
    for (size_t i = 0; i < c->ndangling && !c->e->dev_ctx->failed; i++) {
        const dangling_t *x = &c->dangling[i];
        if ((err = remove_entry(c, x))) {
            // Not there, or changed since: left alone, and the check that follows reports it
            // if it is still wrong. Anything else stops the repair.
            if (err == ENOENT || err == ESTALE)
                continue;
            return err;
        }
        if (x->name_type != FILE_NAME_DOS)
            (*removed)++;
    }
    // Last, the orphans: a removed entry may have freed the name one of them wants. Those that
    // go back in place first, over and over, since a folder put back makes its orphaned
    // children reachable; then the rest go to found.
    for (int last = 0, progress = 1; last < 2 && !c->e->dev_ctx->failed; last += !progress) {
        progress = 0;
        for (size_t i = 0; i < c->norphans && !c->e->dev_ctx->failed; i++) {
            uint64_t rec = c->orphans[i];
            bool to_found;
            if (rec == UINT64_MAX)
                continue;
            if ((err = relink(c, rec, last == 1, &found_rec, &to_found))) {
                if (err == EAGAIN)
                    continue;
                if (err != ENOENT && err != ESTALE)
                    return err;
            } else {
                (*relinked)++;
                *found += to_found;
                if (!to_found) {
                    mark(c->refd, rec);
                    progress = 1;
                }
            }
            c->orphans[i] = UINT64_MAX;
        }
        if (last == 1)
            break;
    }
    for (size_t i = 0; i < c->nbad_sd && !c->e->dev_ctx->failed; i++) {
        if ((err = fix_security(c, c->bad_sd[i]))) {
            if (err == ENOENT || err == ESTALE)
                continue;
            return err;
        }
        out->security_replaced++;
    }
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
    bool lost = res->lost_records || res->lost_clusters;
    if (!err && repair && res->problems && res->problems == res->repairable) {
        res->repair_ran = true;
        err = chk_repair(&c, res);
        chk_free(&c);
        if (!err && !(err = chk_init(&c, e, NULL, NULL, &again))) {
            err = chk_run(&c);
            res->remaining = again.problems;
            lost = again.lost_records || again.lost_clusters;
        }
    }
    // Last, with nothing else left, the space nothing uses (D-102); checked once more.
    if (!err && repair && res->remaining == 0 && lost) {
        res->repair_ran = true;
        err = clear_lost(&c, &res->records_freed, &res->clusters_freed);
        chk_free(&c);
        if (!err && !(err = chk_init(&c, e, NULL, NULL, &again))) {
            err = chk_run(&c);
            // Still lost: counted as left, so the dirty mark stays.
            res->remaining = again.problems + (again.lost_records || again.lost_clusters);
        }
    }
    chk_free(&c);
    return err;
}

// libntfs-3g's mount compares $MFTMirr with $MFT's first records and refuses the volume when
// they differ, so a write cut off between the two leaves a volume nothing on the Mac can open.
// D-97 writes $MFT's first page before $MFTMirr, so $MFT is the newer copy: the repair copies
// its first records, raw as they are on the disk, over $MFTMirr (D-98), as ntfsfix's
// fix_mftmirr and chkdsk do, but only when every one of them is whole. The other way round
// (the older mirror over a torn $MFT record) could describe a shorter $MFT or lose a change,
// so it is left to chkdsk.
typedef struct {
    uint64_t first;                   // the first record that differs, plus one; 0: none
    uint64_t bytes, mirr_off;         // what the repair writes, and where
    u8 *raw;                          // $MFT's first records as on the disk
    char refuse[160];                 // why the repair would not run; "" when it can
} mirror_t;

// The LCN and length of the first run of record m's unnamed $DATA, or false.
static bool first_data_run(MFT_RECORD *m, u32 rs, int64_t *lcn, int64_t *len)
{
    chk_t c = { .recsize = rs };

    for (ATTR_RECORD *a = next_attr(&c, m, NULL); a; a = next_attr(&c, m, a)) {
        if (a->type != AT_DATA || a->name_length || !a->non_resident || a->lowest_vcn)
            continue;
        u32 off = le16_to_cpu(a->mapping_pairs_offset), alen = le32_to_cpu(a->length);
        if (off >= alen)
            return false;
        const u8 *mp = (const u8 *)a + off;
        unsigned lb = mp[0] & 0xf, ob = mp[0] >> 4;
        if (!lb || lb > 8 || !ob || ob > 8 || off + 1 + lb + ob > alen)
            return false;
        int64_t v[2] = { 0, 0 };
        for (int k = 0; k < 2; k++) {
            unsigned nb = k ? ob : lb, base = 1 + (k ? lb : 0);
            uint64_t x = 0;
            for (unsigned i = 0; i < nb; i++)
                x |= (uint64_t)mp[base + i] << (8 * i);
            if (nb < 8 && (mp[base + nb - 1] & 0x80))
                x |= ~0ull << (8 * nb);     // sign-extended
            v[k] = (int64_t)x;
        }
        *len = v[0];
        *lcn = v[1];
        return true;
    }
    return false;
}

#define REFUSE(...) (snprintf(m->refuse, sizeof(m->refuse), __VA_ARGS__), true)

// Fills m when $MFTMirr differs from $MFT within the records libntfs-3g compares; m->refuse
// says why $MFT's records cannot be copied over it. False: they do not differ, or the boot
// sector does not say where they are.
static bool mirror_examine(const ue_io_t *io, mirror_t *m)
{
    NTFS_BOOT_SECTOR bs;
    ntfs_volume v;                    // what ntfs_mft_record_check needs: the record size

    memset(m, 0, sizeof(*m));
    if (io->pread(io->ctx, &bs, sizeof(bs), 0) || !ntfs_boot_sector_is_ntfs(&bs))
        return false;
    u32 ss = le16_to_cpu(bs.bpb.bytes_per_sector), spc = bs.bpb.sectors_per_cluster;
    spc = spc > 0x80 ? 1u << (256 - spc) : spc;
    uint64_t cs = (uint64_t)ss * spc;
    s8 cpr = bs.clusters_per_mft_record;
    uint64_t rs = cpr < 0 ? (cpr > -31 ? 1ull << -cpr : 0) : (uint64_t)cpr * cs;
    if (!rs || rs > 65536 || (rs & (rs - 1)) || rs < NTFS_BLOCK_SIZE || !cs || cs > (2u << 20))
        return false;
    uint64_t n = cs <= 4 * rs ? 4 : cs / rs;   // vol->mftmirr_size
    s64 mft = sle64_to_cpu(bs.mft_lcn), mirr = sle64_to_cpu(bs.mftmirr_lcn);
    if (mft < 0 || mirr < 0 || mft == mirr || (uint64_t)mft > io->size_bytes / cs ||
        (uint64_t)mirr > io->size_bytes / cs || ((uint64_t)mft + 1) * cs + n * rs > io->size_bytes ||
        ((uint64_t)mirr + 1) * cs + n * rs > io->size_bytes)
        return false;
    u8 *a = malloc(n * rs), *b = malloc(n * rs), *raw = malloc(n * rs);
    if (!a || !b || !raw || io->pread(io->ctx, raw, n * rs, (uint64_t)mft * cs) ||
        io->pread(io->ctx, b, n * rs, (uint64_t)mirr * cs)) {
        free(a);
        free(b);
        free(raw);
        return false;
    }
    memcpy(a, raw, n * rs);           // fixed up for the comparison; raw is what is written
    memset(&v, 0, sizeof(v));
    v.mft_record_size = (u32)rs;
    u32 muted = ntfs_log_clear_levels(NTFS_LOG_LEVEL_ERROR | NTFS_LOG_LEVEL_PERROR);
    for (uint64_t i = 0; i < n; i++) {
        MFT_RECORD *ma = (MFT_RECORD *)(a + i * rs), *mb = (MFT_RECORD *)(b + i * rs);
        bool fa = ntfs_is_file_record(ma->magic);
        bool ta = !fa || ntfs_mst_post_read_fixup_warn((NTFS_RECORD *)ma, (u32)rs, FALSE) != 0;
        bool tb = !ntfs_is_file_record(mb->magic) ||
                  ntfs_mst_post_read_fixup_warn((NTFS_RECORD *)mb, (u32)rs, FALSE) != 0;
        bool in_use = (ma->flags & MFT_RECORD_IN_USE) != 0;
        u32 used = le32_to_cpu(ma->bytes_in_use);
        // libntfs-3g compares the system records only.
        if (!m->first && i < FILE_first_user &&
            (ta || tb || used <= sizeof(MFT_RECORD) || used > rs || memcmp(ma, mb, used)))
            m->first = i + 1;
        if (m->refuse[0])
            continue;
        if (!fa)
            REFUSE("record %llu of $MFT is not an MFT record", (unsigned long long)i);
        else if (ta)
            REFUSE("record %llu of $MFT is torn (a write cut off inside it)", (unsigned long long)i);
        else if (!in_use && i <= FILE_Volume)
            REFUSE("record %llu of $MFT is not in use", (unsigned long long)i);
        else if (in_use && ntfs_mft_record_check(&v, i, ma))
            REFUSE("record %llu of $MFT is damaged", (unsigned long long)i);
    }
    ntfs_log_set_levels(muted & (NTFS_LOG_LEVEL_ERROR | NTFS_LOG_LEVEL_PERROR));
    // $MFT's and $MFTMirr's own records must put them where the boot sector does, as chkdsk
    // checks, so a wrong boot sector never sends the copy anywhere else.
    int64_t lcn, len;
    if (!m->refuse[0] && (!first_data_run((MFT_RECORD *)a, (u32)rs, &lcn, &len) || lcn != mft ||
                          len <= 0 || (uint64_t)len * cs < n * rs))
        REFUSE("$MFT's record does not place $MFT where the boot sector does");
    if (!m->refuse[0] && (!first_data_run((MFT_RECORD *)(a + rs), (u32)rs, &lcn, &len) || lcn != mirr ||
                          len <= 0 || (uint64_t)len * cs < n * rs))
        REFUSE("$MFTMirr's record does not place $MFTMirr where the boot sector does");
    free(a);
    free(b);
    m->raw = raw;
    m->bytes = n * rs;
    m->mirr_off = (uint64_t)mirr * cs;
    if (!m->first) {
        free(raw);
        m->raw = NULL;
    }
    return m->first != 0;
}

#undef REFUSE

// The check of a volume libntfs-3g opens, and its repair.
static int check_opened(const ue_io_t *io, ue_vol_state_t state, bool repair, ue_check_fn fn, void *ctx,
                        ue_check_result_t *out)
{
    bool dirty = state == UE_VOL_DIRTY;
    ue_check_result_t rw;
    ue_engine_t *e;
    int err, cerr;

    if (repair && (state == UE_VOL_HIBERNATED || state == UE_VOL_UNCLEAN_LOG))
        return EPERM;
    if (repair && (!io->writable || !io->pwrite))
        return EROFS;

    // The check itself writes nothing.
    if ((err = ue_open(io, UE_OPEN_RDONLY, &e, NULL)))
        return err;
    err = check_volume(e, fn, ctx, out, false);
    cerr = ue_close(e);
    out->state = state;
    if (err || cerr)
        return err ? err : cerr;
    if (!repair || (out->problems ? out->problems != out->repairable
                                  : !dirty && !out->lost_records && !out->lost_clusters))
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
    out->entries_removed = rw.entries_removed;
    out->orphans_relinked = rw.orphans_relinked;
    out->orphans_found = rw.orphans_found;
    out->records_freed = rw.records_freed;
    out->clusters_freed = rw.clusters_freed;
    out->security_replaced = rw.security_replaced;
    out->remaining = err ? out->problems : rw.remaining;
    out->dirty_cleared = dirty && !err && !cerr && rw.remaining == 0;
    return err ? err : cerr;
}

int ue_check(const ue_io_t *io, uint32_t flags, ue_check_fn fn, void *ctx, ue_check_result_t *out)
{
    bool repair = (flags & UE_CHECK_REPAIR) != 0;
    ue_check_problem_t mp;
    ue_probe_t p;
    mirror_t m;
    int err;

    if (!io || !out)
        return EINVAL;
    memset(out, 0, sizeof(*out));
    if ((err = ue_probe(io, &p)))
        return err;
    if (p.state == UE_VOL_NOT_NTFS)
        return EINVAL;
    if (p.state != UE_VOL_CORRUPT)
        return check_opened(io, p.state, repair, fn, ctx, out);

    if (!mirror_examine(io, &m))
        return EIO;
    bool fixable = !m.refuse[0];
    memset(&mp, 0, sizeof(mp));
    mp.kind = UE_CHK_MFT_MIRROR;
    mp.repairable = fixable;
    mp.rec = m.first - 1;
    snprintf(mp.path, sizeof(mp.path), "/$MFTMirr");
    if (fixable)
        snprintf(mp.detail, sizeof(mp.detail),
                 "$MFTMirr does not match $MFT at record %llu (a write cut off between the two): the volume "
                 "cannot be opened until $MFT is copied over it", (unsigned long long)(m.first - 1));
    else
        snprintf(mp.detail, sizeof(mp.detail),
                 "$MFTMirr does not match $MFT at record %llu, and %s: the volume cannot be opened",
                 (unsigned long long)(m.first - 1), m.refuse);
    out->state = p.state;
    out->problems = out->remaining = out->by_kind[UE_CHK_MFT_MIRROR] = 1;
    out->repairable = fixable;
    if (fn)
        fn(ctx, &mp);
    if (!repair || !fixable) {
        free(m.raw);
        return 0;                     // nothing else can be checked until it opens
    }
    if (!io->writable || !io->pwrite) {
        free(m.raw);
        return EROFS;
    }
    err = io->pwrite(io->ctx, m.raw, m.bytes, m.mirr_off);
    if (!err && io->flush)
        err = io->flush(io->ctx);
    free(m.raw);
    if (err)
        return err;
    out->mirror_repaired = out->repair_ran = true;
    out->remaining = 0;
    if ((err = ue_probe(io, &p)))
        return err;
    if (p.state == UE_VOL_CORRUPT || p.state == UE_VOL_NOT_NTFS) {
        out->remaining = 1;           // something else keeps it from opening
        return EIO;
    }

    // The rest of the volume, now that it opens: checked and repaired in this same call.
    ue_check_result_t rest;
    memset(&rest, 0, sizeof(rest));
    err = check_opened(io, p.state, true, fn, ctx, &rest);
    rest.state = UE_VOL_CORRUPT;
    rest.problems++;
    rest.repairable++;
    rest.by_kind[UE_CHK_MFT_MIRROR]++;
    rest.repair_ran = true;
    rest.mirror_repaired = true;
    *out = rest;
    return err;
}
