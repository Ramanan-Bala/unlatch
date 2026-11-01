// SPDX-License-Identifier: GPL-3.0-or-later
// Deterministic test data: byte i of stream `seed` is byte (i % 8) of splitmix64(seed, i / 8).
// Shared by mkfixture and the engine tests; random access at any offset.
#ifndef UE_GEN_H
#define UE_GEN_H

#include <stddef.h>
#include <stdint.h>

static inline uint64_t gen_word(uint64_t seed, uint64_t idx)
{
    uint64_t z = seed * 0x9E3779B97F4A7C15ull + idx * 0xBF58476D1CE4E5B9ull + 1;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static inline void gen_fill(uint64_t seed, uint64_t off, void *buf, size_t len)
{
    unsigned char *p = buf;

    while (len > 0) {
        uint64_t w = gen_word(seed, off / 8);
        unsigned k = (unsigned)(off % 8);
        for (; k < 8 && len > 0; k++, len--, off++)
            *p++ = (unsigned char)(w >> (8 * k));
    }
}

#endif // UE_GEN_H
