// SPDX-License-Identifier: GPL-3.0-or-later
// Readdir with a concurrent writer (Step 17). In <dir>, creates N files, then lists the
// directory with one stream while, part way through, a writer deletes every third file and
// creates as many new ones. POSIX leaves the added and removed names unspecified, but every
// name that existed for the whole listing must be returned exactly once, and nothing may be
// returned twice.
//
//   readdir-writer <dir> [N]          exit 0 when the listing is consistent
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int touch(const char *dir, const char *name)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    if (fd < 0)
        return -1;
    return close(fd);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: readdir-writer <dir> [N]\n");
        return 64;
    }
    const char *dir = argv[1];
    int n = argc > 2 ? atoi(argv[2]) : 3000;
    int *seen = calloc((size_t)n, sizeof(int));
    char name[64], path[4096];
    int bad = 0;

    for (int i = 0; i < n; i++) {
        snprintf(name, sizeof(name), "f%06d", i);
        if (touch(dir, name)) {
            perror(name);
            return 1;
        }
    }
    DIR *d = opendir(dir);
    if (!d) {
        perror(dir);
        return 1;
    }
    struct dirent *de;
    int count = 0, added = 0;
    while ((de = readdir(d))) {
        int i;
        if (de->d_name[0] == '.')
            continue;
        if (sscanf(de->d_name, "f%06d", &i) == 1 && i >= 0 && i < n && strlen(de->d_name) == 7)
            seen[i]++;
        if (++count == n / 3) {
            // The writer: remove every third original, add as many new names (sorting before
            // and after the originals, so index blocks split and merge).
            for (int k = 0; k < n; k += 3) {
                snprintf(path, sizeof(path), "%s/f%06d", dir, k);
                if (unlink(path))
                    perror(path);
                snprintf(name, sizeof(name), k % 2 ? "a%06d" : "z%06d", k);
                if (touch(dir, name) == 0)
                    added++;
            }
        }
    }
    closedir(d);
    for (int i = 0; i < n; i++) {
        bool stayed = i % 3 != 0;
        if (seen[i] > 1 || (stayed && seen[i] != 1)) {
            if (bad < 10)
                fprintf(stderr, "f%06d returned %d times%s\n", i, seen[i], stayed ? "" : " (deleted)");
            bad++;
        }
    }
    printf("readdir-writer: %d names, %d added during the listing, %d inconsistent\n", n, added, bad);
    return bad ? 1 : 0;
}
