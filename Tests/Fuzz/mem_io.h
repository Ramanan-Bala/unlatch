// SPDX-License-Identifier: GPL-3.0-or-later
// In-memory ue_io_t built from a fuzz input in the "sector list" format (Tests/Fuzz/README.md):
//   u32le image size in sectors, then records of { u32le sector, 512 bytes }.
// Sectors not listed are zero. Inputs that do not parse are skipped.
#ifndef UE_FUZZ_MEM_IO_H
#define UE_FUZZ_MEM_IO_H

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "unlatch_engine.h"

#define FUZZ_MIN_SECTORS 4096u          // 2 MiB: the smallest volume mkntfs makes
#define FUZZ_MAX_SECTORS 16384u         // 8 MiB

typedef struct {
    uint8_t *data;
    uint64_t size;
    ue_io_t io;
} mem_io_t;

static uint32_t fuzz_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int mem_pread(void *ctx, void *buf, size_t len, uint64_t off)
{
    mem_io_t *m = ctx;

    if (off > m->size || len > m->size - off)
        return EIO;
    memcpy(buf, m->data + off, len);
    return 0;
}

static int mem_pwrite(void *ctx, const void *buf, size_t len, uint64_t off)
{
    mem_io_t *m = ctx;

    if (off > m->size || len > m->size - off)
        return EIO;
    memcpy(m->data + off, buf, len);
    return 0;
}

static int mem_from_input(mem_io_t *m, const uint8_t *d, size_t n, int writable)
{
    uint32_t sectors;

    if (n < 4)
        return -1;
    sectors = fuzz_le32(d);
    if (sectors < FUZZ_MIN_SECTORS || sectors > FUZZ_MAX_SECTORS)
        return -1;
    m->size = (uint64_t)sectors * 512;
    m->data = calloc(1, (size_t)m->size);
    if (!m->data)
        return -1;
    for (size_t p = 4; p + 516 <= n; p += 516) {
        uint32_t s = fuzz_le32(d + p);
        if (s < sectors)
            memcpy(m->data + (uint64_t)s * 512, d + p + 4, 512);
    }
    memset(&m->io, 0, sizeof(m->io));
    m->io.ctx = m;
    m->io.pread = mem_pread;
    m->io.pwrite = writable ? mem_pwrite : NULL;
    m->io.size_bytes = m->size;
    m->io.block_size = 512;
    m->io.writable = writable != 0;
    return 0;
}

static void mem_free(mem_io_t *m)
{
    free(m->data);
    m->data = NULL;
}

#endif // UE_FUZZ_MEM_IO_H
