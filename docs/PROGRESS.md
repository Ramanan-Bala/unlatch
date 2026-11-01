# Progress

## Status

| Field | Value |
|---|---|
| Current phase | 1 Engine on host |
| Last completed step | 05 Engine write path (2026-11-01) |
| Next step | 06 Randomized model test (fsx) and Windows oracle |
| libntfs-3g | 2026.9.28 (`Engine/vendor/VERSION`) + patch 0001 |
| Tested on | macOS 27 (arm64), Xcode 27.2 (27B52), Swift 6.4.2, XcodeGen 2.46.0, Git LFS 3.8.1 |

### Risk register

| Risk | Where it is settled | Fallback | State |
|---|---|---|---|
| FSKit read-write throughput too low | Step 11 gate, Step 15 KOIO | Bring KOIO forward; NO-GO review | open |
| libntfs-3g custom device quirks | Step 03 | Patch in `Engine/vendor/patches/` | settled: works unpatched, one EROFS workaround (D-14) |
| Restricted entitlement on the CLI | Step 12 S1 | CLI talks to the app over XPC | open |
| Apple's NTFS driver wins the mount race | Step 12 S2 | Unmount-and-swap | open |
| Corruption | Steps 05–07, 10, 15, 17 | Read-only default for that scenario until fixed | open; Step 05 tests pass, fsx and chkdsk (Step 06) pending |
| Stale data exposed through KOIO | Step 15 explicit test | Inhibit KOIO for files with `init_size < size` | open |
| FSKit API changes in 27.x | every step re-checks the docs | pin the tested 27.x builds in README | open |

## Steps (log)

### Step 01: Repository scaffold — done 2026-11-01

**Done:**
- `git init` (branch `main`), `.gitignore`, Git LFS tracking `Tests/Images/*.img.zst` (`.gitattributes`).
- `LICENSE` (full GPL-3.0 text from gnu.org), `README.md`, `AGENTS.md`, `CONTRIBUTING.md`, `DCO` (v1.1).
- `project.yml` (XcodeGen) with targets `Unlatch` (menu-bar app, `LSUIElement`), `UnlatchFSModule` (ExtensionKit placeholder, extension point `com.apple.fskit.fsmodule`, embedded in the app), `UnlatchEngine` (static C library with `ue_version()`), `unlatch-engine` (C tool, `--version`). Schemes `Unlatch` (all four) and `unlatch-engine`.
- `Shared/` Swift package `UnlatchCore` (identifier and logger constants, one Swift Testing test). `CLI/` Swift package, executable `unlatch` on `swift-argument-parser`.
- `Scripts/bootstrap.sh`: checks Xcode 27+, XcodeGen, Git LFS; runs `git lfs install --local` and `xcodegen generate`.
- Empty layout dirs: `Tests/Images/`, `Bench/results/` (kept with `.gitkeep`).
- Committed locally as `step 01: repository scaffold`.

**Verify:**
```sh
Scripts/bootstrap.sh
xcodebuild -scheme Unlatch build
swift build --package-path Shared
swift test --package-path Shared
swift build --package-path CLI
swift run --package-path CLI unlatch --help
```
All pass from a clean checkout (also `xcodebuild -scheme unlatch-engine build`).

**Left:** nothing. The owner created the public repo `Ramanan-Bala/unlatch` and pushed it (task 7), because the session was not allowed to create a public repo.

**Surprises:**
- XcodeGen rewrites an entitlements file from `project.yml` whenever the target has an `entitlements:` key, so files with no `properties` come out empty.
- With App Group or FSKit entitlements applied, ad-hoc builds fail: "requires a provisioning profile". The entitlements are kept unapplied for now (D-06).
- `xcodebuild` prints a "CoreSimulator is out of date" warning on this machine. It is harmless for macOS builds.

### Step 02: Vendor and build libntfs-3g plus ntfsprogs — done 2026-11-01

**Done:**
- Pinned ntfs-3g **2026.9.28**, the latest stable release (D-09). The release tarball's SHA-256 matches the published value. It is extracted into `Engine/vendor/ntfs-3g/` (`COPYING` GPL-2.0, `COPYING.LIB`), with the pin in `Engine/vendor/VERSION`.
- `Scripts/build-ntfs3g.sh` builds out of tree into `build/ntfs3g/{lib,include,bin}`: arm64, macOS 27, no FUSE, no mount helper, no crypto, no plugins, no nfconv, static only (D-10, D-12). It applies `Engine/vendor/patches/*.patch` (none needed) and runs `autoreconf` only if `configure` is missing.
- `Scripts/make-image.sh <name> <size> [label]` (mkfile -n + mkntfs -F -Q -L, prints the path) and `Scripts/attach-image.sh <img>` (prints `diskN`).
- `UnlatchEngine` now has a private header path to `build/ntfs3g/include`, and `libntfs-3g.a` is merged into `libUnlatchEngine.a` (D-11). A pre-build check fails with a clear message if the library is missing. `Scripts/bootstrap.sh` builds it when missing.

**Verify:**
```sh
rm -rf build && Scripts/bootstrap.sh            # builds libntfs-3g from scratch (~20 s)
lipo -info build/ntfs3g/lib/libntfs-3g.a        # arm64
mkdir -p build/images && cd build/images
../../Scripts/make-image.sh t1 512m T1          # prints .../t1.img
../ntfs3g/bin/ntfsinfo -m t1.img                # Volume Name: T1, Version 3.1
../ntfs3g/bin/ntfsls -s -l t1.img               # root: $MFT, $LogFile, ... (plain ntfsls prints nothing: root is empty)
cd ../.. && xcodebuild -scheme Unlatch build
nm -g build/DerivedData/Build/Products/Debug/libUnlatchEngine.a | grep ' T _ntfs_device_mount'   # with -derivedDataPath build/DerivedData
ls Engine/vendor/ntfs-3g/COPYING*
```
I also tested write and read-back with `ntfscp`/`ntfscat`, and an `attach-image.sh` then `hdiutil detach` cycle on `t1.img`. diskutil reports the attached disk as NTFS, not mounted. I verified `build-ntfs3g.sh` on a fresh `git clone` of the step commit.

**Left:** nothing in scope.

**Surprises:**
- `tuxera.com` answers with a 301 redirect to `www.tuxera.com` and stalls when you follow it. Download from `https://www.tuxera.com/...` directly.
- On non-Linux hosts, configure's `install-exec-hook` moves `libntfs-3g.so*` to `/lib` unless you pass `--exec-prefix`. That flag fixes it without a patch.
- No macOS patches were needed. Upstream compiles with a few warnings: unused variables, packed-member addresses, and "Trimming not supported" in `ioctl.c`.
- `hdiutil attach -nomount` and `hdiutil detach` now print deprecation warnings that recommend `diskutil image attach --noMount` and `diskutil eject`. They still work.
- macOS identifies the attached image as NTFS (`File System Personality: NTFS`). With `-nomount`, Apple's driver does not mount it.

### Step 03: Engine core (device bridge, probe, open/close) and host harness — done 2026-11-01

**Done:**
- `Engine/include/unlatch_engine.h`: the full seam from the plan, with Step 03 functions plus declarations for the read side (Step 04) and write side (Step 05). Adds `ue_vol_state_name()` and documents what `ue_probe` and `ue_open` return.
- `Engine/src/internal.h`: the only file that includes ntfs-3g headers, after libntfs-3g's own `config.h` (D-13).
- `Engine/src/device.c`: `ntfs_device_operations` backed by `ue_io_t`. Seek position, bounds checks (seek past the end fails), `EROFS` on a read-write open of a read-only `ue_io_t`, `stat` reports a block device, and `ioctl` answers `DKIOCGETBLOCKSIZE` and `DKIOCGETBLOCKCOUNT`, the two queries libntfs-3g makes on Darwin.
- `Engine/src/volume.c`:
  - `ue_probe`: boot-sector check, read-only mount, dirty flag, hiberfile check, logfile check, mapped to a state with a readable reason (D-15). It also fills label, serial, version, cluster size and total/free bytes.
  - `ue_open`: read-only or strict read-write, then `ntfs_create_lru_caches`, `ntfs_set_ignore_case`, `ntfs_set_shown_files(vol, FALSE, TRUE, FALSE)`.
  - `ue_close` and `ue_is_readonly`.
- `EngineTool`: `unlatch-engine probe <img>` and `info [--rw] <img>`, using the file-backed `ue_io_t` in `file_io.c` (pread/pwrite loops, `F_FULLFSYNC`).
- `Tests/Engine/test_engine.c`, 8 tests:
  - clean image → CLEAN, with label, sizes and version
  - dirty image → DIRTY
  - zeroed boot sector → NOT_NTFS
  - truncated image → CORRUPT
  - active `hiberfil.sys` → HIBERNATED; read-write open fails with EPERM, read-only open works
  - read-only `ue_io_t` with a read-write open → EROFS
  - bad arguments → EINVAL
  - 100 read-write plus 100 read-only open/close cycles, and the volume is still CLEAN afterwards

  `Tests/Engine/set_dirty.c` is the dirty-flag fixture tool. `Scripts/test-engine.sh` runs everything (D-17).

**Verify:**
```sh
Scripts/test-engine.sh                       # ends with "test-engine: PASSED"
xcodebuild -scheme unlatch-engine -derivedDataPath build/DerivedData build
build/DerivedData/Build/Products/Debug/unlatch-engine probe build/test-engine/clean.img
grep -rnE '#\s*include\s*[<"]ntfs-3g/' --include='*.[ch]' --include='*.swift' . --exclude-dir=vendor --exclude-dir=build   # only Engine/src
xcodebuild -scheme Unlatch build             # still green
```
`leaks --atExit` over 100 read-write open/close cycles reports 0 leaks. For the dirty fixture, `ntfsinfo -f` independently reports `Volume Flags: 0x0001 DIRTY`.

**Left:** nothing in scope. The UNCLEAN_LOG and Fast Startup paths have no fixture yet; they need a Windows-made image (Step 07).

**Surprises:**
- Open question 3: the custom device path worked first time. The one trap is `ntfs_volume_startup`'s silent read-only retry on `EROFS` (D-14).
- `ntfs_volume_check_logfile` is `static`, so the probe rebuilds it from the exported functions. `ntfs_volume` doesn't keep the serial number, so the probe reads it from the boot sector.
- The installed ntfs-3g headers fail `-Wdocumentation` under `-Werror`, so they are included with `-isystem`.
- A failed `ntfs_device_mount` releases the volume and closes the device but does not free it; `ntfs_umount` frees both. `mount_volume` and `unmount_volume` in `volume.c` encode this.
- The Xcode project has to be regenerated (`Scripts/bootstrap.sh`) after adding a source file. `test-engine.sh` only generates the project when it's missing.

### Step 04: Engine read path — done 2026-11-01

**Done:**
- `Engine/src/icache.c`: open-inode cache (open addressing, backward-shift delete) keyed by MFT record (D-18).
- `Engine/src/read.c`:
  - `ue_lookup`: case-insensitive; hides metafiles; rejects `/`, `.`, `..` and empty names.
  - `ue_getattr`: from `$STANDARD_INFORMATION` plus unnamed `$DATA`. Times via `ntfs2timespec`, DOS flags mapped to `UE_F_*`, `nlink` without 8.3 names, `UE_F_RESIDENT`, `init_size` (D-23).
  - `ue_readdir`: batches of 128 names from `ntfs_readdir`, then attributes read outside the walk. Case-preserved (D-19), skips `.`, `..`, DOS names and metafiles; stable cookies; verifier.
  - `ue_read`: `ntfs_attr_pread`, zeros past `init_size` and through holes.
  - `ue_statfs`, `ue_release`, `ue_get_stats`.
- Harness: `ls [--stats]`, `stat`, `cat`, `tree`. `EngineTool/walk.c` does the path walk on top of `ue_lookup`.
- Fixtures: `Engine/src/fixtures/mkfixture.c` plus `gen.h` (D-20).
- Tests: 10 new read tests:
  - nested dirs, and a 100 KB file checked against the generator
  - mixed-case lookups; readdir keeps case and hides metafiles
  - NFC Unicode name, including a case-insensitive lookup on É
  - 10k-entry directory: full listing and paging (97 per call, refusing the 98th), with no duplicates or losses
  - 3 GiB sparse file: every byte checked
  - `init_size < size`: zeros past `init_size`
  - resident file with the hidden flag
  - error codes; release; statfs
- Harness checks: the 10k listing does 10,000 inode opens (one per MFT record), and `cat` of a 4 GiB file matches `shasum`. Second leak loop: open, walk, read, readdir, close.
- Patch 0001 fixes a libntfs-3g `ntfs_readdir` resume bug (D-21).

**Verify:**
```sh
Scripts/test-engine.sh     # 18 unit tests ok; "/big: entries: 10000 inode_opens: 10000";
                           # "cat of a 4 GiB file matches shasum"; 0 leaks twice; PASSED
```

**Left:** nothing in scope. Symlinks and junctions show as files or dirs with `UE_F_REPARSE` (Step 16).

**Surprises:**
- **Upstream bug:** `ntfs_readdir` resumed at a mid-byte bitmap position tests misaligned bits. Every resumed walk near the end of a large directory failed with "Failed to read index block" (EIO), and fragmented directories could skip entries silently. Patched (0001).
- **Case:** in ignore-case mode libntfs-3g lowercases every name `ntfs_readdir` returns (D-19).
- **Pause rule:** a stopped `ntfs_readdir` leaves `*pos` on the entry it refused, and resuming at `pos+1` skips exactly that entry. That makes "position plus 1" a correct next-cookie.
- **Fixtures:** `ntfs_attr_truncate` creates sparse holes, so `uninit.bin` is also flagged sparse, and a 3 GiB file costs 1 MiB.
- **Timing:** 4 GiB `cat` through the engine takes about 15 s, roughly 270 MB/s, on a debug build plus `shasum`.

### Step 05: Engine write path — done 2026-11-01

**Done:**
- **Per-call inodes (D-24, supersedes D-18):** `Engine/src/inode.c` handles open, sequence check, attributes and name checks. `Engine/src/held.c` tracks the items the host holds, replacing the open-inode cache. `read.c` now uses per-call opens.
- **`Engine/src/write.c`:**
  - `ue_write`, `ue_create` (Win32 name rules, D-27), `ue_setattr` (size, times, DOS flags, mode → READONLY), `ue_sync`.
  - `ue_remove`: type checks, ENOTEMPTY; a held file's last name becomes a ghost, deleted on `ue_release` or at `ue_close` (D-25).
  - `ue_rename`: link/unlink sequences for move, replace, case-only rename and directory moves, with rollback (D-26).
  - A read-only engine returns EROFS from every mutating call.
- **Harness:** `put` and `get` (both accept `-`), `mkdir`, `mv`, `rm [-d]`, `truncate`, `touch`.
- **Tests:** 8 new tests, 26 in total:
  - round trip: two writes with a gap, an overwrite, reopen read-only, verify every range
  - name rules, including 255/256-character names and reserved names
  - EROFS from a read-only engine
  - remove: type errors, metafiles, the held-file ghost (data stays readable, the name can be reused, space comes back on release), a ghost cleaned up at close
  - the rename matrix
  - setattr: grow, shrink, exact times, flags, mode
  - the readdir verifier changes after create and rename
  - filling a 32 MB volume gives ENOSPC, then remove gives the space back
- **Image checks:** after every write test the image re-probes CLEAN. `test-engine.sh` then checks each `w-*.img` with probe, `ntfsinfo`, `ntfsfix -n`, and `ntfsls -R` (no `.unlatch-` names left). A third leak loop runs create, write, rename and remove.
- **4 GiB round trip:** `put` from stdin, then `get` to stdout, matches `shasum`, and the image is CLEAN afterwards.

**Verify:**
```sh
Scripts/test-engine.sh   # 26 unit tests ok; "4 GiB put (10 s) and get (16 s) round trip matches shasum;
                         # image CLEAN"; 3x 0 leaks; "8 written images"; PASSED (about 2 minutes)
```

**Left:** nothing in scope.

**Surprises:**
- **Inode model:** libntfs-3g is built around short-lived inode opens. `ntfs_delete` closes the inodes passed to it, linking needs a closed inode, and syncing a file reopens its parent by reference. Keeping inodes open between calls, as the plan's inode cache did, risks two copies of one record, so the cache became a table of held items (D-24).
- **Parent times:** `ntfs_create` does not update the parent's times (lowntfs-3g does that itself). Without the fix, the readdir verifier didn't change after a create.
- **Case-only rename:** needs a temp name, because `$I30` is case-insensitive.
- **Timing (debug build):** writes run at about 400 MB/s from stdin.

## Carry-overs

- **Step 08:** set a development team and reference `Extension/Resources/UnlatchFSModule.entitlements` and `App/Resources/Unlatch.entitlements` from `project.yml` (D-06). The extension cannot load without `com.apple.developer.fskit.fsmodule`. Keep an ad-hoc build path for CI (Step 07), for example an xcconfig override.
- **Step 08:** `Extension/Sources/UnlatchFSModule.swift` is a placeholder with no `@main`; the appex builds but is not a working extension.
- **Step 04+:** the ntfs-3g build sets no `--enable-debug`. Decide whether Debug engine builds want ntfs-3g debug logging. For now the log handler is null (D-16).
- **Step 07:** add fixtures for UNCLEAN_LOG and Fast Startup (`$LogFile` restart page 2.0), from Windows-made images. The probe code paths exist but are untested.
- **Step 07 (CI):** run `Scripts/test-engine.sh`. It needs `leaks`, which comes with Xcode command-line tools.
- **Upstream:** send patch 0001 to tuxera/ntfs-3g (a public PR or issue; needs the owner's OK).
- **Step 06:** names created by libntfs-3g are in the POSIX namespace. Confirm Windows `chkdsk` accepts fsx-made images with such names, plus ghost and rename-temp leftovers.
- **Step 11/15:** `ue_write` updates times and syncs the MFT record on every call, one MFT write per `ue_write`. Measure it at the M0 gate; lowntfs-3g's `dmtime` throttle is the model if needed.
- **Step 17:** a crash can leave `.unlatch-ghost-*` or `.unlatch-rename-*` names, or both names of a rename in progress (D-25, D-26). Sweep the ghost and temp names on read-write open, at least in directories touched recently, or document it.
- **Step 08:** route libntfs-3g logging (`ntfs_log_set_handler`) to OSLog category `engine` (D-16).
- **Step 07 (CI):** cache `build/ntfs3g`, keyed on `Engine/vendor/VERSION` plus `Engine/vendor/patches/*` plus `Scripts/build-ntfs3g.sh`.
- **Step 18:** the notice bundle must include `Engine/vendor/ntfs-3g/COPYING`, `COPYING.LIB`, `AUTHORS` and `CREDITS`.
- **Scripts (any step):** `hdiutil attach/detach` are deprecated. Move `attach-image.sh` to `diskutil image attach --noMount` if hdiutil goes away.
- **Step 18:** `ENABLE_HARDENED_RUNTIME` is already on; Developer ID signing and notarization are not set up.
