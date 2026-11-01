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
    const char *mountpoint;           // absolute; readlink shows absolute Windows targets under it
    uid_t uid;
    gid_t gid;
    uf_node_t *nodes;                 // open addressing, power-of-two capacity
    char *rbuf;                       // read buffer, reused (the loop is single-threaded)
    size_t rbuf_cap;
    size_t cap, count;
    bool failed_logged;               // a failed write-back was logged once
    int close_err;                    // from ue_close, once closed
} uf_fs_t;

// Closes the engine once (on FUSE DESTROY, or after the loop if that never came) and logs.
void uf_close_engine(uf_fs_t *fs);

extern const struct fuse_lowlevel_ops uf_ops;

#endif // UNLATCH_FUSE_OPS_H
