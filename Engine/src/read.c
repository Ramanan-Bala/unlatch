// SPDX-License-Identifier: GPL-3.0-or-later
// Read path: lookup, getattr, readdir, read, statfs, release.
#include "internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <ntfs-3g/attrib.h>
#include <ntfs-3g/dir.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/layout.h>
#include <ntfs-3g/unistr.h>

#define READDIR_BATCH 128

int ue_getattr(ue_engine_t *e, ue_ino_t ino, ue_attr_t *out)
{
    ntfs_inode *ni;
    int err, cerr;

    if (!e || !out)
        return EINVAL;
    if ((err = ue_iget(e, ino, &ni)))
        return err;
    err = ue_fill_attr(ni, out);
    cerr = ue_iput(ni);
    return err ? err : cerr;
}

int ue_lookup(ue_engine_t *e, ue_ino_t dir, const char *name, ue_attr_t *out)
{
    ntfs_inode *dni, *ni;
    ntfschar *uname;
    int len, err;
    u64 mref;

    if (!e || !out)
        return EINVAL;
    if ((err = ue_to_ucs(name, &uname, &len)))
        return err;
    if ((err = ue_iget(e, dir, &dni))) {
        free(uname);
        return err;
    }
    if (!ue_is_dir(dni)) {
        free(uname);
        ue_iput(dni);
        return ENOTDIR;
    }
    mref = ntfs_inode_lookup_by_name(dni, uname, len);   // case-insensitive (ignore_case)
    err = errno;
    free(uname);
    if (mref == (u64)-1) {
        ue_iput(dni);
        return err == ENOENT || !err ? ENOENT : err;
    }
    if (ue_hidden_entry(dni->mft_no, MREF(mref), name)) {
        ue_iput(dni);
        return ENOENT;
    }
    // Close the directory first: it is never needed open while the child is.
    if ((err = ue_iput(dni)))
        return err;
    if ((err = ue_iget(e, mref, &ni)))
        return err == ESTALE ? EIO : err;              // index points at a reused record
    err = ue_fill_attr(ni, out);
    if (!err)
        err = ue_hold(e, ni);
    int cerr = ue_iput(ni);
    return err ? err : cerr;
}

void ue_release(ue_engine_t *e, ue_ino_t ino)
{
    ue_held_t *h;

    if (!e || !(h = ue_held_find(&e->held, MREF(ino))) || h->seq != MSEQNO(ino))
        return;
    if (h->ghost)
        ue_delete_ghost(e, h);          // last reference to a removed file: delete it now
    ue_held_remove(&e->held, MREF(ino));
}

// readdir: ntfs_readdir fills a batch of names; attributes are read after it returns,
// so no other inode is opened while libntfs-3g is walking the index.
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
    if (pos < 2 || name_type == FILE_NAME_DOS)
        return 0;
    if (b->n == READDIR_BATCH) {        // stop; ntfs_readdir leaves *pos on this entry
        b->full = true;
        return 1;
    }
    if (ntfs_ucstombs(name, name_len, &utf8, 0) < 0) {
        b->err = errno ? errno : EILSEQ;
        return 1;
    }
    if (ue_hidden_entry(b->dir_rec, MREF(mref), utf8)) {
        free(utf8);
        return 0;
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
    ntfs_inode *ni = ue_iopen_rec(e, MREF(mref));
    int err;

    if (!ni)
        return errno ? errno : EIO;
    err = ue_fill_attr(ni, out);
    int cerr = ue_iput(ni);
    return err ? err : cerr;
}

// Reads one batch of names from the open directory, starting at *pos.
// Returns 0 or errno; b->full means there is more.
static int read_batch(ue_engine_t *e, ntfs_inode *dni, s64 *pos, batch_t *b)
{
    int err, rc;

    b->full = false;
    b->err = 0;
    // In ignore-case mode ntfs_filldir lowercases names; force case-sensitive to keep
    // them case-preserved, then restore (D-19).
    NVolSetCaseSensitive(e->vol);
    rc = ntfs_readdir(dni, pos, b, collect);
    err = errno;
    NVolClearCaseSensitive(e->vol);
    if (b->err || (rc && !b->full)) {
        free_batch(b, 0);
        return b->err ? b->err : (err ? err : EIO);
    }
    return 0;
}

int ue_readdir(ue_engine_t *e, ue_ino_t dir, uint64_t cookie, ue_dirent_fn emit, void *ctx,
               uint64_t *verifier)
{
    ntfs_inode *dni;
    batch_t *b;
    s64 pos = (s64)cookie;
    int err;

    if (!e || !emit)
        return EINVAL;
    if ((err = ue_iget(e, dir, &dni)))
        return err;
    if (!ue_is_dir(dni)) {
        ue_iput(dni);
        return ENOTDIR;
    }
    if (verifier)
        *verifier = (uint64_t)sle64_to_cpu(dni->last_data_change_time);
    b = calloc(1, sizeof(*b));
    if (!b) {
        ue_iput(dni);
        return ENOMEM;
    }
    b->dir_rec = dni->mft_no;
    // The directory stays open for the whole call; children are opened read-only after
    // each batch, outside ntfs_readdir, and closed clean, so nothing syncs into dni.
    for (;;) {
        if ((err = read_batch(e, dni, &pos, b)))
            break;
        for (int i = 0; i < b->n; i++) {
            ue_attr_t attr;
            if ((err = entry_attr(e, b->ents[i].mref, &attr))) {
                if (e->dev_ctx->failed || err == ENOMEM) {
                    free_batch(b, i);
                    goto out;
                }
                // A damaged or stale entry (its MFT record unreadable, e.g. all zeros) hides
                // only itself, as with ntfs-3g; the rest of the directory still lists (D-54).
                ue_logf(UE_LOG_WARNING, "skipping \"%s\" in directory %llu: MFT record %llu: %s",
                        b->ents[i].name, (unsigned long long)dni->mft_no,
                        (unsigned long long)MREF(b->ents[i].mref), strerror(err));
                free(b->ents[i].name);
                err = 0;
                continue;
            }
            // Resuming at pos + 1 skips exactly this entry (dir.c: entries with an
            // offset >= *pos are returned).
            bool more = emit(ctx, b->ents[i].name, &attr, (uint64_t)b->ents[i].pos + 1);
            free(b->ents[i].name);
            if (!more) {
                free_batch(b, i + 1);
                goto out;
            }
        }
        b->n = 0;
        if (!b->full)
            break;
    }
out:
    free(b);
    int cerr = ue_iput(dni);
    return err ? err : cerr;
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
    if ((err = ue_iget(e, ino, &ni)))
        return err;
    if (ue_is_dir(ni)) {
        ue_iput(ni);
        return EISDIR;
    }
    na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
    if (!na) {
        err = errno == ENOENT ? 0 : (errno ? errno : EIO);
        ue_iput(ni);
        return err;
    }
    if (off < (uint64_t)na->data_size) {
        if (len > (uint64_t)na->data_size - off)
            len = (size_t)((uint64_t)na->data_size - off);
        bc_set_cold(e->dev_ctx->cache, true);
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
        bc_set_cold(e->dev_ctx->cache, false);
    }
    ntfs_attr_close(na);
    ue_iput(ni);
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
    out->held_inodes = e->held.count;
}
