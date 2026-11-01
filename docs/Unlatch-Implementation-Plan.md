# Unlatch: Implementation Plan

Unlatch mounts NTFS volumes read-write on macOS 27+ (Apple Silicon). It is an FSKit extension with libntfs-3g running in-process, plus a menu-bar app and a CLI called `unlatch`. The license is GPL-3.0-or-later.

> **Plan change, 2026-11-01 (D-39).** The owner has no paid Apple Developer membership, and a free Personal Team cannot sign an FSKit module (D-38). Unlatch therefore mounts through **macFUSE's FSKit backend** (`-o backend=fskit`): macFUSE's own signed FSKit extension talks to a small Unlatch daemon, `unlatch-fuse`, which runs the same engine over libfuse's low-level API. Steps 01–07 are unchanged. Steps 08–11 below are rewritten for it. Steps 12–20 still mention FSKit, entitlements and KOIO; each of those steps maps them to the daemon first (see "Re-scoping later steps" at the end of Phase 2). The Swift FSKit module in `Extension/` stays in the tree, dormant, in case a paid team becomes available.

Background: `docs/Unlatch-Review.md` explains why the plan differs from the original guide. The original guide is `docs/NTFS for Mac Complete Build Guide.md`, which is now reference only.

---

## How to use this plan

Each **Step** runs as one fresh session. Start every session with this prompt:

> Implement Step NN of `docs/Unlatch-Implementation-Plan.md`. First read `AGENTS.md`, `docs/PROGRESS.md`, `docs/DECISIONS.md`, and the files listed under "Read first" for the step. Do only this step. Finish by meeting its "Done when" criteria, then update `docs/PROGRESS.md` and, if anything was decided, `docs/DECISIONS.md`.

Rules for every session:

1. **Scope.** Do only the step you were given. If you find a problem that belongs to a later step, write it under "Carry-overs" in `PROGRESS.md` and leave it unfixed.
2. **Handoff.** Before the session ends, add an entry to `docs/PROGRESS.md` with: what was done, the exact commands that verify it, what is left, and any surprises.
3. **Decisions.** Any choice another step depends on goes in `docs/DECISIONS.md`, one line each: `D-NN: decision — reason — step`.
4. **Real data.** Never use a drive that holds real data. Use image files and disposable disks only.
5. **Commits.** One commit per step at least, with the message `step NN: <summary>`.
6. **Apple APIs.** Before writing any FSKit, DiskArbitration or ServiceManagement code, check the current signatures in Apple's docs (`https://developer.apple.com/documentation/fskit/<symbol>`; adding `.md` gives text). The signatures in this plan show the shape of the code and may not match the SDK exactly.

### Fixed facts

| Item | Value |
|---|---|
| Product / CLI / FS short name | `Unlatch` / `unlatch` / `unlatch` |
| License / distribution | GPL-3.0-or-later. Public GitHub repo, free Developer ID DMG on GitHub Releases. Contributions under DCO, no CLA. |
| Bundle prefix | `BUNDLE_PREFIX` in `project.yml`, set in Step 01 (for example `io.github.<you>`) |
| App / extension IDs | `$(BUNDLE_PREFIX).Unlatch`, `$(BUNDLE_PREFIX).Unlatch.FSModule` |
| App Group | `group.$(BUNDLE_PREFIX).Unlatch` (status records shared by the extension, app and CLI) |
| Platform | macOS 27+, arm64 only |
| Engine | libntfs-3g at a pinned tag, in-process, behind `Engine/include/unlatch_engine.h` (prefix `ue_`) |
| Entitlements | extension: `com.apple.developer.fskit.fsmodule`; mounting caller: `com.apple.developer.fskit.mount`; app and extension: App Group |
| Logging | `OSLog` subsystem `$(BUNDLE_PREFIX).unlatch`, categories `engine`, `io`, `mount`, `app`, `cli` |
| Safety rule | When in doubt, mount read-only. Never pass `NTFS_MNT_RECOVER` or `NTFS_MNT_IGNORE_HIBERFILE` unless the user explicitly confirms. |

### Target layout

```
Unlatch/
├── AGENTS.md  LICENSE  README.md  project.yml (XcodeGen)
├── docs/            PROGRESS.md  DECISIONS.md  plan, review, guides
├── Engine/          C: include/unlatch_engine.h, src/*.c, vendor/ntfs-3g (pinned)
├── EngineTool/      C: unlatch-engine host harness (works on image files)
├── Extension/       Swift FSKit module (links Engine)
├── Shared/          Swift package UnlatchCore: models, MountService, DiskWatcher, StatusStore
├── CLI/             Swift `unlatch` (ArgumentParser, depends on UnlatchCore)
├── App/             SwiftUI menu-bar app, embeds Extension + CLI
├── Tests/           Engine C tests, Images/ (golden, Git LFS), Swift tests
├── Bench/           scripts, results/
└── Scripts/         build-ntfs3g.sh, make-image.sh, sign.sh, notarize.sh, release.sh
```

### Step map

| Phase | Steps | Exit gate |
|---|---|---|
| 0 Foundations | 01–02 | Repo builds, libntfs-3g and its tools build |
| 1 Engine on host | 03–07 | Engine read-write on images passes the model test and fuzz smoke; CI green |
| 2 Mounting | 08–11 | **M0 GO/NO-GO**: read-write mount through macFUSE works and meets the speed gate |
| 3 Mount management | 12–14 | Detection, auto-mount, CLI and app all work |
| 4 Performance and depth | 15–17 | KOIO is fast, xattrs and links work, robustness suite passes |
| 5 Ship | 18–20 | Notarized DMG, updates, beta, release |

Dependencies: each step depends on the step before it, except Step 07, which can run any time after Step 04, and Step 16, which can run any time after Step 10.

---

## Phase 0: Foundations

### Step 01: Repository scaffold
**Read first:** this plan and `docs/Unlatch-Review.md`.
**Goal:** an empty project that builds, with the conventions every later session relies on.

Tasks:
1. `git init`. Add `.gitignore` (Xcode, `build/`, `*.img`, `Bench/results/raw/`). Set up Git LFS for `Tests/Images/*.img.zst`.
2. Add `LICENSE` with the full GPL-3.0 text and a `README.md` covering what the tool does, its status, and that it is GPL-3.0-or-later.
3. Write `AGENTS.md`:
   - the session rules above
   - the build and test commands, updated by each step
   - code conventions: Swift 6 strict concurrency; C11 with `-Wall -Wextra -Werror` in `Engine/`; no ntfs-3g headers outside `Engine/src`
   - the logging subsystem
4. Create `docs/PROGRESS.md` with sections Status, Steps (log), and Carry-overs. Create `docs/DECISIONS.md` and seed it with D-01 license, D-02 macOS 27/arm64, D-03 naming, and D-04 the bundle prefix.
5. Install XcodeGen (`brew install xcodegen`). Write `project.yml` with these targets:
   - `Unlatch` (app; `LSUIElement`)
   - `UnlatchFSModule` (FSKit extension placeholder)
   - `UnlatchEngine` (static C library, empty `ue_version()` for now)
   - `unlatch-engine` (command-line tool)

   Then create `Shared/` as a Swift package (`UnlatchCore` library plus tests) and `CLI/` as a Swift package executable that depends on `UnlatchCore` and `swift-argument-parser`.
6. Write `Scripts/bootstrap.sh`, which checks Xcode, XcodeGen and Git LFS, then runs `xcodegen generate`.
7. Add `CONTRIBUTING.md` (DCO sign-off, `git commit -s`, small PRs, one step per PR) and a `DCO` text file. Create the **public** GitHub repo `unlatch` and push, so the source obligation starts satisfied from the first commit.

**Done when:** these all succeed:
- `Scripts/bootstrap.sh`
- `xcodebuild -scheme Unlatch build`
- `swift build --package-path Shared`
- `swift build --package-path CLI`
- `swift run --package-path CLI unlatch --help` prints help

PROGRESS.md and DECISIONS.md exist.
**Out of scope:** any real functionality and signing (local ad-hoc signing is enough).

### Step 02: Vendor and build libntfs-3g plus ntfsprogs
**Read first:** `AGENTS.md`, `PROGRESS.md`.
**Goal:** a reproducible, pinned arm64 static `libntfs-3g.a` and the ntfsprogs tools, built with no FUSE.

Tasks:
1. Pin the latest stable tag of `github.com/tuxera/ntfs-3g`; check it, since 2022.10.3 was current when this was written. Vendor it as a git subtree or tarball in `Engine/vendor/ntfs-3g/`, and record the tag and SHA-256 in `Engine/vendor/VERSION`.
2. Write `Scripts/build-ntfs3g.sh`. It runs autoreconf if needed, then configures for arm64 macOS 27 with no FUSE driver and no mount helper, ntfsprogs on, and crypto off. Check the configure flags in that release (`./configure --help`); expect options like `--disable-ntfs-3g`, `--disable-mount-helper`, `--enable-ntfsprogs`, `--disable-crypto`. Install into `build/ntfs3g/{lib,include,bin}`. Apply any macOS build fixes as patch files in `Engine/vendor/patches/`.
3. Write `Scripts/make-image.sh <name> <size> [label]`. It runs `mkfile -n`, then `build/ntfs3g/bin/mkntfs -F -Q -L`, and prints the path.
4. Write `Scripts/attach-image.sh`, which runs `hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount` and prints the `diskN`.
5. Wire `UnlatchEngine` in `project.yml` to link `build/ntfs3g/lib/libntfs-3g.a`, with the header search path private to the Engine target.

**Done when:**
- `Scripts/build-ntfs3g.sh` runs from a clean checkout.
- `lipo -info` on the library reports `arm64`.
- `make-image.sh t1 512m T1` gives an image that `build/ntfs3g/bin/ntfsinfo -m t1.img` reads.
- `ntfsls` on the image lists the root.
- The license files of the vendored source are present.

**Out of scope:** engine code.

---

## Phase 1: Engine on host

### The seam (written in Step 03, then extended)

```c
// Engine/include/unlatch_engine.h — the only header code outside Engine/ may include.
// THREADING: not thread-safe. The host serializes ALL calls on one ue_engine_t.
// ERRORS: functions return 0 or a POSIX errno value.
// NAMES: UTF-8, NFC, no path separators; the host normalizes before calling.
#define UE_ABI_VERSION 1
typedef struct ue_engine ue_engine_t;
typedef uint64_t ue_ino_t;            // NTFS file reference: (seq << 48) | mft_record
#define UE_ROOT_INO 5                 // FILE_root; the engine fills in seq

typedef struct {                      // host-supplied block I/O; must transfer len fully or return errno
    void *ctx;
    int (*pread)(void *ctx, void *buf, size_t len, uint64_t off);
    int (*pwrite)(void *ctx, const void *buf, size_t len, uint64_t off);
    int (*flush)(void *ctx);
    uint64_t size_bytes;
    uint32_t block_size;
    bool writable;
} ue_io_t;

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

int  ue_probe(const ue_io_t *io, ue_probe_t *out);
int  ue_open(const ue_io_t *io, uint32_t flags, ue_engine_t **out, ue_probe_t *probe_out);
int  ue_close(ue_engine_t *e);                 // sync, mark clean, release; always frees e
bool ue_is_readonly(const ue_engine_t *e);
int  ue_statfs(ue_engine_t *e, uint64_t *total, uint64_t *free_bytes, uint32_t *cluster);

int  ue_lookup(ue_engine_t *e, ue_ino_t dir, const char *name, ue_attr_t *out);
int  ue_getattr(ue_engine_t *e, ue_ino_t ino, ue_attr_t *out);
typedef bool (*ue_dirent_fn)(void *ctx, const char *name, const ue_attr_t *attr, uint64_t next_cookie);
int  ue_readdir(ue_engine_t *e, ue_ino_t dir, uint64_t cookie, ue_dirent_fn emit, void *ctx, uint64_t *verifier);
int  ue_read(ue_engine_t *e, ue_ino_t ino, uint64_t off, void *buf, size_t len, size_t *done);
void ue_release(ue_engine_t *e, ue_ino_t ino); // FSKit reclaimed the item; drop cache entry

// write side (Step 05)
int  ue_write(ue_engine_t *e, ue_ino_t ino, uint64_t off, const void *buf, size_t len, size_t *done);
int  ue_create(ue_engine_t *e, ue_ino_t dir, const char *name, uint32_t type, uint32_t mode, ue_attr_t *out);
int  ue_remove(ue_engine_t *e, ue_ino_t dir, const char *name, bool is_dir);
int  ue_rename(ue_engine_t *e, ue_ino_t odir, const char *oname, ue_ino_t ndir, const char *nname, uint32_t flags);
int  ue_setattr(ue_engine_t *e, ue_ino_t ino, uint32_t mask, const ue_attr_t *in, ue_attr_t *out);
int  ue_sync(ue_engine_t *e);
// later: ue_xattr_* / ue_readlink / ue_symlink / ue_link (Step 16), ue_blockmap / ue_complete_io (Step 15)
```

### Step 03: Engine core (device bridge, probe, open/close) and host harness
**Read first:** the seam above; `Engine/vendor/ntfs-3g/include/ntfs-3g/{device.h,volume.h,logfile.h}`; `libntfs-3g/volume.c` (`ntfs_device_mount`, `ntfs_volume_startup`, `ntfs_volume_check_hiberfile`, `ntfs_mount`).
**Goal:** the engine opens and probes NTFS images through host callbacks only.

Tasks:
1. Write `Engine/include/unlatch_engine.h` as above, with the Step 03 functions plus declarations for the read and write sides.
2. `Engine/src/device.c`: an `ntfs_device_operations` table (`open`, `close`, `seek`, `read`, `write`, `pread`, `pwrite`, `sync`, `stat`, `ioctl`) backed by `ue_io_t`.
   - Keep a seek position for `read` and `write`.
   - `stat` reports a block device of `size_bytes`.
   - `ioctl` answers the block-size and size queries libntfs-3g makes (check which ones in `unix_io.c`).
   - `open` with `O_RDWR` on a device that isn't writable returns `EROFS`.
   - Allocate with `ntfs_device_alloc("unlatch", 0, &ops, ctx)`.
3. `Engine/src/volume.c`:
   - `ue_open` calls `ntfs_device_mount(dev, flags)`, then `ntfs_create_lru_caches(vol)` (which `ntfs_mount` does itself), then `ntfs_set_ignore_case(vol)` and `ntfs_set_shown_files(vol, false, true, false)`. Check the names against the pinned headers.
   - Flags: `NTFS_MNT_RDONLY` when asked. Otherwise use read-write **without** `NTFS_MNT_MAY_RDONLY`, so the probe stays the single place that decides.
   - `ue_probe` mounts read-only and reads `vol->flags & VOLUME_IS_DIRTY`. It runs the hiberfile check and the logfile check (`ntfs_check_logfile`/`ntfs_is_logfile_clean`, or whatever the pinned release exports), then unmounts. It maps the results to `ue_vol_state_t` with a readable `reason`.
   - An `EINVAL` from the mount means not NTFS; `EIO` means corrupt.
4. `ue_close` calls `ntfs_umount(vol, FALSE)` and frees the context.
5. `EngineTool/main.c`, named `unlatch-engine`, with subcommands `probe <img>` and `info <img>`. A file-backed `ue_io_t` uses `pread`/`pwrite`/`fsync`.
6. Tests in `Tests/Engine/` (C, run by `Scripts/test-engine.sh`):
   - probe a fresh image → `CLEAN` with the right label
   - flip the dirty flag with `ntfsfix`/`ntfsinfo` or a direct byte patch → `DIRTY`
   - zeroed boot sector → `NOT_NTFS`
   - a read-only `ue_io_t` with a read-write open → `EROFS`
   - repeat open and close 100 times with no leaks (`leaks --atExit`)

**Done when:** `Scripts/test-engine.sh` passes, `unlatch-engine probe` prints the state, label and sizes, and no `ntfs-3g/*.h` include appears outside `Engine/src`. Record in DECISIONS.md whether the custom-device path needed any workaround (Open question 3).
**Out of scope:** file operations. A Fast Startup image comes in Step 07.

### Step 04: Engine read path
**Read first:** the seam, `Engine/src/*`, and libntfs-3g `dir.h`, `inode.h`, `attrib.h`, `unistr.h`.
**Goal:** correct, efficient read-only access.

Tasks:
1. Inode cache: a hash map from `ue_ino_t` to `ntfs_inode*`, opened on first use (`ntfs_inode_open(vol, MREF(ino))` plus a sequence check: a mismatch returns `ESTALE`). `ue_release` closes the entry, and `ue_close` closes everything.
2. `ue_lookup`: UTF-8 to UTF-16 (`ntfs_mbstoucs`, or a local converter if locale-dependent; record which in DECISIONS), then `ntfs_inode_lookup_by_name` with case-insensitive matching, then fill `ue_attr_t`.
3. `ue_getattr`, taken from `$STANDARD_INFORMATION` plus the unnamed `$DATA`:
   - times are NTFS 100 ns since 1601, converted to `timespec`
   - DOS attributes map to `UE_F_*`
   - `nlink` counts the `$FILE_NAME` attributes, excluding DOS 8.3 names
   - `UE_F_RESIDENT` for resident data
   - set `init_size`
4. `ue_readdir` wraps `ntfs_readdir` with a filldir callback:
   - skip `.` and `..` (FSKit synthesizes them; check)
   - skip DOS-only 8.3 names
   - skip metafiles (record < 24 in the root)
   - attributes are filled for each entry
   - the cookie is libntfs-3g's position, so make sure resuming at it is stable
   - the verifier is the directory's change time or a sequence counter
5. `ue_read`: `ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0)` and `ntfs_attr_pread`. Return zeros past `init_size`. Compressed, sparse and resident files all go through the same call.
6. `ue_statfs`: `ntfs_volume_get_free_space`, cluster size.
7. Add harness commands `ls <img> <path>`, `stat`, `cat` (host-side path walking on top of `ue_lookup`) and `tree`.
8. Tests: seed images with `ntfs-3g` tools or with a Linux/Windows-made image; at minimum use `ntfscp`/`mkntfs` plus a script. Cover:
   - nested dirs, 10k-entry dir (paging resumes correctly)
   - a Unicode NFC name; a mixed-case lookup
   - a multi-GB sparse file read (checksum)
   - a file with `init_size < size`

**Done when:** the tests pass, `unlatch-engine cat` of a 4 GB file matches `shasum`, and listing a 10k directory reads each MFT record once (counted with a debug counter).
**Out of scope:** writes, xattrs, symlinks (they show up as regular files or dirs with `UE_F_REPARSE` for now).

### Step 05: Engine write path
**Read first:** the seam, `Engine/src/*`, libntfs-3g `dir.h` (`ntfs_create`, `ntfs_delete`, `ntfs_link`), `attrib.h` (`ntfs_attr_pwrite`, `ntfs_attr_truncate`).
**Goal:** safe read-write operation.

Tasks:
1. `ue_write`: `ntfs_attr_pwrite`, extending `size` as needed; updates mtime and ctime. Writing to an encrypted file returns `EACCES`.
2. `ue_create`:
   - validate the name (forbidden Win32 chars `\ / : * ? " < > |`, control chars, trailing dot or space, length ≤ 255 UTF-16 units, reserved `CON`/`PRN`/`AUX`/`NUL`/`COM1-9`/`LPT1-9`; failures return `EINVAL` or `ENAMETOOLONG`)
   - `ntfs_create(dir_ni, 0, uname, len, S_IFREG|S_IFDIR)`
   - an existing name returns `EEXIST`
3. `ue_remove`: check the type (`ENOTDIR`/`EISDIR`) and whether a dir is empty (`ENOTEMPTY`), then `ntfs_delete`. Drop the cache entry. If the inode is still open in FSKit, defer the real delete until `ue_release`. Follow how ntfs-3g's driver handles unlink of open files and document it.
4. `ue_rename`, same dir or across dirs:
   - when the target exists and `UE_RENAME_EXCL` is not set, remove the target (type-compatible only)
   - then link the new name and delete the old one, the way ntfs-3g's `ntfs_fuse_rename` does
   - moving a dir into its own subtree returns `EINVAL`
   - a case-only rename (`a` → `A`) works
5. `ue_setattr`: size (`ntfs_attr_truncate`), times, DOS flags. Mode is mostly ignored: a missing write bit maps to `UE_F_READONLY`.
6. `ue_sync`: `ntfs_inode_sync` on dirty cached inodes, then `ntfs_device_sync`/`flush`.
7. The read-only engine returns `EROFS` from every mutating call.
8. Add harness commands `put`, `get`, `mkdir`, `mv`, `rm`, `truncate`, `touch`.
9. Tests:
   - round trip of create, write, close, reopen, read
   - rename matrix
   - fill the disk → `ENOSPC`, and the volume is still clean afterwards
   - after each test, `ue_close` then `ue_probe` → `CLEAN`, and `ntfsinfo` reads the image without errors

**Done when:** the tests pass, a 4 GB `put` and `get` round trip matches `shasum`, and after a test run the image re-probes `CLEAN`.
**Out of scope:** performance tuning, KOIO, xattrs.

### Step 06: Randomized model test (fsx) and Windows oracle
**Read first:** the harness and the tests.
**Goal:** confidence that the write path does not corrupt data.

Tasks:
1. `unlatch-engine fsx <img> --seed N --ops M`: random create, write (random offsets and sizes, including across extents), truncate, rename, remove, mkdir, and periodic `ue_close`/`ue_open` remounts. An in-memory model stores the expected tree and contents. Compare after every op and print the seed when something fails.
2. `Scripts/fsx-soak.sh`: run many seeds, and keep the failing seeds in `Tests/fsx-regressions.txt`. They are re-run in CI.
3. `Scripts/chkdsk-vm.sh`: attach the image to a Windows VM (UTM/Parallels; the CLI is documented in DECISIONS), run `chkdsk /scan` (read-only scan) and collect the output. Mark it manual if automating is too hard, and document the steps in `docs/testing.md`.

**Done when:** 1,000 seeds of 2,000 ops each pass, and at least one fsx-produced image passes Windows `chkdsk` with no errors (the result is saved in `Tests/oracle/`).

### Step 07: Fuzzing, golden images, CI
**Goal:** engine regressions are caught automatically.

Tasks:
1. libFuzzer targets `Tests/Fuzz/fuzz_probe.c` and `fuzz_open_ls.c`. They run in-memory `ue_io_t` over the fuzz input with ASan and UBSan, seeded from small images. Crashes are saved to `Tests/Fuzz/crashes/` as regression cases.
2. Golden images in `Tests/Images/`, zstd-compressed under LFS, each with a README saying how it was made:
   - Windows-made with compressed, sparse and ADS files
   - a junction and a symlink
   - long paths
   - Fast Startup (hibernated)
   - dirty (unplugged mid-write)
   - fragmented MFT

   Add probe and read expectations for each to the tests.
3. `.github/workflows/engine.yml` on a macOS runner: build ntfs-3g (cached), run engine tests and fsx regressions, run fuzzers for 60 s each. FSKit is not tested in CI (documented).

**Done when:** CI is green, and each golden image has a passing expectation test (the Fast Startup image probes `HIBERNATED`).

---

## Phase 2: Mounting (macFUSE FSKit backend)

### Step 08: FUSE daemon skeleton: probe, safety check, read-only mount
**Read first:** macFUSE's `fuse_lowlevel.h` and `fuse_common.h` (`$(MACFUSE_PREFIX)/include/fuse3`, the `__APPLE__` / Darwin-extension parts in particular), the macFUSE wiki page on the FSKit backend, `EngineTool/file_io.c`.

**Goal:** an NTFS image or device mounts read-only through macFUSE's FSKit backend, with no kernel extension and no signing of our own.

Tasks:
1. `Fuse/` target `unlatch-fuse` (a command-line tool): links `UnlatchEngine` and macFUSE's `libfuse3`. macFUSE is a prerequisite the user installs; Unlatch never bundles it (its license forbids bundling with commercial software, and a GPL project only links the LGPL libfuse).
2. Block I/O: `EngineTool/file_io.c` serves image files and block devices (`DKIOCGETBLOCKSIZE`/`DKIOCGETBLOCKCOUNT`, aligned bounce for unaligned requests).
3. Safety at mount time: `ue_probe`, refuse `CORRUPT`/`NOT_NTFS`, everything else read-only in this step. Write the status record (D-41).
4. Low-level ops with the Darwin extensions: `lookup`, `forget`/`forget_multi` (a lookup count per node, `ue_release` at zero), `getattr` (flags, birth time), `opendir`/`readdir`/`releasedir` (`.` and `..` as offsets 1 and 2, engine cookies after), `open`/`read`/`release`, `statfs`. Write ops return `EROFS`. Single-threaded session loop, so engine calls are serialized.
5. Every incoming name is NFC-normalized in one helper.
6. `docs/dev-setup.md`: installing macFUSE, enabling its file system extension, mounting, unmounting.

**Done when:** a golden image file, and the same image attached with `attach-image.sh`, mount and show correct `ls -la`, `stat` times and `df` numbers; `shasum` through the mount matches the harness; `umount` is clean; the Fast Startup image mounts read-only with its status record saying `hibernated`. Record in DECISIONS: the backend, the mount command, and whether sudo was needed.

### Step 09: Read path: attributes, Finder, read speed
**Read first:** Step 08 code; macFUSE mount options (`noappledouble`, `local`, `iosize`, caching options as the FSKit backend supports them).

Tasks:
1. Reads: avoid the extra copy where libfuse allows it; a read-ahead or `iosize` setting if the backend honours it.
2. Attributes complete: mode (no `w` when read-only), uid/gid (the mounting user; record it), size, allocated size, nlink, all four times, `UF_HIDDEN`, file IDs, generation from the sequence number.
3. Directory listings page correctly with offsets under concurrent changes (the verifier is not available in FUSE; re-check the engine's cookie stability).
4. Route libntfs-3g logging and the daemon's own errors to OSLog (`engine`, `mount`).
5. Read benchmark: `dd if=file of=/dev/null bs=4m` after `purge`, against a raw `dd` of the device. Note both in PROGRESS.

**Done when:** recursive `shasum` of every file on a golden image matches the harness, Finder browses it (Quick Look works, hidden flags respected), and the read numbers are recorded.

### Step 10: Write path through FUSE
**Read first:** Step 05 tests; libfuse semantics of `create`, `unlink` while open, `rename` flags (`RENAME_EXCL`, `RENAME_SWAP`), `setattr` with the Darwin `to_set` bits.

Tasks:
1. Read-write when the probe says `CLEAN`, the device is writable and `ro` was not asked for.
2. Wire `write`, `create`, `mkdir`, `unlink`/`rmdir` (deferred delete while open is the engine's ghost mechanism), `rename` (EXCL; SWAP → `ENOTSUP`), `setattr` (size, times, flags, mode → read-only), `flush`/`fsync`/`fsyncdir` → `ue_sync`. Symlinks → `ENOTSUP` for now.
3. Unmount (`destroy`): `ue_sync` then `ue_close`; status record `readOnly: false`, cleanly unmounted.
4. Surprise removal: when block I/O returns `ENXIO`/`EIO`, the engine fails every later call with `EIO`; never retry writes; never mark clean.
5. `Tests/Fuse/rw-suite.sh` on a mounted image: `cp`/`rsync` a tree, `shasum -c`, `mv`/`rm -r`/`truncate`/`touch -t`, `ditto`, a git clone, unmount, then the harness probes `CLEAN`.
6. A short note on `._` AppleDouble files (or `-o noappledouble`), until Step 16.

**Done when:** the rw-suite passes 3 times in a row on a fresh image, the image probes `CLEAN` afterwards, and Windows `chkdsk /scan` passes on the result (Step 06 script).

### Step 11: Benchmark harness and **M0 GO/NO-GO gate**
**Goal:** measured speed against baselines. Decide whether to continue.

Tasks:
1. `Bench/run.sh <volume> <label>`: sequential write (`dd if=/dev/zero bs=4m` plus `sync`, `mkfile` larger than RAM), sequential read (`purge`, `dd bs=4m`, an `F_NOCACHE` reader), small files (untar about 30k files), metadata churn (create, rename, delete 10k), optional `fio` 4k random. Median of 5 runs into `Bench/results/<date>-<commit>-<label>.json` with the drive, connection and macOS build.
2. Baselines on the same disposable SSD and port: raw `dd` of `/dev/rdiskN`, APFS, exFAT, and stock ntfs-3g on macFUSE (FSKit backend). Document in `Bench/README.md`.
3. `Bench/compare.py`: a markdown table.
4. `docs/M0-gate.md`. Proposed: sequential read ≥ 60% of APFS, sequential write ≥ 40%, and not slower than stock ntfs-3g on the same backend. Agree the thresholds after the first run; record them in DECISIONS.

**Done when:** `docs/M0-gate.md` has the numbers and an explicit **GO** or **NO-GO**. A NO-GO stops the plan for a review (multi-threaded session loop, larger I/O sizes, or the kext backend as an opt-in).

### Re-scoping later steps (D-39)

When a later step starts, it first maps its FSKit wording to the daemon:
- **Step 12:** mounting means launching `unlatch-fuse` (no `FSClient`, no mount entitlement, so spike S1 disappears). DiskArbitration detection and the race with Apple's NTFS driver (S2) stay.
- **Step 13/14:** `doctor` and onboarding check that macFUSE is installed and its extension is enabled, instead of our own extension.
- **Step 15:** KOIO is not available through macFUSE; the step becomes I/O-size, caching and threading tuning.
- **Step 16:** xattrs depend on what macFUSE's FSKit backend passes through; otherwise `noappledouble` and `._` files.
- **Step 18:** without a membership there is no Developer ID or notarization. Distribution is source plus a Homebrew formula or tap that depends on the macFUSE cask; an unsigned DMG is optional.

---

## Phase 3: Mount management

### Step 12: UnlatchCore: DiskWatcher, StatusStore, MountService (and spikes)
**Read first:** Apple docs for `DASessionCreate`, `DARegisterDiskAppearedCallback`, `DARegisterDiskMountApprovalCallback`, `DADiskUnmount`/`DADiskEject`/`DADiskMount`, `FSClient.mountSingleVolume`, `FSClient.fetchInstalledExtensions`; DECISIONS.
**Goal:** one library that detects, mounts, unmounts and ejects, used by both the CLI and the app.

Spikes first; record the results as decisions:
- **S1:** can a plain CLI binary signed with `com.apple.developer.fskit.mount` (a restricted entitlement needs a provisioning profile) call `mountSingleVolume`? If not, the CLI asks the running app over XPC (`NSXPCConnection` to a Mach service the app registers, or an `SMAppService.agent` login item). Choose, then document.
- **S2:** with Apple's NTFS and Unlatch both installed, what happens on attach? Does a mount-approval dissent (`kDAReturnExclusiveAccess`/`kDAReturnNotPermitted`) stop Apple's mount and still let `mountSingleVolume` proceed? If not, fall back to unmount-and-swap.

Tasks:
1. `DiskWatcher`:
   - owns its `DASession` (failable init) and unregisters callbacks in `stop()`
   - context through `Unmanaged.passRetained`, released in `stop()`
   - matches NTFS by `kDADiskDescriptionVolumeKindKey == "ntfs"` (also check the partition type `Microsoft Basic Data` when a volume kind is missing)
   - publishes `DiskInfo` (bsdName, label, size, internal/removable/ejectable, current mount point, current FS: apple-ntfs, unlatch or none) as an `AsyncStream`
2. `StatusStore`: reads and watches the App Group `status/*.json` written by the extension.
3. `MountService` (an actor):
   - `list()`
   - `mount(bsdName:readOnly:)`: if Apple's driver has it mounted, unmount it; then `mountSingleVolume(resource:bundleID: extension ID, options: readOnly ? ["-o","rdonly"] : [])`. Check the option format. Read the status record. If the mount fails, remount the original with `DADiskMount`.
   - `unmount(bsdName:)`: `DADiskUnmount`, forced only on an explicit flag
   - `eject`: unmount, then `DADiskEject`
   - `status(bsdName:)`
4. Auto-mount policy: `AutoMountPolicy` covers external, removable or ejectable disks by default, plus a per-volume allow or deny list keyed by serial (UserDefaults suite in the App Group). Mount-approval dissent only for opted-in volumes (S2).
5. Errors: `MountError` with user-facing reasons such as extension not enabled, volume dirty, hibernated, busy or not NTFS. Turn "extension not enabled" into a clear message with an onboarding hint by checking `fetchInstalledExtensions`.
6. Unit tests with a fake DA layer behind a small protocol, only where it's needed for tests.

**Done when:** a small test executable (or a test with an attached image) can list, mount read-write, write a file, unmount, eject, and recover Apple's mount on a forced failure. S1 and S2 are recorded in DECISIONS.

### Step 13: CLI `unlatch`
**Read first:** UnlatchCore API and D-S1.
**Goal:** a scriptable CLI.

Tasks:
1. Subcommands:
   - `list [--json]`
   - `mount <disk> [--read-only]`
   - `unmount <disk> [--force]`
   - `eject <disk>`
   - `status [<disk>] [--json]`
   - `doctor`: extension installed and enabled, macOS version, entitlements, App Group reachable

   Accept a BSD name, `/dev/diskNsM`, a mount path or a label for `<disk>`.
2. Exit codes: 0 success; 1 error; 2 usage; 3 mounted but forced read-only (prints the reason); 4 extension not enabled. Document them in `--help` and `docs/cli.md`.
3. The JSON schema is fixed and versioned (`"schema": 1`).
4. Transport follows D-S1 (direct or XPC to the app).
5. Tests: argument parsing, JSON snapshot tests, exit-code mapping.

**Done when:** every subcommand works against an attached image, `unlatch status --json | jq` works, and the dirty image returns exit code 3 with its reason.

### Step 14: Menu-bar app and onboarding
**Read first:** UnlatchCore; Apple docs for `MenuBarExtra`, `UserNotifications`, `SMAppService.mainApp`, `FSClient.openFileSystemExtensionsSettings`. UI design goes to the designer agent.
**Goal:** a polished menu-bar app.

Tasks:
1. `MenuBarExtra` (window style if it needs richer rows):
   - one row per NTFS volume with label, size, and a state badge: read-write, read-only (with reason), dirty, hibernated, error, Apple read-only
   - actions: mount read-write, mount read-only, unmount, eject, reveal in Finder
   - "Auto-mount" toggle, per-volume "Always / Never auto-mount", Settings, Quit
2. Notifications: mounted read-write; fell back to read-only, with the reason and a "Learn more" link; ejected safely; a volume vanished unexpectedly ("may be dirty").
3. Onboarding window (first run, and whenever `doctor` fails):
   - macOS version check
   - "Enable Unlatch in System Settings" button (`openFileSystemExtensionsSettings`) that polls `fetchInstalledExtensions` until it is enabled
   - self-test: mount the bundled tiny NTFS image read-only (via `hdiutil` attach of `Resources/selftest.img`), list it, eject
   - optional: install CLI symlink (`~/.local/bin/unlatch`, or copy a sudo command), launch at login (`SMAppService.mainApp`), Spotlight off on Unlatch volumes (default on)
4. Settings: auto-mount scope (external only / all), notifications, Spotlight indexing, log export (`log show` for the subsystem into a file for bug reports).
5. Hosts the XPC service if D-S1 needs it.
6. Copy review: read every user-facing string once (plain, short, says what to do).

**Done when:**
- a fresh user account can install, onboard, enable the extension, pass the self-test, and plug in an NTFS disk, which auto-mounts read-write with a notification
- the dirty disk shows its badge and reason
- eject from the menu works
- VoiceOver reads every row and action

---

## Phase 4: Performance and depth

### Step 15: Kernel-offloaded I/O and cache policy
**Read first:**
- Apple docs: `FSVolume.KernelOffloadedIOHandler`, `FSExtentPacker`, `FSBlockmapFlags`, `FSCompleteIOFlags`, `FSItem.Attribute.inhibitKernelOffloadedIO`, `FSVolume.DataCacheHandler`, `KernelCacheCoherencyType`, `FSVolume.PreallocateHandler`
- libntfs-3g `runlist.h`, `attrib.h` (`ntfs_attr_map_whole_runlist`, `ntfs_attr_vcn_to_lcn`, `ntfs_attr_truncate`)
- `docs/M0-gate.md`

**Goal:** file data flows kernel ↔ device directly.

Tasks:
1. Engine: `ue_blockmap(e, ino, off, len, for_write, emit(ctx, file_off, dev_off, len, type))`.
   - Walk the runlist, and emit data extents and holes (zero-fill).
   - With `for_write`, allocate clusters for holes and beyond EOF. The allocation is rounded up to a growth step (start at 1 MiB, then tune), which covers the guide's "grow in big steps".
   - It fails with `ENOTSUP` for resident, compressed or encrypted data.
2. Engine: `ue_complete_io(e, ino, off, len, wrote, status)` updates `data_size`/`initialized_size` and mtime after kernel writes, and frees over-allocation on close or truncate. Zero-fill between the old `init_size` and the write offset is done by the extent type, never by leaving stale disk bytes readable. **This is a security requirement: test it explicitly.**
3. Extension: adopt `KernelOffloadedIOHandler` (`blockmapFile`, `completeIO`, KOIO `lookupItem`/`createFile`) and `PreallocateHandler`. Set `inhibitKernelOffloadedIO` on items with `UE_F_RESIDENT|COMPRESSED|ENCRYPTED`. Those keep using `ReadWriteHandler`. A resident file growing past the MFT record becomes non-resident; refresh the item's attributes so later I/O uses KOIO.
4. Adopt `DataCacheHandler`: `writeBack` for plain files, `noCache` or `writeThrough` where the engine itself writes data (inhibited items). Handle `setCacheState` downgrades around rename, truncate and delete. Confirm `synchronize` and `unmount` flush the kernel cache before `ue_close`.
5. Re-run the Step 10 rw-suite, the Step 06 chkdsk oracle and the Step 11 benchmarks. Record the gains, and decide whether KOIO is the default.

**Done when:** the rw-suite and chkdsk pass, the stale-data test passes (extend a file by seeking past EOF, then read the gap: all zeros), and the benchmarks show the gain over Step 11 (or explain why not).

### Step 16: Xattrs, Finder metadata, links, flags
**Read first:** Apple docs for `FSVolume.XattrHandler`, `createSymbolicLink`, `readSymbolicLink`, `createLink`; ntfs-3g `xattrs.c`, `reparse.c`, `ntfs_fuse_*xattr` for the stream mapping.
**Goal:** no `._` litter, and links behave.

Tasks:
1. Engine: `ue_xattr_list/get/set/remove` map names to NTFS named `$DATA` streams. Use ntfs-3g's `user.` convention so files stay interoperable with Linux ntfs-3g, and document the mapping. `com.apple.FinderInfo` and `com.apple.ResourceFork` become streams too. Size limits are enforced.
2. Extension: `XattrHandler`. The capability is reported, so macOS stops writing `._` files; verify with `cp -p`, Finder tags and `xattr -w`.
3. Symlinks: read NTFS symlink and junction reparse points (`ue_readlink`), translating absolute Windows targets the way ntfs-3g does or showing them as unresolvable. Create symlinks in the ntfs-3g/WSL-compatible format; record which in DECISIONS. Hard links: `ue_link` with `ntfs_link`, plus the capability flag.
4. `chflags hidden` maps to `UE_F_HIDDEN` and `uchg` to `UE_F_READONLY`.
5. Tests: harness unit tests plus an rw-suite extension, and a Windows check that the symlinks and ADS created are readable there.

**Done when:** copying a tagged and commented file in Finder leaves no `._` file and the tags survive a remount, `ln -s`/`ln` work, and Windows reads the results.

### Step 17: Robustness and the manual matrix
**Goal:** proof that the failure modes in the safety model actually behave as designed.

Tasks:
1. Interruption tests:
   - kill the extension process mid-copy
   - `hdiutil detach -force` mid-copy
   - pull a real disposable USB SSD mid-copy

   Expected each time: no crash in the app, no stale `/Volumes` entry, and the next mount is read-only or clean according to the probe.
2. Sleep and wake with a mounted volume, plus Power Nap. Note how FSKit behaves.
3. Capacity: a 50 GB+ single file with a matching checksum, a full disk, a 1M-file directory tree.
4. Names: Unicode NFC/NFD round trip with Windows, emoji, 255-char names, reserved names rejected clearly, case-only renames.
5. Spotlight and Time Machine: confirm the indexing setting works and Time Machine does not offer Unlatch volumes.
6. A 24-hour soak: a continuous fuzz and fsx soak, plus a copy loop on a real disk.
7. Write all results into `docs/test-matrix.md` (scenario, expected, actual, date, build). Every failure gets fixed here or recorded as a carry-over with a severity.

**Done when:** the matrix shows no open data-loss or corruption issues.

---

## Phase 5: Ship

### Step 18: Signing, notarization, DMG, notices
**Goal:** a notarized DMG that works on a clean Mac.

Tasks:
1. Developer ID certificates and App IDs with the FSKit and App Group capabilities. Provisioning profiles for the app and the extension (and the CLI if D-S1 needs one). The steps go in `docs/release.md`.
2. `Scripts/release.sh`:
   - `xcodebuild archive` (Release, arm64)
   - sign inside out with hardened runtime and `--timestamp`: engine and tools, CLI, extension, app
   - `codesign --verify --deep --strict` and `spctl -a -vv`
   - create the DMG (`hdiutil create`, with an Applications link)
   - `notarytool submit --wait`, then `stapler staple`
3. A third-party notices bundle in the app: the GPL-3.0 text, the ntfs-3g copyright notices and license, the swift-argument-parser license, and a written source offer with the repository URL and tag for each release.
4. `.github/workflows/release.yml` on tag: build, sign (secrets), notarize, attach the DMG to a GitHub Release.

**Done when:** the DMG downloads to a clean Mac (a VM is fine), Gatekeeper passes, onboarding works, and a disk mounts read-write.

### Step 19: Updates and uninstall
**Goal:** safe updates and a clean uninstall.

Tasks:
1. Sparkle 2: an EdDSA key, an appcast hosted on GitHub Pages or Releases, and the release script generating the appcast entries. Before installing, check `MountService.list()`. If any Unlatch volume is mounted, ask to unmount, or cancel.
2. "Uninstall Unlatch…" in Settings: unmount all, remove the CLI symlink, unregister login items, open Extensions settings to disable, then move the app to the Trash. Document the manual steps in `docs/uninstall.md`.

**Done when:** an update from vN to vN+1 works with a volume mounted (it asks first), and uninstall leaves no login item and no extension listed.

### Step 20: Beta and release
**Goal:** v1.0.

Tasks:
1. Beta on at least 3 Macs and 2 macOS 27.x builds, with a variety of real NTFS disks (USB HDD, SSD, a Windows-formatted stick). Gather logs with the app's log export.
2. Fix triage: data-integrity issues block the release, and everything else is filed.
3. Docs: README (install, enable, use, limitations: no NTFS encryption write, Win32 name rules, Fast Startup advice), `docs/troubleshooting.md`, `SECURITY.md`, `CONTRIBUTING.md`.
4. Tag v1.0.0. The release workflow publishes. Post-v1 roadmap in the README: engine replacement behind `unlatch_engine.h`, a repair assistant, a format command (`startFormat` with mkntfs logic), localization.

**Done when:** v1.0.0 is published and the beta found no open data-integrity bugs.

---

## Risk register (kept current in PROGRESS.md)

| Risk | Where it is settled | Fallback |
|---|---|---|
| FSKit read-write throughput too low | Step 11 gate, Step 15 KOIO | Bring KOIO forward; NO-GO review |
| libntfs-3g custom device quirks | Step 03 | Patch in `Engine/vendor/patches/` |
| Restricted entitlement on the CLI | Step 12 S1 | CLI talks to the app over XPC |
| Apple's NTFS driver wins the mount race | Step 12 S2 | Unmount-and-swap |
| Corruption | Steps 05–07, 10, 15, 17 | Read-only default for that scenario until fixed |
| Stale data exposed through KOIO | Step 15 explicit test | Inhibit KOIO for files with `init_size < size` |
| FSKit API changes in 27.x | every step re-checks the docs | pin the tested 27.x builds in README |
