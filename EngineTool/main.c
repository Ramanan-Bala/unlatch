// SPDX-License-Identifier: GPL-3.0-or-later
// unlatch-engine: host harness for the engine. Works on image files only.
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "file_io.h"
#include "unlatch_engine.h"

#define EX_USAGE 64
#define EX_FAIL 1

static void usage(FILE *out)
{
    fprintf(out,
            "usage: unlatch-engine <command> [args]\n"
            "\n"
            "commands:\n"
            "  probe <img>          safety probe: state, reason, label, sizes\n"
            "  info [--rw] <img>    open the volume (read-only unless --rw) and print it\n"
            "  --version            print the engine version\n");
}

static void print_probe(const ue_probe_t *p, bool with_state)
{
    if (with_state) {
        printf("state:        %s\n", ue_vol_state_name(p->state));
        printf("reason:       %s\n", p->reason);
    }
    if (p->state == UE_VOL_NOT_NTFS)
        return;
    printf("label:        %s\n", p->label);
    printf("serial:       %016" PRIX64 "\n", p->serial);
    printf("ntfs_version: %u.%u\n", p->major_ver, p->minor_ver);
    printf("cluster_size: %" PRIu32 "\n", p->cluster_size);
    printf("total_bytes:  %" PRIu64 "\n", p->total_bytes);
    printf("free_bytes:   %" PRIu64 "\n", p->free_bytes);
}

static int cmd_probe(const char *path)
{
    file_io_t f;
    ue_probe_t p;
    int err;

    if ((err = file_io_open(&f, path, false))) {
        fprintf(stderr, "unlatch-engine: %s: %s\n", path, strerror(err));
        return EX_FAIL;
    }
    err = ue_probe(&f.io, &p);
    file_io_close(&f);
    if (err) {
        fprintf(stderr, "unlatch-engine: probe failed: %s\n", strerror(err));
        return EX_FAIL;
    }
    print_probe(&p, true);
    return 0;
}

static int cmd_info(const char *path, bool rw)
{
    file_io_t f;
    ue_engine_t *e;
    ue_probe_t p;
    int err;

    if ((err = file_io_open(&f, path, rw))) {
        fprintf(stderr, "unlatch-engine: %s: %s\n", path, strerror(err));
        return EX_FAIL;
    }
    err = ue_open(&f.io, rw ? 0 : UE_OPEN_RDONLY, &e, &p);
    if (err) {
        fprintf(stderr, "unlatch-engine: open failed: %s\n", strerror(err));
        file_io_close(&f);
        return EX_FAIL;
    }
    printf("mode:         %s\n", ue_is_readonly(e) ? "read-only" : "read-write");
    printf("dirty:        %s\n", p.state == UE_VOL_DIRTY ? "yes" : "no");
    print_probe(&p, false);
    err = ue_close(e);
    file_io_close(&f);
    if (err) {
        fprintf(stderr, "unlatch-engine: close failed: %s\n", strerror(err));
        return EX_FAIL;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("unlatch-engine %s (ABI %d)\n", ue_version(), UE_ABI_VERSION);
        return 0;
    }
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        usage(stdout);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "probe") == 0)
        return cmd_probe(argv[2]);
    if (argc == 3 && strcmp(argv[1], "info") == 0)
        return cmd_info(argv[2], false);
    if (argc == 4 && strcmp(argv[1], "info") == 0 && strcmp(argv[2], "--rw") == 0)
        return cmd_info(argv[3], true);
    usage(stderr);
    return EX_USAGE;
}
