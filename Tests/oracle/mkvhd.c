// SPDX-License-Identifier: GPL-3.0-or-later
// mkvhd <volume.img> <out.vhd>: wraps a bare NTFS volume image as a fixed VHD that Windows can
// attach (Mount-DiskImage): an MBR with one NTFS partition (type 0x07) at 1 MiB, the volume
// copied in with its boot sectors' hidden-sectors field set to 2048, and a VHD footer.
// The output stays sparse: all-zero chunks are not written.
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LBA0 2048u
#define CHUNK (64u << 10)

static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void be64(uint8_t *p, uint64_t v) { be32(p, (uint32_t)(v >> 32)); be32(p + 4, (uint32_t)v); }
static void le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static int die(const char *what)
{
    fprintf(stderr, "mkvhd: %s: %s\n", what, strerror(errno));
    return 1;
}

// CHS geometry from the VHD specification (appendix "CHS calculation").
static void geometry(uint64_t size, uint8_t out[4])
{
    uint64_t sectors = size / 512, cyl_heads;
    uint32_t spt, heads;

    if (sectors > 65535ull * 16 * 255)
        sectors = 65535ull * 16 * 255;
    if (sectors >= 65535ull * 16 * 63) {
        spt = 255;
        heads = 16;
        cyl_heads = sectors / spt;
    } else {
        spt = 17;
        cyl_heads = sectors / spt;
        heads = (uint32_t)((cyl_heads + 1023) / 1024);
        if (heads < 4)
            heads = 4;
        if (cyl_heads >= heads * 1024ull || heads > 16) {
            spt = 31;
            heads = 16;
            cyl_heads = sectors / spt;
        }
        if (cyl_heads >= heads * 1024ull) {
            spt = 63;
            heads = 16;
            cyl_heads = sectors / spt;
        }
    }
    uint32_t cyl = (uint32_t)(cyl_heads / heads);
    out[0] = cyl >> 8;
    out[1] = cyl;
    out[2] = (uint8_t)heads;
    out[3] = (uint8_t)spt;
}

int main(int argc, char **argv)
{
    uint8_t *buf = malloc(CHUNK), mbr[512] = { 0 }, footer[512] = { 0 };
    struct stat st;
    int in, out;

    if (argc != 3 || !buf) {
        fprintf(stderr, "usage: mkvhd <volume.img> <out.vhd>\n");
        return 64;
    }
    if ((in = open(argv[1], O_RDONLY)) < 0 || fstat(in, &st) < 0)
        return die(argv[1]);
    uint64_t vol = (uint64_t)st.st_size;
    uint64_t disk = ((uint64_t)LBA0 * 512 + vol + CHUNK - 1) / CHUNK * CHUNK;
    if ((out = open(argv[2], O_RDWR | O_CREAT | O_TRUNC, 0644)) < 0 || ftruncate(out, (off_t)(disk + 512)))
        return die(argv[2]);

    // MBR: one active=0 NTFS partition from LBA 2048 covering the volume.
    le32(mbr + 440, 0x554E4C54);                  // disk signature "TLNU"
    uint8_t *pe = mbr + 446;
    pe[0] = 0x00;
    pe[1] = 0xFE; pe[2] = 0xFF; pe[3] = 0xFF;     // CHS start: use LBA
    pe[4] = 0x07;                                 // NTFS / exFAT / HPFS
    pe[5] = 0xFE; pe[6] = 0xFF; pe[7] = 0xFF;     // CHS end: use LBA
    le32(pe + 8, LBA0);
    le32(pe + 12, (uint32_t)(vol / 512));
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    if (pwrite(out, mbr, 512, 0) != 512)
        return die("write MBR");

    // Volume, keeping holes.
    for (uint64_t off = 0; off < vol; off += CHUNK) {
        size_t n = vol - off < CHUNK ? (size_t)(vol - off) : CHUNK;
        ssize_t r = pread(in, buf, n, (off_t)off);
        if (r != (ssize_t)n)
            return die("read volume");
        if (off == 0 || off + n >= vol) {
            // Boot sector (first) and backup boot sector (last sector): hidden sectors = 2048.
            if (off == 0 && !memcmp(buf + 3, "NTFS    ", 8))
                le32(buf + 0x1C, LBA0);
            uint8_t *last = buf + n - 512;
            if (off + n == vol && !memcmp(last + 3, "NTFS    ", 8))
                le32(last + 0x1C, LBA0);
        }
        size_t k = 0;
        while (k < n && buf[k] == 0)
            k++;
        if (k < n && pwrite(out, buf, n, (off_t)(LBA0 * 512ull + off)) != (ssize_t)n)
            return die("write volume");
    }

    // Fixed VHD footer (big-endian).
    memcpy(footer, "conectix", 8);
    be32(footer + 8, 2);                          // features: reserved bit
    be32(footer + 12, 0x00010000);                // format version 1.0
    be64(footer + 16, UINT64_MAX);                // data offset: none (fixed disk)
    be32(footer + 24, (uint32_t)(time(NULL) - 946684800));
    memcpy(footer + 28, "unlt", 4);
    be32(footer + 32, 0x00010000);
    memcpy(footer + 36, "Wi2k", 4);
    be64(footer + 40, disk);
    be64(footer + 48, disk);
    geometry(disk, footer + 56);
    be32(footer + 60, 2);                         // disk type: fixed
    for (int i = 0; i < 16; i++)
        footer[68 + i] = (uint8_t)(rand() ^ (time(NULL) >> (i % 8)));
    uint32_t sum = 0;
    for (int i = 0; i < 512; i++)
        sum += footer[i];
    be32(footer + 64, ~sum);
    if (pwrite(out, footer, 512, (off_t)disk) != 512)
        return die("write footer");
    if (close(out))
        return die(argv[2]);
    close(in);
    free(buf);
    printf("%s\n", argv[2]);
    return 0;
}
