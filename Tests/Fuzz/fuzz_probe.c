// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer target: ue_probe on a mutated image.
#include "mem_io.h"

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n);

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    mem_io_t m;
    ue_probe_t p;

    if (mem_from_input(&m, d, n, 0))
        return 0;
    if (ue_probe(&m.io, &p) == 0 && (unsigned)p.state > UE_VOL_NOT_NTFS)
        abort();                        // the probe must always reach a known verdict
    mem_free(&m);
    return 0;
}
