// SPDX-License-Identifier: GPL-3.0-or-later
// Volume status record, status/<serial>.json, shared with the app and CLI (D-41).
#ifndef UNLATCH_FUSE_STATUS_H
#define UNLATCH_FUSE_STATUS_H

#include <stdbool.h>

#include "unlatch_engine.h"

// Writes the record atomically. Returns 0 or errno; a failure is logged, never fatal.
int status_write(const char *bsd_name, const ue_probe_t *p, bool read_only, bool mounted);

#endif // UNLATCH_FUSE_STATUS_H
