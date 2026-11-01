// SPDX-License-Identifier: GPL-3.0-or-later
// Write path: write, create, remove, rename, setattr, sync.
// Every name change is built from two primitives, link_name and unlink_name, in the order
// lowntfs-3g's ntfs_fuse_rename uses, so each intermediate state is a valid volume.
#include "internal.h"

#include <limits.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <ntfs-3g/attrib.h>
#include <ntfs-3g/dir.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/layout.h>
#include <ntfs-3g/ntfstime.h>
#include <ntfs-3g/unistr.h>

#define MAX_DEPTH 4096

static void set_archive(ntfs_inode *ni)
{
    if (!(ni->flags & FILE_ATTR_ARCHIVE)) {
        ni->flags |= FILE_ATTR_ARCHIVE;
        NInoSetDirty(ni);
        NInoFileNameSetDirty(ni);
    }
}

static int open_dir(ue_engine_t *e, ue_ino_t dir, ntfs_inode **out)
{
    int err = ue_iget(e, dir, out);

    if (!err && !ue_is_dir(*out)) {
        ue_iput(*out);
        return ENOTDIR;
    }
    return err;
}

// Finds name in dir. Returns 0, ENOENT or another errno.
static int find(ntfs_inode *dni, const ntfschar *u, int len, u64 *mref)
{
    *mref = ntfs_inode_lookup_by_name(dni, u, len);
    if (*mref == (u64)-1)
        return errno && errno != ENOENT ? errno : ENOENT;
    return 0;
}

// Adds name `u` in dir for child. Closes both before returning (dir first, as lowntfs-3g
// does, so syncing the child's names finds the new index entry on disk).
static int link_name(ue_engine_t *e, ue_ino_t child, ue_ino_t dir, const ntfschar *u, int len)
{
    ntfs_inode *ni, *dni;
    int err;

    if ((err = ue_iget(e, child, &ni)))
        return err;
    if ((err = open_dir(e, dir, &dni))) {
        ue_iput(ni);
        return err;
    }
    if (ntfs_link(ni, dni, u, (u8)len))
        err = errno ? errno : EIO;
    else {
        ntfs_inode_update_times(ni, NTFS_UPDATE_CTIME);
        ntfs_inode_update_times(dni, NTFS_UPDATE_MCTIME);
    }
    int e1 = ue_iput(dni), e2 = ue_iput(ni);
    return err ? err : (e1 ? e1 : e2);
}

// Removes name `u` from dir. ntfs_delete frees the record when it was the last link.
static int unlink_name(ue_engine_t *e, ue_ino_t dir, const ntfschar *u, int len)
{
    ntfs_inode *ni, *dni;
    u64 mref;
    int err;

    if ((err = open_dir(e, dir, &dni)))
        return err;
    if ((err = find(dni, u, len, &mref)) || (err = ue_iget(e, mref, &ni))) {
        ue_iput(dni);
        return err;
    }
    // ntfs_delete always closes ni and dni, on success and on failure.
    if (ntfs_delete(e->vol, NULL, ni, dni, u, (u8)len))
        return errno ? errno : EIO;
    return 0;
}

static int tmp_name(ue_engine_t *e, const char *prefix, uint64_t id, ntfschar **u, int *len)
{
    char name[64];

    snprintf(name, sizeof(name), "%s%llu", prefix, (unsigned long long)id);
    (void)e;
    return ue_to_ucs(name, u, len);
}

static int real_links(ntfs_inode *ni)
{
    ue_attr_t a;

    return ue_fill_attr(ni, &a) ? 1 : (int)a.nlink;
}

// Sets FILE_ATTR_HIDDEN|SYSTEM on an inode, so a ghost name stays out of sight on Windows.
static int mark_ghost(ue_engine_t *e, ue_ino_t ino)
{
    ntfs_inode *ni;
    int err = ue_iget(e, ino, &ni);

    if (err)
        return err;
    ni->flags |= FILE_ATTR_HIDDEN | FILE_ATTR_SYSTEM;
    NInoSetDirty(ni);
    NInoFileNameSetDirty(ni);
    return ue_iput(ni);
}

// Removes `u` from dir. A file whose last name goes away while the host still holds it is
// moved to a hidden ghost name in the root instead, and deleted on ue_release (D-25).
static int remove_name(ue_engine_t *e, ue_ino_t dir, const ntfschar *u, int len, int want_dir)
{
    ntfs_inode *dni, *ni;
    ue_held_t *h;
    ue_ino_t dir_ino, ino;
    u64 mref;
    int err, links;
    bool isdir;

    if ((err = open_dir(e, dir, &dni)))
        return err;
    dir_ino = ue_ino_of(dni);
    if (dni->mft_no == FILE_Extend) {
        ue_iput(dni);
        return EPERM;
    }
    if ((err = find(dni, u, len, &mref)) || (err = ue_iget(e, mref, &ni))) {
        ue_iput(dni);
        return err;
    }
    if (ue_hidden_entry(dni->mft_no, ni->mft_no, NULL)) {
        err = ENOENT;                   // metafiles are invisible, as in lookup
        goto close_both;
    }
    if (ni->mft_no < FILE_first_user) {
        err = EPERM;
        goto close_both;
    }
    isdir = ue_is_dir(ni);
    if (want_dir == 1 && !isdir) {
        err = ENOTDIR;
        goto close_both;
    }
    if (want_dir == 0 && isdir) {
        err = EISDIR;
        goto close_both;
    }
    if (isdir && ntfs_check_empty_dir(ni)) {
        err = errno ? errno : ENOTEMPTY;
        goto close_both;
    }
    ino = ue_ino_of(ni);
    links = real_links(ni);
    h = ue_held_find(&e->held, ni->mft_no);
    if (!isdir && links == 1 && h && h->seq == MSEQNO(ino) && !h->ghost) {
        ntfschar *g;
        int glen;
        uint64_t id = ++e->next_tmp_id;

        int e1 = ue_iput(ni), e2 = ue_iput(dni);
        if (e1 || e2)
            return e1 ? e1 : e2;
        if ((err = tmp_name(e, UE_GHOST_PREFIX, id, &g, &glen)))
            return err;
        // Ghosts live in the root, not the parent, so the parent can still be removed
        // (rmdir of a directory whose only content is an open, unlinked file) and a
        // crash leaves leftovers in one known place (found by fsx).
        if (!(err = link_name(e, ino, UE_ROOT_INO, g, glen))) {
            if ((err = unlink_name(e, dir_ino, u, len)))
                unlink_name(e, UE_ROOT_INO, g, glen);  // undo the ghost link
        }
        free(g);
        if (err)
            return err;
        mark_ghost(e, ino);
        h = ue_held_find(&e->held, MREF(ino));     // the table may have moved
        if (h) {
            h->ghost = true;
            h->ghost_dir = UE_ROOT_INO;
            h->ghost_id = id;
        }
        return 0;
    }
    // ntfs_delete always closes ni and dni.
    if (ntfs_delete(e->vol, NULL, ni, dni, u, (u8)len))
        return errno ? errno : EIO;
    return 0;

close_both:
    ue_iput(ni);
    ue_iput(dni);
    return err;
}

int ue_delete_ghost(ue_engine_t *e, ue_held_t *h)
{
    ntfschar *g;
    int glen, err;

    if (!h->ghost)
        return 0;
    if ((err = tmp_name(e, UE_GHOST_PREFIX, h->ghost_id, &g, &glen)))
        return err;
    err = unlink_name(e, h->ghost_dir, g, glen);
    free(g);
    h->ghost = false;
    return err;
}

int ue_delete_ghosts(ue_engine_t *e)
{
    int err = 0;

    for (size_t i = 0; i < e->held.cap; i++) {
        ue_held_t *h = &e->held.slots[i];
        if (h->used && h->ghost) {
            int r = ue_delete_ghost(e, h);
            if (r && !err)
                err = r;
        }
    }
    return err;
}

// Win32 rules: no \ / : * ? " < > | or control characters, no trailing dot or space,
// no reserved device names; plus the engine's own internal prefixes.
static int check_new_name(ue_engine_t *e, const char *name, ntfschar **u, int *len)
{
    int err = ue_to_ucs(name, u, len);

    if (err)
        return err;
    if (ntfs_forbidden_names(e->vol, *u, *len, TRUE) || ue_hidden_entry(0, UINT64_MAX, name)) {
        free(*u);
        *u = NULL;
        return EINVAL;
    }
    return 0;
}

int ue_write(ue_engine_t *e, ue_ino_t ino, uint64_t off, const void *buf, size_t len, size_t *done)
{
    ntfs_inode *ni;
    ntfs_attr *na;
    size_t total = 0;
    int err = 0;

    if (!e || (!buf && len) || !done)
        return EINVAL;
    *done = 0;
    if (e->readonly)
        return EROFS;
    if (off > (uint64_t)INT64_MAX - len)
        return EFBIG;
    if ((err = ue_iget(e, ino, &ni)))
        return err;
    if (ue_is_dir(ni))
        err = EISDIR;
    else if (ni->flags & FILE_ATTR_ENCRYPTED)
        err = EACCES;
    else if (ni->flags & FILE_ATTR_REPARSE_POINT)
        err = EOPNOTSUPP;              // symlinks, junctions, cloud placeholders (Step 16)
    if (err) {
        ue_iput(ni);
        return err;
    }
    if (len == 0)
        return ue_iput(ni);
    na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
    if (!na) {
        err = errno ? errno : EIO;
        ue_iput(ni);
        return err;
    }
    while (total < len) {
        s64 n = ntfs_attr_pwrite(na, (s64)(off + total), (s64)(len - total), (const char *)buf + total);
        if (n <= 0) {
            err = errno ? errno : EIO;
            break;
        }
        total += (size_t)n;
    }
    ntfs_attr_close(na);
    if (total) {
        ntfs_inode_update_times(ni, NTFS_UPDATE_MCTIME);
        set_archive(ni);
    }
    int cerr = ue_iput(ni);
    *done = total;
    if (total)
        return cerr;
    return err ? err : cerr;
}

int ue_create(ue_engine_t *e, ue_ino_t dir, const char *name, uint32_t type, uint32_t mode, ue_attr_t *out)
{
    ntfs_inode *dni, *ni;
    ntfschar *u;
    u64 mref;
    int len, err;

    if (!e || !out)
        return EINVAL;
    if (e->readonly)
        return EROFS;
    if (type != UE_T_REG && type != UE_T_DIR)
        return type == UE_T_LNK ? EOPNOTSUPP : EINVAL;   // symlinks: Step 16
    if ((err = check_new_name(e, name, &u, &len)))
        return err;
    if ((err = open_dir(e, dir, &dni))) {
        free(u);
        return err;
    }
    if (dni->mft_no == FILE_Extend) {
        err = EPERM;
        goto out;
    }
    if ((err = find(dni, u, len, &mref)) != ENOENT) {
        err = err ? err : EEXIST;
        goto out;
    }
    err = 0;
    ni = ntfs_create(dni, const_cpu_to_le32(0), u, (u8)len, type == UE_T_DIR ? S_IFDIR : S_IFREG);
    if (!ni) {
        err = errno ? errno : EIO;
        goto out;
    }
    if (type == UE_T_REG) {
        ni->flags |= FILE_ATTR_ARCHIVE;
        if (!(mode & S_IWUSR))
            ni->flags |= FILE_ATTR_READONLY;
        NInoFileNameSetDirty(ni);
    }
    ntfs_inode_update_mbsname(dni, name, ni->mft_no);
    // ntfs_create leaves the parent's times alone (lowntfs-3g updates them itself);
    // the readdir verifier depends on this.
    ntfs_inode_update_times(dni, NTFS_UPDATE_MCTIME);
    NInoSetDirty(ni);
    err = ue_fill_attr(ni, out);
    if (!err)
        err = ue_hold(e, ni);
    // Closing ni syncs its name into dni's index; pass dni to avoid opening it twice.
    if (ntfs_inode_close_in_dir(ni, dni) && !err)
        err = errno ? errno : EIO;
out:
    free(u);
    int cerr = ue_iput(dni);
    return err ? err : cerr;
}

// Symlinks are created as WSL symlinks (IO_REPARSE_TAG_LX_SYMLINK, D-49): readable by
// Windows (WSL), Linux ntfs-3g and newer ntfs3, with the target stored as UTF-8 verbatim.
int ue_symlink(ue_engine_t *e, ue_ino_t dir, const char *name, const char *target, ue_attr_t *out)
{
    ntfs_inode *dni, *ni;
    ntfschar *u, *ut = NULL;
    u64 mref;
    int len, tlen, err;

    if (!e || !out || !target || !*target)
        return EINVAL;
    if (e->readonly)
        return EROFS;
    if (strlen(target) >= PATH_MAX)
        return ENAMETOOLONG;
    if ((err = check_new_name(e, name, &u, &len)))
        return err;
    if ((tlen = ntfs_mbstoucs(target, &ut)) <= 0) {
        free(u);
        return EINVAL;
    }
    if ((err = open_dir(e, dir, &dni))) {
        free(u);
        free(ut);
        return err;
    }
    if (dni->mft_no == FILE_Extend) {
        err = EPERM;
        goto out;
    }
    if ((err = find(dni, u, len, &mref)) != ENOENT) {
        err = err ? err : EEXIST;
        goto out;
    }
    err = 0;
    e->vol->special_files = NTFS_FILES_WSL;
    ni = ntfs_create_symlink(dni, const_cpu_to_le32(0), u, (u8)len, ut, tlen);
    if (!ni) {
        err = errno ? errno : EIO;
        goto out;
    }
    ntfs_inode_update_mbsname(dni, name, ni->mft_no);
    ntfs_inode_update_times(dni, NTFS_UPDATE_MCTIME);
    NInoSetDirty(ni);
    err = ue_fill_attr(ni, out);
    if (!err)
        err = ue_hold(e, ni);
    if (ntfs_inode_close_in_dir(ni, dni) && !err)
        err = errno ? errno : EIO;
out:
    free(u);
    free(ut);
    int cerr = ue_iput(dni);
    return err ? err : cerr;
}

// A hard link: another $FILE_NAME for ino in newdir. Directories cannot be linked.
int ue_link(ue_engine_t *e, ue_ino_t ino, ue_ino_t newdir, const char *newname, ue_attr_t *out)
{
    ntfs_inode *ni, *dni;
    ntfschar *u;
    u64 mref;
    int len, err;

    if (!e || !out)
        return EINVAL;
    if (e->readonly)
        return EROFS;
    if ((err = check_new_name(e, newname, &u, &len)))
        return err;
    if ((err = ue_iget(e, ino, &ni))) {
        free(u);
        return err;
    }
    bool isdir = ue_is_dir(ni);
    err = ue_iput(ni);
    if (!err && isdir)
        err = EPERM;
    if (!err && !(err = open_dir(e, newdir, &dni))) {
        if (dni->mft_no == FILE_Extend)
            err = EPERM;
        else if ((err = find(dni, u, len, &mref)) == ENOENT)
            err = 0;
        else if (!err)
            err = EEXIST;
        int cerr = ue_iput(dni);
        err = err ? err : cerr;
    }
    if (!err)
        err = link_name(e, ino, newdir, u, len);
    free(u);
    if (err)
        return err;
    if ((err = ue_iget(e, ino, &ni)))
        return err;
    err = ue_fill_attr(ni, out);
    if (!err)
        err = ue_hold(e, ni);
    int cerr = ue_iput(ni);
    return err ? err : cerr;
}

int ue_remove(ue_engine_t *e, ue_ino_t dir, const char *name, bool is_dir)
{
    ntfschar *u;
    int len, err;

    if (!e)
        return EINVAL;
    if (e->readonly)
        return EROFS;
    if ((err = ue_to_ucs(name, &u, &len)))
        return err;
    if (ue_hidden_entry(0, UINT64_MAX, name)) {
        free(u);
        return ENOENT;
    }
    // Metafiles in the root are rejected inside remove_name (record < FILE_first_user).
    err = remove_name(e, dir, u, len, is_dir ? 1 : 0);
    free(u);
    return err == ENOENT ? ENOENT : err;
}

// MFT reference of a directory's parent (its first non-DOS $FILE_NAME).
static int parent_of(ue_engine_t *e, uint64_t rec, uint64_t *parent)
{
    ntfs_inode *ni = ue_iopen_rec(e, rec);
    ntfs_attr_search_ctx *ctx;
    int err = ENOENT;

    if (!ni)
        return errno ? errno : EIO;
    ctx = ntfs_attr_get_search_ctx(ni, NULL);
    if (!ctx) {
        ue_iput(ni);
        return ENOMEM;
    }
    while (!ntfs_attr_lookup(AT_FILE_NAME, AT_UNNAMED, 0, CASE_SENSITIVE, 0, NULL, 0, ctx)) {
        const FILE_NAME_ATTR *fn = (const FILE_NAME_ATTR *)((const u8 *)ctx->attr +
                                                            le16_to_cpu(ctx->attr->value_offset));
        if (fn->file_name_type != FILE_NAME_DOS) {
            *parent = MREF_LE(fn->parent_directory);
            err = 0;
            break;
        }
    }
    ntfs_attr_put_search_ctx(ctx);
    ue_iput(ni);
    return err;
}

// EINVAL if dir is src or lies inside src's subtree.
static int check_not_inside(ue_engine_t *e, uint64_t src_rec, uint64_t dir_rec)
{
    uint64_t cur = dir_rec;
    int err;

    for (int depth = 0; depth < MAX_DEPTH; depth++) {
        if (cur == src_rec)
            return EINVAL;
        if (cur == FILE_root)
            return 0;
        if ((err = parent_of(e, cur, &cur)))
            return err;
    }
    return ELOOP;
}

static int lookup_ref(ue_engine_t *e, ue_ino_t dir, const ntfschar *u, int len, u64 *mref)
{
    ntfs_inode *dni;
    int err;

    if ((err = open_dir(e, dir, &dni)))
        return err;
    err = find(dni, u, len, mref);
    int cerr = ue_iput(dni);
    return err ? err : cerr;
}

static int kind_of(ue_engine_t *e, u64 mref, bool *isdir, bool *empty)
{
    ntfs_inode *ni;
    int err = ue_iget(e, mref, &ni);

    if (err)
        return err;
    if (ni->mft_no < FILE_first_user) {
        ue_iput(ni);
        return EPERM;
    }
    *isdir = ue_is_dir(ni);
    if (empty)
        *empty = !*isdir || ntfs_check_empty_dir(ni) == 0;
    return ue_iput(ni);
}

int ue_rename(ue_engine_t *e, ue_ino_t odir, const char *oname, ue_ino_t ndir, const char *nname, uint32_t flags)
{
    ntfschar *ou = NULL, *nu = NULL, *tu = NULL;
    int olen, nlen, tlen, err;
    u64 src, tgt;
    bool src_dir, tgt_dir, tgt_empty;
    uint64_t odir_rec = MREF(odir), ndir_rec = MREF(ndir);

    if (!e)
        return EINVAL;
    if (e->readonly)
        return EROFS;
    if ((err = ue_to_ucs(oname, &ou, &olen)))
        return err;
    if (ue_hidden_entry(0, UINT64_MAX, oname)) {
        err = ENOENT;
        goto out;
    }
    if ((err = check_new_name(e, nname, &nu, &nlen)))
        goto out;
    if ((err = lookup_ref(e, odir, ou, olen, &src)))
        goto out;
    if (odir_rec == FILE_root && MREF(src) < 24) {   // metafile in the root
        err = ENOENT;
        goto out;
    }
    if ((err = kind_of(e, src, &src_dir, NULL)))
        goto out;
    if (src_dir && (err = check_not_inside(e, MREF(src), ndir_rec)))
        goto out;
    // Fresh volume-unique temp names; libntfs-3g's lookup cache needs nothing else.
    if ((err = tmp_name(e, UE_RENAME_PREFIX, ++e->next_tmp_id, &tu, &tlen)))
        goto out;

    err = lookup_ref(e, ndir, nu, nlen, &tgt);
    if (err && err != ENOENT)
        goto out;
    if (err == ENOENT) {
        // Plain move: add the new name, then drop the old one.
        if ((err = link_name(e, src, ndir, nu, nlen)))
            goto out;
        if ((err = unlink_name(e, odir, ou, olen)))
            unlink_name(e, ndir, nu, nlen);
        goto out;
    }
    if (MREF(tgt) == MREF(src)) {
        // Same item: a no-op unless only the case (or an 8.3 alias) differs in one dir.
        if (odir_rec != ndir_rec || strcmp(oname, nname) == 0) {
            err = 0;
            goto out;
        }
        // $I30 collates case-insensitively, so "A" cannot be added next to "a": go
        // through a temp name, which also keeps a link at every step.
        if ((err = link_name(e, src, ndir, tu, tlen)))
            goto out;
        if ((err = unlink_name(e, odir, ou, olen))) {
            unlink_name(e, ndir, tu, tlen);
            goto out;
        }
        if ((err = link_name(e, src, ndir, nu, nlen))) {
            link_name(e, src, odir, ou, olen);       // restore the old name
            unlink_name(e, ndir, tu, tlen);
            goto out;
        }
        err = unlink_name(e, ndir, tu, tlen);
        goto out;
    }
    if (flags & UE_RENAME_EXCL) {
        err = EEXIST;
        goto out;
    }
    if ((err = kind_of(e, tgt, &tgt_dir, &tgt_empty)))
        goto out;
    if (src_dir && !tgt_dir) {
        err = ENOTDIR;
        goto out;
    }
    if (!src_dir && tgt_dir) {
        err = EISDIR;
        goto out;
    }
    if (tgt_dir && !tgt_empty) {
        err = ENOTEMPTY;
        goto out;
    }
    // Replace: park the target under a temp name, put src in its place, drop src's old
    // name, then remove the parked target (a ghost if the host still holds it).
    if ((err = link_name(e, tgt, ndir, tu, tlen)))
        goto out;
    if ((err = unlink_name(e, ndir, nu, nlen))) {
        unlink_name(e, ndir, tu, tlen);
        goto out;
    }
    if ((err = link_name(e, src, ndir, nu, nlen))) {
        link_name(e, tgt, ndir, nu, nlen);
        unlink_name(e, ndir, tu, tlen);
        goto out;
    }
    if ((err = unlink_name(e, odir, ou, olen))) {
        unlink_name(e, ndir, nu, nlen);
        link_name(e, tgt, ndir, nu, nlen);
        unlink_name(e, ndir, tu, tlen);
        goto out;
    }
    err = remove_name(e, ndir, tu, tlen, -1);
out:
    free(ou);
    free(nu);
    free(tu);
    return err;
}

int ue_setattr(ue_engine_t *e, ue_ino_t ino, uint32_t mask, const ue_attr_t *in, ue_attr_t *out)
{
    ntfs_inode *ni;
    ntfs_attr *na;
    bool changed = false;
    int err = 0;

    if (!e || (!in && mask))
        return EINVAL;
    if (e->readonly && mask)
        return EROFS;
    if ((err = ue_iget(e, ino, &ni)))
        return err;
    if (mask && ni->mft_no < FILE_first_user && ni->mft_no != FILE_root) {
        err = EPERM;
        goto out;
    }
    if (mask & UE_SET_SIZE) {
        if (ue_is_dir(ni)) {
            err = EISDIR;
            goto out;
        }
        if (ni->flags & (FILE_ATTR_ENCRYPTED | FILE_ATTR_REPARSE_POINT)) {
            err = (ni->flags & FILE_ATTR_ENCRYPTED) ? EACCES : EOPNOTSUPP;
            goto out;
        }
        if (in->size > (uint64_t)INT64_MAX) {
            err = EFBIG;
            goto out;
        }
        na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
        if (!na) {
            err = errno ? errno : EIO;
            goto out;
        }
        if ((s64)in->size != na->data_size) {
            int r;
            // Compressed files grow by writing a final zero, which becomes a hole
            // (as lowntfs-3g's ntfs_fuse_trunc does).
            if ((na->data_flags & ATTR_COMPRESSION_MASK) && (s64)in->size > na->initialized_size) {
                char zero = 0;
                r = ntfs_attr_pwrite(na, (s64)in->size - 1, 1, &zero) <= 0;
            } else {
                r = ntfs_attr_truncate(na, (s64)in->size);
            }
            if (r)
                err = errno ? errno : EIO;
            else {
                ntfs_inode_update_times(ni, NTFS_UPDATE_MCTIME);
                set_archive(ni);
            }
        }
        ntfs_attr_close(na);
        if (err)
            goto out;
    }
    if (mask & UE_SET_BIRTH) {
        ni->creation_time = timespec2ntfs(in->birth);
        changed = true;
    }
    if (mask & UE_SET_MTIME) {
        ni->last_data_change_time = timespec2ntfs(in->mtime);
        changed = true;
    }
    if (mask & UE_SET_ATIME) {
        ni->last_access_time = timespec2ntfs(in->atime);
        changed = true;
    }
    if (mask & (UE_SET_FLAGS | UE_SET_MODE)) {
        le32 keep = ni->flags & ~(FILE_ATTR_HIDDEN | FILE_ATTR_SYSTEM | FILE_ATTR_READONLY | FILE_ATTR_ARCHIVE);
        le32 set = ni->flags & (FILE_ATTR_HIDDEN | FILE_ATTR_SYSTEM | FILE_ATTR_READONLY | FILE_ATTR_ARCHIVE);
        if (mask & UE_SET_FLAGS) {
            set = 0;
            if (in->flags & UE_F_HIDDEN) set |= FILE_ATTR_HIDDEN;
            if (in->flags & UE_F_SYSTEM) set |= FILE_ATTR_SYSTEM;
            if (in->flags & UE_F_READONLY) set |= FILE_ATTR_READONLY;
            if (in->flags & UE_F_ARCHIVE) set |= FILE_ATTR_ARCHIVE;
        }
        // Mode: only a file's owner-write bit means anything (DOS read-only).
        if ((mask & UE_SET_MODE) && !ue_is_dir(ni)) {
            if (in->mode & S_IWUSR)
                set &= ~FILE_ATTR_READONLY;
            else
                set |= FILE_ATTR_READONLY;
        }
        ni->flags = keep | set;
        changed = true;
    }
    if (mask & UE_SET_CTIME) {
        ni->last_mft_change_time = timespec2ntfs(in->ctime);
        changed = true;
    } else if (changed) {
        ntfs_inode_update_times(ni, NTFS_UPDATE_CTIME);
    }
    if (changed) {
        NInoSetDirty(ni);
        NInoFileNameSetDirty(ni);
    }
out:
    if (!err && out)
        err = ue_fill_attr(ni, out);
    int cerr = ue_iput(ni);
    return err ? err : cerr;
}

int ue_sync(ue_engine_t *e)
{
    struct ntfs_device *dev;

    if (!e)
        return EINVAL;
    if (e->readonly)
        return 0;
    if (e->dev_ctx->failed)
        return EIO;
    // No inode is held open between calls and libntfs-3g syncs inodes on close, so only
    // the device needs flushing.
    dev = e->vol->dev;
    if (dev->d_ops->sync(dev))
        return errno ? errno : EIO;
    return 0;
}
