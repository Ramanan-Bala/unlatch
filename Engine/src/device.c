// SPDX-License-Identifier: GPL-3.0-or-later
// ntfs_device_operations backed by a host ue_io_t. libntfs-3g never touches a file descriptor.
#include "internal.h"

#include <ntfs-3g/attrib.h>
#include <ntfs-3g/runlist.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/disk.h>
#include <sys/stat.h>

#define CTX(dev) ((ue_dev_t *)(dev)->d_private)

static int dev_open(struct ntfs_device *dev, int flags)
{
    ue_dev_t *c = CTX(dev);

    if (NDevOpen(dev)) {
        errno = EBUSY;
        return -1;
    }
    if ((flags & O_ACCMODE) != O_RDONLY) {
        if (!c->io.writable || !c->io.pwrite) {
            errno = EROFS;
            return -1;
        }
        NDevClearReadOnly(dev);
    } else {
        NDevSetReadOnly(dev);
    }
    c->pos = 0;
    NDevSetOpen(dev);
    return 0;
}

// The write-back order (D-97): the device ranges of the bitmaps, $MFT and $MFTMirr, from the
// runlists in memory. Only mapped runs count; a run never mapped was never written. No I/O:
// this runs inside write-backs, which may come in the middle of a libntfs-3g call.
static void add_runs(ue_bcache_t *bc, const ntfs_volume *vol, const ntfs_attr *na, int kind)
{
    if (!na || !NAttrNonResident(na) || !na->rl)
        return;
    for (const runlist_element *rl = na->rl; rl->length; rl++)
        if (rl->lcn >= 0)
            bc_add_range(bc, (uint64_t)rl->lcn << vol->cluster_size_bits,
                         (uint64_t)rl->length << vol->cluster_size_bits, kind);
}

static void order_ranges(void *ctx, ue_bcache_t *bc)
{
    const ntfs_volume *vol = ((ue_dev_t *)ctx)->vol;

    if (!vol)
        return;
    add_runs(bc, vol, vol->lcnbmp_na, BC_RANGE_BITMAP);
    add_runs(bc, vol, vol->mftbmp_na, BC_RANGE_BITMAP);
    add_runs(bc, vol, vol->mft_na, BC_RANGE_MFT);
    add_runs(bc, vol, vol->mftmirr_na, BC_RANGE_MIRROR);
    bc_set_mirror_after(bc, (uint64_t)vol->mft_lcn << vol->cluster_size_bits);
    // $Secure's record, from the mapped runs (it is in $MFT's first extent).
    uint64_t pos = (uint64_t)FILE_Secure << vol->mft_record_size_bits, secure = UINT64_MAX;
    VCN vcn = (VCN)(pos >> vol->cluster_size_bits);
    if (vol->mft_na && NAttrNonResident(vol->mft_na) && vol->mft_na->rl)
        for (const runlist_element *rl = vol->mft_na->rl; rl->length; rl++)
            if (rl->lcn >= 0 && vcn >= rl->vcn && vcn < rl->vcn + rl->length) {
                secure = ((uint64_t)(rl->lcn + (vcn - rl->vcn)) << vol->cluster_size_bits) +
                         (pos & (vol->cluster_size - 1));
                break;
            }
    bc_set_secure(bc, secure);
}

void ue_device_set_volume(ue_dev_t *c, ntfs_volume *vol)
{
    c->vol = vol;
    bc_set_order(c->cache, vol ? order_ranges : NULL, c);
    bc_set_record_size(c->cache, vol ? vol->mft_record_size : 0);
}

void ue_device_hint_new_record(ue_dev_t *c, uint64_t rec)
{
    ntfs_volume *vol = c->vol;

    if (!c->cache || !vol || !vol->mft_na)
        return;
    uint64_t pos = rec << vol->mft_record_size_bits;
    LCN lcn = ntfs_attr_vcn_to_lcn(vol->mft_na, (VCN)(pos >> vol->cluster_size_bits));
    if (lcn >= 0)
        bc_hint_new(c->cache, ((uint64_t)lcn << vol->cluster_size_bits) + (pos & (vol->cluster_size - 1)));
}

int ue_device_writeback(ue_dev_t *c)
{
    int err;

    if (c->failed)
        return EIO;
    if ((err = bc_writeback(c->cache))) {
        c->failed = true;             // what reached the disk is unknown: stop all I/O (D-46)
        return err;
    }
    return 0;
}

static int dev_close(struct ntfs_device *dev)
{
    ue_dev_t *c = CTX(dev);
    int err = 0;

    if (!NDevOpen(dev)) {
        errno = EBADF;
        return -1;
    }
    // Nothing cached may be lost at close; after a failure nothing more is written.
    if (!NDevReadOnly(dev) && bc_dirty(c->cache))
        err = ue_device_writeback(c);
    NDevClearOpen(dev);
    if (err) {
        errno = err;
        return -1;
    }
    return 0;
}

static s64 dev_seek(struct ntfs_device *dev, s64 offset, int whence)
{
    ue_dev_t *c = CTX(dev);
    s64 base;

    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = (s64)c->pos; break;
    case SEEK_END: base = (s64)c->io.size_bytes; break;
    default: errno = EINVAL; return -1;
    }
    // Seeking past the end fails, so bootsect.c's last-sector check catches truncated volumes.
    if (offset > (s64)c->io.size_bytes - base || base + offset < 0) {
        errno = EINVAL;
        return -1;
    }
    c->pos = (uint64_t)(base + offset);
    return (s64)c->pos;
}

static s64 dev_pread(struct ntfs_device *dev, void *buf, s64 count, s64 offset)
{
    ue_dev_t *c = CTX(dev);
    int err;

    if (count < 0 || offset < 0) {
        errno = EINVAL;
        return -1;
    }
    if (c->failed) {
        errno = EIO;
        return -1;
    }
    if ((uint64_t)offset >= c->io.size_bytes || count == 0)
        return 0;
    if ((uint64_t)count > c->io.size_bytes - (uint64_t)offset)
        count = (s64)(c->io.size_bytes - (uint64_t)offset);
    err = c->cache ? bc_read(c->cache, buf, (size_t)count, (uint64_t)offset)
                   : c->io.pread(c->io.ctx, buf, (size_t)count, (uint64_t)offset);
    if (err) {
        if (err == ENXIO || err == ENODEV)
            c->failed = true;         // the device is gone (surprise removal)
        errno = err;
        return -1;
    }
    return count;
}

static s64 dev_pwrite(struct ntfs_device *dev, const void *buf, s64 count, s64 offset)
{
    ue_dev_t *c = CTX(dev);
    int err;

    if (NDevReadOnly(dev)) {
        errno = EROFS;
        return -1;
    }
    if (count < 0 || offset < 0) {
        errno = EINVAL;
        return -1;
    }
    if (c->failed) {
        errno = EIO;
        return -1;
    }
    if (count == 0)
        return 0;
    if ((uint64_t)offset >= c->io.size_bytes) {
        errno = ENOSPC;
        return -1;
    }
    if ((uint64_t)count > c->io.size_bytes - (uint64_t)offset)
        count = (s64)(c->io.size_bytes - (uint64_t)offset);
    if (c->unmarked) {
        // A change that did not go through ue_mark_busy: the mark is not on the disk.
        if (c->unmarked_writes++ == 0)
            ue_logf(UE_LOG_ERROR, "write at %lld while the volume is not marked dirty", (long long)offset);
    }
    NDevSetDirty(dev);
    err = c->cache ? bc_write(c->cache, buf, (size_t)count, (uint64_t)offset)
                   : c->io.pwrite(c->io.ctx, buf, (size_t)count, (uint64_t)offset);
    if (err) {
        // A cached write fails only when writing back to make room failed. Never retry a write: what reached the disk is unknown, so stop all I/O (Step 10).
        c->failed = true;
        errno = err;
        return -1;
    }
    return count;
}

static s64 dev_read(struct ntfs_device *dev, void *buf, s64 count)
{
    s64 n = dev_pread(dev, buf, count, (s64)CTX(dev)->pos);

    if (n > 0)
        CTX(dev)->pos += (uint64_t)n;
    return n;
}

static s64 dev_write(struct ntfs_device *dev, const void *buf, s64 count)
{
    s64 n = dev_pwrite(dev, buf, count, (s64)CTX(dev)->pos);

    if (n > 0)
        CTX(dev)->pos += (uint64_t)n;
    return n;
}

static int dev_sync(struct ntfs_device *dev)
{
    ue_dev_t *c = CTX(dev);
    int err;

    if (c->failed) {
        errno = EIO;
        return -1;
    }
    if (NDevReadOnly(dev) || !NDevDirty(dev))
        return 0;
    if ((err = ue_device_writeback(c))) {
        errno = err;
        return -1;
    }
    if (c->io.flush) {
        err = c->io.flush(c->io.ctx);
        if (err) {
            c->failed = true;
            errno = err;
            return -1;
        }
    }
    NDevClearDirty(dev);
    return 0;
}

static int dev_stat(struct ntfs_device *dev, struct stat *st)
{
    ue_dev_t *c = CTX(dev);

    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFBLK | (NDevReadOnly(dev) ? 0400 : 0600);
    st->st_size = (off_t)c->io.size_bytes;
    st->st_blksize = (blksize_t)c->io.block_size;
    st->st_blocks = (blkcnt_t)(c->io.size_bytes / 512);
    return 0;
}

// libntfs-3g on Darwin asks for DKIOCGETBLOCKSIZE (u32) and DKIOCGETBLOCKCOUNT (u64)
// in ntfs_device_sector_size_get / ntfs_device_size_get (device.c).
static int dev_ioctl(struct ntfs_device *dev, unsigned long request, void *argp)
{
    ue_dev_t *c = CTX(dev);

    switch (request) {
    case DKIOCGETBLOCKSIZE:
        *(uint32_t *)argp = c->io.block_size;
        return 0;
    case DKIOCGETBLOCKCOUNT:
        *(uint64_t *)argp = c->io.size_bytes / c->io.block_size;
        return 0;
    default:
        errno = ENOTTY;
        return -1;
    }
}

static struct ntfs_device_operations ue_device_ops = {
    .open = dev_open,
    .close = dev_close,
    .seek = dev_seek,
    .read = dev_read,
    .write = dev_write,
    .pread = dev_pread,
    .pwrite = dev_pwrite,
    .sync = dev_sync,
    .stat = dev_stat,
    .ioctl = dev_ioctl,
};

struct ntfs_device *ue_device_new(const ue_io_t *io, bool cached, ue_dev_t **ctx_out)
{
    struct ntfs_device *dev;
    ue_dev_t *c;
    int err;

    c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->io = *io;
    if (cached && (err = bc_new(io, UE_CACHE_BYTES, &c->cache))) {
        free(c);
        errno = err;
        return NULL;
    }
    dev = ntfs_device_alloc("unlatch", 0, &ue_device_ops, c);
    if (!dev) {
        int eo = errno;
        ue_device_free_ctx(c);
        errno = eo;
        return NULL;
    }
    *ctx_out = c;
    return dev;
}

void ue_device_free_ctx(ue_dev_t *c)
{
    if (c) {
        bc_free(c->cache);
        free(c);
    }
}
