// Minimal libfuse3 low-level FS: one 4 MiB file "f" with deterministic bytes.
#define FUSE_USE_VERSION 318
#include <fuse_lowlevel.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <stdio.h>
#include <unistd.h>
#define SZ (4u << 20)
static char *data;
static void fill(fuse_ino_t ino, struct fuse_darwin_attr *a) {
    memset(a, 0, sizeof(*a)); a->ino = ino;
    if (ino == 1) { a->mode = S_IFDIR | 0755; a->nlink = 2; }
    else { a->mode = S_IFREG | 0444; a->nlink = 1; a->size = SZ; a->blocks = SZ / 512; }
    a->uid = getuid(); a->gid = getgid(); a->blksize = 4096;
}
static void lk(fuse_req_t r, fuse_ino_t p, const char *n) {
    if (p != 1 || strcmp(n, "f")) { fuse_reply_err(r, ENOENT); return; }
    struct fuse_darwin_entry_param e = { .ino = 2, .attr_timeout = 1, .entry_timeout = 1 };
    fill(2, &e.attr); fuse_reply_entry(r, &e);
}
static void ga(fuse_req_t r, fuse_ino_t i, struct fuse_file_info *fi) {
    (void)fi; struct fuse_darwin_attr a; fill(i, &a); fuse_reply_attr(r, &a, 1);
}
static void rd(fuse_req_t r, fuse_ino_t i, size_t sz, off_t off, struct fuse_file_info *fi) {
    (void)i; (void)fi;
    if ((size_t)off >= SZ) { fuse_reply_buf(r, NULL, 0); return; }
    if (off + sz > SZ) sz = SZ - off;
    fuse_reply_buf(r, data + off, sz);
}
static void op(fuse_req_t r, fuse_ino_t i, struct fuse_file_info *fi) { (void)i; fuse_reply_open(r, fi); }
static const struct fuse_lowlevel_ops ops = { .lookup = lk, .getattr = ga, .read = rd, .open = op };
int main(int argc, char **argv) {
    data = malloc(SZ); for (unsigned i = 0; i < SZ; i++) data[i] = (char)(i * 2654435761u >> 24);
    FILE *f = fopen(argv[2], "wb"); fwrite(data, 1, SZ, f); fclose(f);
    char *fa[] = { argv[0], "-o", "ro,backend=fskit,volname=hello", NULL };
    struct fuse_args args = FUSE_ARGS_INIT(3, fa);
    struct fuse_session *se = fuse_session_new(&args, &ops, sizeof(ops), NULL);
    if (!se || fuse_session_mount(se, argv[1])) return 1;
    fuse_daemonize(0); fuse_session_loop(se); fuse_session_unmount(se); return 0;
}
