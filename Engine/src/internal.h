// SPDX-License-Identifier: GPL-3.0-or-later
// Private to Engine/src. The only place ntfs-3g headers may be included.
#ifndef UE_INTERNAL_H
#define UE_INTERNAL_H

// Same config.h libntfs-3g was built with, so struct layouts match (D-13).
#include "config.h"

#include <ntfs-3g/types.h>
#include <ntfs-3g/device.h>
#include <ntfs-3g/volume.h>

#include "unlatch_engine.h"

// Device bridge: one per mounted volume, owned by the engine (or the probe).
typedef struct ue_dev {
    ue_io_t io;
    uint64_t pos;                     // seek position for read/write
} ue_dev_t;

// Allocates an ntfs_device backed by io. On success *ctx_out must outlive the device.
struct ntfs_device *ue_device_new(const ue_io_t *io, ue_dev_t **ctx_out);

// Open-inode cache (icache.c), keyed by MFT record number.
typedef struct {
    uint64_t rec;
    ntfs_inode *ni;                   // NULL marks an empty slot
} ue_icache_ent_t;

typedef struct {
    ue_icache_ent_t *slots;
    size_t cap, count;                // cap is a power of two
} ue_icache_t;

int ue_icache_init(ue_icache_t *c);
ntfs_inode *ue_icache_find(const ue_icache_t *c, uint64_t rec);
int ue_icache_insert(ue_icache_t *c, uint64_t rec, ntfs_inode *ni);
ntfs_inode *ue_icache_remove(ue_icache_t *c, uint64_t rec);   // returns it; caller closes
int ue_icache_close_all(ue_icache_t *c);                      // closes every inode, frees slots

struct ue_engine {
    ntfs_volume *vol;                 // owns the ntfs_device
    ue_dev_t *dev_ctx;
    bool readonly;
    ue_icache_t icache;
    ue_stats_t stats;
};

// read.c: opens FILE_root into the cache (called by ue_open).
int ue_cache_root(ue_engine_t *e);

#endif // UE_INTERNAL_H
