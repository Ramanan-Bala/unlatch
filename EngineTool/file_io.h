// SPDX-License-Identifier: GPL-3.0-or-later
// File-backed ue_io_t for image files (host harness and tests).
#ifndef UE_FILE_IO_H
#define UE_FILE_IO_H

#include <stdbool.h>

#include "unlatch_engine.h"

typedef struct {
    int fd;
    ue_io_t io;
} file_io_t;

// Opens path read-only or read-write and fills f->io. Returns 0 or errno.
int file_io_open(file_io_t *f, const char *path, bool writable);
void file_io_close(file_io_t *f);

#endif // UE_FILE_IO_H
