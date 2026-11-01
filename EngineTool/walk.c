// SPDX-License-Identifier: GPL-3.0-or-later
#include "walk.h"

#include <errno.h>
#include <string.h>

int ue_walk(ue_engine_t *e, const char *path, ue_attr_t *out)
{
    char comp[1024];
    const char *p = path;
    ue_ino_t cur = UE_ROOT_INO;
    int err;

    if ((err = ue_getattr(e, UE_ROOT_INO, out)))
        return err;
    cur = out->ino;
    while (*p) {
        const char *slash;
        size_t n;

        while (*p == '/')
            p++;
        if (!*p)
            break;
        slash = strchr(p, '/');
        n = slash ? (size_t)(slash - p) : strlen(p);
        if (n >= sizeof(comp))
            return ENAMETOOLONG;
        memcpy(comp, p, n);
        comp[n] = '\0';
        p += n;
        if ((err = ue_lookup(e, cur, comp, out)))
            return err;
        cur = out->ino;
    }
    return 0;
}

int ue_walk_parent(ue_engine_t *e, const char *path, ue_attr_t *dir, const char **leaf)
{
    char parent[4096];
    size_t n = strlen(path);
    const char *slash;

    while (n > 0 && path[n - 1] == '/')
        n--;
    if (n == 0 || n >= sizeof(parent))
        return n ? ENAMETOOLONG : EINVAL;
    memcpy(parent, path, n);
    parent[n] = '\0';
    slash = strrchr(parent, '/');
    *leaf = path + (slash ? (size_t)(slash - parent) + 1 : 0);
    if (slash)
        parent[slash - parent] = '\0';
    else
        parent[0] = '\0';
    // *leaf keeps any trailing slashes of path; callers pass clean paths.
    return ue_walk(e, parent, dir);
}
