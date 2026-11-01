// SPDX-License-Identifier: GPL-3.0-or-later
// Host-side path walking on top of ue_lookup (harness and tests).
#ifndef UE_WALK_H
#define UE_WALK_H

#include "unlatch_engine.h"

// Resolves an absolute or relative-to-root path ("/", "/a/b", "a/b"). Returns 0 or errno.
int ue_walk(ue_engine_t *e, const char *path, ue_attr_t *out);

#endif // UE_WALK_H
