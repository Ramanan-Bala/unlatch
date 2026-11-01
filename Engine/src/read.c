// SPDX-License-Identifier: GPL-3.0-or-later
// Read path: inode lifetime, lookup, getattr, readdir, read, statfs.
#include "internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <ntfs-3g/attrib.h>
#include <ntfs-3g/dir.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/layout.h>
#include <ntfs-3g/ntfstime.h>
#include <ntfs-3g/unistr.h>

// Records below this in the root are metafiles ($MFT ... $Extend, then reserved).
#define UE_FIRST_VISIBLE_REC 24
#define READDIR_BATCH 128

static ue_ino_t ino_of(const ntfs_inode *ni)
{
    return MK_MREF(ni->mft_no, le16_to_cpu(ni->mrec->sequence_number));
}

static bool is_dir(const ntfs_inode *ni)
{
    return (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) != 0;
}

static bool hidden_metafile(uint64_t dir_rec, uint64_t rec)
{
    return dir_rec == FILE_root && rec < UE_FIRST_VISIBLE_REC && rec != FILE_root;
}

// Sequence 0 is accepted only for the root, so the host can start from UE_ROOT_INO.
static int check_seq(const ntfs_inode *ni, ue_ino_t ino)
{
    uint16_t want = MSEQNO(ino);

    if (want == 0 && MREF(ino) == FILE_root)
        return 0;
    return want == le16_to_cpu(ni->mrec->sequence_number) ? 0 : ESTALE;
}

static ntfs_inode *open_counted(ue_engine_t *e, uint64_t rec)
{
    e->stats.inode_opens++;
    return ntfs_inode_open(e->vol, rec);
}

// Returns the cached inode for ino, opening and caching it on first use.
static int get_inode(ue_engine_t *e, ue_ino_t ino, ntfs_inode **out)
{
    uint64_t rec = MREF(ino);
    ntfs_inode *ni;
    int err;

    ni = ue_icache_find(&e->icache, rec);
    if (!ni) {
        ni = open_counted(e, rec);
        if (!ni)
            return errno == ENOENT ? ESTALE : (errno ? errno : EIO);
        if ((err = check_seq(ni, ino)) || (err = ue_icache_insert(&e->icache, rec, ni))) {
            ntfs_inode_close(ni);
            return err;
        }
    } else if ((err = check_seq(ni, ino))) {
        return err;
    }
    *out = ni;
    return 0;
}

int ue_cache_root(ue_engine_t *e)
{
    ntfs_inode *ni;

    return get_inode(e, UE_ROOT_INO, &ni);
}

static uint32_t map_flags(le32 fa)
{
    uint32_t f = 0;

    if (fa & FILE_ATTR_HIDDEN) f |= UE_F_HIDDEN;
    if (fa & FILE_ATTR_SYSTEM) f |= UE_F_SYSTEM;
    if (fa & FILE_ATTR_READONLY) f |= UE_F_READONLY;
    if (fa & FILE_ATTR_ARCHIVE) f |= UE_F_ARCHIVE;
    if (fa & FILE_ATTR_COMPRESSED) f |= UE_F_COMPRESSED;
    if (fa & FILE_ATTR_ENCRYPTED) f |= UE_F_ENCRYPTED;
    if (fa & FILE_ATTR_SPARSE_FILE) f |= UE_F_SPARSE;
    if (fa & FILE_ATTR_REPARSE_POINT) f |= UE_F_REPARSE;
    return f;
}

// Hard links: $FILE_NAME attributes, not counting DOS 8.3 aliases.
static uint32_t count_links(ntfs_inode *ni)
{
    ntfs_attr_search_ctx *ctx = ntfs_attr_get_search_ctx(ni, NULL);
    uint32_t n = 0;

    if (!ctx)
        return 1;
    while (!ntfs_attr_lookup(AT_FILE_NAME, AT_UNNAMED, 0, CASE_SENSITIVE, 0, NULL, 0, ctx)) {
        const FILE_NAME_ATTR *fn = (const FILE_NAME_ATTR *)((const u8 *)ctx->attr +
                                                            le16_to_cpu(ctx->attr->value_offset));
        if (fn->file_name_type != FILE_NAME_DOS)
            n++;
    }
    ntfs_attr_put_search_ctx(ctx);
    return n ? n : 1;
}

static int fill_attr(ntfs_inode *ni, ue_attr_t *out)
{
    ntfs_attr *na;

    memset(out, 0, sizeof(*out));
    out->ino = ino_of(ni);
    out->flags = map_flags(ni->flags);
    out->nlink = count_links(ni);
    out->birth = ntfs2timespec(ni->creation_time);
    out->mtime = ntfs2timespec(ni->last_data_change_time);
    out->ctime = ntfs2timespec(ni->last_mft_change_time);
    out->atime = ntfs2timespec(ni->last_access_time);

    if (is_dir(ni)) {
        out->type = UE_T_DIR;
        out->mode = 0755;
        na = ntfs_attr_open(ni, AT_INDEX_ALLOCATION, NTFS_INDEX_I30, 4);
        if (na) {
            out->size = out->init_size = (uint64_t)na->data_size;
            out->alloc_size = (uint64_t)na->allocated_size;
            ntfs_attr_close(na);
        }
        return 0;
    }

    out->type = UE_T_REG;
    out->mode = 0644;
    na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
    if (!na) {
        if (errno == ENOENT)           // no unnamed $DATA: treat as empty
            return 0;
        return errno ? errno : EIO;
    }
    out->size = (uint64_t)na->data_size;
    out->init_size = (uint64_t)na->initialized_size;
    if (na->data_flags & (ATTR_COMPRESSION_MASK | ATTR_IS_SPARSE))
        out->alloc_size = (uint64_t)na->compressed_size;
    else
        out->alloc_size = (uint64_t)na->allocated_size;
    if (!NAttrNonResident(na))
        out->flags |= UE_F_RESIDENT;
    if (na->data_flags & ATTR_IS_SPARSE)
        out->flags |= UE_F_SPARSE;
    if (na->data_flags & ATTR_COMPRESSION_MASK)
        out->flags |= UE_F_COMPRESSED;
    if (na->data_flags & ATTR_IS_ENCRYPTED)
        out->flags |= UE_F_ENCRYPTED;
    ntfs_attr_close(na);
    return 0;
}

// UTF-8 -> UTF-16, rejecting names the seam does not allow.
static int to_ucs(const char *name, ntfschar **out, int *len_out)
{
    int len;

    if (!name || !*name || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, ".."))
        return EINVAL;
    *out = NULL;
    len = ntfs_mbstoucs(name, out);
    if (len < 0)
        return errno == ENAMETOOLONG ? ENAMETOOLONG : EINVAL;
    if (len > NTFS_MAX_NAME_LEN) {
        free(*out);
        return ENAMETOOLONG;
    }
    *len_out = len;
    return 0;
}

int ue_getattr(ue_engine_t *e, ue_ino_t ino, ue_attr_t *out)
{
    ntfs_inode *ni;
    int err;

    if (!e || !out)
        return EINVAL;
    if ((err = get_inode(e, ino, &ni)))
        return err;
    return fill_attr(ni, out);
}

int ue_lookup(ue_engine_t *e, ue_ino_t dir, const char *name, ue_attr_t *out)
{
    ntfs_inode *dni, *ni;
    ntfschar *uname;
    int len, err;
    u64 mref;

    if (!e || !out)
        return EINVAL;
    if ((err = get_inode(e, dir, &dni)))
        return err;
    if (!is_dir(dni))
        return ENOTDIR;
    if ((err = to_ucs(name, &uname, &len)))
        return err;
    mref = ntfs_inode_lookup_by_name(dni, uname, len);   // case-insensitive (ignore_case)
    err = errno;
    free(uname);
    if (mref == (u64)-1)
        return err == ENOENT || !err ? ENOENT : err;
    if (hidden_metafile(dni->mft_no, MREF(mref)))
        return ENOENT;
    if ((err = get_inode(e, mref, &ni)))
        return err == ESTALE ? EIO : err;              // index points at a reused record
    return fill_attr(ni, out);
}

void ue_release(ue_engine_t *e, ue_ino_t ino)
{
    ntfs_inode *ni;

    if (!e || MREF(ino) == FILE_root)   // the root stays open for the volume's lifetime
        return;
    ni = ue_icache_find(&e->icache, MREF(ino));
    if (!ni || check_seq(ni, ino))
        return;
    ue_icache_remove(&e->icache, MREF(ino));
    ntfs_inode_close(ni);
}

// readdir: ntfs_readdir fills a batch of names; attributes are read after it returns,
// so no inode is opened while libntfs-3g is walking the index.
typedef struct {
    char *name;
    MFT_REF mref;
    s64 pos;
} batch_ent_t;

typedef struct {
    uint64_t dir_rec;
    batch_ent_t ents[READDIR_BATCH];
    int n;
    bool full;
    int err;
} batch_t;

static int collect(void *ctx, const ntfschar *name, const int name_len, const int name_type,
                   const s64 pos, const MFT_REF mref, const unsigned dt_type)
{
    batch_t *b = ctx;
    char *utf8 = NULL;

    (void)dt_type;
    if (pos < 2 || name_type == FILE_NAME_DOS || hidden_metafile(b->dir_rec, MREF(mref)))
        return 0;
    if (b->n == READDIR_BATCH) {        // stop; ntfs_readdir leaves *pos on this entry
        b->full = true;
        return 1;
    }
    if (ntfs_ucstombs(name, name_len, &utf8, 0) < 0) {
        b->err = errno ? errno : EILSEQ;
        return 1;
    }
    b->ents[b->n].name = utf8;
    b->ents[b->n].mref = mref;
    b->ents[b->n].pos = pos;
    b->n++;
    return 0;
}

static void free_batch(batch_t *b, int from)
{
    for (int i = from; i < b->n; i++)
        free(b->ents[i].name);
    b->n = 0;
}

static int entry_attr(ue_engine_t *e, MFT_REF mref, ue_attr_t *out)
{
    ntfs_inode *ni = ue_icache_find(&e->icache, MREF(mref));
    int err;

    if (ni)
        return fill_attr(ni, out);
    ni = open_counted(e, MREF(mref));   // not cached: the host holds no item for it
    if (!ni)
        return errno ? errno : EIO;
    err = fill_attr(ni, out);
    ntfs_inode_close(ni);
    return err;
}

int ue_readdir(ue_engine_t *e, ue_ino_t dir, uint64_t cookie, ue_dirent_fn emit, void *ctx,
               uint64_t *verifier)
{
    ntfs_inode *dni;
    batch_t *b;
    s64 pos = (s64)cookie;
    int err, rc;

    if (!e || !emit)
        return EINVAL;
    if ((err = get_inode(e, dir, &dni)))
        return err;
    if (!is_dir(dni))
        return ENOTDIR;
    if (verifier)
        *verifier = (uint64_t)sle64_to_cpu(dni->last_data_change_time);
    b = calloc(1, sizeof(*b));
    if (!b)
        return ENOMEM;
    b->dir_rec = dni->mft_no;

    for (;;) {
        b->full = false;
        b->err = 0;
        // In ignore-case mode ntfs_filldir lowercases names; force case-sensitive to
        // keep them case-preserved, then restore (D-19).
        NVolSetCaseSensitive(e->vol);
        rc = ntfs_readdir(dni, &pos, b, collect);
        err = errno;
        NVolClearCaseSensitive(e->vol);
        if (b->err || (rc && !b->full)) {
            err = b->err ? b->err : (err ? err : EIO);
            free_batch(b, 0);
            free(b);
            return err;
        }
        for (int i = 0; i < b->n; i++) {
            ue_attr_t attr;
            if ((err = entry_attr(e, b->ents[i].mref, &attr))) {
                free_batch(b, i);
                free(b);
                return err;
            }
            // Resuming at pos + 1 skips exactly this entry (dir.c: entries with an
            // offset >= *pos are returned).
            bool more = emit(ctx, b->ents[i].name, &attr, (uint64_t)b->ents[i].pos + 1);
            free(b->ents[i].name);
            if (!more) {
                free_batch(b, i + 1);
                free(b);
                return 0;
            }
        }
        b->n = 0;
        if (!b->full)
            break;
    }
    free(b);
    return 0;
}

int ue_read(ue_engine_t *e, ue_ino_t ino, uint64_t off, void *buf, size_t len, size_t *done)
{
    ntfs_inode *ni;
    ntfs_attr *na;
    size_t total = 0;
    int err = 0;

    if (!e || (!buf && len) || !done)
        return EINVAL;
    *done = 0;
    if ((err = get_inode(e, ino, &ni)))
        return err;
    if (is_dir(ni))
        return EISDIR;
    na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
    if (!na)
        return errno == ENOENT ? 0 : (errno ? errno : EIO);
    if (off < (uint64_t)na->data_size) {
        if (len > (uint64_t)na->data_size - off)
            len = (size_t)((uint64_t)na->data_size - off);
        while (total < len) {
            // ntfs_attr_pread zero-fills past initialized_size and through sparse holes.
            s64 n = ntfs_attr_pread(na, (s64)(off + total), (s64)(len - total), (char *)buf + total);
            if (n < 0) {
                err = errno ? errno : EIO;
                break;
            }
            if (n == 0)
                break;
            total += (size_t)n;
        }
    }
    ntfs_attr_close(na);
    *done = total;
    return total ? 0 : err;
}

int ue_statfs(ue_engine_t *e, uint64_t *total, uint64_t *free_bytes, uint32_t *cluster)
{
    ntfs_volume *vol;

    if (!e)
        return EINVAL;
    vol = e->vol;
    // Free clusters are counted once at open; libntfs-3g keeps the count current.
    if (!NVolFreeSpaceKnown(vol) && ntfs_volume_get_free_space(vol))
        return errno ? errno : EIO;
    if (total)
        *total = (uint64_t)vol->nr_clusters * vol->cluster_size;
    if (free_bytes)
        *free_bytes = vol->free_clusters > 0 ? (uint64_t)vol->free_clusters * vol->cluster_size : 0;
    if (cluster)
        *cluster = vol->cluster_size;
    return 0;
}

void ue_get_stats(const ue_engine_t *e, ue_stats_t *out)
{
    if (!e || !out)
        return;
    *out = e->stats;
    out->cached_inodes = e->icache.count;
}
