// SPDX-License-Identifier: GPL-3.0-or-later
// Writes the same JSON as UnlatchCore.VolumeStatus (sorted keys, ISO 8601 timestamp).
#include "status.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef UNLATCH_VERSION
#define UNLATCH_VERSION "1.0.0-beta.2"
#endif

static const char *state_json(ue_vol_state_t s)
{
    switch (s) {
    case UE_VOL_CLEAN: return "clean";
    case UE_VOL_DIRTY: return "dirty";
    case UE_VOL_HIBERNATED: return "hibernated";
    case UE_VOL_UNCLEAN_LOG: return "unclean_log";
    case UE_VOL_CORRUPT: return "corrupt";
    default: return "not_ntfs";
    }
}

static void put_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            fprintf(f, "\\%c", c);
        else if (c < 0x20)
            fprintf(f, "\\u%04x", c);
        else
            fputc(c, f);
    }
    fputc('"', f);
}

static int mkdirs(char *path)
{
    for (char *p = path + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        int r = mkdir(path, 0755);
        *p = '/';
        if (r < 0 && errno != EEXIST)
            return errno;
    }
    if (mkdir(path, 0755) < 0 && errno != EEXIST)
        return errno;
    return 0;
}

int status_write(const char *bsd_name, const ue_probe_t *p, bool read_only, bool mounted)
{
    const char *home = getenv("HOME");
    char dir[1024], path[1100], tmp[1100], when[32];
    time_t now = time(NULL);
    struct tm tm;
    FILE *f;
    int err;

    if (!home)
        return ENOENT;
    snprintf(dir, sizeof(dir), "%s/Library/Application Support/Unlatch/status", home);
    if ((err = mkdirs(dir)))
        return err;
    snprintf(path, sizeof(path), "%s/%016llX.json", dir, (unsigned long long)p->serial);
    snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int)getpid());
    gmtime_r(&now, &tm);
    strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &tm);

    if (!(f = fopen(tmp, "w")))
        return errno;
    fprintf(f, "{\n  \"bsdName\" : ");
    put_string(f, bsd_name);
    fprintf(f, ",\n  \"engineABI\" : %d,\n  \"label\" : ", UE_ABI_VERSION);
    put_string(f, p->label);
    // pid: the app and CLI force-unmount a mount whose daemon is gone (Step 17, D-68).
    fprintf(f, ",\n  \"mounted\" : %s,\n  \"pid\" : %d,\n  \"readOnly\" : %s,\n  \"reason\" : ",
            mounted ? "true" : "false", (int)getpid(), read_only ? "true" : "false");
    put_string(f, p->reason);
    fprintf(f, ",\n  \"serial\" : \"%016llX\",\n  \"state\" : \"%s\",\n  \"timestamp\" : \"%s\",\n"
               "  \"unlatchVersion\" : \"%s\"\n}\n",
            (unsigned long long)p->serial, state_json(p->state), when, UNLATCH_VERSION);
    if (fclose(f) != 0 || rename(tmp, path) < 0) {
        err = errno;
        unlink(tmp);
        return err;
    }
    return 0;
}
