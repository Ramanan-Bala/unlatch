// SPDX-License-Identifier: GPL-3.0-or-later
// mkfixture: builds engine test images with libntfs-3g directly (test tooling, not product
// code; excluded from the UnlatchEngine target). Paths have no spaces.
//
//   mkfixture <img> < script      one command per line:
//     mkdir PATH
//     file PATH SIZE SEED            SIZE bytes of gen.h stream SEED
//     sparse PATH SIZE OFF LEN SEED  extend to SIZE, then write LEN bytes (stream SEED) at OFF
//     uninit PATH SIZE INIT SEED     write INIT bytes, then extend data_size to SIZE
//     many DIR COUNT                 COUNT empty files f00000, f00001, ... in DIR
//     flags PATH hidden|system|readonly
//     logfile clean|unclean|faststartup   write two $LogFile restart pages (synthetic: v1.1
//                                         clean, v1.1 with an open client, or v2.0 as left
//                                         by Windows Fast Startup)
//     stream PATH NAME SIZE SEED     named $DATA stream (alternate data stream) on PATH
//     reparse PATH KIND TARGET       Windows reparse point on an existing PATH, as Windows
//                                    writes it: KIND symlink-rel (TARGET like ..\\x.txt),
//                                    symlink-abs or junction (TARGET like C:\\Users)
//     zero-record PATH               overwrite PATH's MFT record with zeros, leaving its index
//                                    entry behind (a damaged entry, as seen on a real disk)
//   mkfixture gen SIZE SEED [OFF] write SIZE bytes of the stream, from offset OFF, to stdout
#include "config.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <ntfs-3g/types.h>
#include <ntfs-3g/attrib.h>
#include <ntfs-3g/dir.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/logfile.h>
#include <ntfs-3g/reparse.h>
#include <ntfs-3g/unistr.h>
#include <ntfs-3g/mst.h>
#include <ntfs-3g/unistr.h>
#include <ntfs-3g/volume.h>

#include "gen.h"

#define CHUNK (1 << 20)

static unsigned char buf[CHUNK];

static int die(const char *what, const char *arg)
{
    fprintf(stderr, "mkfixture: %s %s: %s\n", what, arg, strerror(errno));
    return 1;
}

// Splits PATH into its parent (opened) and leaf name (UTF-16).
static ntfs_inode *open_parent(ntfs_volume *vol, const char *path, ntfschar **leaf, int *leaf_len)
{
    char parent[4096];
    const char *slash = strrchr(path, '/');
    ntfs_inode *dir;

    if (!slash || slash == path)
        strcpy(parent, "/");
    else
        snprintf(parent, sizeof(parent), "%.*s", (int)(slash - path), path);
    dir = ntfs_pathname_to_inode(vol, NULL, parent);
    if (!dir)
        return NULL;
    *leaf = NULL;
    *leaf_len = ntfs_mbstoucs(slash ? slash + 1 : path, leaf);
    if (*leaf_len <= 0) {
        ntfs_inode_close(dir);
        return NULL;
    }
    return dir;
}

static ntfs_inode *create(ntfs_volume *vol, const char *path, mode_t type)
{
    ntfschar *leaf;
    int len;
    ntfs_inode *dir = open_parent(vol, path, &leaf, &len), *ni;

    if (!dir)
        return NULL;
    ni = ntfs_create(dir, const_cpu_to_le32(0), leaf, (u8)len, type);
    free(leaf);
    if (ntfs_inode_close(dir) && ni) {
        ntfs_inode_close(ni);
        return NULL;
    }
    return ni;
}

static int write_stream(ntfs_attr *na, uint64_t off, uint64_t len, uint64_t seed)
{
    while (len > 0) {
        size_t n = len > CHUNK ? CHUNK : (size_t)len;
        gen_fill(seed, off, buf, n);
        if (ntfs_attr_pwrite(na, (s64)off, (s64)n, buf) != (s64)n)
            return -1;
        off += n;
        len -= n;
    }
    return 0;
}

static int cmd_file(ntfs_volume *vol, const char *path, uint64_t size, uint64_t seed,
                    uint64_t off, uint64_t len, uint64_t extend_to)
{
    ntfs_inode *ni = create(vol, path, S_IFREG);
    ntfs_attr *na;
    int rc = 0;

    if (!ni)
        return die("create", path);
    na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
    if (!na) {
        ntfs_inode_close(ni);
        return die("open $DATA", path);
    }
    if (size && ntfs_attr_truncate(na, (s64)size))
        rc = die("truncate", path);
    if (!rc && len && write_stream(na, off, len, seed))
        rc = die("write", path);
    if (!rc && extend_to && ntfs_attr_truncate(na, (s64)extend_to))
        rc = die("extend", path);
    ntfs_attr_close(na);
    if (ntfs_inode_close(ni) && !rc)
        rc = die("close", path);
    return rc;
}

static int cmd_flags(ntfs_volume *vol, const char *path, const char *flag)
{
    ntfs_inode *ni = ntfs_pathname_to_inode(vol, NULL, path);

    if (!ni)
        return die("open", path);
    if (!strcmp(flag, "hidden"))
        ni->flags |= FILE_ATTR_HIDDEN;
    else if (!strcmp(flag, "system"))
        ni->flags |= FILE_ATTR_SYSTEM;
    else if (!strcmp(flag, "readonly"))
        ni->flags |= FILE_ATTR_READONLY;
    else {
        ntfs_inode_close(ni);
        fprintf(stderr, "mkfixture: unknown flag %s\n", flag);
        return 1;
    }
    ntfs_inode_mark_dirty(ni);
    NInoFileNameSetDirty(ni);          // also update the copy in the parent's index
    return ntfs_inode_close(ni) ? die("close", path) : 0;
}

static int cmd_many(ntfs_volume *vol, const char *dirpath, unsigned count)
{
    ntfs_inode *dir = ntfs_pathname_to_inode(vol, NULL, dirpath);

    if (!dir)
        return die("open", dirpath);
    for (unsigned i = 0; i < count; i++) {
        char name[16];
        ntfschar *uname = NULL;
        int len;
        ntfs_inode *ni;

        snprintf(name, sizeof(name), "f%05u", i);
        len = ntfs_mbstoucs(name, &uname);
        ni = ntfs_create(dir, const_cpu_to_le32(0), uname, (u8)len, S_IFREG);
        free(uname);
        if (!ni || ntfs_inode_close(ni)) {
            ntfs_inode_close(dir);
            return die("create", name);
        }
    }
    return ntfs_inode_close(dir) ? die("close", dirpath) : 0;
}

static void put16(u8 *p, unsigned v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, uint32_t v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }
static void put64(u8 *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

// Writes two restart pages (offsets 0 and 4096) that pass libntfs-3g's ntfs_check_logfile:
// RSTR header with a 9-entry update sequence array, a restart area with one client.
static int cmd_logfile(ntfs_volume *vol, const char *mode)
{
    enum { PAGE = 4096, RA = 48, CA = 0x30 };
    bool fast = !strcmp(mode, "faststartup"), clean = !strcmp(mode, "clean");
    ntfs_inode *ni;
    ntfs_attr *na;
    int rc = 0;

    if (!fast && !clean && strcmp(mode, "unclean")) {
        fprintf(stderr, "mkfixture: logfile clean|unclean|faststartup\n");
        return 1;
    }
    if (!(ni = ntfs_inode_open(vol, FILE_LogFile)) || !(na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0)))
        return die("open", "$LogFile");
    uint64_t fs = (uint64_t)na->data_size;
    unsigned fs_bits = 0;
    for (uint64_t f = fs; f; f >>= 1)
        fs_bits++;
    for (int copy = 0; copy < 2 && !rc; copy++) {
        u8 page[PAGE];
        memset(page, 0, sizeof(page));
        memcpy(page, "RSTR", 4);
        put16(page + 4, 30);                       // usa_ofs: offsetof(usn)
        put16(page + 6, 1 + PAGE / 512);           // usa_count
        put32(page + 16, PAGE);                    // system_page_size
        put32(page + 20, PAGE);                    // log_page_size
        put16(page + 24, RA);                      // restart_area_offset
        put16(page + 26, fast ? 0 : 1);            // minor_ver
        put16(page + 28, fast ? 2 : 1);            // major_ver
        put16(page + 30, 1);                       // usn
        u8 *ra = page + RA;
        put64(ra + 0, 0x100000 + (uint64_t)copy);  // current_lsn (copy 1 newer)
        put16(ra + 8, 1);                          // log_clients
        put16(ra + 10, LOGFILE_NO_CLIENT_CPU);     // client_free_list
        put16(ra + 12, 0);                         // client_in_use_list: client 0
        put16(ra + 14, clean ? 0x0002 : 0);        // flags: RESTART_VOLUME_IS_CLEAN
        put32(ra + 16, 67 - fs_bits);              // seq_number_bits
        put16(ra + 20, CA + 160);                  // restart_area_length
        put16(ra + 22, CA);                        // client_array_offset
        put64(ra + 24, fs);                        // file_size
        put16(ra + 36, 0x30);                      // log_record_header_length
        put16(ra + 38, 0x40);                      // log_page_data_offset
        put32(ra + 40, 1);                         // restart_log_open_count
        u8 *cr = ra + CA;                          // LOG_CLIENT_RECORD
        put64(cr + 0, 0x100000);                   // oldest_lsn
        put64(cr + 8, 0x100000);                   // client_restart_lsn
        put16(cr + 16, LOGFILE_NO_CLIENT_CPU);     // prev_client
        put16(cr + 18, LOGFILE_NO_CLIENT_CPU);     // next_client
        put16(cr + 20, 1);                         // seq_number
        put32(cr + 28, 8);                         // client_name_length (bytes)
        put16(cr + 32, 'N'); put16(cr + 34, 'T'); put16(cr + 36, 'F'); put16(cr + 38, 'S');
        if (ntfs_mst_pre_write_fixup((NTFS_RECORD *)page, PAGE) ||
            ntfs_attr_pwrite(na, (s64)copy * PAGE, PAGE, page) != PAGE)
            rc = die("write", "$LogFile");
    }
    ntfs_attr_close(na);
    if (ntfs_inode_close(ni) && !rc)
        rc = die("close", "$LogFile");
    return rc;
}

static int cmd_stream(ntfs_volume *vol, const char *path, const char *name, uint64_t size, uint64_t seed)
{
    ntfs_inode *ni = ntfs_pathname_to_inode(vol, NULL, path);
    ntfschar *uname = NULL;
    ntfs_attr *na;
    int len, rc = 0;

    if (!ni)
        return die("open", path);
    len = ntfs_mbstoucs(name, &uname);
    if (len <= 0 || ntfs_attr_add(ni, AT_DATA, uname, (u8)len, NULL, 0) ||
        !(na = ntfs_attr_open(ni, AT_DATA, uname, (u32)len))) {
        free(uname);
        ntfs_inode_close(ni);
        return die("add stream", name);
    }
    if (write_stream(na, 0, size, seed))
        rc = die("write stream", name);
    ntfs_attr_close(na);
    free(uname);
    if (ntfs_inode_close(ni) && !rc)
        rc = die("close", path);
    return rc;
}

// IO_REPARSE_TAG_SYMLINK / MOUNT_POINT buffers per [MS-FSCC] 2.1.2.4 and 2.1.2.5: substitute
// name "\\??\\<target>" for absolute targets, the target itself for relative symlinks;
// print name = target.
static int cmd_reparse(ntfs_volume *vol, const char *path, const char *kind, const char *target)
{
    bool junction = !strcmp(kind, "junction"), rel = !strcmp(kind, "symlink-rel");
    char subst[4200];
    ntfschar *us = NULL, *up = NULL;
    int ls, lp, rc = 0;

    if (!junction && !rel && strcmp(kind, "symlink-abs"))
        return fprintf(stderr, "mkfixture: unknown reparse kind %s\n", kind), 1;
    snprintf(subst, sizeof(subst), "%s%s", rel ? "" : "\\??\\", target);
    if ((ls = ntfs_mbstoucs(subst, &us)) <= 0 || (lp = ntfs_mbstoucs(target, &up)) <= 0)
        return die("name", target);
    size_t hdr = junction ? 8 : 12, names = (size_t)(ls + lp) * 2;
    size_t dlen = hdr + names, total = 8 + dlen;
    unsigned char *buf = calloc(1, total), *d = buf + 8;
    if (!buf)
        return die("alloc", path);
    uint32_t tag = junction ? 0xA0000003u : 0xA000000Cu;
    memcpy(buf, &tag, 4);                           // little-endian host (arm64)
    uint16_t v = (uint16_t)dlen;
    memcpy(buf + 4, &v, 2);
    uint16_t f[4] = { 0, (uint16_t)(ls * 2), (uint16_t)(ls * 2), (uint16_t)(lp * 2) };
    memcpy(d, f, 8);                                // subst off/len, print off/len
    if (!junction) {
        uint32_t flags = rel ? 1 : 0;               // SYMLINK_FLAG_RELATIVE
        memcpy(d + 8, &flags, 4);
    }
    memcpy(d + hdr, us, (size_t)ls * 2);
    memcpy(d + hdr + (size_t)ls * 2, up, (size_t)lp * 2);
    ntfs_inode *ni = ntfs_pathname_to_inode(vol, NULL, path);
    if (!ni)
        rc = die("open", path);
    else {
        if (ntfs_set_ntfs_reparse_data(ni, (const char *)buf, total, 0))
            rc = die("reparse", path);
        if (ntfs_inode_close(ni) && !rc)
            rc = die("close", path);
    }
    free(buf);
    free(us);
    free(up);
    return rc;
}

static int cmd_zero_record(ntfs_volume *vol, const char *path)
{
    ntfs_inode *ni = ntfs_pathname_to_inode(vol, NULL, path);
    if (!ni)
        return die("open", path);
    u64 mft_no = ni->mft_no;
    if (ntfs_inode_close(ni))
        return die("close", path);
    char *zeros = calloc(1, vol->mft_record_size);
    if (!zeros)
        return die("alloc", path);
    s64 off = (s64)(mft_no << vol->mft_record_size_bits);
    int rc = ntfs_attr_pwrite(vol->mft_na, off, vol->mft_record_size, zeros) == vol->mft_record_size ? 0
                                                                                                 : die("zero", path);
    free(zeros);
    return rc;
}

static int run(ntfs_volume *vol, char *line)
{
    char cmd[16], path[4096], word[32];
    unsigned long long a, b, c, d;

    if (line[0] == '#' || line[0] == '\n' || line[0] == '\0')
        return 0;
    if (sscanf(line, "%15s %4095s", cmd, path) != 2)
        return fprintf(stderr, "mkfixture: bad line: %s", line), 1;
    if (!strcmp(cmd, "mkdir")) {
        ntfs_inode *ni = create(vol, path, S_IFDIR);
        return !ni || ntfs_inode_close(ni) ? die("mkdir", path) : 0;
    }
    if (!strcmp(cmd, "file") && sscanf(line, "%*s %*s %llu %llu", &a, &b) == 2)
        return cmd_file(vol, path, 0, b, 0, a, 0);
    if (!strcmp(cmd, "sparse") && sscanf(line, "%*s %*s %llu %llu %llu %llu", &a, &b, &c, &d) == 4)
        return cmd_file(vol, path, a, d, b, c, 0);
    if (!strcmp(cmd, "uninit") && sscanf(line, "%*s %*s %llu %llu %llu", &a, &b, &c) == 3)
        return cmd_file(vol, path, 0, c, 0, b, a);
    if (!strcmp(cmd, "many") && sscanf(line, "%*s %*s %llu", &a) == 1)
        return cmd_many(vol, path, (unsigned)a);
    if (!strcmp(cmd, "flags") && sscanf(line, "%*s %*s %31s", word) == 1)
        return cmd_flags(vol, path, word);
    if (!strcmp(cmd, "logfile"))
        return cmd_logfile(vol, path);
    if (!strcmp(cmd, "stream") && sscanf(line, "%*s %*s %31s %llu %llu", word, &a, &b) == 3)
        return cmd_stream(vol, path, word, a, b);
    if (!strcmp(cmd, "zero-record"))
        return cmd_zero_record(vol, path);
    char target[4096];
    if (!strcmp(cmd, "reparse") && sscanf(line, "%*s %*s %31s %4095s", word, target) == 2)
        return cmd_reparse(vol, path, word, target);
    fprintf(stderr, "mkfixture: bad line: %s", line);
    return 1;
}

int main(int argc, char **argv)
{
    char line[8192];
    ntfs_volume *vol;
    int rc = 0;

    if ((argc == 4 || argc == 5) && !strcmp(argv[1], "gen")) {
        uint64_t size = strtoull(argv[2], NULL, 10), seed = strtoull(argv[3], NULL, 10);
        uint64_t base = argc == 5 ? strtoull(argv[4], NULL, 10) : 0, off = 0;
        while (off < size) {
            size_t n = size - off > CHUNK ? CHUNK : (size_t)(size - off);
            gen_fill(seed, base + off, buf, n);
            if (fwrite(buf, 1, n, stdout) != n)
                return 1;
            off += n;
        }
        return fflush(stdout) ? 1 : 0;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: mkfixture <img> < script | mkfixture gen SIZE SEED\n");
        return 64;
    }
    vol = ntfs_mount(argv[1], 0);
    if (!vol)
        return die("mount", argv[1]);
    while (!rc && fgets(line, sizeof(line), stdin))
        rc = run(vol, line);
    if (ntfs_umount(vol, FALSE) && !rc)
        rc = die("umount", argv[1]);
    return rc;
}
