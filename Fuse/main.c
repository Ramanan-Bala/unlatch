// SPDX-License-Identifier: GPL-3.0-or-later
// unlatch-fuse: mounts an NTFS image or device through macFUSE (plan Step 08, D-40).
//
//   unlatch-fuse [-f] [-d] [-r] [--backend kext|fskit] [-o opt,...] <image-or-device> <mountpoint>
//
// The default backend is macFUSE's kernel extension: its FSKit backend fails large reads on
// macOS 27 (D-42, D-43).
//
// The safety decision is made here, before mounting: CORRUPT and NOT_NTFS are refused;
// read-write only when the volume probes CLEAN, the device opens writable and -r was not
// given; everything else mounts read-only. The verdict goes to the status record (status.c).
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>
#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <unistd.h>

#include "file_io.h"
#include "log.h"
#include "ops.h"
#include "status.h"

static void usage(void)
{
    fprintf(stderr,
            "usage: unlatch-fuse [-f] [-d] [-r] [--backend kext|fskit] [-o opt,...] <image-or-device> <mountpoint>\n"
            "  -f            stay in the foreground\n"
            "  -d            FUSE debug output (implies -f)\n"
            "  -r            mount read-only even when the volume could be written\n"
            "  --backend B   macFUSE backend: kext (default) or fskit (no kernel extension;\n"
            "                mounts only under /Volumes; large reads fail on macOS 27, D-42)\n"
            "  -o opt,...    extra macFUSE mount options\n");
}

static void record_status(const char *bsd_name, const ue_probe_t *p, bool read_only, bool mounted)
{
    int err = status_write(bsd_name, p, read_only, mounted);

    if (err)
        uf_log(UF_LOG_MOUNT, OS_LOG_TYPE_ERROR, "status record for %s: %s", bsd_name, strerror(err));
}

// Backgrounding without fork(): OSLog and CoreFoundation are not safe in a child that forked
// without exec. The daemon re-runs itself in the foreground with --ready-fd and waits for one
// byte saying the mount succeeded; the child's errors still reach this terminal until then.
// True once `path` is the root of a macFUSE mount.
static bool is_mounted(const char *path)
{
    struct statfs st;
    char real[PATH_MAX];

    return realpath(path, real) && statfs(real, &st) == 0 && strcmp(st.f_mntonname, real) == 0 &&
           strcmp(st.f_fstypename, "macfuse") == 0;
}

static int run_in_background(int argc, char **argv, const char *mountpoint)
{
    extern char **environ;
    char fdarg[16], **cargv;
    int pipefd[2], status;
    pid_t pid;
    char ok;

    if (pipe(pipefd) < 0)
        return 1;
    snprintf(fdarg, sizeof(fdarg), "%d", pipefd[1]);
    if (!(cargv = calloc((size_t)argc + 4, sizeof(*cargv))))
        return 1;
    cargv[0] = argv[0];
    cargv[1] = "-f";
    cargv[2] = "--ready-fd";
    cargv[3] = fdarg;
    for (int i = 1; i < argc; i++)
        cargv[i + 3] = argv[i];
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
    int err = posix_spawn(&pid, argv[0], NULL, &attr, cargv, environ);
    posix_spawnattr_destroy(&attr);
    free(cargv);
    close(pipefd[1]);
    if (err) {
        fprintf(stderr, "unlatch-fuse: cannot start the daemon: %s\n", strerror(err));
        close(pipefd[0]);
        return 1;
    }
    if (read(pipefd[0], &ok, 1) == 1 && ok == '0') {
        // The mount appears once the child's loop has answered FUSE_INIT; wait up to 10 s.
        for (int i = 0; i < 1000; i++) {
            if (is_mounted(mountpoint))
                return 0;             // mounted; the child carries on
            if (waitpid(pid, &status, WNOHANG) == pid)
                return 1;
            usleep(10000);
        }
        fprintf(stderr, "unlatch-fuse: %s did not appear as a mount\n", mountpoint);
        return 1;
    }
    waitpid(pid, &status, 0);         // it failed and has already said why
    return 1;
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
        { "read-only", no_argument, NULL, 'R' },
        { "ready-fd", required_argument, NULL, 'F' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    const char *backend = "kext", *extra = NULL;
    bool foreground = false, debug = false, want_ro = false;
    int ch, ready_fd = -1;

    while ((ch = getopt_long(argc, argv, "fdro:h", longopts, NULL)) != -1) {
        switch (ch) {
        case 'f': foreground = true; break;
        case 'r': case 'R': want_ro = true; break;
        case 'd': debug = foreground = true; break;
        case 'o': extra = optarg; break;
        case 'b': backend = optarg; break;
        case 'F': ready_fd = atoi(optarg); break;
        default: usage(); return ch == 'h' ? 0 : 2;
        }
    }
    if (argc - optind != 2 || (strcmp(backend, "fskit") && strcmp(backend, "kext"))) {
        usage();
        return 2;
    }
    if (!foreground)
        return run_in_background(argc, argv, argv[optind + 1]);
    uf_log_init();
    const char *source = argv[optind];
    // mount_macfuse resolves a relative mount point against /, so make it absolute.
    char mountpoint[PATH_MAX];
    if (!realpath(argv[optind + 1], mountpoint)) {
        uf_log(UF_LOG_MOUNT, OS_LOG_TYPE_ERROR, "%s: %s", argv[optind + 1], strerror(errno));
        return 1;
    }
    char source_copy[1024];
    snprintf(source_copy, sizeof(source_copy), "%s", source);
    const char *bsd_name = basename(source_copy);

    file_io_t f;
    ue_probe_t p;
    int err;
    // /dev/diskN is used as given: its buffer cache makes metadata work 8-12x faster than the
    // raw /dev/rdiskN (sequential writes are 2.4x slower), D-51. /dev/rdiskN is honoured too.
    if ((err = file_io_open(&f, source, false))) {
        uf_log(UF_LOG_MOUNT, OS_LOG_TYPE_ERROR, "%s: %s", source, strerror(err));
        return 1;
    }
    if ((err = ue_probe(&f.io, &p))) {
        uf_log(UF_LOG_MOUNT, OS_LOG_TYPE_ERROR, "probe %s: %s", source, strerror(err));
        file_io_close(&f);
        return 1;
    }
    if (p.state == UE_VOL_CORRUPT || p.state == UE_VOL_NOT_NTFS) {
        uf_log(UF_LOG_MOUNT, OS_LOG_TYPE_ERROR, "%s: %s (%s), not mounting", source, ue_vol_state_name(p.state), p.reason);
        record_status(bsd_name, &p, true, false);
        file_io_close(&f);
        return 1;
    }

    // When in doubt, read-only.
    uf_fs_t fs = { .read_only = true, .mountpoint = mountpoint, .uid = getuid(), .gid = getgid() };
    // Real disks' device nodes are root:operator 0640, so they are mounted with sudo until the
    // Step 12 helper exists. Files then belong to the user who ran sudo, and the mount is made
    // visible to other users (macFUSE hides a root mount otherwise), D-53.
    bool via_sudo = getuid() == 0 && getenv("SUDO_UID") && getenv("SUDO_GID");
    if (via_sudo) {
        fs.uid = (uid_t)strtoul(getenv("SUDO_UID"), NULL, 10);
        fs.gid = (gid_t)strtoul(getenv("SUDO_GID"), NULL, 10);
    }
    if (p.state != UE_VOL_CLEAN) {
        uf_log(UF_LOG_MOUNT, OS_LOG_TYPE_DEFAULT, "%s: %s (%s), read-only", source, ue_vol_state_name(p.state), p.reason);
    } else if (!want_ro) {
        file_io_t w;
        if ((err = file_io_open(&w, source, true))) {
            uf_log(UF_LOG_MOUNT, OS_LOG_TYPE_DEFAULT, "%s cannot be opened for writing (%s), read-only",
                   source, strerror(err));
        } else {
            file_io_close(&f);
            f = w;
            f.io.ctx = &f;            // file_io's context is the struct itself
            fs.read_only = false;
        }
    }
    ue_probe_t info;
    ue_attr_t root;
    err = ue_open(&f.io, fs.read_only ? UE_OPEN_RDONLY : 0, &fs.engine, &info);
    if (err == EROFS && !fs.read_only) {
        uf_log(UF_LOG_MOUNT, OS_LOG_TYPE_DEFAULT, "%s is write-protected, read-only", source);
        fs.read_only = true;
        err = ue_open(&f.io, UE_OPEN_RDONLY, &fs.engine, &info);
    }
    if (err) {
        uf_log(UF_LOG_MOUNT, OS_LOG_TYPE_ERROR, "open %s: %s", source, strerror(err));
        file_io_close(&f);
        return 1;
    }
    if ((err = ue_getattr(fs.engine, UE_ROOT_INO, &root))) {
        uf_log(UF_LOG_MOUNT, OS_LOG_TYPE_ERROR, "root of %s: %s", source, strerror(err));
        ue_close(fs.engine);
        file_io_close(&f);
        return 1;
    }
    fs.root = root.ino;

    char volname[600], opts[2048];
    escape_opt(p.label[0] ? p.label : "NTFS", volname, sizeof(volname));
    snprintf(opts, sizeof(opts), "%s%sfsname=unlatch@%s,volname=%s,backend=%s%s%s", fs.read_only ? "ro," : "",
             via_sudo ? "allow_other," : "", bsd_name, volname, backend, extra ? "," : "", extra ? extra : "");
    char *fargv[] = { argv[0], "-o", opts, debug ? "-d" : NULL, NULL };
    struct fuse_args args = FUSE_ARGS_INIT(debug ? 4 : 3, fargv);

    int rc = 1;
    struct fuse_session *se = fuse_session_new(&args, &uf_ops, sizeof(uf_ops), &fs);
    if (!se)
        goto close;
    if (fuse_set_signal_handlers(se) != 0)
        goto destroy;
    if (fuse_session_mount(se, mountpoint) != 0) {
        uf_log(UF_LOG_MOUNT, OS_LOG_TYPE_ERROR, "mounting %s on %s failed (%s backend)", source, mountpoint, backend);
        goto signals;
    }
    record_status(bsd_name, &p, fs.read_only, true);
    uf_log(UF_LOG_MOUNT, OS_LOG_TYPE_DEFAULT, "mounted %s on %s %s (%s, %s backend)", source, mountpoint,
           fs.read_only ? "read-only" : "read-write", ue_vol_state_name(p.state), backend);
    if (ready_fd >= 0) {              // tell the parent, then detach from its terminal
        int null = open("/dev/null", O_RDWR);
        if (write(ready_fd, "0", 1) != 1)
            uf_log(UF_LOG_MOUNT, OS_LOG_TYPE_ERROR, "cannot signal the parent: %s", strerror(errno));
        close(ready_fd);
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
            dup2(null, STDOUT_FILENO);
            dup2(null, STDERR_FILENO);
            close(null);
        }
        uf_log_set_stderr(0);
    }
    rc = fuse_session_loop(se) == 0 ? 0 : 1;
    fuse_session_unmount(se);
    uf_log(UF_LOG_MOUNT, OS_LOG_TYPE_DEFAULT, "unmounted %s", mountpoint);
signals:
    fuse_remove_signal_handlers(se);
destroy:
    fuse_session_destroy(se);
close:
    if (ue_has_failed(fs.engine))
        uf_log(UF_LOG_MOUNT, OS_LOG_TYPE_ERROR, "%s failed during use; nothing more was written", source);
    if ((err = ue_close(fs.engine))) {
        uf_log(UF_LOG_MOUNT, OS_LOG_TYPE_ERROR, "closing %s: %s; the volume may need Windows chkdsk", source,
               strerror(err));
        rc = 1;
    }
    free(fs.rbuf);
    free(fs.nodes);
    record_status(bsd_name, &p, fs.read_only, false);
    file_io_close(&f);
    return rc;
}
