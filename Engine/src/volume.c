// SPDX-License-Identifier: GPL-3.0-or-later
// Volume lifetime: probe, open, close.
#include "internal.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <ntfs-3g/attrib.h>
#include <ntfs-3g/bootsect.h>
#include <ntfs-3g/cache.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/layout.h>
#include <ntfs-3g/logfile.h>
#include <ntfs-3g/security.h>

const char *ue_vol_state_name(ue_vol_state_t state)
{
    switch (state) {
    case UE_VOL_CLEAN: return "clean";
    case UE_VOL_DIRTY: return "dirty";
    case UE_VOL_HIBERNATED: return "hibernated";
    case UE_VOL_UNCLEAN_LOG: return "unclean_log";
    case UE_VOL_CORRUPT: return "corrupt";
    case UE_VOL_NOT_NTFS: return "not_ntfs";
    }
    return "unknown";
}

static int check_io(const ue_io_t *io)
{
    if (!io->pread || io->size_bytes == 0 || io->block_size < 512 ||
        (io->block_size & (io->block_size - 1)))
        return EINVAL;
    return 0;
}

static void set_state(ue_probe_t *out, ue_vol_state_t state, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void set_state(ue_probe_t *out, ue_vol_state_t state, const char *fmt, ...)
{
    va_list ap;

    out->state = state;
    va_start(ap, fmt);
    vsnprintf(out->reason, sizeof(out->reason), fmt, ap);
    va_end(ap);
}

// Mounts through the bridge. On failure returns NULL with errno set and frees everything.
static ntfs_volume *mount_volume(const ue_io_t *io, ntfs_mount_flags flags, bool cached, ue_dev_t **ctx_out)
{
    struct ntfs_device *dev;
    ntfs_volume *vol;
    ue_dev_t *ctx;
    int eo;

    dev = ue_device_new(io, cached, &ctx);
    if (!dev)
        return NULL;
    vol = ntfs_device_mount(dev, flags);
    if (!vol) {
        eo = errno ? errno : EIO;
        ntfs_device_free(dev);     // ntfs_device_mount already closed it
        ue_device_free_ctx(ctx);
        errno = eo;
        return NULL;
    }
    *ctx_out = ctx;
    return vol;
}

// Everything libntfs-3g holds goes to the device, written back and flushed.
static int sync_volume(ntfs_volume *vol)
{
    ntfs_inode *sys[] = { vol->lcnbmp_ni, vol->mft_ni, vol->mftmirr_ni };

    if (ntfs_close_secure(vol))       // its index contexts write on close; release skips it now
        return errno ? errno : EIO;
    for (size_t i = 0; i < sizeof(sys) / sizeof(sys[0]); i++)
        if (sys[i] && NInoDirty(sys[i]) && ntfs_inode_sync(sys[i]))
            return errno ? errno : EIO;
    return vol->dev->d_ops->sync(vol->dev) ? (errno ? errno : EIO) : 0;
}

// Like Windows and Linux's ntfs3, VOLUME_IS_DIRTY is on the disk while the volume is open for
// writing, so a crash, a pull mid-write-back or a failed write leaves it DIRTY and the next
// mount is read-only until it is checked. libntfs-3g does not do this, and a stick pulled
// mid-copy probed CLEAN with "inconsistent metadata" logged (D-86). Each change is flushed on
// its own, so the mark never reaches the disk out of order with the metadata it covers.
//
// Since D-95 the mark is on only while the volume is being changed: ue_mark_busy sets it
// before the first change, and ue_settle clears it once everything is on the disk and the
// volume has been idle, so a disk pulled out while idle is not marked as needing a check.
static int write_dirty(ue_engine_t *e, bool dirty)
{
    ntfs_volume *vol = e->vol;
    le16 flags = dirty ? vol->flags | VOLUME_IS_DIRTY : vol->flags & ~VOLUME_IS_DIRTY;
    int err;

    e->dev_ctx->unmarked = false;
    if (ntfs_volume_write_flags(vol, flags))
        return errno ? errno : EIO;
    if ((err = vol->dev->d_ops->sync(vol->dev) ? (errno ? errno : EIO) : 0))
        return err;
    e->mark_on = dirty;
    e->dev_ctx->unmarked = !dirty;
    return 0;
}

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

int ue_mark_busy(ue_engine_t *e)
{
    if (e->readonly)
        return EROFS;
    if (e->dev_ctx->failed)
        return EIO;
    e->last_change_ns = now_ns();
    return e->mark_on ? 0 : write_dirty(e, true);
}

int ue_settle(ue_engine_t *e, uint32_t idle_ms)
{
    int err;

    if (!e)
        return EINVAL;
    if (e->dev_ctx->failed)
        return EIO;
    // A volume that was dirty at open keeps its mark (D-86) unless UE_OPEN_CLEAR_DIRTY.
    if (e->readonly || !e->marked_dirty || !e->mark_on)
        return 0;
    if (now_ns() - e->last_change_ns < (uint64_t)idle_ms * 1000000u)
        return EAGAIN;
    // As at a clean close: everything libntfs-3g holds reaches the disk, then the mark is
    // cleared. $Secure is closed to write its index blocks, and opened again.
    if ((err = sync_volume(e->vol)))
        return err;
    if (ntfs_open_secure(e->vol))
        return errno ? errno : EIO;
    return write_dirty(e, false);
}

static int unmount_volume(ntfs_volume *vol, ue_dev_t *ctx)
{
    int err = ntfs_umount(vol, FALSE) ? (errno ? errno : EIO) : 0;  // frees vol and the device

    ue_device_free_ctx(ctx);
    return err;
}

// Clusters and MFT records are at least 512 bytes, so a bitmap holds at most one bit per 512
// bytes of the volume; one cluster of slack covers rounding.
bool ue_bitmaps_plausible(const ntfs_volume *vol)
{
    s64 limit = (s64)(((uint64_t)vol->nr_clusters * vol->cluster_size) / 512 / 8) + vol->cluster_size;

    return vol->lcnbmp_na && vol->mftbmp_na && vol->lcnbmp_na->data_size <= limit &&
           vol->mftbmp_na->data_size <= limit;
}

// Label, sizes, versions and serial. Leaves state and reason alone.
static void fill_info(ntfs_volume *vol, const ue_io_t *io, ue_probe_t *out)
{
    NTFS_BOOT_SECTOR bs;

    snprintf(out->label, sizeof(out->label), "%s", vol->vol_name ? vol->vol_name : "");
    out->cluster_size = vol->cluster_size;
    out->total_bytes = (uint64_t)vol->nr_clusters * vol->cluster_size;
    if (ue_bitmaps_plausible(vol) && ntfs_volume_get_free_space(vol) == 0 && vol->free_clusters >= 0)
        out->free_bytes = (uint64_t)vol->free_clusters * vol->cluster_size;
    out->major_ver = vol->major_ver;
    out->minor_ver = vol->minor_ver;
    // ntfs_volume does not keep the serial number, so read it from the boot sector.
    if (io->pread(io->ctx, &bs, sizeof(bs), 0) == 0)
        out->serial = sle64_to_cpu(bs.volume_serial_number);
}

// Same checks as the static ntfs_volume_check_logfile() in volume.c.
// Returns 0 (clean), EOPNOTSUPP (unclean), EPERM (restart page 2.0: Fast Startup or
// abrupt unplug), or EIO.
static int check_logfile(ntfs_volume *vol)
{
    RESTART_PAGE_HEADER *rp = NULL;
    ntfs_inode *ni;
    ntfs_attr *na;
    int err = 0;

    ni = ntfs_inode_open(vol, FILE_LogFile);
    if (!ni)
        return EIO;
    na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
    if (!na) {
        ntfs_inode_close(ni);
        return EIO;
    }
    if (!ntfs_check_logfile(na, &rp) || !ntfs_is_logfile_clean(na, rp))
        err = EOPNOTSUPP;
    if (rp && rp->major_ver == const_cpu_to_le16(2) && rp->minor_ver == const_cpu_to_le16(0))
        err = EPERM;
    free(rp);
    ntfs_attr_close(na);
    if (ntfs_inode_close(ni) && !err)
        err = EIO;
    return err;
}

int ue_probe(const ue_io_t *io, ue_probe_t *out)
{
    NTFS_BOOT_SECTOR bs;
    ntfs_volume *vol;
    ue_dev_t *ctx;
    bool dirty;
    int hiber, hiber_err, log, err;

    if (!io || !out)
        return EINVAL;
    memset(out, 0, sizeof(*out));
    if ((err = check_io(io)))
        return err;

    if (io->size_bytes < sizeof(bs)) {
        set_state(out, UE_VOL_NOT_NTFS, "device is smaller than a boot sector");
        return 0;
    }
    if ((err = io->pread(io->ctx, &bs, sizeof(bs), 0))) {
        set_state(out, UE_VOL_CORRUPT, "cannot read the boot sector: %s", strerror(err));
        return 0;
    }
    if (!ntfs_boot_sector_is_ntfs(&bs)) {
        set_state(out, UE_VOL_NOT_NTFS, "no NTFS boot sector");
        return 0;
    }
    out->serial = sle64_to_cpu(bs.volume_serial_number);

    // Read-only mount: libntfs-3g skips its hiberfile and journal checks, so run them here.
    vol = mount_volume(io, NTFS_MNT_RDONLY, false, &ctx);
    if (!vol) {
        err = errno;
        if (err == ENOMEM)
            return ENOMEM;
        // The boot sector is NTFS, so any mount failure (EINVAL included) means damage.
        set_state(out, UE_VOL_CORRUPT, "NTFS metadata is damaged: %s", strerror(err));
        return 0;
    }
    if (!ue_bitmaps_plausible(vol)) {
        set_state(out, UE_VOL_CORRUPT, "NTFS metadata is damaged: a bitmap is larger than the volume");
        unmount_volume(vol, ctx);
        return 0;
    }
    fill_info(vol, io, out);

    dirty = (vol->flags & VOLUME_IS_DIRTY) != 0;
    hiber = ntfs_volume_check_hiberfile(vol, 0);
    hiber_err = hiber ? errno : 0;
    log = check_logfile(vol);
    unmount_volume(vol, ctx);

    // Most severe first.
    if (hiber && hiber_err == EPERM)
        set_state(out, UE_VOL_HIBERNATED,
                  "Windows is hibernated on this volume; resume Windows and shut it down fully");
    else if (log == EPERM)
        set_state(out, UE_VOL_HIBERNATED,
                  "Windows Fast Startup (or an abrupt unplug) left cached metadata; "
                  "shut Windows down fully or turn off Fast Startup");
    else if (hiber)
        set_state(out, UE_VOL_CORRUPT, "cannot read hiberfil.sys: %s", strerror(hiber_err));
    else if (log == EIO)
        set_state(out, UE_VOL_CORRUPT, "cannot read $LogFile");
    else if (log == EOPNOTSUPP)
        set_state(out, UE_VOL_UNCLEAN_LOG,
                  "$LogFile has unfinished transactions; mount it in Windows and shut down cleanly");
    else if (dirty)
        set_state(out, UE_VOL_DIRTY,
                  "volume is marked dirty; run chkdsk on Windows before writing to it");
    else
        set_state(out, UE_VOL_CLEAN, "volume is clean");
    return 0;
}

int ue_open(const ue_io_t *io, uint32_t flags, ue_engine_t **out, ue_probe_t *probe_out)
{
    bool readonly = (flags & UE_OPEN_RDONLY) != 0;
    ue_engine_t *e;
    ntfs_volume *vol;
    ue_dev_t *ctx;
    int err;

    if (!io || !out)
        return EINVAL;
    *out = NULL;
    if ((err = check_io(io)))
        return err;
    if (!readonly && (!io->writable || !io->pwrite))
        return EROFS;

    // Read-write without NTFS_MNT_MAY_RDONLY: hibernation and unclean journals fail the
    // open instead of degrading, so ue_probe stays the one place that decides.
    vol = mount_volume(io, readonly ? NTFS_MNT_RDONLY : 0, (flags & UE_OPEN_CACHE) != 0, &ctx);
    if (!vol)
        return errno;
    // ntfs_volume_startup retries read-only on EROFS even without MAY_RDONLY (D-14).
    if (!readonly && NVolReadOnly(vol)) {
        unmount_volume(vol, ctx);
        return EROFS;
    }
    // The mark is written before the first change (ue_mark_busy, D-95), not here. A volume
    // that was dirty already keeps its mark.
    bool was_dirty = (vol->flags & VOLUME_IS_DIRTY) != 0;

    ntfs_create_lru_caches(vol);       // ntfs_mount() does this; ntfs_device_mount() does not
    if (ntfs_set_ignore_case(vol) ||
        ntfs_set_shown_files(vol, FALSE, TRUE, FALSE)) {
        unmount_volume(vol, ctx);
        return ENOMEM;
    }

    e = calloc(1, sizeof(*e));
    if (!e) {
        unmount_volume(vol, ctx);
        return ENOMEM;
    }
    e->vol = vol;
    e->dev_ctx = ctx;
    e->readonly = readonly;
    e->marked_dirty = !readonly && (!was_dirty || (flags & UE_OPEN_CLEAR_DIRTY));
    e->mark_on = was_dirty;
    e->last_change_ns = now_ns();
    ctx->unmarked = !readonly && !was_dirty;
    e->ghost_open_only = (flags & UE_OPEN_GHOST_OPEN_ONLY) != 0;
    if ((err = ue_held_init(&e->held))) {
        unmount_volume(vol, ctx);
        free(e);
        return err;
    }

    // Temp names from earlier sessions may still exist (crash leftovers): never reuse an id.
    e->next_tmp_id = (uint64_t)time(NULL) * 1000;
    if (!readonly && (err = ue_sweep_root(e)) && (err == EIO || e->dev_ctx->failed)) {
        ue_close(e);
        return EIO;
    }

    if (probe_out) {
        memset(probe_out, 0, sizeof(*probe_out));
        fill_info(vol, io, probe_out);
        if (was_dirty)
            set_state(probe_out, UE_VOL_DIRTY, "volume is marked dirty");
        else
            set_state(probe_out, UE_VOL_CLEAN, "volume is clean");
    }
    *out = e;
    return 0;
}

int ue_close(ue_engine_t *e)
{
    int err;

    if (!e)
        return EINVAL;
    // __ntfs_volume_release syncs dirty system inodes and calls the device sync.
    // Files removed while held are deleted now; the host holds nothing past unmount.
    // After a device failure nothing more is written; unmount fails and reports EIO, and the
    // dirty mark stays (D-86).
    err = e->readonly || e->dev_ctx->failed ? 0 : ue_delete_ghosts(e);
    // The dirty mark is cleared only once everything else is on the device, and only when
    // nothing failed: a crash between the two leaves the volume DIRTY, never falsely CLEAN.
    if (!err && !e->readonly && !e->dev_ctx->failed && !(err = sync_volume(e->vol)) && e->marked_dirty && e->mark_on)
        err = write_dirty(e, false);
    ue_held_free(&e->held);
    int uerr = unmount_volume(e->vol, e->dev_ctx);
    if (!err)
        err = uerr;
    free(e);
    return err;
}

int ue_writeback(ue_engine_t *e, uint32_t min_age_ms)
{
    if (!e)
        return EINVAL;
    if (e->dev_ctx->failed)
        return EIO;
    if (!e->dev_ctx->cache || bc_dirty_age_ms(e->dev_ctx->cache) < min_age_ms ||
        bc_dirty(e->dev_ctx->cache) == 0)
        return 0;
    return ue_device_writeback(e->dev_ctx);
}

void ue_get_cache_stats(const ue_engine_t *e, ue_cache_stats_t *out)
{
    bc_get_stats(e ? e->dev_ctx->cache : NULL, out);
}

bool ue_is_readonly(const ue_engine_t *e)
{
    return e ? e->readonly : true;
}

bool ue_is_marked_dirty(const ue_engine_t *e)
{
    return e && e->mark_on;
}

bool ue_has_failed(const ue_engine_t *e)
{
    return e && e->dev_ctx->failed;
}
