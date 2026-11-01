// SPDX-License-Identifier: GPL-3.0-or-later
// Engine/include/unlatch_engine.h — the only header code outside Engine/ may include.
// THREADING: not thread-safe. The host serializes ALL calls on one ue_engine_t,
//            and ue_probe/ue_open calls with each other (libntfs-3g has globals).
// ERRORS: functions return 0 or a POSIX errno value.
// NAMES: UTF-8, NFC, no path separators; the host normalizes before calling.
#ifndef UNLATCH_ENGINE_H
#define UNLATCH_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UE_ABI_VERSION 1
typedef struct ue_engine ue_engine_t;
typedef uint64_t ue_ino_t;            // NTFS file reference: (seq << 48) | mft_record
#define UE_ROOT_INO 5                 // FILE_root; the engine fills in seq

typedef struct {                      // host-supplied block I/O; must transfer len fully or return errno
    void *ctx;
    int (*pread)(void *ctx, void *buf, size_t len, uint64_t off);
    int (*pwrite)(void *ctx, const void *buf, size_t len, uint64_t off);  // may be NULL if !writable
    int (*flush)(void *ctx);                                              // may be NULL
    uint64_t size_bytes;
    uint32_t block_size;              // power of two, >= 512
    bool writable;
} ue_io_t;
// The engine copies the ue_io_t itself, but ctx must stay valid until ue_close
// (for ue_probe: until it returns).

typedef enum { UE_VOL_CLEAN, UE_VOL_DIRTY, UE_VOL_HIBERNATED, UE_VOL_UNCLEAN_LOG,
               UE_VOL_CORRUPT, UE_VOL_NOT_NTFS } ue_vol_state_t;
typedef struct {
    ue_vol_state_t state; char reason[256]; char label[256];  // label in UTF-8
    uint64_t serial; uint32_t cluster_size; uint64_t total_bytes, free_bytes;
    uint8_t major_ver, minor_ver;
} ue_probe_t;

enum { UE_T_REG = 1, UE_T_DIR, UE_T_LNK };
enum { UE_F_HIDDEN = 1, UE_F_SYSTEM = 2, UE_F_READONLY = 4, UE_F_ARCHIVE = 8,
       UE_F_COMPRESSED = 16, UE_F_ENCRYPTED = 32, UE_F_SPARSE = 64, UE_F_REPARSE = 128,
       UE_F_RESIDENT = 256 /* unnamed $DATA is resident: no KOIO */ };
typedef struct {
    ue_ino_t ino; uint32_t type, flags, nlink, mode;
    uint64_t size, alloc_size, init_size;
    struct timespec birth, mtime, ctime, atime;
} ue_attr_t;

enum { UE_OPEN_RDONLY = 1 };
enum { UE_SET_SIZE = 1, UE_SET_MTIME = 2, UE_SET_ATIME = 4, UE_SET_BIRTH = 8, UE_SET_CTIME = 16, UE_SET_FLAGS = 32, UE_SET_MODE = 64 };
enum { UE_RENAME_EXCL = 1 };

/// Engine version string, for example "0.0.0".
const char *ue_version(void);
/// Short lowercase name of a state: "clean", "dirty", "hibernated", "unclean_log", "corrupt", "not_ntfs".
const char *ue_vol_state_name(ue_vol_state_t state);

// Volume (Step 03)
// ue_probe mounts read-only and returns 0 with out->state set whenever it reached a verdict,
// including NOT_NTFS and CORRUPT. It returns an errno only for bad arguments or ENOMEM.
int  ue_probe(const ue_io_t *io, ue_probe_t *out);
// ue_open never falls back to read-only: a read-write open of a hibernated, unclean or
// read-only device fails (EPERM, EOPNOTSUPP, EROFS). probe_out may be NULL; if given, it gets
// label, sizes, versions and CLEAN or DIRTY (the journal is not re-checked).
int  ue_open(const ue_io_t *io, uint32_t flags, ue_engine_t **out, ue_probe_t *probe_out);
int  ue_close(ue_engine_t *e);                 // sync, mark clean, release; always frees e
bool ue_is_readonly(const ue_engine_t *e);

// Read side (Step 04)
int  ue_statfs(ue_engine_t *e, uint64_t *total, uint64_t *free_bytes, uint32_t *cluster);
int  ue_lookup(ue_engine_t *e, ue_ino_t dir, const char *name, ue_attr_t *out);
int  ue_getattr(ue_engine_t *e, ue_ino_t ino, ue_attr_t *out);
typedef bool (*ue_dirent_fn)(void *ctx, const char *name, const ue_attr_t *attr, uint64_t next_cookie);
int  ue_readdir(ue_engine_t *e, ue_ino_t dir, uint64_t cookie, ue_dirent_fn emit, void *ctx, uint64_t *verifier);
int  ue_read(ue_engine_t *e, ue_ino_t ino, uint64_t off, void *buf, size_t len, size_t *done);
void ue_release(ue_engine_t *e, ue_ino_t ino); // FSKit reclaimed the item; drop cache entry
// Lookup and readdir skip ".", "..", DOS-only 8.3 names and the root's metafiles ($MFT, ...).
// Lookup is case-insensitive (NTFS $UpCase); readdir returns names case-preserved.
// readdir: cookie 0 starts; each entry carries the cookie that resumes after it. emit
// returns false when it did not take the entry (packer full): readdir stops, and the host
// resumes with the next_cookie of the last entry it took. *verifier changes when the
// directory changes.
// ue_read returns zeros past init_size and stops at EOF (*done < len, return 0).

// Debug counters (tests, benchmarks).
typedef struct {
    uint64_t inode_opens;             // ntfs_inode_open calls made by the engine
    uint64_t cached_inodes;           // inodes currently held open for the host
} ue_stats_t;
void ue_get_stats(const ue_engine_t *e, ue_stats_t *out);

// Write side (Step 05)
int  ue_write(ue_engine_t *e, ue_ino_t ino, uint64_t off, const void *buf, size_t len, size_t *done);
int  ue_create(ue_engine_t *e, ue_ino_t dir, const char *name, uint32_t type, uint32_t mode, ue_attr_t *out);
int  ue_remove(ue_engine_t *e, ue_ino_t dir, const char *name, bool is_dir);
int  ue_rename(ue_engine_t *e, ue_ino_t odir, const char *oname, ue_ino_t ndir, const char *nname, uint32_t flags);
int  ue_setattr(ue_engine_t *e, ue_ino_t ino, uint32_t mask, const ue_attr_t *in, ue_attr_t *out);
int  ue_sync(ue_engine_t *e);
// later: ue_xattr_* / ue_readlink / ue_symlink / ue_link (Step 16), ue_blockmap / ue_complete_io (Step 15)

#ifdef __cplusplus
}
#endif

#endif // UNLATCH_ENGINE_H
