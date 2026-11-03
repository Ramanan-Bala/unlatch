// SPDX-License-Identifier: GPL-3.0-or-later
// Extended attributes as named $DATA streams (plan Step 16, D-48).
//
// An xattr named N is the stream N (':' stored as U+F03A), as ntfs-3g does on macOS: Windows sees
// file:N as an alternate data stream and Linux ntfs-3g (streams_interface=xattr) shows it as
// user.N. com.apple.FinderInfo and com.apple.ResourceFork are streams like any other.
#include "internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifndef ENOATTR
#define ENOATTR 93
#endif

// Stream names cannot hold ':' (it separates file and stream in Win32 paths), but macOS
// uses it (com.apple.metadata:_kMDItemUserTags), so ':' is stored as U+F03A, the private-use
// substitute that Windows Services for Mac and WSL use for it. '/' is refused as in names.
#define COLON_SUB "\xEF\x80\xBA"              // U+F03A in UTF-8

static int stream_name(const char *name, ntfschar **u, int *len)
{
    char mapped[1024];
    size_t o = 0;

    if (!name || !*name)
        return EINVAL;
    for (const char *p = name; *p; p++) {
        if (o + 4 > sizeof(mapped))
            return ENAMETOOLONG;
        if (*p == ':') {
            memcpy(mapped + o, COLON_SUB, 3);
            o += 3;
        } else {
            mapped[o++] = *p;
        }
    }
    mapped[o] = 0;
    return ue_to_ucs(mapped, u, len);
}

// The reverse of stream_name, in place.
static void unmap_colons(char *s)
{
    char *w = s;

    for (const char *r = s; *r;) {
        if (!strncmp(r, COLON_SUB, 3)) {
            *w++ = ':';
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;
}

int ue_xattr_list(ue_engine_t *e, ue_ino_t ino, char *buf, size_t size, size_t *needed)
{
    ntfs_attr_search_ctx *ctx;
    ntfs_inode *ni;
    size_t used = 0;
    int err;

    if (!e || !needed || (size && !buf))
        return EINVAL;
    if ((err = ue_iget(e, ino, &ni)))
        return err;
    if (!(ctx = ntfs_attr_get_search_ctx(ni, NULL))) {
        ue_iput(ni);
        return ENOMEM;
    }
    while (!ntfs_attr_lookup(AT_DATA, NULL, 0, CASE_SENSITIVE, 0, NULL, 0, ctx)) {
        ATTR_RECORD *a = ctx->attr;
        char *name = NULL;
        // The unnamed stream, and later extents of a non-resident stream (lowest_vcn shares
        // its bytes with the value header of a resident one).
        if (!a->name_length || (a->non_resident && a->lowest_vcn))
            continue;
        if (ntfs_ucstombs((ntfschar *)((u8 *)a + le16_to_cpu(a->name_offset)), a->name_length, &name, 0) < 0)
            continue;                              // not representable in UTF-8: skip it
        unmap_colons(name);
        size_t n = strlen(name) + 1;
        if (buf && used + n <= size)
            memcpy(buf + used, name, n);
        else if (buf)
            err = ERANGE;
        used += n;
        free(name);
    }
    ntfs_attr_put_search_ctx(ctx);
    *needed = used;
    int cerr = ue_iput(ni);
    return err ? err : cerr;
}

int ue_xattr_get(ue_engine_t *e, ue_ino_t ino, const char *name, uint64_t pos, void *buf, size_t size,
                 size_t *len)
{
    ntfs_inode *ni;
    ntfs_attr *na;
    ntfschar *u;
    int ulen, err;

    if (!e || !len || (size && !buf))
        return EINVAL;
    if ((err = stream_name(name, &u, &ulen)))
        return err;
    if ((err = ue_iget(e, ino, &ni))) {
        free(u);
        return err;
    }
    na = ntfs_attr_open(ni, AT_DATA, u, (u32)ulen);
    free(u);
    if (!na) {
        err = errno == ENOENT ? ENOATTR : (errno ? errno : EIO);
        ue_iput(ni);
        return err;
    }
    uint64_t total = (uint64_t)na->data_size;
    if (!buf) {                                    // size query
        *len = total > pos ? (size_t)(total - pos) : 0;
    } else if (pos > total) {
        *len = 0;
    } else {
        size_t want = (size_t)(total - pos);
        if (want > size)
            err = ERANGE;
        size_t done = 0;
        while (!err && done < want) {
            s64 n = ntfs_attr_pread(na, (s64)(pos + done), (s64)(want - done), (char *)buf + done);
            if (n <= 0) {
                err = errno ? errno : EIO;
                break;
            }
            done += (size_t)n;
        }
        *len = done;
    }
    ntfs_attr_close(na);
    int cerr = ue_iput(ni);
    return err ? err : cerr;
}

int ue_xattr_set(ue_engine_t *e, ue_ino_t ino, const char *name, uint64_t pos, const void *buf, size_t size,
                 uint32_t flags)
{
    ntfs_inode *ni;
    ntfs_attr *na;
    ntfschar *u;
    int ulen, err;

    if (!e || (size && !buf))
        return EINVAL;
    if ((err = ue_mark_busy(e)))       // D-95
        return err;
    if (pos + size > UE_XATTR_MAX)
        return E2BIG;
    if ((err = stream_name(name, &u, &ulen)))
        return err;
    if ((err = ue_iget(e, ino, &ni))) {
        free(u);
        return err;
    }
    bool exists = ntfs_attr_exist(ni, AT_DATA, u, (u32)ulen);
    if (exists && (flags & UE_XATTR_CREATE))
        err = EEXIST;
    else if (!exists && (flags & UE_XATTR_REPLACE))
        err = ENOATTR;
    else if (!exists && ntfs_attr_add(ni, AT_DATA, u, (u8)ulen, NULL, 0))
        err = errno ? errno : EIO;
    if (err) {
        free(u);
        ue_iput(ni);
        return err;
    }
    na = ntfs_attr_open(ni, AT_DATA, u, (u32)ulen);
    free(u);
    if (!na) {
        err = errno ? errno : EIO;
        ue_iput(ni);
        return err;
    }
    // A write at 0 replaces the value; a positioned write (resource forks) patches it.
    if (pos == 0 && na->data_size != (s64)size && ntfs_attr_truncate(na, (s64)size))
        err = errno ? errno : EIO;
    size_t done = 0;
    while (!err && done < size) {
        s64 n = ntfs_attr_pwrite(na, (s64)(pos + done), (s64)(size - done), (const char *)buf + done);
        if (n <= 0) {
            err = errno ? errno : EIO;
            break;
        }
        done += (size_t)n;
    }
    ntfs_attr_close(na);
    ntfs_inode_update_times(ni, NTFS_UPDATE_CTIME);
    NInoSetDirty(ni);
    int cerr = ue_iput(ni);
    return err ? err : cerr;
}

int ue_xattr_remove(ue_engine_t *e, ue_ino_t ino, const char *name)
{
    ntfs_inode *ni;
    ntfschar *u;
    int ulen, err;

    if (!e)
        return EINVAL;
    if ((err = ue_mark_busy(e)))       // D-95
        return err;
    if ((err = stream_name(name, &u, &ulen)))
        return err;
    if ((err = ue_iget(e, ino, &ni))) {
        free(u);
        return err;
    }
    if (!ntfs_attr_exist(ni, AT_DATA, u, (u32)ulen))
        err = ENOATTR;
    else if (ntfs_attr_remove(ni, AT_DATA, u, (u32)ulen))
        err = errno ? errno : EIO;
    free(u);
    if (!err) {
        ntfs_inode_update_times(ni, NTFS_UPDATE_CTIME);
        NInoSetDirty(ni);
    }
    int cerr = ue_iput(ni);
    return err ? err : cerr;
}
