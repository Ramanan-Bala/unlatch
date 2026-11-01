// SPDX-License-Identifier: GPL-3.0-or-later
// Host-side path walking on top of ue_lookup (harness and tests).
#ifndef UE_WALK_H
#define UE_WALK_H

#include "unlatch_engine.h"

// Resolves an absolute or relative-to-root path ("/", "/a/b", "a/b"). Returns 0 or errno.
int ue_walk(ue_engine_t *e, const char *path, ue_attr_t *out);

// Resolves path's parent directory into *dir and points *leaf at the last component
// (inside path). Fails with EINVAL for "/" or an empty path.
int ue_walk_parent(ue_engine_t *e, const char *path, ue_attr_t *dir, const char **leaf);

#endif // UE_WALK_H
