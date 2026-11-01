// SPDX-License-Identifier: GPL-3.0-or-later
// unlatch-helper: the only part of Unlatch that runs as root (D-57).
//
// launchd starts it on demand for connections to its socket. It does one thing: open the
// device node of an NTFS volume for the console user and pass the descriptor back
// (SCM_RIGHTS), because real disks' nodes are root:operator 0640. It never mounts, reads or
// writes the disk itself; unlatch-fuse does that as the user.
//
// Protocol, one request per connection, lines of ASCII:
//   client:  open <bsd-name> ro|rw\n
//   helper:  ok\n (plus the descriptor)  |  err <errno> <message>\n
// Refused: callers other than the console user; names that are not diskN or diskNsM; volumes
// DiskArbitration does not call NTFS (or that look like one by partition type).
#include <CoreFoundation/CoreFoundation.h>
#include <DiskArbitration/DiskArbitration.h>
#include <errno.h>
#include <fcntl.h>
#include <launch.h>
#include <os/log.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define HELPER_LABEL "io.github.ramanan-bala.unlatch.helper"

static os_log_t hlog;

static int reply(int s, int fd, int err, const char *msg)
{
    char line[300];
    struct msghdr m = { 0 };
    struct iovec iov;
    union {
        struct cmsghdr h;
        char buf[CMSG_SPACE(sizeof(int))];
    } ctl;

    if (err)
        snprintf(line, sizeof(line), "err %d %s\n", err, msg);
    else
        snprintf(line, sizeof(line), "ok\n");
    iov.iov_base = line;
    iov.iov_len = strlen(line);
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    if (fd >= 0) {
        memset(&ctl, 0, sizeof(ctl));
        m.msg_control = ctl.buf;
        m.msg_controllen = sizeof(ctl.buf);
        struct cmsghdr *c = CMSG_FIRSTHDR(&m);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof(int));
    }
    return sendmsg(s, &m, 0) < 0 ? errno : 0;
}

// True if DiskArbitration describes bsd as an NTFS volume (or an NTFS-typed partition).
static bool is_ntfs(const char *bsd)
{
    bool ok = false;
    DASessionRef session = DASessionCreate(kCFAllocatorDefault);
    if (!session)
        return false;
    DADiskRef disk = DADiskCreateFromBSDName(kCFAllocatorDefault, session, bsd);
    if (disk) {
        CFDictionaryRef d = DADiskCopyDescription(disk);
        if (d) {
            CFStringRef kind = CFDictionaryGetValue(d, kDADiskDescriptionVolumeKindKey);
            CFStringRef content = CFDictionaryGetValue(d, kDADiskDescriptionMediaContentKey);
            if (kind)
                ok = CFStringCompare(kind, CFSTR("ntfs"), 0) == kCFCompareEqualTo;
            else if (content)
                ok = CFStringCompare(content, CFSTR("Windows_NTFS"), 0) == kCFCompareEqualTo ||
                     CFStringCompare(content, CFSTR("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"), 0) == kCFCompareEqualTo;
            CFRelease(d);
        }
        CFRelease(disk);
    }
    CFRelease(session);
    return ok;
}

static void serve(int s)
{
    char req[128] = { 0 }, bsd[32], mode[4];
    uid_t uid;
    gid_t gid;
    struct stat console;
    regex_t re;

    if (getpeereid(s, &uid, &gid) < 0) {
        reply(s, -1, errno, "cannot identify the caller");
        return;
    }
    // Only the user at the screen; not other local or remote users.
    if (stat("/dev/console", &console) < 0 || console.st_uid != uid || uid == 0) {
        os_log_error(hlog, "refused uid %d (console uid %d)", uid, (int)console.st_uid);
        reply(s, -1, EPERM, "only the console user may open disks");
        return;
    }
    ssize_t n = read(s, req, sizeof(req) - 1);
    if (n <= 0 || sscanf(req, "open %31s %3s", bsd, mode) != 2 || (strcmp(mode, "ro") && strcmp(mode, "rw"))) {
        reply(s, -1, EINVAL, "bad request");
        return;
    }
    if (regcomp(&re, "^disk[0-9]+(s[0-9]+)?$", REG_EXTENDED | REG_NOSUB) != 0)
        return;
    int bad = regexec(&re, bsd, 0, NULL, 0);
    regfree(&re);
    if (bad) {
        reply(s, -1, EINVAL, "not a disk name");
        return;
    }
    if (!is_ntfs(bsd)) {
        os_log(hlog, "refused %{public}s for uid %d: not NTFS", bsd, uid);
        reply(s, -1, EPERM, "not an NTFS volume");
        return;
    }
    char path[64];
    snprintf(path, sizeof(path), "/dev/%s", bsd);
    int fd = open(path, strcmp(mode, "rw") ? O_RDONLY : O_RDWR);
    if (fd < 0) {
        int err = errno;
        reply(s, -1, err, strerror(err));
        return;
    }
    os_log(hlog, "opened %{public}s %{public}s for uid %d", path, mode, uid);
    reply(s, fd, 0, NULL);
    close(fd);
}

int main(void)
{
    int *fds = NULL;
    size_t count = 0;

    hlog = os_log_create("io.github.ramanan-bala.unlatch", "helper");
    int err = launch_activate_socket("Listeners", &fds, &count);
    if (err || count == 0) {
        os_log_error(hlog, "no launchd socket: %d", err);
        return 1;
    }
    // Serve until idle; launchd starts the helper again on the next connection.
    for (;;) {
        fd_set set;
        struct timeval idle = { .tv_sec = 30 };
        FD_ZERO(&set);
        FD_SET(fds[0], &set);
        if (select(fds[0] + 1, &set, NULL, NULL, &idle) <= 0)
            break;
        int s = accept(fds[0], NULL, NULL);
        if (s < 0)
            continue;
        struct timeval to = { .tv_sec = 5 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
        serve(s);
        close(s);
    }
    return 0;
}
