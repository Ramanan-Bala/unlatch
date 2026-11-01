// SPDX-License-Identifier: GPL-3.0-or-later
// Private to Engine/src. The only place ntfs-3g headers may be included.
#ifndef UE_INTERNAL_H
#define UE_INTERNAL_H

// Same config.h libntfs-3g was built with, so struct layouts match (D-13).
#include "config.h"

#include <ntfs-3g/types.h>
#include <ntfs-3g/device.h>
#include <ntfs-3g/volume.h>
#include <ntfs-3g/logging.h>

#include "unlatch_engine.h"

// Device bridge: one per mounted volume, owned by the engine (or the probe).
typedef struct ue_dev {
    ue_io_t io;
    uint64_t pos;                     // seek position for read/write
    bool failed;                      // a write failed or the device went away: all I/O -> EIO
} ue_dev_t;

// Allocates an ntfs_device backed by io. On success *ctx_out must outlive the device.
struct ntfs_device *ue_device_new(const ue_io_t *io, ue_dev_t **ctx_out);

// Held-inode table (held.c), keyed by MFT record number.
typedef struct {
    uint64_t rec;
    uint16_t seq;
    bool used;
    bool ghost;                       // its last name was removed while held (D-25)
    uint64_t ghost_dir;               // directory holding the ghost name (the root)
    uint64_t ghost_id;                // name is UE_GHOST_PREFIX + ghost_id
} ue_held_t;

typedef struct {
    ue_held_t *slots;
    size_t cap, count;                // cap is a power of two
} ue_held_map_t;

int ue_held_init(ue_held_map_t *m);
void ue_held_free(ue_held_map_t *m);
ue_held_t *ue_held_find(const ue_held_map_t *m, uint64_t rec);
int ue_held_add(ue_held_map_t *m, uint64_t rec, uint16_t seq);
void ue_held_remove(ue_held_map_t *m, uint64_t rec);

struct ue_engine {
    ntfs_volume *vol;                 // owns the ntfs_device
    ue_dev_t *dev_ctx;
    bool readonly;
    ue_held_map_t held;
    uint64_t next_tmp_id;             // ghost and rename temp names
    ue_stats_t stats;
};

// Internal names the engine creates; hidden from lookup and readdir (D-25).
#define UE_GHOST_PREFIX ".unlatch-ghost-"
#define UE_RENAME_PREFIX ".unlatch-rename-"

// inode.c: per-call inode access (D-24).
int ue_iget(ue_engine_t *e, ue_ino_t ino, ntfs_inode **out);   // open + sequence check
ntfs_inode *ue_iopen_rec(ue_engine_t *e, uint64_t rec);          // open by record, counted
int ue_iput(ntfs_inode *ni);                                     // close; 0 or errno
bool ue_is_dir(const ntfs_inode *ni);
ue_ino_t ue_ino_of(const ntfs_inode *ni);
int ue_fill_attr(ntfs_inode *ni, ue_attr_t *out);
int ue_to_ucs(const char *name, ntfschar **out, int *len_out);  // validates seam names
bool ue_hidden_entry(uint64_t dir_rec, uint64_t rec, const char *utf8_name);
int ue_hold(ue_engine_t *e, const ntfs_inode *ni);              // host now holds this item
int ue_delete_ghost(ue_engine_t *e, ue_held_t *h);              // write.c
int ue_delete_ghosts(ue_engine_t *e);                            // ue_close

#endif // UE_INTERNAL_H
