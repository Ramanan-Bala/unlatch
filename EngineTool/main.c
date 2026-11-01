// SPDX-License-Identifier: GPL-3.0-or-later
// unlatch-engine: host harness for the engine. Works on image files only.
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "file_io.h"
#include "unlatch_engine.h"
#include "walk.h"

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
            "  ls [--stats] <img> [path]   list a directory (type, size, name)\n"
            "  stat <img> <path>    print a file's attributes\n"
            "  cat <img> <path>     write a file's contents to stdout\n"
            "  tree <img> [path]    list a directory recursively\n"
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

typedef struct {
    file_io_t f;
    ue_engine_t *e;
} session_t;

static int session_open(session_t *s, const char *img)
{
    int err;

    if ((err = file_io_open(&s->f, img, false))) {
        fprintf(stderr, "unlatch-engine: %s: %s\n", img, strerror(err));
        return EX_FAIL;
    }
    if ((err = ue_open(&s->f.io, UE_OPEN_RDONLY, &s->e, NULL))) {
        fprintf(stderr, "unlatch-engine: open failed: %s\n", strerror(err));
        file_io_close(&s->f);
        return EX_FAIL;
    }
    return 0;
}

static int session_close(session_t *s, int rc)
{
    int err = ue_close(s->e);

    file_io_close(&s->f);
    if (err) {
        fprintf(stderr, "unlatch-engine: close failed: %s\n", strerror(err));
        return EX_FAIL;
    }
    return rc;
}

static int walk_or_fail(session_t *s, const char *path, ue_attr_t *a)
{
    int err = ue_walk(s->e, path, a);

    if (err)
        fprintf(stderr, "unlatch-engine: %s: %s\n", path, strerror(err));
    return err;
}

static char type_char(uint32_t type)
{
    return type == UE_T_DIR ? 'd' : type == UE_T_LNK ? 'l' : '-';
}

typedef struct {
    uint64_t count;
    int depth;                       // -1: flat ls
    ue_engine_t *e;
    int err;
} list_ctx_t;

static bool list_entry(void *ctx, const char *name, const ue_attr_t *a, uint64_t next)
{
    list_ctx_t *l = ctx;

    (void)next;
    l->count++;
    if (l->depth < 0) {
        printf("%c %12" PRIu64 " %s\n", type_char(a->type), a->size, name);
        return true;
    }
    printf("%*s%s%s\n", l->depth * 2, "", name, a->type == UE_T_DIR ? "/" : "");
    if (a->type == UE_T_DIR) {
        ue_attr_t child;
        list_ctx_t sub = { 0, l->depth + 1, l->e, 0 };
        uint64_t v;
        // Children of a directory need it in the cache; look it up, list, release.
        if (ue_getattr(l->e, a->ino, &child) == 0) {
            int err = ue_readdir(l->e, a->ino, 0, list_entry, &sub, &v);
            if (err || sub.err)
                l->err = err ? err : sub.err;
            l->count += sub.count;
            ue_release(l->e, a->ino);
        }
    }
    return l->err == 0;
}

static int cmd_ls(const char *img, const char *path, bool stats, bool tree)
{
    session_t s;
    ue_attr_t a;
    ue_stats_t before, after;
    list_ctx_t l = { 0, tree ? 0 : -1, NULL, 0 };
    uint64_t verifier;
    int err;

    if (session_open(&s, img))
        return EX_FAIL;
    l.e = s.e;
    if (walk_or_fail(&s, path, &a))
        return session_close(&s, EX_FAIL);
    ue_get_stats(s.e, &before);
    err = ue_readdir(s.e, a.ino, 0, list_entry, &l, &verifier);
    ue_get_stats(s.e, &after);
    if (err || l.err) {
        fprintf(stderr, "unlatch-engine: readdir: %s\n", strerror(err ? err : l.err));
        return session_close(&s, EX_FAIL);
    }
    if (stats)
        fprintf(stderr, "entries: %" PRIu64 " inode_opens: %" PRIu64 "\n", l.count,
                after.inode_opens - before.inode_opens);
    return session_close(&s, 0);
}

static void print_time(const char *key, struct timespec ts)
{
    char buf[64];
    struct tm tm;
    time_t t = ts.tv_sec;

    gmtime_r(&t, &tm);
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    printf("%-11s %s.%07ldZ\n", key, buf, ts.tv_nsec / 100);
}

static int cmd_stat(const char *img, const char *path)
{
    static const struct { uint32_t bit; const char *name; } names[] = {
        { UE_F_HIDDEN, "hidden" }, { UE_F_SYSTEM, "system" }, { UE_F_READONLY, "readonly" },
        { UE_F_ARCHIVE, "archive" }, { UE_F_COMPRESSED, "compressed" },
        { UE_F_ENCRYPTED, "encrypted" }, { UE_F_SPARSE, "sparse" }, { UE_F_REPARSE, "reparse" },
        { UE_F_RESIDENT, "resident" },
    };
    session_t s;
    ue_attr_t a;

    if (session_open(&s, img))
        return EX_FAIL;
    if (walk_or_fail(&s, path, &a))
        return session_close(&s, EX_FAIL);
    printf("ino:        %" PRIu64 " (record %" PRIu64 ", seq %" PRIu64 ")\n", a.ino,
           a.ino & 0xFFFFFFFFFFFFull, a.ino >> 48);
    printf("type:       %s\n", a.type == UE_T_DIR ? "dir" : a.type == UE_T_LNK ? "symlink" : "file");
    printf("size:       %" PRIu64 "\n", a.size);
    printf("alloc_size: %" PRIu64 "\n", a.alloc_size);
    printf("init_size:  %" PRIu64 "\n", a.init_size);
    printf("nlink:      %" PRIu32 "\n", a.nlink);
    printf("mode:       %04o\n", a.mode);
    printf("flags:     ");
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (a.flags & names[i].bit)
            printf(" %s", names[i].name);
    printf("\n");
    print_time("birth:", a.birth);
    print_time("mtime:", a.mtime);
    print_time("ctime:", a.ctime);
    print_time("atime:", a.atime);
    return session_close(&s, 0);
}

static int cmd_cat(const char *img, const char *path)
{
    enum { CHUNK = 1 << 20 };
    session_t s;
    ue_attr_t a;
    uint64_t off = 0;
    char *buf;
    int rc = 0;

    if (session_open(&s, img))
        return EX_FAIL;
    if (walk_or_fail(&s, path, &a))
        return session_close(&s, EX_FAIL);
    if (!(buf = malloc(CHUNK)))
        return session_close(&s, EX_FAIL);
    for (;;) {
        size_t done;
        int err = ue_read(s.e, a.ino, off, buf, CHUNK, &done);
        if (err) {
            fprintf(stderr, "unlatch-engine: read at %" PRIu64 ": %s\n", off, strerror(err));
            rc = EX_FAIL;
            break;
        }
        if (done == 0)
            break;
        if (fwrite(buf, 1, done, stdout) != done) {
            rc = EX_FAIL;
            break;
        }
        off += done;
    }
    free(buf);
    if (fflush(stdout))
        rc = EX_FAIL;
    return session_close(&s, rc);
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
    if (argc >= 3 && strcmp(argv[1], "ls") == 0) {
        bool stats = strcmp(argv[2], "--stats") == 0;
        int i = stats ? 3 : 2;
        if (argc == i + 1 || argc == i + 2)
            return cmd_ls(argv[i], argc == i + 2 ? argv[i + 1] : "/", stats, false);
    }
    if ((argc == 3 || argc == 4) && strcmp(argv[1], "tree") == 0)
        return cmd_ls(argv[2], argc == 4 ? argv[3] : "/", false, true);
    if (argc == 4 && strcmp(argv[1], "stat") == 0)
        return cmd_stat(argv[2], argv[3]);
    if (argc == 4 && strcmp(argv[1], "cat") == 0)
        return cmd_cat(argv[2], argv[3]);
    usage(stderr);
    return EX_USAGE;
}
