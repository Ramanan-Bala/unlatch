// SPDX-License-Identifier: GPL-3.0-or-later
// Symbolic links: reading every kind NTFS has (plan Step 16, D-49).
#include "internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <ntfs-3g/reparse.h>

#define INTX_MAX_TARGET 4096              // UTF-16 units; ntfs-3g uses PATH_MAX

// Interix symlink (what older ntfs-3g created): a system file whose data is "IntxLNK\1" plus
// the UTF-16 target. Returns the target in UTF-8 (malloc'd), or NULL if ni is not one.
static char *intx_target(ntfs_inode *ni)
{
    ntfs_attr *na;
    INTX_FILE *f;
    char *target = NULL;

    if (!(ni->flags & FILE_ATTR_SYSTEM) || ue_is_dir(ni))
        return NULL;
    if (!(na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0)))
        return NULL;
    s64 size = na->data_size;
    if (size > (s64)sizeof(INTX_FILE_TYPES) &&
        size <= (s64)(sizeof(INTX_FILE_TYPES) + INTX_MAX_TARGET * sizeof(ntfschar)) &&
        (f = malloc((size_t)size))) {
        if (ntfs_attr_pread(na, 0, size, f) == size && f->magic == INTX_SYMBOLIC_LINK &&
            ntfs_ucstombs(f->target, (int)((size_t)(size - (s64)sizeof(INTX_FILE_TYPES)) / sizeof(ntfschar)),
                          &target, 0) <= 0)
            target = NULL;
        free(f);
    }
    ntfs_attr_close(na);
    return target;
}

bool ue_is_symlink(ntfs_inode *ni)
{
    if (ni->flags & FILE_ATTR_REPARSE_POINT)
        return ntfs_possible_symlink(ni);
    if (ni->flags & FILE_ATTR_SYSTEM) {
        char *t = intx_target(ni);
        free(t);
        return t != NULL;
    }
    return false;
}

int ue_readlink(ue_engine_t *e, ue_ino_t ino, const char *mount_point, char *buf, size_t size)
{
    ntfs_inode *ni;
    char *target = NULL;
    int err;

    if (!e || !buf || !size)
        return EINVAL;
    if ((err = ue_iget(e, ino, &ni)))
        return err;
    if (ni->flags & FILE_ATTR_REPARSE_POINT) {
        // Native symlinks, junctions and WSL links. Absolute Windows targets (C:\...) come
        // back under mount_point, the way ntfs-3g shows them; unresolvable ones as-is.
        errno = 0;
        target = ntfs_make_symlink(ni, mount_point ? mount_point : "/");
        if (!target)
            err = errno == EOPNOTSUPP || !errno ? EINVAL : errno;
    } else if (!(target = intx_target(ni))) {
        err = EINVAL;                        // readlink(2) on something that is not a link
    }
    if (target) {
        size_t n = strlen(target);
        if (n >= size)
            err = ENAMETOOLONG;
        else
            memcpy(buf, target, n + 1);
        free(target);
    }
    int cerr = ue_iput(ni);
    return err ? err : cerr;
}
