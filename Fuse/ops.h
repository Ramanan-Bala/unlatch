// SPDX-License-Identifier: GPL-3.0-or-later
// FUSE low-level operations over the engine (D-40).
#ifndef UNLATCH_FUSE_OPS_H
#define UNLATCH_FUSE_OPS_H

#define FUSE_USE_VERSION 318

#include <fuse_lowlevel.h>
#include <stdbool.h>

#include "unlatch_engine.h"

typedef struct {
    uint64_t ino;                     // engine reference
    uint64_t nlookup;                 // FUSE lookup count; 0 = free slot
} uf_node_t;

typedef struct {
    ue_engine_t *engine;
    ue_ino_t root;                    // engine reference of the root (FUSE_ROOT_ID maps to it)
    bool read_only;
    uid_t uid;
    gid_t gid;
    uf_node_t *nodes;                 // open addressing, power-of-two capacity
    size_t cap, count;
} uf_fs_t;

extern const struct fuse_lowlevel_ops uf_ops;

#endif // UNLATCH_FUSE_OPS_H
