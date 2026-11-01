// SPDX-License-Identifier: GPL-3.0-or-later
// set_dirty <img>: sets VOLUME_IS_DIRTY in $Volume's $VOLUME_INFORMATION, in both $MFT and
// $MFTMirr, by patching bytes directly. Test fixture only; assumes a fresh mkntfs image
// (contiguous $MFT, record 3 inside the mirror).
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)le16(p) | (uint32_t)le16(p + 2) << 16; }
static int64_t le64(const uint8_t *p) { return (int64_t)((uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32); }

static int patch_record(FILE *f, long long rec_off, uint32_t rec_size, uint32_t bps)
{
    uint8_t rec[4096];
    uint32_t a;

    if (rec_size > sizeof(rec) || fseeko(f, rec_off, SEEK_SET) || fread(rec, rec_size, 1, f) != 1)
        return -1;
    if (memcmp(rec, "FILE", 4) != 0)
        return -1;
    for (a = le16(rec + 0x14); a + 8 <= rec_size; a += le32(rec + a + 4)) {
        uint32_t type = le32(rec + a);
        if (type == 0xFFFFFFFF || le32(rec + a + 4) == 0)
            return -1;
        if (type == 0x70) {            // $VOLUME_INFORMATION, always resident
            uint32_t flags = a + le16(rec + a + 0x14) + 0x0A;
            // The last two bytes of each sector hold the update sequence number.
            if (flags % bps >= bps - 2)
                return -1;
            rec[flags] |= 0x01;        // VOLUME_IS_DIRTY
            if (fseeko(f, rec_off + flags, SEEK_SET) || fwrite(&rec[flags], 1, 1, f) != 1)
                return -1;
            return 0;
        }
    }
    return -1;
}

int main(int argc, char **argv)
{
    uint8_t bs[512];
    FILE *f;

    if (argc != 2 || !(f = fopen(argv[1], "r+b")) || fread(bs, sizeof(bs), 1, f) != 1) {
        fprintf(stderr, "usage: set_dirty <img>\n");
        return 1;
    }
    uint32_t bps = le16(bs + 0x0B);
    uint32_t cluster = bps * bs[0x0D];
    int8_t cpr = (int8_t)bs[0x40];
    uint32_t rec_size = cpr > 0 ? (uint32_t)cpr * cluster : 1u << -cpr;
    long long mft = le64(bs + 0x30) * cluster, mirr = le64(bs + 0x38) * cluster;

    if (patch_record(f, mft + 3LL * rec_size, rec_size, bps) ||
        patch_record(f, mirr + 3LL * rec_size, rec_size, bps)) {
        fprintf(stderr, "set_dirty: could not patch $Volume\n");
        fclose(f);
        return 1;
    }
    return fclose(f) ? 1 : 0;
}
