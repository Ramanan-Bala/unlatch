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
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define UF_TIMEOUT 60.0               // read-only: nothing changes behind the kernel's back
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
    st->blksize = 4096;
    if (a->flags & UE_F_HIDDEN)
        st->flags |= UF_HIDDEN;
}

static void reply_entry(fuse_req_t req, uf_fs_t *fs, const ue_attr_t *a)
{
    struct fuse_darwin_entry_param e;
    int err;

    if ((err = node_ref(fs, a->ino))) {
        fuse_reply_err(req, err);
        return;
    }
    memset(&e, 0, sizeof(e));
    e.ino = to_fuse(fs, a->ino);
    e.generation = a->ino >> 48;
    fill_attr(fs, a, &e.attr);
    e.attr_timeout = UF_TIMEOUT;
    e.entry_timeout = UF_TIMEOUT;
    if (fuse_reply_entry(req, &e) != 0 && node_unref(fs, a->ino, 1) && a->ino != fs->root)
        ue_release(fs->engine, a->ino);     // the kernel never got the entry
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
    fuse_reply_attr(req, &st, UF_TIMEOUT);
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
    fi->keep_cache = fs->read_only;
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
    if (!(buf = malloc(size ? size : 1))) {
        fuse_reply_err(req, ENOMEM);
        return;
    }
    if ((err = ue_read(fs->engine, to_engine(fs, ino), (uint64_t)off, buf, size, &done)))
        fuse_reply_err(req, err);
    else
        fuse_reply_buf(req, buf, done);
    free(buf);
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

// MARK: write side: read-only until Step 10

static void uf_setattr(fuse_req_t req, fuse_ino_t ino, struct fuse_darwin_attr *attr, int to_set,
                       struct fuse_file_info *fi)
{
    (void)ino;
    (void)attr;
    (void)to_set;
    (void)fi;
    fuse_reply_err(req, EROFS);
}

static void uf_mkdir(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode)
{
    (void)parent;
    (void)name;
    (void)mode;
    fuse_reply_err(req, EROFS);
}

static void uf_create(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode,
                      struct fuse_file_info *fi)
{
    (void)parent;
    (void)name;
    (void)mode;
    (void)fi;
    fuse_reply_err(req, EROFS);
}

static void uf_remove(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    (void)parent;
    (void)name;
    fuse_reply_err(req, EROFS);
}

static void uf_rename(fuse_req_t req, fuse_ino_t parent, const char *name, fuse_ino_t newparent,
                      const char *newname, unsigned int flags)
{
    (void)parent;
    (void)name;
    (void)newparent;
    (void)newname;
    (void)flags;
    fuse_reply_err(req, EROFS);
}

static void uf_write(fuse_req_t req, fuse_ino_t ino, const char *buf, size_t size, off_t off,
                     struct fuse_file_info *fi)
{
    (void)ino;
    (void)buf;
    (void)size;
    (void)off;
    (void)fi;
    fuse_reply_err(req, EROFS);
}

const struct fuse_lowlevel_ops uf_ops = {
    .lookup = uf_lookup,
    .forget = uf_forget,
    .forget_multi = uf_forget_multi,
    .getattr = uf_getattr,
    .setattr = uf_setattr,
    .mkdir = uf_mkdir,
    .unlink = uf_remove,
    .rmdir = uf_remove,
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
};
