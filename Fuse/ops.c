// SPDX-License-Identifier: GPL-3.0-or-later
// FUSE low-level operations over the engine (plan Step 08, D-40).
//
// The session loop is single-threaded, so every engine call is serialized (seam contract).
// FUSE node IDs are engine references, except the root, which FUSE calls FUSE_ROOT_ID. The
// engine holds an item from ue_lookup until ue_release; FUSE counts lookups and forgets them,
// so the adapter keeps the count and releases at zero.
#include "ops.h"

#include <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <time.h>

// Attribute and entry cache lifetimes. Every change comes through the kernel either way, but
// read-write mounts keep them short so a missed invalidation heals quickly.
#define UF_TIMEOUT_RO 60.0
#define UF_TIMEOUT_RW 1.0
#define UF_TIMEOUT(fs) ((fs)->read_only ? UF_TIMEOUT_RO : UF_TIMEOUT_RW)
#define UF_NAME_MAX 1024              // UTF-8 bytes for 255 UTF-16 units, with room to spare

// MARK: node table (engine reference -> lookup count)

static size_t slot_of(const uf_fs_t *fs, uint64_t ino)
{
    return (size_t)((ino * 0x9E3779B97F4A7C15ull) >> 32) & (fs->cap - 1);
}

static uf_node_t *node_find(const uf_fs_t *fs, uint64_t ino)
{
    if (!fs->cap)
        return NULL;
    for (size_t i = slot_of(fs, ino);; i = (i + 1) & (fs->cap - 1)) {
        if (!fs->nodes[i].nlookup)
            return NULL;
        if (fs->nodes[i].ino == ino)
            return &fs->nodes[i];
    }
}

static int node_grow(uf_fs_t *fs)
{
    size_t old_cap = fs->cap, cap = old_cap ? old_cap * 2 : 256;
    uf_node_t *old = fs->nodes;

    if (!(fs->nodes = calloc(cap, sizeof(*fs->nodes)))) {
        fs->nodes = old;
        return ENOMEM;
    }
    fs->cap = cap;
    for (size_t i = 0; i < old_cap; i++) {
        if (!old[i].nlookup)
            continue;
        size_t j = slot_of(fs, old[i].ino);
        while (fs->nodes[j].nlookup)
            j = (j + 1) & (cap - 1);
        fs->nodes[j] = old[i];
    }
    free(old);
    return 0;
}

static int node_ref(uf_fs_t *fs, uint64_t ino)
{
    uf_node_t *n = node_find(fs, ino);
    int err;

    if (n) {
        n->nlookup++;
        return 0;
    }
    if ((fs->count + 1) * 2 > fs->cap && (err = node_grow(fs)))
        return err;
    size_t i = slot_of(fs, ino);
    while (fs->nodes[i].nlookup)
        i = (i + 1) & (fs->cap - 1);
    fs->nodes[i] = (uf_node_t){ .ino = ino, .nlookup = 1 };
    fs->count++;
    return 0;
}

// Drops n lookups; at zero the slot is freed (backward-shift deletion) and true is returned.
static bool node_unref(uf_fs_t *fs, uint64_t ino, uint64_t n)
{
    uf_node_t *node = node_find(fs, ino);

    if (!node)
        return false;
    if (node->nlookup > n) {
        node->nlookup -= n;
        return false;
    }
    size_t i = (size_t)(node - fs->nodes);
    for (size_t j = (i + 1) & (fs->cap - 1); fs->nodes[j].nlookup; j = (j + 1) & (fs->cap - 1)) {
        size_t home = slot_of(fs, fs->nodes[j].ino);
        // Move j into the hole at i unless its home lies cyclically in (i, j].
        if ((j > i && (home <= i || home > j)) || (j < i && home <= i && home > j)) {
            fs->nodes[i] = fs->nodes[j];
            i = j;
        }
    }
    fs->nodes[i] = (uf_node_t){ 0 };
    fs->count--;
    return true;
}

// MARK: helpers

static uf_fs_t *fs_of(fuse_req_t req)
{
    return fuse_req_userdata(req);
}

static ue_ino_t to_engine(const uf_fs_t *fs, fuse_ino_t ino)
{
    return ino == FUSE_ROOT_ID ? fs->root : ino;
}

static fuse_ino_t to_fuse(const uf_fs_t *fs, ue_ino_t ino)
{
    return ino == fs->root ? FUSE_ROOT_ID : ino;
}

// NTFS stores what Windows writes (NFC) and the engine compares without normalizing (D-22),
// so every incoming name is converted to NFC here.
static int nfc_name(const char *in, char out[UF_NAME_MAX])
{
    CFStringRef s = CFStringCreateWithCString(NULL, in, kCFStringEncodingUTF8);
    CFMutableStringRef m;
    Boolean ok;

    if (!s)
        return EILSEQ;
    m = CFStringCreateMutableCopy(NULL, 0, s);
    CFRelease(s);
    if (!m)
        return ENOMEM;
    CFStringNormalize(m, kCFStringNormalizationFormC);
    ok = CFStringGetCString(m, out, UF_NAME_MAX, kCFStringEncodingUTF8);
    CFRelease(m);
    return ok ? 0 : ENAMETOOLONG;
}

static void fill_attr(const uf_fs_t *fs, const ue_attr_t *a, struct fuse_darwin_attr *st)
{
    mode_t perm;

    memset(st, 0, sizeof(*st));
    st->ino = a->ino == fs->root ? 2 : (ino_t)a->ino;
    switch (a->type) {
    case UE_T_DIR:
        perm = S_IFDIR | 0755;
        break;
    case UE_T_LNK:
        perm = S_IFLNK | 0777;
        break;
    default:
        perm = S_IFREG | 0644;
        if (a->flags & UE_F_READONLY)
            perm &= ~(mode_t)0222;
        break;
    }
    if (fs->read_only && a->type != UE_T_LNK)
        perm &= ~(mode_t)0222;
    st->mode = perm;
    st->nlink = a->nlink ? a->nlink : 1;
    st->uid = fs->uid;
    st->gid = fs->gid;
    st->atimespec = a->atime;
    st->mtimespec = a->mtime;
    st->ctimespec = a->ctime;
    st->btimespec = a->birth;
    st->size = (off_t)a->size;
    st->blocks = (blkcnt_t)((a->alloc_size + 511) / 512);
    st->blksize = 1 << 20;            // macFUSE's kext sizes its reads from this: 4 KiB -> 16 KiB reads
    // Windows marks the root hidden+system; passed through, Finder would hide the volume.
    if ((a->flags & UE_F_HIDDEN) && a->ino != fs->root)
        st->flags |= UF_HIDDEN;
}

// Takes a lookup reference for the kernel and fills the entry. Returns 0 or errno.
static int make_entry(uf_fs_t *fs, const ue_attr_t *a, struct fuse_darwin_entry_param *e)
{
    int err;

    if ((err = node_ref(fs, a->ino)))
        return err;
    memset(e, 0, sizeof(*e));
    e->ino = to_fuse(fs, a->ino);
    e->generation = a->ino >> 48;
    fill_attr(fs, a, &e->attr);
    e->attr_timeout = UF_TIMEOUT(fs);
    e->entry_timeout = UF_TIMEOUT(fs);
    return 0;
}

// The kernel never got the entry, so drop the reference make_entry took.
static void unmake_entry(uf_fs_t *fs, const ue_attr_t *a)
{
    if (node_unref(fs, a->ino, 1) && a->ino != fs->root)
        ue_release(fs->engine, a->ino);
}

static void reply_entry(fuse_req_t req, uf_fs_t *fs, const ue_attr_t *a)
{
    struct fuse_darwin_entry_param e;
    int err;

    if ((err = make_entry(fs, a, &e))) {
        fuse_reply_err(req, err);
        return;
    }
    if (fuse_reply_entry(req, &e) != 0)
        unmake_entry(fs, a);
}

// MARK: session

// NTFS lookups are case-insensitive. Without telling the kernel, a case-only rename
// (README -> readme) resolves both names to one vnode and becomes a silent no-op.
static void uf_init(void *userdata, struct fuse_conn_info *conn)
{
    (void)userdata;
    fuse_darwin_set_feature_flag(conn, FUSE_DARWIN_CAP_CASE_INSENSITIVE);
    fuse_darwin_set_feature_flag(conn, FUSE_DARWIN_CAP_RENAME_EXT);   // RENAME_EXCL/SWAP reach uf_rename
}

// MARK: read side

static void uf_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    uf_fs_t *fs = fs_of(req);
    char nfc[UF_NAME_MAX];
    ue_attr_t a;
    int err;

    if ((err = nfc_name(name, nfc)) || (err = ue_lookup(fs->engine, to_engine(fs, parent), nfc, &a))) {
        fuse_reply_err(req, err);
        return;
    }
    reply_entry(req, fs, &a);
}

static void forget_one(uf_fs_t *fs, fuse_ino_t ino, uint64_t nlookup)
{
    ue_ino_t eino = to_engine(fs, ino);

    if (node_unref(fs, eino, nlookup) && eino != fs->root)
        ue_release(fs->engine, eino);
}

static void uf_forget(fuse_req_t req, fuse_ino_t ino, uint64_t nlookup)
{
    forget_one(fs_of(req), ino, nlookup);
    fuse_reply_none(req);
}

static void uf_forget_multi(fuse_req_t req, size_t count, struct fuse_forget_data *forgets)
{
    uf_fs_t *fs = fs_of(req);

    for (size_t i = 0; i < count; i++)
        forget_one(fs, forgets[i].ino, forgets[i].nlookup);
    fuse_reply_none(req);
}

static void uf_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    uf_fs_t *fs = fs_of(req);
    struct fuse_darwin_attr st;
    ue_attr_t a;
    int err;

    (void)fi;
    if ((err = ue_getattr(fs->engine, to_engine(fs, ino), &a))) {
        fuse_reply_err(req, err);
        return;
    }
    fill_attr(fs, &a, &st);
    fuse_reply_attr(req, &st, UF_TIMEOUT(fs));
}

static void uf_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    (void)ino;
    fi->keep_cache = 1;
    fuse_reply_open(req, fi);
}

typedef struct {
    fuse_req_t req;
    const uf_fs_t *fs;
    char *buf;
    size_t size, used;
} dir_ctx_t;

static bool add_entry(dir_ctx_t *c, const char *name, const struct fuse_darwin_attr *st, uint64_t next)
{
    size_t n = fuse_add_direntry(c->req, c->buf + c->used, c->size - c->used, name, st, (off_t)next);

    if (n > c->size - c->used)
        return false;
    c->used += n;
    return true;
}

static bool emit(void *ctx, const char *name, const ue_attr_t *attr, uint64_t next_cookie)
{
    dir_ctx_t *c = ctx;
    struct fuse_darwin_attr st;

    fill_attr(c->fs, attr, &st);
    return add_entry(c, name, &st, next_cookie);
}

// Offsets are engine cookies: "." and ".." are positions 1 and 2 (the engine's own cookies
// for them), and the engine's entries resume from 2.
static void uf_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi)
{
    uf_fs_t *fs = fs_of(req);
    dir_ctx_t c = { .req = req, .fs = fs, .size = size };
    struct fuse_darwin_attr dot = { .mode = S_IFDIR };
    uint64_t verifier;
    int err = 0;

    (void)fi;
    if (!(c.buf = malloc(size))) {
        fuse_reply_err(req, ENOMEM);
        return;
    }
    dot.ino = ino == FUSE_ROOT_ID ? 2 : (ino_t)ino;
    if (off < 1 && !add_entry(&c, ".", &dot, 1))
        goto out;
    if (off < 2 && !add_entry(&c, "..", &dot, 2))
        goto out;
    err = ue_readdir(fs->engine, to_engine(fs, ino), off < 2 ? 2 : (uint64_t)off, emit, &c, &verifier);
out:
    if (err && !c.used)
        fuse_reply_err(req, err);
    else
        fuse_reply_buf(req, c.buf, c.used);
    free(c.buf);
}

static void uf_releasedir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    (void)ino;
    (void)fi;
    fuse_reply_err(req, 0);
}

static void uf_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    uf_fs_t *fs = fs_of(req);

    (void)ino;
    if (fs->read_only && (fi->flags & O_ACCMODE) != O_RDONLY) {
        fuse_reply_err(req, EROFS);
        return;
    }
    fi->keep_cache = fs->read_only;   // read-write: the kernel's cache is dropped on open
    fuse_reply_open(req, fi);
}

static void uf_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi)
{
    uf_fs_t *fs = fs_of(req);
    size_t done = 0;
    char *buf;
    int err;

    (void)fi;
    if (off < 0) {
        fuse_reply_err(req, EINVAL);
        return;
    }
    if (size > fs->rbuf_cap) {
        if (!(buf = realloc(fs->rbuf, size))) {
            fuse_reply_err(req, ENOMEM);
            return;
        }
        fs->rbuf = buf;
        fs->rbuf_cap = size;
    }
    buf = fs->rbuf;
    if ((err = ue_read(fs->engine, to_engine(fs, ino), (uint64_t)off, buf, size, &done)))
        fuse_reply_err(req, err);
    else
        fuse_reply_buf(req, buf, done);
}

static void uf_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    (void)ino;
    (void)fi;
    fuse_reply_err(req, 0);
}

static void uf_statfs(fuse_req_t req, fuse_ino_t ino)
{
    uf_fs_t *fs = fs_of(req);
    uint64_t total, free_bytes;
    uint32_t cluster;
    struct statfs st;
    int err;

    (void)ino;
    if ((err = ue_statfs(fs->engine, &total, &free_bytes, &cluster))) {
        fuse_reply_err(req, err);
        return;
    }
    memset(&st, 0, sizeof(st));
    st.f_bsize = cluster;
    st.f_iosize = 1 << 20;
    st.f_blocks = total / cluster;
    st.f_bfree = free_bytes / cluster;
    st.f_bavail = st.f_bfree;
    fuse_reply_statfs(req, &st);
}

// MARK: write side

static void reply_attr_of(fuse_req_t req, uf_fs_t *fs, const ue_attr_t *a)
{
    struct fuse_darwin_attr st;

    fill_attr(fs, a, &st);
    fuse_reply_attr(req, &st, UF_TIMEOUT(fs));
}

// NTFS has no owners: chown and chgrp succeed and change nothing (D-46). Of the BSD flags,
// UF_HIDDEN maps to hidden, and setting UF_IMMUTABLE (uchg) on a file sets read-only. The
// read-only bit is shown only as a missing w in the mode, never as uchg: an immutable file
// cannot be unlinked, and git and others delete their 0444 files (D-50). It is cleared with
// chmod u+w. Other flag bits are ignored.
static void uf_setattr(fuse_req_t req, fuse_ino_t ino, struct fuse_darwin_attr *attr, int to_set,
                       struct fuse_file_info *fi)
{
    uf_fs_t *fs = fs_of(req);
    ue_ino_t eino = to_engine(fs, ino);
    ue_attr_t in, out;
    uint32_t mask = 0;
    struct timespec now;
    int err;

    (void)fi;
    if (fs->read_only) {
        fuse_reply_err(req, EROFS);
        return;
    }
    memset(&in, 0, sizeof(in));
    clock_gettime(CLOCK_REALTIME, &now);
    if (to_set & FUSE_SET_ATTR_SIZE) {
        if (attr->size < 0) {
            fuse_reply_err(req, EINVAL);
            return;
        }
        mask |= UE_SET_SIZE;
        in.size = (uint64_t)attr->size;
    }
    if (to_set & (FUSE_SET_ATTR_MTIME | FUSE_SET_ATTR_MTIME_NOW)) {
        mask |= UE_SET_MTIME;
        in.mtime = (to_set & FUSE_SET_ATTR_MTIME_NOW) ? now : attr->mtimespec;
    }
    if (to_set & (FUSE_SET_ATTR_ATIME | FUSE_SET_ATTR_ATIME_NOW)) {
        mask |= UE_SET_ATIME;
        in.atime = (to_set & FUSE_SET_ATTR_ATIME_NOW) ? now : attr->atimespec;
    }
    if (to_set & FUSE_SET_ATTR_BTIME) {
        mask |= UE_SET_BIRTH;
        in.birth = attr->btimespec;
    }
    if (to_set & FUSE_SET_ATTR_CTIME) {
        mask |= UE_SET_CTIME;
        in.ctime = attr->ctimespec;
    }
    if (to_set & FUSE_SET_ATTR_MODE) {
        mask |= UE_SET_MODE;
        in.mode = attr->mode;
    }
    if (to_set & FUSE_SET_ATTR_FLAGS) {
        ue_attr_t cur;
        if ((err = ue_getattr(fs->engine, eino, &cur))) {
            fuse_reply_err(req, err);
            return;
        }
        mask |= UE_SET_FLAGS;
        in.flags = (cur.flags & ~(uint32_t)UE_F_HIDDEN) | ((attr->flags & UF_HIDDEN) ? UE_F_HIDDEN : 0);
        if (cur.type == UE_T_REG && (attr->flags & UF_IMMUTABLE))
            in.flags |= UE_F_READONLY;
    }
    if ((err = ue_setattr(fs->engine, eino, mask, &in, &out))) {
        fuse_reply_err(req, err);
        return;
    }
    reply_attr_of(req, fs, &out);
}

static int create_common(fuse_req_t req, fuse_ino_t parent, const char *name, uint32_t type, mode_t mode,
                         ue_attr_t *out)
{
    uf_fs_t *fs = fs_of(req);
    char nfc[UF_NAME_MAX];
    int err;

    if (fs->read_only)
        return EROFS;
    if ((err = nfc_name(name, nfc)))
        return err;
    return ue_create(fs->engine, to_engine(fs, parent), nfc, type, mode & 07777, out);
}

static void uf_mkdir(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode)
{
    ue_attr_t a;
    int err = create_common(req, parent, name, UE_T_DIR, mode, &a);

    if (err)
        fuse_reply_err(req, err);
    else
        reply_entry(req, fs_of(req), &a);
}

static void uf_create(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode,
                      struct fuse_file_info *fi)
{
    uf_fs_t *fs = fs_of(req);
    struct fuse_darwin_entry_param e;
    ue_attr_t a;
    int err = create_common(req, parent, name, UE_T_REG, mode, &a);

    if (!err)
        err = make_entry(fs, &a, &e);
    if (err) {
        fuse_reply_err(req, err);
        return;
    }
    fi->keep_cache = 0;
    if (fuse_reply_create(req, &e, fi) != 0)
        unmake_entry(fs, &a);
}

// Removing a name the kernel still holds is fine: the engine keeps the file as a hidden
// ghost until the last forget releases it (D-25).
static void remove_common(fuse_req_t req, fuse_ino_t parent, const char *name, bool is_dir)
{
    uf_fs_t *fs = fs_of(req);
    char nfc[UF_NAME_MAX];
    int err;

    if (fs->read_only)
        err = EROFS;
    else if (!(err = nfc_name(name, nfc)))
        err = ue_remove(fs->engine, to_engine(fs, parent), nfc, is_dir);
    fuse_reply_err(req, err);
}

static void uf_unlink(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    remove_common(req, parent, name, false);
}

static void uf_rmdir(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    remove_common(req, parent, name, true);
}

static void uf_rename(fuse_req_t req, fuse_ino_t parent, const char *name, fuse_ino_t newparent,
                      const char *newname, unsigned int flags)
{
    uf_fs_t *fs = fs_of(req);
    char from[UF_NAME_MAX], to[UF_NAME_MAX];
    int err;

    if (fs->read_only)
        err = EROFS;
    else if (flags & RENAME_SWAP)
        err = ENOTSUP;
    else if (flags & ~(unsigned)RENAME_EXCL)
        err = EINVAL;
    else if (!(err = nfc_name(name, from)) && !(err = nfc_name(newname, to)))
        err = ue_rename(fs->engine, to_engine(fs, parent), from, to_engine(fs, newparent), to,
                        (flags & RENAME_EXCL) ? UE_RENAME_EXCL : 0);
    fuse_reply_err(req, err);
}

static void uf_write(fuse_req_t req, fuse_ino_t ino, const char *buf, size_t size, off_t off,
                     struct fuse_file_info *fi)
{
    uf_fs_t *fs = fs_of(req);
    size_t done = 0;
    int err;

    (void)fi;
    if (fs->read_only) {
        fuse_reply_err(req, EROFS);
        return;
    }
    if (off < 0) {
        fuse_reply_err(req, EINVAL);
        return;
    }
    err = ue_write(fs->engine, to_engine(fs, ino), (uint64_t)off, buf, size, &done);
    if (err && !done)
        fuse_reply_err(req, err);
    else
        fuse_reply_write(req, done);
}

// Engine writes go straight to the device, so close needs nothing; fsync flushes the device
// cache (F_FULLFSYNC on the image or disk).
static void uf_flush(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    (void)ino;
    (void)fi;
    fuse_reply_err(req, ue_has_failed(fs_of(req)->engine) ? EIO : 0);
}

static void uf_fsync(fuse_req_t req, fuse_ino_t ino, int datasync, struct fuse_file_info *fi)
{
    (void)ino;
    (void)datasync;
    (void)fi;
    fuse_reply_err(req, ue_sync(fs_of(req)->engine));
}

static void uf_symlink(fuse_req_t req, const char *link, fuse_ino_t parent, const char *name)
{
    uf_fs_t *fs = fs_of(req);
    char nfc[UF_NAME_MAX];
    ue_attr_t a;
    int err;

    if (fs->read_only)
        err = EROFS;
    else if (!(err = nfc_name(name, nfc)))
        err = ue_symlink(fs->engine, to_engine(fs, parent), nfc, link, &a);
    if (err)
        fuse_reply_err(req, err);
    else
        reply_entry(req, fs, &a);
}

static void uf_link(fuse_req_t req, fuse_ino_t ino, fuse_ino_t newparent, const char *newname)
{
    uf_fs_t *fs = fs_of(req);
    char nfc[UF_NAME_MAX];
    ue_attr_t a;
    int err;

    if (fs->read_only)
        err = EROFS;
    else if (!(err = nfc_name(newname, nfc)))
        err = ue_link(fs->engine, to_engine(fs, ino), to_engine(fs, newparent), nfc, &a);
    if (err)
        fuse_reply_err(req, err);
    else
        reply_entry(req, fs, &a);
}

static void uf_readlink(fuse_req_t req, fuse_ino_t ino)
{
    uf_fs_t *fs = fs_of(req);
    char target[PATH_MAX];
    int err = ue_readlink(fs->engine, to_engine(fs, ino), fs->mountpoint, target, sizeof(target));

    if (err)
        fuse_reply_err(req, err);
    else
        fuse_reply_readlink(req, target);
}

static void uf_mknod(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode, dev_t rdev)
{
    ue_attr_t a;
    int err;

    (void)rdev;
    if (!S_ISREG(mode)) {
        fuse_reply_err(req, fs_of(req)->read_only ? EROFS : ENOTSUP);
        return;
    }
    if ((err = create_common(req, parent, name, UE_T_REG, mode, &a)))
        fuse_reply_err(req, err);
    else
        reply_entry(req, fs_of(req), &a);
}

// MARK: extended attributes (named streams, D-48)

static void uf_setxattr(fuse_req_t req, fuse_ino_t ino, const char *name, const char *value, size_t size,
                        int flags, uint32_t position)
{
    uf_fs_t *fs = fs_of(req);
    uint32_t uflags = 0;

    if (fs->read_only) {
        fuse_reply_err(req, EROFS);
        return;
    }
    if (flags & XATTR_CREATE)
        uflags |= UE_XATTR_CREATE;
    if (flags & XATTR_REPLACE)
        uflags |= UE_XATTR_REPLACE;
    fuse_reply_err(req, ue_xattr_set(fs->engine, to_engine(fs, ino), name, position, value, size, uflags));
}

static void uf_getxattr(fuse_req_t req, fuse_ino_t ino, const char *name, size_t size, uint32_t position)
{
    uf_fs_t *fs = fs_of(req);
    size_t len = 0;
    char *buf = NULL;
    int err;

    if (size && !(buf = malloc(size))) {
        fuse_reply_err(req, ENOMEM);
        return;
    }
    err = ue_xattr_get(fs->engine, to_engine(fs, ino), name, position, buf, size, &len);
    if (err)
        fuse_reply_err(req, err);
    else if (!size)
        fuse_reply_xattr(req, len);
    else
        fuse_reply_buf(req, buf, len);
    free(buf);
}

static void uf_listxattr(fuse_req_t req, fuse_ino_t ino, size_t size)
{
    uf_fs_t *fs = fs_of(req);
    size_t needed = 0;
    char *buf = NULL;
    int err;

    if (size && !(buf = malloc(size))) {
        fuse_reply_err(req, ENOMEM);
        return;
    }
    err = ue_xattr_list(fs->engine, to_engine(fs, ino), buf, size, &needed);
    if (err)
        fuse_reply_err(req, err);
    else if (!size)
        fuse_reply_xattr(req, needed);
    else
        fuse_reply_buf(req, buf, needed);
    free(buf);
}

static void uf_removexattr(fuse_req_t req, fuse_ino_t ino, const char *name)
{
    uf_fs_t *fs = fs_of(req);

    if (fs->read_only)
        fuse_reply_err(req, EROFS);
    else
        fuse_reply_err(req, ue_xattr_remove(fs->engine, to_engine(fs, ino), name));
}

const struct fuse_lowlevel_ops uf_ops = {
    .init = uf_init,
    .lookup = uf_lookup,
    .forget = uf_forget,
    .forget_multi = uf_forget_multi,
    .getattr = uf_getattr,
    .setattr = uf_setattr,
    .mkdir = uf_mkdir,
    .unlink = uf_unlink,
    .rmdir = uf_rmdir,
    .rename = uf_rename,
    .open = uf_open,
    .read = uf_read,
    .write = uf_write,
    .release = uf_release,
    .opendir = uf_opendir,
    .readdir = uf_readdir,
    .releasedir = uf_releasedir,
    .statfs = uf_statfs,
    .create = uf_create,
    .flush = uf_flush,
    .fsync = uf_fsync,
    .fsyncdir = uf_fsync,
    .symlink = uf_symlink,
    .link = uf_link,
    .mknod = uf_mknod,
    .readlink = uf_readlink,
    .setxattr = uf_setxattr,
    .getxattr = uf_getxattr,
    .listxattr = uf_listxattr,
    .removexattr = uf_removexattr,
};
