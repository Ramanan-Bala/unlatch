// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef UE_FSX_H
#define UE_FSX_H

#include <stdbool.h>
#include <stdint.h>

// Runs the randomized model test on an empty, freshly formatted image. Returns 0 on success.
int fsx_main(const char *img, uint64_t seed, unsigned long ops, bool verbose);

#endif // UE_FSX_H
