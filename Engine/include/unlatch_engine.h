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

enum { UE_OPEN_RDONLY = 1,
       UE_OPEN_CACHE = 2 /* write-back page cache in front of the device (Step 15, D-62) */,
       UE_OPEN_GHOST_OPEN_ONLY = 4 /* removed files are ghosts only while open (D-82) */,
       UE_OPEN_CLEAR_DIRTY = 8 /* the caller has checked the volume: a clean read-write close
                                  clears VOLUME_IS_DIRTY even if it was set at open (D-86) */ };
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
// True after a device write failed or the device disappeared (ENXIO/ENODEV). From then on
// every call returns EIO, nothing is retried, and ue_close writes nothing (it returns EIO).
bool ue_has_failed(const ue_engine_t *e);

// Read side (Step 04)
int  ue_statfs(ue_engine_t *e, uint64_t *total, uint64_t *free_bytes, uint32_t *cluster);
int  ue_lookup(ue_engine_t *e, ue_ino_t dir, const char *name, ue_attr_t *out);
int  ue_getattr(ue_engine_t *e, ue_ino_t ino, ue_attr_t *out);
typedef bool (*ue_dirent_fn)(void *ctx, const char *name, const ue_attr_t *attr, uint64_t next_cookie);
int  ue_readdir(ue_engine_t *e, ue_ino_t dir, uint64_t cookie, ue_dirent_fn emit, void *ctx, uint64_t *verifier);
int  ue_read(ue_engine_t *e, ue_ino_t ino, uint64_t off, void *buf, size_t len, size_t *done);
void ue_release(ue_engine_t *e, ue_ino_t ino); // FSKit reclaimed the item (deferred delete happens here)
// Open handles, with UE_OPEN_GHOST_OPEN_ONLY (D-82). A host whose kernel holds looked-up items
// long after their last use (macOS FUSE sends FORGET late or never) counts opens instead: a
// file removed while it has no open handle is deleted at once, even if held (later calls on
// it return ESTALE), and one removed while open is deleted when the last handle closes.
// Without the flag a held file is a ghost until ue_release (D-25), and these do nothing.
int  ue_file_opened(ue_engine_t *e, ue_ino_t ino);    // ESTALE if ino is not held
void ue_file_closed(ue_engine_t *e, ue_ino_t ino);
// Lookup and readdir skip ".", "..", DOS-only 8.3 names and the root's metafiles ($MFT, ...).
// Lookup is case-insensitive (NTFS $UpCase); readdir returns names case-preserved.
// readdir: cookie 0 starts; each entry carries the cookie that resumes after it. emit
// returns false when it did not take the entry (packer full): readdir stops, and the host
// resumes with the next_cookie of the last entry it took. *verifier changes when the
// directory changes.
// ue_read returns zeros past init_size and stops at EOF (*done < len, return 0).

// Log sink for libntfs-3g's own messages (errors, warnings, info; debug is compiled out of
// the release build). Global, like libntfs-3g's logging: set it before ue_probe/ue_open.
// NULL (the default) keeps the engine silent. msg has no trailing newline.
enum { UE_LOG_ERROR, UE_LOG_WARNING, UE_LOG_INFO, UE_LOG_DEBUG };
typedef void (*ue_log_fn)(void *ctx, int level, const char *msg);
void ue_set_log(ue_log_fn fn, void *ctx);

// Debug counters (tests, benchmarks).
typedef struct {
    uint64_t inode_opens;             // ntfs_inode_open calls made by the engine
    uint64_t held_inodes;             // items the host holds (lookup/create until ue_release)
    uint64_t unmarked_writes;         // device writes while the dirty mark was off: a bug (D-95)
} ue_stats_t;
void ue_get_stats(const ue_engine_t *e, ue_stats_t *out);

// Write side (Step 05)
int  ue_write(ue_engine_t *e, ue_ino_t ino, uint64_t off, const void *buf, size_t len, size_t *done);
int  ue_create(ue_engine_t *e, ue_ino_t dir, const char *name, uint32_t type, uint32_t mode, ue_attr_t *out);
int  ue_remove(ue_engine_t *e, ue_ino_t dir, const char *name, bool is_dir);
int  ue_rename(ue_engine_t *e, ue_ino_t odir, const char *oname, ue_ino_t ndir, const char *nname, uint32_t flags);
int  ue_setattr(ue_engine_t *e, ue_ino_t ino, uint32_t mask, const ue_attr_t *in, ue_attr_t *out);
int  ue_sync(ue_engine_t *e);

// Extended attributes (Step 16, D-48): each one is a named $DATA stream of the same name.
// list: NUL-terminated UTF-8 names into buf; *needed is the full size (ERANGE if size is too
// small; buf NULL asks for the size). get: buf NULL returns the value size in *len; pos is
// for resource forks, 0 otherwise. set: pos 0 replaces the value. ENOATTR when absent.
#define UE_XATTR_MAX (64u << 20)
enum { UE_XATTR_CREATE = 1, UE_XATTR_REPLACE = 2 };
int  ue_xattr_list(ue_engine_t *e, ue_ino_t ino, char *buf, size_t size, size_t *needed);
int  ue_xattr_get(ue_engine_t *e, ue_ino_t ino, const char *name, uint64_t pos, void *buf, size_t size, size_t *len);
int  ue_xattr_set(ue_engine_t *e, ue_ino_t ino, const char *name, uint64_t pos, const void *buf, size_t size,
                  uint32_t flags);
int  ue_xattr_remove(ue_engine_t *e, ue_ino_t ino, const char *name);

// Links (Step 16, D-49). readlink reads native symlinks, junctions, WSL and Interix links;
// absolute Windows targets are shown under mount_point. buf gets a NUL-terminated UTF-8
// target (ENAMETOOLONG if it does not fit; EINVAL if ino is not a link). symlink creates a
// WSL symlink. link adds a hard link (EPERM for directories). Both hold the new item.
int  ue_readlink(ue_engine_t *e, ue_ino_t ino, const char *mount_point, char *buf, size_t size);
int  ue_symlink(ue_engine_t *e, ue_ino_t dir, const char *name, const char *target, ue_attr_t *out);
int  ue_link(ue_engine_t *e, ue_ino_t ino, ue_ino_t newdir, const char *newname, ue_attr_t *out);
// Page cache (Step 15, D-62), with UE_OPEN_CACHE. libntfs-3g's small metadata transfers
// (MFT records, index blocks, bitmaps) are kept in 4 KiB pages and written back sorted and
// coalesced; transfers of 64 KiB and more go straight to the device. Dirty pages reach the
// device on ue_writeback, ue_sync (which then flushes the device), ue_close, or when the
// cache is full. A failed write-back is a device failure (ue_has_failed).
// ue_writeback writes dirty pages back if the oldest has been dirty for at least min_age_ms
// (0: now); it does not flush the device's own cache.
typedef struct {
    uint64_t hits, misses, evictions;
    uint64_t writebacks;              // write-back transfers (each may cover many pages)
    uint64_t direct_reads, direct_writes;
    uint64_t read_bytes, write_bytes; // device traffic through the cache layer
    uint64_t dirty_pages;
    uint64_t writeback_us;            // time spent writing back
    uint32_t page_size;               // 0: no cache
} ue_cache_stats_t;
int  ue_writeback(ue_engine_t *e, uint32_t min_age_ms);
// The dirty mark (D-86, D-95): a read-write engine puts VOLUME_IS_DIRTY on the disk before
// its first change and keeps it while changes go on. ue_settle, once no change has come for
// idle_ms, writes everything back, flushes, and clears the mark, so a disk unplugged while
// idle is clean; the next change sets it again. EAGAIN when not idle long enough; 0 when it
// cleared the mark or there was nothing to do (read-only, already clear, or a volume that
// was dirty at open, which keeps its mark unless opened with UE_OPEN_CLEAR_DIRTY).
int  ue_settle(ue_engine_t *e, uint32_t idle_ms);
bool ue_is_marked_dirty(const ue_engine_t *e);  // VOLUME_IS_DIRTY is on the disk now
void ue_get_cache_stats(const ue_engine_t *e, ue_cache_stats_t *out);

// Check Disk (D-93). ue_check opens the volume itself, read-only, and checks that every MFT
// record in use is marked in $MFT's bitmap, every cluster a file's runlist uses is marked in
// $Bitmap and belongs to that file only, every directory index can be read and points at
// records in use with matching sequence numbers, and every named file is in some directory.
// Each problem goes to fn (may be NULL) as it is found. The problem's path names the file (or
// the directory and entry) when one can be found, "" otherwise.
//
// UE_CHECK_REPAIR (needs io->writable) does the safe repairs: it sets the bitmap bits of
// records and clusters in use, so at worst space is lost and nothing is overwritten, and it
// removes folder entries that lead to no file (their record is free, has another sequence
// number, or has no name in that folder; the record is not touched, D-96). It runs only when
// every problem found is of those kinds; anything else is reported and nothing is written. Then the volume is checked again, and its dirty mark (D-86) is cleared only when
// that check finds nothing. A volume with nothing found that is marked dirty is only unmarked.
// Hibernated and unclean-journal volumes hold Windows' unfinished changes: REPAIR returns
// EPERM for them. Returns 0 when the check ran (problems or not), or an errno: EINVAL for a
// volume that is not NTFS, EIO for one too damaged to open (other than UE_CHK_MFT_MIRROR,
// which is reported with state CORRUPT), EROFS, EPERM.
enum { UE_CHECK_REPAIR = 1 };
typedef enum {
    UE_CHK_RECORD_UNMARKED = 1,       // a record in use is free in $MFT's bitmap (repairable)
    UE_CHK_CLUSTERS_UNMARKED,         // clusters a file uses are free in $Bitmap (repairable)
    UE_CHK_CLUSTERS_SHARED,           // clusters claimed by two files (other: the first owner)
    UE_CHK_BAD_RUNLIST,               // a runlist that cannot be read or runs past the volume
    UE_CHK_BAD_RECORD,                // a record in use that cannot be read (torn or damaged)
    UE_CHK_INDEX_UNREADABLE,          // a directory whose index cannot be read
    UE_CHK_ENTRY_FREE_RECORD,         // a directory entry points at a record not in use (repairable
                                      // unless past the end of the MFT or at an extension record)
    UE_CHK_ENTRY_WRONG_SEQ,           // a directory entry's sequence number does not match (repairable)
    UE_CHK_ORPHAN,                    // a named file that no directory lists
    UE_CHK_MFT_MIRROR,                // $MFTMirr does not match $MFT: libntfs-3g cannot open
                                      // the volume at all, so this is the only problem reported
    UE_CHK_ENTRY_WRONG_PARENT,        // a directory entry points at a record with no name in that
                                      // directory (repairable)
    UE_CHK_KINDS
} ue_check_kind_t;
typedef struct {
    ue_check_kind_t kind;
    bool repairable;
    uint64_t rec;                     // the record concerned (the directory, for entries)
    uint64_t other;                   // the record an entry points at; the first owner of shared clusters
    uint64_t lcn, count;              // first cluster and number of clusters, for cluster kinds
    char path[1024];                  // UTF-8, "" when none could be found
    char detail[256];                 // one line, for people
} ue_check_problem_t;
typedef void (*ue_check_fn)(void *ctx, const ue_check_problem_t *p);
typedef struct {
    ue_vol_state_t state;             // the volume's state before the check
    uint64_t records, records_in_use, directories, entries;
    uint64_t clusters, clusters_in_use;
    uint64_t lost_records, lost_clusters;   // marked in a bitmap, used by nothing: space only
    uint64_t problems, repairable;    // what the check found (by_kind counts each kind)
    uint64_t by_kind[UE_CHK_KINDS];
    uint64_t repaired;                // bitmap bits set by the repair
    uint64_t entries_removed;         // folder entries the repair removed (a DOS+Win32 pair counts once)
    uint64_t remaining;               // problems the last check found (after any repair)
    bool repair_ran;
    bool dirty_cleared;               // the volume was marked dirty and is not any more
} ue_check_result_t;
int  ue_check(const ue_io_t *io, uint32_t flags, ue_check_fn fn, void *ctx, ue_check_result_t *out);
/// Short lowercase name of a kind: "record_unmarked", "clusters_shared", ...
const char *ue_check_kind_name(ue_check_kind_t kind);

#ifdef __cplusplus
}
#endif

#endif // UNLATCH_ENGINE_H
