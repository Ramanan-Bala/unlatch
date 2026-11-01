// SPDX-License-Identifier: GPL-3.0-or-later
#include "file_io.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fio_pread(void *ctx, void *buf, size_t len, uint64_t off)
{
    int fd = *(int *)ctx;
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

static int fio_pwrite(void *ctx, const void *buf, size_t len, uint64_t off)
{
    int fd = *(int *)ctx;
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

static int fio_flush(void *ctx)
{
    int fd = *(int *)ctx;

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
    f->io.ctx = &f->fd;
    f->io.pread = fio_pread;
    f->io.pwrite = writable ? fio_pwrite : NULL;
    f->io.flush = writable ? fio_flush : NULL;
    f->io.size_bytes = (uint64_t)st.st_size;
    f->io.block_size = 512;
    f->io.writable = writable;
    return 0;
}

void file_io_close(file_io_t *f)
{
    if (f->fd >= 0)
        close(f->fd);
    f->fd = -1;
}
