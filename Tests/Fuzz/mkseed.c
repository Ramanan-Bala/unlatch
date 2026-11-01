// SPDX-License-Identifier: GPL-3.0-or-later
// mkseed <image> <out>: converts an image into the fuzz "sector list" format, keeping only
// non-zero sectors (see mem_io.h).
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    unsigned char sec[512], zero[512] = { 0 }, hdr[4];
    FILE *in, *out;
    uint32_t s = 0;
    long size;

    if (argc != 3 || !(in = fopen(argv[1], "rb")) || !(out = fopen(argv[2], "wb"))) {
        fprintf(stderr, "usage: mkseed <image> <out>\n");
        return 64;
    }
    fseek(in, 0, SEEK_END);
    size = ftell(in);
    fseek(in, 0, SEEK_SET);
    uint32_t sectors = (uint32_t)(size / 512);
    for (int i = 0; i < 4; i++)
        hdr[i] = (unsigned char)(sectors >> (8 * i));
    fwrite(hdr, 1, 4, out);
    while (fread(sec, 1, 512, in) == 512) {
        if (memcmp(sec, zero, 512)) {
            for (int i = 0; i < 4; i++)
                hdr[i] = (unsigned char)(s >> (8 * i));
            fwrite(hdr, 1, 4, out);
            fwrite(sec, 1, 512, out);
        }
        s++;
    }
    fclose(in);
    return fclose(out) ? 1 : 0;
}
