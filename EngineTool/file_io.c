// SPDX-License-Identifier: GPL-3.0-or-later
#include "file_io.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

static int raw_pread(int fd, void *buf, size_t len, uint64_t off)
{
    char *p = buf;

    while (len > 0) {
        ssize_t n = pread(fd, p, len, (off_t)off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return errno;
        }
        if (n == 0)
            return EIO;                // past the end of the image
        p += n;
        len -= (size_t)n;
        off += (uint64_t)n;
    }
    return 0;
}

static int raw_pwrite(int fd, const void *buf, size_t len, uint64_t off)
{
    const char *p = buf;

    while (len > 0) {
        ssize_t n = pwrite(fd, p, len, (off_t)off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return errno;
        }
        p += n;
        len -= (size_t)n;
        off += (uint64_t)n;
    }
    return 0;
}

// Devices take only block-aligned transfers; libntfs-3g asks for byte ranges. An unaligned
// request goes through a bounce buffer covering the aligned range (read-modify-write for
// writes). Image files take any range.
static int fio_pread(void *ctx, void *buf, size_t len, uint64_t off)
{
    file_io_t *f = ctx;
    uint64_t bs = f->io.block_size, start, end;
    char *tmp;
    int err;

    if (!f->is_device || (off % bs == 0 && len % bs == 0))
        return raw_pread(f->fd, buf, len, off);
    start = off - off % bs;
    end = (off + len + bs - 1) / bs * bs;
    if (!(tmp = malloc(end - start)))
        return ENOMEM;
    if (!(err = raw_pread(f->fd, tmp, end - start, start)))
        memcpy(buf, tmp + (off - start), len);
    free(tmp);
    return err;
}

static int fio_pwrite(void *ctx, const void *buf, size_t len, uint64_t off)
{
    file_io_t *f = ctx;
    uint64_t bs = f->io.block_size, start, end;
    char *tmp;
    int err;

    if (!f->is_device || (off % bs == 0 && len % bs == 0))
        return raw_pwrite(f->fd, buf, len, off);
    start = off - off % bs;
    end = (off + len + bs - 1) / bs * bs;
    if (!(tmp = malloc(end - start)))
        return ENOMEM;
    // Only the first and last blocks hold bytes outside the request.
    err = raw_pread(f->fd, tmp, bs, start);
    if (!err && end - start > bs)
        err = raw_pread(f->fd, tmp + (end - start - bs), bs, end - bs);
    if (!err) {
        memcpy(tmp + (off - start), buf, len);
        err = raw_pwrite(f->fd, tmp, end - start, start);
    }
    free(tmp);
    return err;
}

static int fio_flush(void *ctx)
{
    int fd = ((file_io_t *)ctx)->fd;

    if (fcntl(fd, F_FULLFSYNC) == 0 || fsync(fd) == 0)
        return 0;
    return errno;
}

int file_io_open(file_io_t *f, const char *path, bool writable)
{
    struct stat st;

    memset(f, 0, sizeof(*f));
    f->fd = open(path, writable ? O_RDWR : O_RDONLY);
    if (f->fd < 0)
        return errno;
    if (fstat(f->fd, &st) < 0) {
        int err = errno;
        close(f->fd);
        f->fd = -1;
        return err;
    }
    f->io.ctx = f;
    f->io.pread = fio_pread;
    f->io.pwrite = writable ? fio_pwrite : NULL;
    f->io.flush = writable ? fio_flush : NULL;
    f->io.size_bytes = (uint64_t)st.st_size;
    f->io.block_size = 512;
    if (S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode)) {
        uint32_t bs = 0;
        uint64_t count = 0;
        if (ioctl(f->fd, DKIOCGETBLOCKSIZE, &bs) < 0 || ioctl(f->fd, DKIOCGETBLOCKCOUNT, &count) < 0) {
            int err = errno;
            close(f->fd);
            f->fd = -1;
            return err;
        }
        if (bs < 512 || (bs & (bs - 1))) {
            close(f->fd);
            f->fd = -1;
            return EINVAL;
        }
        f->is_device = true;
        f->io.block_size = bs;
        f->io.size_bytes = (uint64_t)bs * count;
    }
    f->io.writable = writable;
    return 0;
}

void file_io_close(file_io_t *f)
{
    if (f->fd >= 0)
        close(f->fd);
    f->fd = -1;
}
