// SPDX-License-Identifier: GPL-3.0-or-later
// unlatch-fuse: mounts an NTFS image or device through macFUSE (plan Step 08, D-40).
//
//   unlatch-fuse [-f] [-d] [-r] [--backend auto|fskit|kext] [-o opt,...] <image-or-device> <mountpoint>
//
// The default backend is macFUSE's FSKit backend, which needs no kernel extension and so no
// trip to Recovery. It mounts only directly under /Volumes, so other mount points (tests,
// benchmarks) use the kernel extension (D-78).
//
// The safety decision is made here, before mounting: CORRUPT and NOT_NTFS are refused;
// read-write only when the volume probes CLEAN, the device opens writable and -r was not
// given; everything else mounts read-only. The verdict goes to the status record (status.c).
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>
#include <limits.h>
#include <pthread.h>
#include <time.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/disk.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "file_io.h"
#include "log.h"
#include "ops.h"
#include "status.h"

static void usage(void)
{
    fprintf(stderr,
            "usage: unlatch-fuse [-f] [-d] [-r] [--backend auto|fskit|kext] [-o opt,...] <image-or-device> <mountpoint>\n"
            "  -f            stay in the foreground\n"
            "  -d            FUSE debug output (implies -f)\n"
            "  -r            mount read-only even when the volume could be written\n"
            "  --fd N        use the already open descriptor N for the device (from the privileged\n"
            "                helper); the path then only names the volume\n"
            "  --backend B   macFUSE backend: fskit (no kernel extension; mounts only directly\n"
            "                under /Volumes), kext (macFUSE's kernel extension, allowed once in\n"
            "                Recovery), or auto (default: fskit under /Volumes, else kext)\n"
            "  -o opt,...    extra macFUSE mount options\n"
            "  --no-cache    no engine page cache (for comparisons)\n");
}

// Dirty cached pages older than this are written back, so a crash or unplug loses at most
// about this much metadata; the kernel's own buffer cache allows 30 s (D-62).
#define WRITEBACK_MS 1000

// The session loop is fuse_session_loop's, plus a lock: requests are processed one at a time
// on this thread while holding fs->lock, and a timer thread takes the same lock to write back
// dirty cached pages that are WRITEBACK_MS old. Engine calls stay serialized. (Polling the
// session fd instead made rm -r 15x slower: macFUSE 5 does not signal every request on it.)
typedef struct {
    uf_fs_t *fs;
    pthread_mutex_t *lock;
    pthread_cond_t cond;
    bool stop;
} flusher_t;

static void *flusher(void *arg)
{
    flusher_t *f = arg;

    pthread_mutex_lock(f->lock);
    while (!f->stop) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += (WRITEBACK_MS / 4) * 1000000L;
        ts.tv_sec += ts.tv_nsec / 1000000000L;
        ts.tv_nsec %= 1000000000L;
        pthread_cond_timedwait(&f->cond, f->lock, &ts);
        uf_fs_t *fs = f->fs;
        if (!f->stop && fs->engine && !fs->read_only && ue_writeback(fs->engine, WRITEBACK_MS) == EIO &&
            !fs->failed_logged) {
            uf_log(UF_LOG_MOUNT, OS_LOG_TYPE_ERROR, "writing cached metadata back failed; nothing more is written");
            fs->failed_logged = true;
        }
    }
    pthread_mutex_unlock(f->lock);
    return NULL;
}

static int session_loop(struct fuse_session *se, uf_fs_t *fs)
{
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    flusher_t f = { .fs = fs, .lock = &lock, .cond = PTHREAD_COND_INITIALIZER };
    struct fuse_buf buf = { .mem = NULL };
    pthread_t thread = NULL;
    bool have_thread = !fs->read_only && pthread_create(&thread, NULL, flusher, &f) == 0;
    int res = 0;

    while (!fuse_session_exited(se)) {
        res = fuse_session_receive_buf(se, &buf);
        if (res == -EINTR)
            continue;
        if (res <= 0)
            break;
        pthread_mutex_lock(&lock);
        fuse_session_process_buf(se, &buf);
        pthread_mutex_unlock(&lock);
    }
    if (have_thread) {
        pthread_mutex_lock(&lock);
        f.stop = true;
        pthread_cond_signal(&f.cond);
        pthread_mutex_unlock(&lock);
        pthread_join(thread, NULL);
    }
    free(buf.mem);
    if (res > 0)
        res = 0;
    fuse_session_reset(se);
    return res < 0 ? -1 : 0;
}

// /dev/diskN -> /dev/rdiskN: the engine's page cache does what the buffer cache did, without
// splitting large transfers (D-62). Other paths are used as given.
static void raw_path(const char *in, char *out, size_t cap)
{
    if (!strncmp(in, "/dev/disk", 9))
        snprintf(out, cap, "/dev/r%s", in + 5);
    else
        snprintf(out, cap, "%s", in);
}

static void record_status(const char *bsd_name, const ue_probe_t *p, bool read_only, const char *mount_point)
{
    int err = status_write(bsd_name, p, read_only, mount_point);

    if (err)
        uf_log(UF_LOG_MOUNT, OS_LOG_TYPE_ERROR, "status record for %s: %s", bsd_name, strerror(err));
}

// A disk pulled out without ejecting: macOS tells the FUSE daemon nothing, and the mount and
// its lock (D-81) stayed until something touched the volume, 20-40 s on a USB stick, so the
// next mount of the same disk was refused. A thread asks the device for its size every
// second; ENXIO/ENODEV means it is gone, and the daemon force-unmounts itself (D-85).
typedef struct {
    int fd;
    const char *mountpoint;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool stop;
} watchdog_t;

static void *watchdog(void *arg)
{
    watchdog_t *w = arg;
    uint64_t count;

    pthread_mutex_lock(&w->lock);
    while (!w->stop) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        pthread_cond_timedwait(&w->cond, &w->lock, &ts);
        if (w->stop)
            break;
        if (ioctl(w->fd, DKIOCGETBLOCKCOUNT, &count) < 0 && (errno == ENXIO || errno == ENODEV)) {
            uf_log(UF_LOG_MOUNT, OS_LOG_TYPE_ERROR, "the disk under %s went away; unmounting", w->mountpoint);
            unmount(w->mountpoint, MNT_FORCE);   // the session loop then ends
            break;
        }
    }
    pthread_mutex_unlock(&w->lock);
    return NULL;
}

// One mount per volume: a read-write mount holds an exclusive flock on a file named after the
// volume serial, a read-only one a shared lock, so a second read-write mount of the same volume
// (the app's auto-mount racing `unlatch mount`, or an image and its attached device) is refused
// (D-81). The serial, not the path, names the volume: /dev/diskN, /dev/rdiskN and an image file
// all lead to it. The directory is shared by every user, since daemons run as the user (with the
// helper's descriptor) or as root (sudo). The lock goes with the process, however it ends.
#define LOCK_DIR "/private/tmp/io.github.ramanan-bala.unlatch.locks"

static int lock_volume(uint64_t serial, bool exclusive, int *out_fd)
{
    char path[sizeof(LOCK_DIR) + 32];
    struct stat st;
    mode_t old = umask(0);
    int fd;

    if (mkdir(LOCK_DIR, 0777) == 0)
        chmod(LOCK_DIR, 01777);       // sticky, like /tmp: nobody removes another user's lock file
    else if (errno != EEXIST) {
        int err = errno;
        umask(old);
        return err;
    }
    umask(old);
    if (lstat(LOCK_DIR, &st) < 0)
        return errno;
    if (!S_ISDIR(st.st_mode))
        return ENOTDIR;
    snprintf(path, sizeof(path), LOCK_DIR "/%016llX", (unsigned long long)serial);
    if ((fd = open(path, O_RDONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0444)) < 0)
        return errno;
    if (flock(fd, (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB) < 0) {
        int err = errno == EWOULDBLOCK ? EBUSY : errno;
        close(fd);
        return err;
    }
    *out_fd = fd;
    return 0;
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

// Printed last when an FSKit mount fails, which the CLI and the app show. Usual causes: the
// extension is off, or macFUSE's daemon just restarted and reports it "not found" or "not
// enabled" for a minute or so (D-78).
#define FSKIT_HINT "macFUSE's file system extension did not take the mount; try again in a minute, and check " \
                   "that it is on: System Settings > General > Login Items & Extensions > File System " \
                   "Extensions > macFUSE"

static bool directly_in_volumes(const char *path)
{
    return !strncmp(path, "/Volumes/", 9) && path[9] && !strchr(path + 9, '/');
}

static int run_in_background(int argc, char **argv, const char *mountpoint, bool fskit)
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
        // With FSKit, macFUSE can still refuse after the child's mount call returned.
        for (int i = 0; i < 1000; i++) {
            if (is_mounted(mountpoint))
                return 0;             // mounted; the child carries on
            if (waitpid(pid, &status, WNOHANG) == pid)
                break;
            usleep(10000);
        }
        fprintf(stderr, "unlatch-fuse: %s did not appear as a mount\n", mountpoint);
        if (fskit)
            fprintf(stderr, "unlatch-fuse: " FSKIT_HINT "\n");
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
        { "fd", required_argument, NULL, 'D' },
        { "ready-fd", required_argument, NULL, 'F' },
        { "no-cache", no_argument, NULL, 'C' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    const char *backend = "auto", *extra = NULL;
    bool foreground = false, debug = false, want_ro = false, cache = true;
    int ch, ready_fd = -1, dev_fd = -1;

    while ((ch = getopt_long(argc, argv, "fdro:h", longopts, NULL)) != -1) {
        switch (ch) {
        case 'f': foreground = true; break;
        case 'r': case 'R': want_ro = true; break;
        case 'd': debug = foreground = true; break;
        case 'o': extra = optarg; break;
        case 'b': backend = optarg; break;
        case 'F': ready_fd = atoi(optarg); break;
        case 'D': dev_fd = atoi(optarg); break;
        case 'C': cache = false; break;
        default: usage(); return ch == 'h' ? 0 : 2;
        }
    }
    if (argc - optind != 2 || (strcmp(backend, "auto") && strcmp(backend, "fskit") && strcmp(backend, "kext"))) {
        usage();
        return 2;
    }
    if (!foreground)
        return run_in_background(argc, argv, argv[optind + 1],
                                 strcmp(backend, "kext") && directly_in_volumes(argv[optind + 1]));
    uf_log_init();
    char source_raw[PATH_MAX];
    raw_path(argv[optind], source_raw, sizeof(source_raw));
    const char *source = source_raw;
    // mount_macfuse resolves a relative mount point against /, so make it absolute.
    char mountpoint[PATH_MAX];
    // /Volumes/<name> need not exist: mount_macfuse creates it there (users cannot), D-58.
    if (!strncmp(argv[optind + 1], "/Volumes/", 9) && !strchr(argv[optind + 1] + 9, '/') &&
        access(argv[optind + 1], F_OK) != 0)
        snprintf(mountpoint, sizeof(mountpoint), "%s", argv[optind + 1]);
    else if (!realpath(argv[optind + 1], mountpoint)) {
        uf_log(UF_LOG_MOUNT, OS_LOG_TYPE_ERROR, "%s: %s", argv[optind + 1], strerror(errno));
        return 1;
    }
    bool in_volumes = directly_in_volumes(mountpoint);
    if (!strcmp(backend, "auto"))
        backend = in_volumes ? "fskit" : "kext";
    else if (!strcmp(backend, "fskit") && !in_volumes) {
        uf_log(UF_LOG_MOUNT, OS_LOG_TYPE_ERROR, "%s: the fskit backend mounts only directly under /Volumes", mountpoint);
        return 1;
    }
    // basename() may return a static buffer that later calls overwrite (libfuse's own use of
    // it turned the status record's bsdName into "unlatch-fuse"), so keep a copy.
    char source_copy[1024], bsd_buf[256];
    snprintf(source_copy, sizeof(source_copy), "%s", argv[optind]);
    snprintf(bsd_buf, sizeof(bsd_buf), "%s", basename(source_copy));
    const char *bsd_name = bsd_buf;

    file_io_t f;
    ue_probe_t p;
    int err;
    // /dev/diskN is opened as /dev/rdiskN, with the engine's page cache in front (D-62).
    bool fd_writable = dev_fd >= 0 && (fcntl(dev_fd, F_GETFL) & O_ACCMODE) == O_RDWR;
    if ((err = dev_fd >= 0 ? file_io_from_fd(&f, dev_fd, fd_writable) : file_io_open(&f, source, false))) {
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
        record_status(bsd_name, &p, true, NULL);
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
    } else if (!want_ro && dev_fd >= 0) {
        if (fd_writable)
            fs.read_only = false;
        else
            uf_log(UF_LOG_MOUNT, OS_LOG_TYPE_DEFAULT, "%s was handed over read-only, read-only", source);
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
    int lock_fd = -1;
    if ((err = lock_volume(p.serial, !fs.read_only, &lock_fd))) {
        if (err == EBUSY)
            uf_log(UF_LOG_MOUNT, OS_LOG_TYPE_ERROR, "%s: this volume (serial %016llX) is already mounted%s; "
                   "unmount it first", source, (unsigned long long)p.serial,
                   fs.read_only ? " read-write" : "");
        else
            uf_log(UF_LOG_MOUNT, OS_LOG_TYPE_ERROR, "%s: cannot lock the volume (%s), not mounting", source,
                   strerror(err));
        file_io_close(&f);
        return 1;
    }
    ue_probe_t info;
    ue_attr_t root;
    uint32_t cflag = (cache ? UE_OPEN_CACHE : 0) | UE_OPEN_GHOST_OPEN_ONLY;   // D-82
    err = ue_open(&f.io, (fs.read_only ? UE_OPEN_RDONLY : 0) | cflag, &fs.engine, &info);
    if (err == EROFS && !fs.read_only) {
        uf_log(UF_LOG_MOUNT, OS_LOG_TYPE_DEFAULT, "%s is write-protected, read-only", source);
        fs.read_only = true;
        err = ue_open(&f.io, UE_OPEN_RDONLY | cflag, &fs.engine, &info);
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
        if (!strcmp(backend, "fskit"))
            uf_log(UF_LOG_MOUNT, OS_LOG_TYPE_ERROR, FSKIT_HINT);
        goto signals;
    }
    record_status(bsd_name, &p, fs.read_only, mountpoint);
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
    watchdog_t wd = { .fd = f.fd, .mountpoint = mountpoint, .lock = PTHREAD_MUTEX_INITIALIZER,
                      .cond = PTHREAD_COND_INITIALIZER };
    struct stat dst;
    pthread_t wd_thread = NULL;
    bool have_wd = fstat(f.fd, &dst) == 0 && (S_ISBLK(dst.st_mode) || S_ISCHR(dst.st_mode)) &&
                   pthread_create(&wd_thread, NULL, watchdog, &wd) == 0;
    rc = session_loop(se, &fs) == 0 ? 0 : 1;
    if (have_wd) {
        pthread_mutex_lock(&wd.lock);
        wd.stop = true;
        pthread_cond_signal(&wd.cond);
        pthread_mutex_unlock(&wd.lock);
        pthread_join(wd_thread, NULL);
    }
    fuse_session_unmount(se);
    uf_log(UF_LOG_MOUNT, OS_LOG_TYPE_DEFAULT, "unmounted %s", mountpoint);
signals:
    fuse_remove_signal_handlers(se);
destroy:
    fuse_session_destroy(se);
close:
    uf_close_engine(&fs);             // normally already done on DESTROY
    if (fs.close_err)
        rc = 1;
    free(fs.rbuf);
    free(fs.nodes);
    record_status(bsd_name, &p, fs.read_only, NULL);
    file_io_close(&f);
    close(lock_fd);                   // after the volume is closed and the device released
    return rc;
}
