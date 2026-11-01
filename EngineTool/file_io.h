// SPDX-License-Identifier: GPL-3.0-or-later
// File-backed ue_io_t for image files and block devices (/dev/diskN, /dev/rdiskN).
#ifndef UE_FILE_IO_H
#define UE_FILE_IO_H

#include <stdbool.h>

#include "unlatch_engine.h"

typedef struct {
    int fd;
    bool is_device;                   // block-aligned transfers only
    ue_io_t io;
} file_io_t;

// Opens path read-only or read-write and fills f->io. Returns 0 or errno.
int file_io_open(file_io_t *f, const char *path, bool writable);
// Same for a descriptor that is already open (a device the privileged helper opened, D-57).
// Takes ownership of fd, also on failure.
int file_io_from_fd(file_io_t *f, int fd, bool writable);
void file_io_close(file_io_t *f);

#endif // UE_FILE_IO_H
