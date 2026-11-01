// SPDX-License-Identifier: GPL-3.0-or-later
// Per-call inode access, attributes and name rules shared by the read and write paths.
//
// The engine opens an ntfs_inode at the start of a call and closes it before returning,
// the way libntfs-3g's own lowntfs-3g driver does. libntfs-3g caches closed inodes
// (CACHE_NIDATA, synced on close) and opens parent directories by reference internally
// (for example to sync a file's name entry), so holding an inode open across calls could
// create a second, diverging copy of the same record (D-24).
#include "internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <ntfs-3g/attrib.h>
#include <ntfs-3g/dir.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/layout.h>
#include <ntfs-3g/ntfstime.h>
#include <ntfs-3g/unistr.h>

// Records below this in the root are metafiles ($MFT ... $Extend, then reserved).
#define UE_FIRST_VISIBLE_REC 24

ue_ino_t ue_ino_of(const ntfs_inode *ni)
{
    return MK_MREF(ni->mft_no, le16_to_cpu(ni->mrec->sequence_number));
}

bool ue_is_dir(const ntfs_inode *ni)
{
    return (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) != 0;
}

ntfs_inode *ue_iopen_rec(ue_engine_t *e, uint64_t rec)
{
    e->stats.inode_opens++;
    return ntfs_inode_open(e->vol, rec);
}

// Sequence 0 is accepted only for the root, so the host can start from UE_ROOT_INO.
int ue_iget(ue_engine_t *e, ue_ino_t ino, ntfs_inode **out)
{
    uint16_t want = MSEQNO(ino);
    ntfs_inode *ni;

    if (e->dev_ctx->failed)            // every call after a device failure (Step 10)
        return EIO;
    ni = ue_iopen_rec(e, MREF(ino));
    if (!ni)                           // freed records fail with ENOENT
        return errno == ENOENT ? ESTALE : (errno ? errno : EIO);
    if (!(want == 0 && MREF(ino) == FILE_root) && want != le16_to_cpu(ni->mrec->sequence_number)) {
        ntfs_inode_close(ni);
        return ESTALE;
    }
    *out = ni;
    return 0;
}

int ue_iput(ntfs_inode *ni)
{
    if (!ni)
        return 0;
    return ntfs_inode_close(ni) ? (errno ? errno : EIO) : 0;
}

static bool has_prefix(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

bool ue_hidden_entry(uint64_t dir_rec, uint64_t rec, const char *utf8_name)
{
    if (dir_rec == FILE_root && rec < UE_FIRST_VISIBLE_REC && rec != FILE_root)
        return true;
    return utf8_name && (has_prefix(utf8_name, UE_GHOST_PREFIX) ||
                         has_prefix(utf8_name, UE_RENAME_PREFIX));
}

static uint32_t map_flags(le32 fa)
{
    uint32_t f = 0;

    if (fa & FILE_ATTR_HIDDEN) f |= UE_F_HIDDEN;
    if (fa & FILE_ATTR_SYSTEM) f |= UE_F_SYSTEM;
    if (fa & FILE_ATTR_READONLY) f |= UE_F_READONLY;
    if (fa & FILE_ATTR_ARCHIVE) f |= UE_F_ARCHIVE;
    if (fa & FILE_ATTR_COMPRESSED) f |= UE_F_COMPRESSED;
    if (fa & FILE_ATTR_ENCRYPTED) f |= UE_F_ENCRYPTED;
    if (fa & FILE_ATTR_SPARSE_FILE) f |= UE_F_SPARSE;
    if (fa & FILE_ATTR_REPARSE_POINT) f |= UE_F_REPARSE;
    return f;
}

// Hard links: $FILE_NAME attributes, not counting DOS 8.3 aliases.
static uint32_t count_links(ntfs_inode *ni)
{
    ntfs_attr_search_ctx *ctx = ntfs_attr_get_search_ctx(ni, NULL);
    uint32_t n = 0;

    if (!ctx)
        return 1;
    while (!ntfs_attr_lookup(AT_FILE_NAME, AT_UNNAMED, 0, CASE_SENSITIVE, 0, NULL, 0, ctx)) {
        const FILE_NAME_ATTR *fn = (const FILE_NAME_ATTR *)((const u8 *)ctx->attr +
                                                            le16_to_cpu(ctx->attr->value_offset));
        if (fn->file_name_type != FILE_NAME_DOS)
            n++;
    }
    ntfs_attr_put_search_ctx(ctx);
    return n ? n : 1;
}

int ue_fill_attr(ntfs_inode *ni, ue_attr_t *out)
{
    ntfs_attr *na;

    memset(out, 0, sizeof(*out));
    out->ino = ue_ino_of(ni);
    out->flags = map_flags(ni->flags);
    out->nlink = count_links(ni);
    out->birth = ntfs2timespec(ni->creation_time);
    out->mtime = ntfs2timespec(ni->last_data_change_time);
    out->ctime = ntfs2timespec(ni->last_mft_change_time);
    out->atime = ntfs2timespec(ni->last_access_time);

    // Symlinks and junctions (a junction is a directory, but shows as a link, like ntfs-3g).
    if ((ni->flags & (FILE_ATTR_REPARSE_POINT | FILE_ATTR_SYSTEM)) && ue_is_symlink(ni)) {
        out->type = UE_T_LNK;
        out->mode = 0777;
        return 0;
    }
    if (ue_is_dir(ni)) {
        out->type = UE_T_DIR;
        out->mode = 0755;
        na = ntfs_attr_open(ni, AT_INDEX_ALLOCATION, NTFS_INDEX_I30, 4);
        if (na) {
            out->size = out->init_size = (uint64_t)na->data_size;
            out->alloc_size = (uint64_t)na->allocated_size;
            ntfs_attr_close(na);
        }
        return 0;
    }

    out->type = UE_T_REG;
    out->mode = (ni->flags & FILE_ATTR_READONLY) ? 0444 : 0644;
    na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
    if (!na) {
        if (errno == ENOENT)           // no unnamed $DATA: treat as empty
            return 0;
        return errno ? errno : EIO;
    }
    out->size = (uint64_t)na->data_size;
    out->init_size = (uint64_t)na->initialized_size;
    if (na->data_flags & (ATTR_COMPRESSION_MASK | ATTR_IS_SPARSE))
        out->alloc_size = (uint64_t)na->compressed_size;
    else
        out->alloc_size = (uint64_t)na->allocated_size;
    if (!NAttrNonResident(na))
        out->flags |= UE_F_RESIDENT;
    if (na->data_flags & ATTR_IS_SPARSE)
        out->flags |= UE_F_SPARSE;
    if (na->data_flags & ATTR_COMPRESSION_MASK)
        out->flags |= UE_F_COMPRESSED;
    if (na->data_flags & ATTR_IS_ENCRYPTED)
        out->flags |= UE_F_ENCRYPTED;
    ntfs_attr_close(na);
    return 0;
}

// UTF-8 -> UTF-16, rejecting names the seam does not allow.
int ue_to_ucs(const char *name, ntfschar **out, int *len_out)
{
    int len;

    if (!name || !*name || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, ".."))
        return EINVAL;
    *out = NULL;
    len = ntfs_mbstoucs(name, out);
    if (len < 0)
        return errno == ENAMETOOLONG ? ENAMETOOLONG : EINVAL;
    if (len > NTFS_MAX_NAME_LEN) {
        free(*out);
        *out = NULL;
        return ENAMETOOLONG;
    }
    *len_out = len;
    return 0;
}

int ue_hold(ue_engine_t *e, const ntfs_inode *ni)
{
    return ue_held_add(&e->held, ni->mft_no, le16_to_cpu(ni->mrec->sequence_number));
}
