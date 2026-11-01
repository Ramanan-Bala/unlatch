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

struct ue_engine {
    ntfs_volume *vol;                 // owns the ntfs_device
    ue_dev_t *dev_ctx;
    bool readonly;
};

#endif // UE_INTERNAL_H
