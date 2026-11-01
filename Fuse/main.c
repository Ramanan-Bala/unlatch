// SPDX-License-Identifier: GPL-3.0-or-later
// unlatch-fuse: mounts an NTFS image or device through macFUSE (plan Step 08, D-40).
//
//   unlatch-fuse [-f] [-d] [--backend fskit|kext] [-o opt,...] <image-or-device> <mountpoint>
//
// The safety decision is made here, before mounting: CORRUPT and NOT_NTFS are refused,
// anything else mounts read-only (read-write comes in Step 10). The verdict goes to the
// status record (status.c).
#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "file_io.h"
#include "ops.h"
#include "status.h"

static void usage(void)
{
    fprintf(stderr,
            "usage: unlatch-fuse [-f] [-d] [--backend fskit|kext] [-o opt,...] <image-or-device> <mountpoint>\n"
            "  -f            stay in the foreground\n"
            "  -d            FUSE debug output (implies -f)\n"
            "  --backend B   macFUSE backend: fskit (default, no kernel extension) or kext\n"
            "  -o opt,...    extra macFUSE mount options\n");
}

// fuse_opt splits on commas, so a comma in the volume name is escaped.
static void escape_opt(const char *in, char *out, size_t cap)
{
    size_t o = 0;

    for (; *in && o + 2 < cap; in++) {
        if (*in == ',' || *in == '\\')
            out[o++] = '\\';
        out[o++] = *in;
    }
    out[o] = 0;
}

int main(int argc, char **argv)
{
    static const struct option longopts[] = {
        { "backend", required_argument, NULL, 'b' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    const char *backend = "fskit", *extra = NULL;
    bool foreground = false, debug = false;
    int ch;

    while ((ch = getopt_long(argc, argv, "fdo:h", longopts, NULL)) != -1) {
        switch (ch) {
        case 'f': foreground = true; break;
        case 'd': debug = foreground = true; break;
        case 'o': extra = optarg; break;
        case 'b': backend = optarg; break;
        default: usage(); return ch == 'h' ? 0 : 2;
        }
    }
    if (argc - optind != 2 || (strcmp(backend, "fskit") && strcmp(backend, "kext"))) {
        usage();
        return 2;
    }
    const char *source = argv[optind], *mountpoint = argv[optind + 1];
    char source_copy[1024];
    snprintf(source_copy, sizeof(source_copy), "%s", source);
    const char *bsd_name = basename(source_copy);

    file_io_t f;
    ue_probe_t p;
    int err;
    if ((err = file_io_open(&f, source, false))) {
        fprintf(stderr, "unlatch-fuse: %s: %s\n", source, strerror(err));
        return 1;
    }
    if ((err = ue_probe(&f.io, &p))) {
        fprintf(stderr, "unlatch-fuse: probe %s: %s\n", source, strerror(err));
        file_io_close(&f);
        return 1;
    }
    if (p.state == UE_VOL_CORRUPT || p.state == UE_VOL_NOT_NTFS) {
        fprintf(stderr, "unlatch-fuse: %s: %s (%s), not mounting\n", source, ue_vol_state_name(p.state), p.reason);
        status_write(bsd_name, &p, true, false);
        file_io_close(&f);
        return 1;
    }
    if (p.state != UE_VOL_CLEAN)
        fprintf(stderr, "unlatch-fuse: %s: %s (%s), read-only\n", source, ue_vol_state_name(p.state), p.reason);

    uf_fs_t fs = { .read_only = true, .uid = getuid(), .gid = getgid() };
    ue_probe_t info;
    ue_attr_t root;
    if ((err = ue_open(&f.io, UE_OPEN_RDONLY, &fs.engine, &info))) {
        fprintf(stderr, "unlatch-fuse: open %s: %s\n", source, strerror(err));
        file_io_close(&f);
        return 1;
    }
    if ((err = ue_getattr(fs.engine, UE_ROOT_INO, &root))) {
        fprintf(stderr, "unlatch-fuse: root of %s: %s\n", source, strerror(err));
        ue_close(fs.engine);
        file_io_close(&f);
        return 1;
    }
    fs.root = root.ino;

    char volname[600], opts[2048];
    escape_opt(p.label[0] ? p.label : "NTFS", volname, sizeof(volname));
    snprintf(opts, sizeof(opts), "ro,fsname=unlatch@%s,volname=%s,backend=%s%s%s",
             bsd_name, volname, backend, extra ? "," : "", extra ? extra : "");
    char *fargv[] = { argv[0], "-o", opts, debug ? "-d" : NULL, NULL };
    struct fuse_args args = FUSE_ARGS_INIT(debug ? 4 : 3, fargv);

    int rc = 1;
    struct fuse_session *se = fuse_session_new(&args, &uf_ops, sizeof(uf_ops), &fs);
    if (!se)
        goto close;
    if (fuse_set_signal_handlers(se) != 0)
        goto destroy;
    if (fuse_session_mount(se, mountpoint) != 0)
        goto signals;
    status_write(bsd_name, &p, true, true);
    fuse_daemonize(foreground);
    rc = fuse_session_loop(se) == 0 ? 0 : 1;
    fuse_session_unmount(se);
signals:
    fuse_remove_signal_handlers(se);
destroy:
    fuse_session_destroy(se);
close:
    ue_close(fs.engine);
    status_write(bsd_name, &p, true, false);
    file_io_close(&f);
    return rc;
}
