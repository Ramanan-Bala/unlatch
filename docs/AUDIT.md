# Unlatch — Whole-repo audit

Date: 2026-11-02. A one-shot report, made by reading the code only. No code was changed, and nothing was built or run (macOS-only project, audited from a Windows machine).

**What this repo does:** Unlatch lets a Mac read and write Windows (NTFS) drives. A C engine built on libntfs-3g does the disk work. A macFUSE background program (`unlatch-fuse`) shows the drive as a normal folder. A small root helper opens the raw disk for the logged-in user, and a menu-bar app mounts drives automatically when you plug them in.

**Assumed load:** one person on a desktop Mac with one or a few drives attached. Keeping data safe matters far more than speed.

## Must fix

### 1. A crash during writing leaves a damaged drive that still says "clean"
`Engine/src/volume.c:L288`, `Tests/Fuse/interrupt.sh:L32-42`

**Status (2026-11-02):** Fixed (D-86): `Engine/src/volume.c`, `Tests/Fuse/interrupt.sh`, `test_dirty_mark`.

- **What this is:** While a drive is mounted for writing, file-system bookkeeping (which disk blocks are used, folder listings) waits in memory for up to about 1 second. It is then written to disk in disk-position order, not in a safe order (`Engine/src/bcache.c` `bc_writeback`).
- **Problem:** The engine never marks the drive "dirty" (NTFS's "in use, may be inconsistent" flag) while it is open for writing. The comment at L288 says "libntfs-3g never sets VOLUME_IS_DIRTY on mount, so there is nothing to clear here". If the program is killed, the Mac crashes, or the cable is pulled halfway through a write-back, the drive can be half-updated but still marked clean. On the next plug-in, the probe says CLEAN and the app mounts it for writing again, so new files can be given blocks that old files still use. That silently corrupts both. Windows won't offer to repair it either, because the flag says clean. The project's own build guide (`docs/NTFS for Mac Complete Build Guide.md` L255) says to leave the drive marked dirty, and `interrupt.sh` currently accepts `clean` after a kill and then mounts the drive for writing again.
- **Fix:** In `ue_open` for writing, set the dirty flag (`ntfs_volume_write_flags(vol, vol->flags | VOLUME_IS_DIRTY)`) and flush it to disk before any other write. In a successful `ue_close`, after the final write-back and flush, clear the flag and flush again. Change `interrupt.sh` to require `dirty` after a kill. The existing "dirty → read-only + run chkdsk" path then handles everything else. Windows' own NTFS driver (`ntfs.sys`) and Linux's ntfs3 driver work the same way.
- **If we skip it:** Some unclean removals will silently corrupt user files, and nobody will be warned.

### 2. When auto-mount fails, the drive disappears completely
`App/AppModel.swift:L46`, `L78-84`, `L114-118`

**Status (2026-11-02):** Fixed (D-88): `App/Sources/AppModel.swift` (`canMount`, released volumes), `MountService.mountWithMacOS`.

- **What this is:** The app tells macOS "don't mount this drive, I will" (Disk Arbitration's mount-approval callback in `Shared/DiskWatcher.swift`) for every NTFS drive its auto-mount settings allow.
- **Problem:** It does this without checking that it can actually mount. Examples: the helper isn't installed or lacks Full Disk Access, or macFUSE is missing or switched off. The mount then fails and Apple's read-only driver has already been turned away. The fallback to Apple's driver in `MountService` only runs when Apple had the drive first (`hadApple`), and the app's own refusal would block it anyway. The user gets "Could not mount" and the drive is not visible at all, not even read-only.
- **Fix:** Only claim the drive when macFUSE and the helper are installed (both checks are cheap). If an automatic mount fails, stop claiming that drive and ask macOS to mount it normally (`daMount`).
- **If we skip it:** After a macOS update or a revoked permission, users' drives "vanish". That looks like data loss and makes for a bad first impression.

### 3. A mount that times out keeps going in the background
`Fuse/main.c:L238-248`, `Shared/MountService.swift`

**Status (2026-11-02):** Fixed: `Fuse/main.c` `run_in_background` sends SIGTERM, then SIGKILL after 5 s, and waits.

- **What this is:** The parent process starts the mount program and waits up to 10 s for the mount to appear.
- **Problem:** On timeout it prints "did not appear as a mount" and returns failure, but it never stops the child process. The child can still finish and mount the drive for writing. Meanwhile `MountService` thinks it failed and may hand the drive back to Apple's driver. Two drivers on one disk is a corruption risk, and the user gets a "failed" message for a drive that is in fact mounted for writing.
- **Fix:** On timeout, `kill(pid, SIGTERM)` and `waitpid` before returning 1.
- **If we skip it:** On a slow USB drive, the same disk can end up mounted by two drivers at once.

### 4. The root helper gives raw write access to non-NTFS partitions
`Helper/main.c:L64-87`, `L128-129`

**Status (2026-11-02):** Fixed (D-87): `Helper/policy.c` `helper_kind_allowed`; the partition type allows read-only only.

- **What this is:** The helper runs as root and passes an open handle to a raw disk partition to the logged-in user.
- **Problem:** When macOS doesn't know a partition's file system, the helper accepts any partition tagged `Windows_NTFS` or Microsoft Basic Data (`EBD0A0A2-…`). That tag is also used by exFAT, FAT32, BitLocker and unformatted partitions. A non-admin user can ask for `rw` on, say, a BitLocker partition and get raw write access to it. The engine only refuses later, after the handle has been given out.
- **Fix:** For `rw`, require macOS to report the file system as `ntfs`. Remove the partition-tag fallback, or allow it for `ro` only.
- **If we skip it:** Any logged-in user can overwrite non-NTFS partitions on attached disks.

## Should fix

### 5. The only root code has no tests
`Helper/main.c`

**Status (2026-11-02):** Fixed (D-87): `Tests/Helper/test_policy.c`, run by `Scripts/test-engine.sh`.

- **What this is:** The checks for "is the caller the logged-in user" and "is this really NTFS" decide who gets raw disk access.
- **Problem:** No test covers them, so a change could silently widen access (finding 4 is one example).
- **Fix:** Move `is_ntfs` and the peer check into small functions that take plain values. Add one test file with a few cases: ntfs, exfat, missing kind, root caller, wrong user.
- **If we skip it:** Security mistakes in the helper only show up in production.

### 6. A failed file create can leave a file stuck as "in use"
`Fuse/ops.c:L687-707`

**Status (2026-11-02):** Fixed: `Fuse/ops.c` `make_entry` drops the engine hold on failure; `uf_create` checks `ue_file_opened`.

- **What this is:** `uf_create` makes the file in the engine, then builds the macFUSE entry.
- **Problem:** If building the entry fails (out of memory), the engine's open count on the new file is never released, and the return value of `ue_file_opened` (L702) is ignored. The file then counts as open until unmount, so deleting it leaves a hidden leftover until then.
- **Fix:** On that failure path, call the matching release, and check `ue_file_opened`'s return value.
- **If we skip it:** A rare leak, but in code that writes to disk.

### 7. "Mount" can report the wrong mode
`Shared/MountService.swift:L117-119`

**Status (2026-11-02):** Fixed: `MountService.mount` reports `st?.readOnly ?? readOnly`.

- **What this is:** If Unlatch already has the drive mounted, `mount()` returns early.
- **Problem:** It reports the mode that was asked for, not the real one. Running `unlatch mount` on a drive that is mounted read-only says "read-write" and exits with success.
- **Fix:** Use `st?.readOnly ?? readOnly`, as L145 already does.
- **If we skip it:** Scripts and users believe they can write when they can't.

### 8. Stale notes about flushing
`Fuse/ops.c:L783-790`, `docs/DECISIONS.md` D-46

**Status (2026-11-02):** Fixed: `Fuse/ops.c` flush comment; D-46 points to D-62.

- **What this is:** These notes say closing a file needs no work "because engine writes go straight to the device".
- **Problem:** That stopped being true when the write-back cache (D-62) was added. The next person to touch `flush`/`fsync` will reason from a wrong picture.
- **Fix:** Rewrite the comment to say data reaches the disk by the 1 s timer, `fsync`, or unmount. Add a note to D-46 pointing to D-62.
- **If we skip it:** A future change could drop a write-back that is actually needed.

## Nice to have

### 9. The background start fails when run by name from PATH
`Fuse/main.c:L204-252`

**Status (2026-11-02):** Fixed: `Fuse/main.c` spawns its own path from `_NSGetExecutablePath`.

- **What this is:** Without `-f`, `unlatch-fuse` starts itself again in the background.
- **Problem:** `posix_spawn(argv[0])` with `argv[0]="unlatch-fuse"` looks in the current folder, not PATH. The app passes a full path, so only manual or Homebrew use without `-f` fails.
- **Fix:** Spawn the program's own path from `_NSGetExecutablePath`.
- **If we skip it:** Homebrew users see "cannot start the daemon".

### 10. Lock files live in a shared, world-writable `/tmp` folder
`Fuse/main.c:L146-177`

**Status (2026-11-02):** Not fixed: see the carry-over in `docs/PROGRESS.md` (a device-name key would undo D-81).

- **What this is:** Each mount takes a lock under `/private/tmp/io.github.ramanan-bala.unlatch.locks/<serial>`.
- **Problem:** Any local user can hold a drive's lock and block other users' mounts. Two drives with the same serial number (a cloned disk) also can't be mounted together.
- **Fix:** Put the locks in a folder owned by the helper or by root, and include the disk's device name in the lock key.
- **If we skip it:** Only matters on shared Macs and for cloned disks.

### 11. The menu can freeze while a mount starts
`Shared/MountService.swift:L188-230`

**Status (2026-11-02):** Fixed: `runDaemon` waits in a detached task; concurrent mounts of one volume share one attempt.

- **What this is:** `runDaemon` starts the mount program and waits for it.
- **Problem:** It waits inside the service for up to 10 s or more, so list and status calls from the app queue behind it.
- **Fix:** Do the wait off the service and only record the result in it.
- **If we skip it:** The menu stalls on slow drives.

### 12. A partial read returns an error instead of the bytes it got
`Fuse/ops.c:L525-550`

**Status (2026-11-02):** Fixed: `uf_read` returns the bytes read when `done > 0`.

- **What this is:** `uf_read` reads a file in chunks.
- **Problem:** If a later chunk fails, it returns the error even when some bytes were already read.
- **Fix:** Return the byte count when `done > 0`, as `uf_write` already does.
- **If we skip it:** Apps can fail on a read from a bad sector even though part of the data was read.

### 13. The helper logs a garbage user ID
`Helper/main.c:L103`

**Status (2026-11-02):** Fixed: `Helper/main.c` logs the console uid only when `stat` succeeded.

- **What this is:** The helper logs who owns `/dev/console` when it refuses a caller.
- **Problem:** When the `stat` of `/dev/console` fails, the log prints a user ID that was never set.
- **Fix:** Log only when `stat` succeeded.
- **If we skip it:** Misleading logs.

---

**Verdict:** The engine, cache and tests are careful and well above average, but fix 1 (mark the drive dirty while writing) before 1.0. Then fix 2 and 3, which make a drive disappear or end up mounted twice.

**Not checked:**

- Not read:
  - the vendored ntfs-3g and its patches
  - the engine's read, inode, xattr and link code
  - the internals of `ue_create`, `ue_symlink`, `ue_link` and `ue_remove`
  - `EngineTool` main and fsx
  - the dormant FSKit `Extension/`
  - CLI `Setup`/`Commands`, `MacFUSE.swift`, `UpdateChecker.swift`, `DiskInfo.swift`
  - the app's Checks, Onboarding, Settings and Menu views
  - the release scripts, CI workflows and benchmarks
- Not confirmed: whether `F_FULLFSYNC`/`fsync` on a raw disk really empties the drive's own write cache (`EngineTool/file_io.c:L98-105`). If it doesn't, even fix 1 is weaker.
- Not run: nothing was built or tested. Every finding comes from reading the code.

**Known carry-overs** (already tracked in `docs/PROGRESS.md`, so not listed above):

- the Windows chkdsk oracle has never been run
- Windows-made images with Fast Startup, a dirty flag or heavy fragmentation are missing
- integration tests fail with "busy" when run in parallel
- the patches have not been sent upstream
