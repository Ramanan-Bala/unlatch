// SPDX-License-Identifier: GPL-3.0-or-later
// unlatch-engine: host harness for the engine. Commands arrive in Step 03.
#include <stdio.h>
#include <string.h>

#include "unlatch_engine.h"

static void usage(FILE *out)
{
    fprintf(out, "usage: unlatch-engine --version\n");
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("unlatch-engine %s (ABI %d)\n", ue_version(), UE_ABI_VERSION);
        return 0;
    }
    usage(argc == 2 && strcmp(argv[1], "--help") == 0 ? stdout : stderr);
    return argc == 2 && strcmp(argv[1], "--help") == 0 ? 0 : 64;
}
