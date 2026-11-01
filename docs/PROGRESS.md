# Progress

## Status

| Field | Value |
|---|---|
| Current phase | 5 Ship (beta prepared) |
| Last completed step | 20 release prepared (1.0.0-beta.1, unpublished). 19 updates and uninstall. 18 packaging (unsigned DMG, Homebrew files). 17 robustness (owner-hardware items open). 15 disk-path speed (page cache, D-62). 14 menu-bar app and onboarding (owner's UI review left). 13 CLI `unlatch` and the privileged helper (2026-11-01; verified on the owner's real disk, read-only). 12 UnlatchCore; 11 M0 GO; 16 early |
| Next step | leftovers: Windows-made golden images, FSKit cleanup, CI; then the owner's beta |
| libntfs-3g | 2026.9.28 (`Engine/vendor/VERSION`) + patches 0001, 0002 |
| Tested on | macOS 27 (arm64), Xcode 27.2 (27B52), macFUSE 5.5.0, Swift 6.4.2, XcodeGen 2.46.0, Git LFS 3.8.1 |

### Risk register

| Risk | Where it is settled | Fallback | State |
|---|---|---|---|
| Read-write throughput too low | Step 11 gate, Step 15 | NO-GO review | settled at M0: GO like for like (D-52); disk path slow, Step 15 |
| libntfs-3g custom device quirks | Step 03 | Patch in `Engine/vendor/patches/` | settled: works unpatched, one EROFS workaround (D-14) |
| Root needed to open real disks | Step 12 S1 | privileged helper hands over a descriptor (D-57) | design settled; helper in Step 13 |
| Apple's NTFS driver wins the mount race | Step 12 S2 | Unmount-and-swap | settled: mount-approval dissent works (D-56) |
| Corruption | Steps 05–07, 10, 15, 17 | Read-only default for that scenario until fixed | open; 1,000 fsx seeds × 2,000 ops pass; Windows chkdsk not yet run (needs a VM) |
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

### Step 06: Randomized model test (fsx) and Windows oracle — done 2026-11-01 (oracle run in a Windows 11 VM)

**Done:**
- `unlatch-engine fsx <img> --seed N --ops M [--verbose]` (`EngineTool/fsx.c`), D-29:
  - Operations: create and mkdir (a name pool with case variants, Latin-1 and CJK names), writes at random offsets and sizes (up to 256 KiB, past EOF, small resident writes), truncate, rename (same dir, across dirs, case-only, replace, EXCL, into own subtree), remove (wrong type, non-empty), release, sync and remount.
  - The model predicts exact errnos.
  - Every op: the whole tree is compared, plus the touched file's contents. Every remount and the end: all contents, then a CLEAN probe.
- `Scripts/fsx-soak.sh` (parallel, APFS clones, keeps failures, appends to `Tests/fsx-regressions.txt`). `Scripts/test-engine.sh` runs the regression seeds.
- **Bug found by fsx (seeds 2 and 4):** ghosts were created in the removed file's parent, so `rmdir` of an apparently empty directory failed with ENOTEMPTY. Ghosts now live in the root (D-30).
- `Scripts/chkdsk-vm.sh` plus `Tests/oracle/mkvhd.c` (D-31): fixed-VHD wrapper (MBR partition at 1 MiB, hidden sectors patched), read-only `Mount-DiskImage`, `chkdsk` with no `/f`, report saved in `Tests/oracle/`. Automated over SSH, or as manual steps. Checked on macOS: the VHD attaches as `FDisk_partition_scheme` with a `Windows_NTFS` partition, and the volume carved back out probes CLEAN.
- `docs/testing.md`: how to run everything, the fsx design, and the Windows VM setup.

**Verify:**
```sh
Scripts/fsx-soak.sh --seeds 1000 --ops 2000   # "1000 of 1000 seeds passed in 516 s"
Scripts/fsx-soak.sh --regressions             # 2 of 2
Scripts/test-engine.sh                        # PASSED (includes the regressions)
Scripts/chkdsk-vm.sh --fsx 42                 # builds build/oracle/fsx-seed42-2000.vhd; needs WINDOWS_SSH to run chkdsk
```

**Left (blocked on the owner):** "at least one fsx-produced image passes Windows chkdsk". There is no VM software on this Mac (no UTM, Parallels, VMware or VirtualBox). Set up a Windows 11 Arm VM with OpenSSH Server (`docs/testing.md`), then run `WINDOWS_SSH=user@host Scripts/chkdsk-vm.sh --fsx 42` and commit `Tests/oracle/fsx-seed42-2000.chkdsk.txt`.

**Surprises:**
- fsx found the ghost-placement bug within five seeds; after the fix, 2 million random ops ran clean.
- Speed: about 2 seeds per second on 8 cores (debug build).

### Step 07: Fuzzing, golden images, CI — done 2026-11-01, except Windows-made images

**Done:**
- Fuzzing (D-32, D-33): `Tests/Fuzz/fuzz_probe.c` and `fuzz_open_ls.c`, libFuzzer with ASan and UBSan, against `Scripts/build-ntfs3g.sh --fuzz`. The input format is a sector list over a seed image (`Tests/Fuzz/README.md`, `mkseed.c`). `Scripts/fuzz.sh --time N` and `--regress`.
- **Upstream bug found by fuzzing:** an unchecked boot-sector sector count overflowed (UB). Fixed in `Engine/vendor/patches/0002-bootsect-reject-overflowing-sector-count.patch`; the input is kept in `Tests/Fuzz/crashes/`.
- Golden images (D-34): 7 images under Git LFS (`Tests/Images/*.img.zst`), each with a hand-written `.expect` (60 checks). They include synthetic `$LogFile` restart pages for clean, unclean and Fast Startup, which give the UNCLEAN_LOG and HIBERNATED probe paths their first fixtures. `Scripts/test-golden.sh`; `Tests/Images/make-golden.sh` regenerates.
- CI (D-35): `.github/workflows/engine.yml` on the `xcode-27` runner, with ntfs-3g cached.

**Verify:**
```sh
Scripts/fuzz.sh --time 120      # ~900k inputs per target, PASSED
Scripts/fuzz.sh --regress
Scripts/test-golden.sh          # 60 of 60
gh run list --workflow engine
```

**Left:** Windows-made golden images (NTFS formatted by Windows, a real Fast Startup hibernation, compressed and EFS files). They need the Windows VM (Step 06 "Left"); `Tests/Images/README.md` lists them.

**Surprises:**
- The first CI run hung for an hour in the fuzz build's `configure`: an ASan-built test program never exited on the runner. Sanitizer flags now go only to `make`, and the build step has a 15-minute timeout.
- The CI-fix commit also carried a staged deletion of the Step 08 placeholder, so CI built with no extension sources until Step 08 was committed.

### Step 08: FSKit module, read-only — code done 2026-11-01; mount UNTESTED (needs signing)

**Done:**
- `Extension/Sources/` (D-36):
  - `UnlatchFSExtension.swift`: the `@main` entry.
  - `UnlatchFileSystem.swift`: probe (clean → usable, dirty, hibernated or unclean log → usable-but-limited, corrupt → recognized), load (read-only unless CLEAN, writable and not `rdonly`), unload, `startCheck` (the safety probe), `startFormat` → ENOTSUP.
  - `NTFSVolume.swift`: the item table, attributes, lookup, enumerate with `.` and `..`, read; every write op → EROFS.
  - `BlockIO.swift`: `ue_io_t` over `FSBlockDeviceResource`, aligned bounce through one scratch block.
  - `Support.swift`: `posixError`, `engineName` (NFC), `EngineQueue`, probe helpers.
- `Shared/Sources/UnlatchCore/VolumeStatus.swift`: the App Group record `status/<serial>.json`, written at check and load.
- `project.yml`: the full `EXAppExtensionAttributes` (short name `unlatch`, media types `Windows_NTFS`, the GPT basic-data GUID, `Partitionless`), the engine linked through `module UnlatchEngineC`.
- Signing (D-37): `Config/Base.xcconfig` (ad-hoc, for CI) plus a git-ignored `Config/Local.xcconfig`.
- `docs/dev-setup.md`: signing, enabling, mounting, logs, and the Step 08 check table.

**Verify (now):**
```sh
Scripts/bootstrap.sh && xcodebuild -quiet -scheme Unlatch -derivedDataPath build/DerivedData build   # no warnings under Swift 6 strict concurrency
```

**Left:** every "Done when" item that needs a mount, listed in `docs/dev-setup.md` §5: listing, times, `df`, content hashes, and the Fast Startup image mounting read-only. Blocked on the owner:
1. A paid Apple Developer Program membership: a free Personal Team cannot carry the FSKit Module capability (D-38). Then put the paid team's ID in `Config/Local.xcconfig`.
2. Enable the extension in System Settings after the first signed install.

**Surprises:**
- `FSDirectoryVerifier` is a plain `UInt64` in Swift, not a wrapper type.
- With no attributes requested, FSKit still expects `.` and `..` to be packed.

### Step 08 (second version): mount daemon on macFUSE — done 2026-11-01 (kext backend; FSKit backend blocked upstream)

A free Personal Team cannot sign an FSKit module (D-38), and the owner chose not to buy a membership, so the plan moved to macFUSE's FSKit backend (D-39). Plan Steps 08–11 are rewritten; Steps 12–20 are re-scoped when each starts.

**Done:**
- `Fuse/` → `unlatch-fuse` (D-40): libfuse3 low-level ops with the Darwin extensions over the engine. Probe and safety check before mounting (CORRUPT/NOT_NTFS refused, everything else read-only in this step), lookup counts with `ue_release` at zero, readdir with `.`/`..` as offsets 1 and 2, `UF_HIDDEN` and birth times, NFC names through CoreFoundation, `statfs`, and `EROFS` for every write op.
- `EngineTool/file_io.c` serves block devices too (block size and count via `DKIOCGETBLOCK*`, aligned bounce).
- Status records moved to `~/Library/Application Support/Unlatch/status/` (D-41), written by `Fuse/status.c` in `VolumeStatus` JSON.
- `Tests/Fuse/check-readonly.sh <img> <mnt>`: every directory listing and every file hash against the harness, `df` against `info`, writes refused.
- `docs/dev-setup.md` rewritten for macFUSE.
- `Tests/Fuse/macfuse-repro/`: a 60-line libfuse3 file system that reproduces the macFUSE bug below without Unlatch.

- Default backend is macFUSE's kext (D-43); `--backend fskit` stays for when D-42 is fixed. Relative mount points are made absolute.

**Verify** (kext backend; macFUSE 5.5.0 library, 5.4.0 kext loaded):
```sh
xcodebuild -quiet -scheme Unlatch -derivedDataPath build/DerivedData build
F=build/DerivedData/Build/Products/Debug/unlatch-fuse
zstd -d Tests/Images/content-ntfs3g.img.zst -o build/c.img; mkdir -p build/m
$F -r build/c.img build/m && Tests/Fuse/check-readonly.sh build/c.img build/m && umount build/m
#   "10 directories, 308 files compared, 0 failures" (listings, sizes, hashes incl. 256 MiB sparse, df, EROFS)
disk=$(Scripts/attach-image.sh build/c.img); $F -r /dev/$disk build/m && Tests/Fuse/check-readonly.sh build/c.img build/m; umount build/m; hdiutil detach $disk
#   same result through the block device, no sudo
zstd -d Tests/Images/logfile-faststartup-synthetic.img.zst -o build/fs.img; $F build/fs.img build/m
cat ~/Library/Application\ Support/Unlatch/status/54DDA118567EC0ED.json   # "state" : "hibernated", "readOnly" : true, "mounted" : true
umount build/m                                                            # record then says "mounted" : false
swift test --package-path Shared                                          # decodes the daemon's JSON
```
`stat` times match the harness (`unlatch-engine stat`), and `mount` shows `read-only`.

**FSKit backend (D-42):** the same checks pass except reads. macFUSE's FSKit backend (5.4.0 and 5.5.0, macOS 27) fails uncached reads of 256 KiB or more with `EIO` although the daemon's reply is complete, and 5.4.0 silently returned junk for ranges it never asked the daemon for. `Tests/Fuse/macfuse-repro/` reproduces it with no Unlatch code. Rapid mount/unmount cycles also switch the extension off. Reporting it upstream needs the owner's OK (it's a public post).

**Surprises:**
- The FSKit backend mounts only under `/Volumes`, ignores `ro`, and ignores `max_read`/`max_readahead`.
- `mount_macfuse` resolves a relative mount point against `/`.
- The golden images share one serial (54DDA118567EC0ED), so their status records overwrite each other.
- macFUSE's FSKit extension was not registered with pluginkit until the first mount attempt registered it.

### Step 09: Read path: attributes, Finder, read speed — done 2026-11-01

**Done:**
- Logging (D-44): `ue_set_log` in the seam; libntfs-3g messages → OSLog `engine`, daemon messages → OSLog `mount` (and stderr in the foreground or before detaching).
- Backgrounding without fork (D-45): `posix_spawn` of `-f --ready-fd`; the parent waits until the mount is really there, so `ls` right after `unlatch-fuse` returns sees the files. Mount, unmount and failures are all logged.
- Reads: one reused buffer; `st_blksize` 1 MiB so the kext sends 1 MiB reads (was 16 KiB); disks opened through `/dev/rdiskN`.
- Attributes: mode without `w` (read-only mount), uid/gid of the mounting user, size, allocated size (`st_blocks`), nlink, all four times, `UF_HIDDEN` except on the root, file IDs (root → 2), generation = sequence number.
- Directory offsets: the engine's cookies are index positions (`pos + 1`). Stable while the volume is read-only; under writes, removing an earlier entry shifts later positions (see carry-over for Step 10).

**Verify:**
```sh
F=build/DerivedData/Build/Products/Debug/unlatch-fuse
$F -r build/c.img build/m && Tests/Fuse/check-readonly.sh build/c.img build/m; umount build/m    # 308 files, 0 failures
log show --last 5m --info --predicate 'subsystem == "io.github.ramanan-bala.unlatch"'          # mounted / unmounted lines
# Finder fixture: hidden.txt hidden, the rest visible, root not hidden (Finder confirmed by the owner)
img=$(Scripts/make-image.sh build/finder 64m FINDER | tail -1)
printf 'mkdir /Folder\nfile /visible.txt 2000 1\nfile /hidden.txt 100 2\nflags /hidden.txt hidden\n' | build/test-engine/mkfixture $img
$F $img build/m && ls -laO build/m && qlmanage -t -s 128 -o /tmp build/m/visible.txt; umount build/m
```

**Read speed** (512 MiB random file, 1 GiB image, cold mount each time, image in the host cache, median of 5):

| Path | Throughput |
|---|---|
| Raw `/dev/rdiskN`, `dd bs=1m` / `bs=4m` | 2.76 / 3.25 GB/s |
| Mount from the image file, `dd bs=4m` | 3.92 GB/s |
| Mount from `/dev/diskN` (opened as rdisk), `dd bs=4m` | 1.59 GB/s (58% of raw at 1 MiB) |
| Same, before D-45 (16 KiB reads, buffered disk node) | 0.37–0.95 GB/s |
| Cached re-read through the mount | 1.97 GB/s |

`purge` needs root, so "cold" means a fresh FUSE mount; the image file itself stays in the host's cache.

**Surprises:**
- macFUSE's kext sizes reads from `st_blksize`, not `iosize` or `f_iosize`.
- The NTFS root is hidden+system.

### Step 10: Write path through FUSE — done 2026-11-01 (git check since Step 16; Windows chkdsk passed)

**Done:**
- Engine: failed state (D-46). A failed device write, or ENXIO/ENODEV on a read, makes every later call return EIO (`ue_iget`, `ue_sync`, the device ops); `ue_close` deletes no ghosts and reports EIO; `ue_has_failed` in the seam. Test `test_device_failure`: the device turns read-only under a mounted engine.
- Daemon: read-write when CLEAN, writable and no `-r` (D-46). `write`, `create`, `mkdir`, `mknod` (regular files), `unlink`/`rmdir` (open files live on as ghosts until forget), `rename` (EXCL; SWAP → ENOTSUP), `setattr` (size, times incl. birth, mode → DOS read-only, `UF_HIDDEN`, chown as a no-op), `flush`, `fsync`/`fsyncdir`. Symlinks and hard links → ENOTSUP. Close logs an EIO from `ue_close` ("may need Windows chkdsk").
- `FUSE_DARWIN_CAP_CASE_INSENSITIVE` and `RENAME_EXT` (D-47); the first fixed case-only renames.
- `Tests/Fuse/rw-suite.sh [--runs N] [--backend] [--git]`.

**Verify:**
```sh
UNLATCH_SKIP_BIG=1 Scripts/test-engine.sh      # includes test_device_failure
Tests/Fuse/rw-suite.sh --runs 3                # "3 run(s) passed", ~23 s
#   per run: fresh 2 GiB image; cp -R, rsync -a, ditto of Engine/vendor/ntfs-3g and a 300 MiB file, all by SHA-256;
#   mv, case-only rename, rm -rf, truncate grow/shrink (zero tail), touch -t, chflags hidden, read after unlink;
#   unmount, probe CLEAN, ntfsfix -n; read-only remount and every hash again
F=build/DerivedData/Build/Products/Debug/unlatch-fuse
zstd -d Tests/Images/dirty-ntfs3g.img.zst -o build/d.img && $F build/d.img build/m   # "read-only (dirty ...)"; writes fail EROFS
```

**Left:**
- The git clone check: passes since Step 16 (native xattrs, D-48); now part of the default suite.
- Windows `chkdsk /scan` on a suite image: needs the Windows VM (Step 06).
- Surprise removal through a real mount: `hdiutil detach -force` refuses while the daemon holds the disk ("Resource busy"), so only the engine test covers it. Try a USB stick in Step 17.

**Surprises:**
- Without the case-insensitive capability, `mv README readme` succeeds and does nothing.
- libntfs-3g's readdir is in index-block order, not collation order.
- macOS 27 tags every new file with `com.apple.provenance`.

### Step 16: Xattrs, Finder metadata, links, flags — done 2026-11-01 (brought forward after Step 10, D-47); Windows check done, symlink finding D-55

**Done:**
- Engine xattrs (D-48): `ue_xattr_list/get/set/remove` over named streams, CREATE/REPLACE flags, positioned resource-fork writes, `:` ↔ U+F03A, 64 MiB cap. Daemon: `setxattr`/`getxattr`/`listxattr`/`removexattr` with macFUSE's position argument. macOS no longer writes `._` files.
- Links (D-49): `ue_symlink` (WSL format), `ue_readlink` (native, junction, WSL, Interix), `ue_link`; symlinks and junctions report `UE_T_LNK`. Daemon: `symlink`, `readlink` (absolute targets under the mount point), `link`.
- Flags (D-50): `chflags hidden` ↔ hidden; `chflags uchg` sets read-only, shown as mode only.
- `mkfixture reparse PATH symlink-rel|symlink-abs|junction TARGET` writes reparse points as Windows does ([MS-FSCC] layouts); `test-engine.sh` builds `reparse.img` with all four kinds.
- Tests: `test_xattr`, `test_links`, `test_windows_links`; `rw-suite.sh` now checks xattrs, `cp -p`, `ln -s`, `ln`, uchg, plain `rm -r`, no `._` anywhere, a git clone with `fsck --strict`, and xattrs and links again after a read-only remount.

**Verify:**
```sh
UNLATCH_SKIP_BIG=1 Scripts/test-engine.sh     # test_xattr, test_links, test_windows_links; 10 written images CLEAN
Tests/Fuse/rw-suite.sh --runs 3               # "3 run(s) passed" (~23 s), git clone + fsck included
Scripts/fuzz.sh --time 60                     # PASSED (ue_fill_attr now reads reparse and system-file data)
swift Tests/Fuse/tags.swift <apfs-file> <mount>/tagged.txt copy   # tags Red, Work, copies with FileManager
umount <mount>; unlatch-fuse <img> <mount>; swift Tests/Fuse/tags.swift /dev/null <mount>/tagged.txt   # same tags
```
Tagged copy: `FileManager.copyItem` of a file tagged Red and Work onto the mount kept both tags after unmount and remount, with no `._` file.

**Left:** "Windows reads the results" (symlinks and ADS made here, opened on Windows) waits for the Windows VM, like Steps 06 and 07. A Finder drag of a tagged and commented file was not done by hand; the `copyfile` path Finder uses was.

**Surprises:**
- `ue_xattr_list` first skipped every small xattr: `lowest_vcn` overlaps the value header of resident attributes.
- Showing read-only as `uchg` broke git, because git deletes its own 0444 pack files.

### Step 11: Benchmark harness and M0 gate — done 2026-11-01: **GO**

**Done:**
- `Bench/io.c` (timed primitives), `Bench/run.sh` (median of N, JSON into `Bench/results/`, `--remount` for cold reads, `--device` for raw reads), `Bench/baselines.sh` (APFS, exFAT, stock ntfs-3g, Unlatch on the image file, Unlatch on the disk, each on a 4 GiB image), `Bench/compare.py` (markdown table), `Bench/README.md`.
- `Scripts/build-ntfs3g.sh --driver`: stock ntfs-3g and lowntfs-3g against macFUSE's libfuse 2, a baseline only. The sanitizer condition is fixed (it applied to any variant).
- `unlatch-fuse` back to the buffered `/dev/diskN` (D-51).
- `docs/M0-gate.md`: numbers, thresholds and verdict (D-52).

**Verify:**
```sh
Scripts/build-ntfs3g.sh --driver
Bench/baselines.sh            # ~25 min on this 8 GiB M2; or name setups: Bench/baselines.sh --runs 3 unlatch
Bench/compare.py
```

**Left:** the disposable external SSD run (owner's drive), and the larger-than-RAM `mkfile` test.

**Surprises:**
- Stock ntfs-3g refuses block devices for an unprivileged user with an external libfuse.
- An attached image is locked against other openers.
- An APFS image attaches as a physical store plus a separate volume device.
- `diskutil unmount` has no `-quiet`.
- macOS bash 3.2 has no associative arrays.
- An earlier run went slowly because `rm -rf` of 30,000 files per run sits outside the timed part and the Mac was swapping; it was not a hang.

### Real-disk findings (owner's NTFS disk, read-only) — 2026-11-01

- Mounting a real disk read-only needed `sudo`: device nodes are `root:operator` 0640 (D-53). Step 12's helper should do this.
- The volume probed clean, but a directory listed as empty: one index entry points to MFT record 352642, which reads as zeros. The listing failed as a whole; it now skips that entry (D-54).
- Not yet known: whether that record is stale on the disk (Windows `chkdsk` would tell) or a misread of a far, fragmented `$MFT` record. The owner can check, read-only: `sudo build/ntfs3g/bin/ntfsinfo -i 352642 /dev/diskNs1`.

### Windows VM checks — 2026-11-01

UTM VM (Windows 11 24H2, Arm), SSH through a port forward (`WINDOWS_SSH=vagrant@127.0.0.1 WINDOWS_SSH_PORT=2222`).

- **Step 06:** `Scripts/chkdsk-vm.sh --fsx <seed>` for seeds 1, 2, 3, 4, 7, 42 and 100 (2,000 ops each): "Windows has scanned the file system and found no problems". Reports are in `Tests/oracle/`.
- **Step 10:** `Scripts/chkdsk-vm.sh build/rw-suite/run.img rw-suite` after a suite run: passed.
- **Step 16:** `Tests/oracle/windows-check.ps1` on the same volume:
  - file data, the `com.example.note` stream, a Finder-tag stream (U+F03A name) and `com.apple.provenance` are all visible;
  - the hard link shows 2 links, the hidden flag maps to Hidden, and all 378 git-clone files are present;
  - the WSL symlink is a reparse point that Windows programs do not follow (D-55).

```sh
export WINDOWS_SSH=vagrant@127.0.0.1 WINDOWS_SSH_PORT=2222
Scripts/chkdsk-vm.sh --fsx 42
Tests/Fuse/rw-suite.sh --runs 1 && Scripts/chkdsk-vm.sh build/rw-suite/run.img rw-suite
```

### Step 12: UnlatchCore: DiskWatcher, StatusStore, MountService — done 2026-11-01

Re-scoped for macFUSE (D-39): there is no `FSClient.mountSingleVolume` and no mount entitlement, so S1 becomes "who opens the device".

**Done:**
- Spikes: S2 (dissent stops Apple's mount, D-56) and S1 (a privileged fd-passing helper, D-57).
- `unlatch-fuse --fd N` uses a device opened by someone else (`file_io_from_fd`), and accepts a `/Volumes/<name>` that does not exist yet (D-58).
- `UnlatchCore`:
  - `DiskInfo`: DiskArbitration plus the mount table. NTFS is matched by volume kind, or by an `Windows_NTFS`/Basic Data content hint when the kind is missing. The owner is Apple, Unlatch (`fsname unlatch@<bsd>`), another driver, or none.
  - `DiskWatcher`: an `AsyncStream` of appeared, disappeared and changed events; an optional `claim` dissents mounts; callbacks are unregistered and the context released in `stop()`, which is safe to call from the stream's termination.
  - `StatusStore`: `all`, `latest(bsdName:)`, `changes()` (a dispatch source on the status directory).
  - `MountService` (an actor):
    - `list`, `disk`, `status`;
    - `mount` unmounts Apple's driver first and gives the volume back with `DADiskMount` if Unlatch fails;
    - `unmount` (`unmount(2)` for Unlatch, DiskArbitration otherwise; force only on request), `eject` (whole disk);
    - `runDaemon` spawns `unlatch-fuse --fd 3` with only fds 2 and 3 open.
  - `AutoMountPolicy`: external, removable or ejectable by default; per-volume allow or deny by volume UUID.
  - `MountError`: user-facing reasons, including macFUSE missing and helper needed.
- Tests: unit tests (policy, status store, mount-point names), plus integration tests run with `UNLATCH_INTEGRATION=1` on attached images.

**Verify:**
```sh
xcodebuild -quiet -scheme Unlatch -derivedDataPath build/DerivedData build
swift test --package-path Shared                              # unit tests; integration ones skipped
UNLATCH_INTEGRATION=1 swift test --package-path Shared        # 8 of 8: list, mount rw, write, unmount, remount ro, eject;
                                                              # Apple's mount restored after a forced failure; watcher sees an attach
```

**Left:** the privileged helper (D-57) is Step 13 work, so real disks still mount only with `sudo unlatch-fuse` (D-53). The auto-mount loop that joins `DiskWatcher` to `MountService` belongs to whichever process runs it: the app (Step 14), or a CLI daemon mode in Step 13.

**Surprises:**
- `DADiskGetBSDName` points into the `DADisk`; reading it from a temporary whole-disk object gave garbage names and `kDAReturnBadArgument`.
- A stray copy of the stderr pipe's write end in the spawned daemon made reading it hang.
- Finishing an `AsyncStream` runs its termination handler synchronously, which re-entered `stop()` and trapped in `queue.sync`.

### Step 13: CLI `unlatch` and the privileged helper — done 2026-11-01

**Done:**
- `Helper/main.c` → `io.github.ramanan-bala.unlatch.helper` (D-57). A launchd socket job; it accepts only the console user, only `diskN`/`diskNsM` names, and only volumes DiskArbitration calls NTFS. It opens the node and returns the descriptor over `SCM_RIGHTS`, and exits after 30 s idle.
- UnlatchCore: `HelperOpener` (the client) and `AutoOpener` (direct, then the helper), which is now `MountService`'s default.
- `CLI/`: the `UnlatchCLI` library plus a thin `unlatch` executable (D-59):
  - `list`, `mount`, `unmount`, `eject`, `status`, each with `--json` where it makes sense;
  - `doctor`, `install-helper`, `uninstall-helper`;
  - exit codes 0 to 4, and `docs/cli.md`.
- Tests: argument parsing, disk resolution, exit codes, and a JSON schema-1 snapshot.

**Verify:**
```sh
xcodebuild -quiet -scheme Unlatch -derivedDataPath build/DerivedData build     # unlatch-fuse, the helper
swift test --package-path CLI                                                  # 4 of 4
U=CLI/.build/debug/unlatch
d=$(Scripts/attach-image.sh build/cli.img)          # an image from make-image.sh
$U list; $U mount CLITEST; $U status $d --json | jq; $U unmount /Volumes/CLITEST; $U eject $d
zstd -d Tests/Images/dirty-ntfs3g.img.zst -o build/d.img; $U mount $(Scripts/attach-image.sh build/d.img); echo $?   # 3, with the reason
$U mount; echo $?                                    # 2
$U doctor
```

**Real disk (owner's Seagate, read-only):** `sudo unlatch install-helper`, then Full Disk Access for the helper (D-60), then `unlatch mount seagate -r` mounted it at `/Volumes/seagate` with no sudo. Apple's driver had it before and got it back after each failed attempt.

**Left:**
- An auto-mount loop (`DiskWatcher` plus `MountService` plus `AutoMountPolicy`); the app runs it in Step 14.

**Surprises:**
- ArgumentParser's `CommandError` is not public, so parse errors are caught on their own to map them to exit 2.
- The macFUSE kext's bundle ID carries a macOS-major suffix (`.25`).
- The installed macFUSE is still 5.4.0, although Homebrew lists 5.5.0.

### Step 14: Menu-bar app and onboarding — done 2026-11-01, except the owner's UI review

**Done so far:**
- `App/Sources`: `AppModel` (volumes, the auto-mount loop via `DiskWatcher(claim:)`, notifications), `MenuView` (a row per volume with a state badge and actions: mount read-write or read-only, unmount, eject, show in Finder, Always/Never auto-mount), `OnboardingView` and `Checks`, `SettingsView` (auto-mount scope, notifications, log export).
- `AutoMountPolicy`: an `enabled` master switch and an `includeInternal` scope.
- The app bundles `unlatch-fuse`, the helper and the CLI in `Contents/Helpers`. Not `Contents/MacOS`: on a case-insensitive disk the CLI `unlatch` overwrote the app's own `Unlatch`. The FSKit module is no longer embedded. There is an Xcode `unlatch-cli` target.
- `App/Resources/selftest.img`: a 2 MiB NTFS volume, label UNLATCHTEST, with a marker file, excluded from auto-mount. The onboarding self-test mounts it read-only, reads the marker and ejects it. Verified from the log.

**Verified:**
- With the app running, `hdiutil attach` of a clean NTFS image: Apple's mount is refused ("no mountable file systems"), and the app mounts it read-write at `/Volumes/AUTOTEST`.
- A dirty image mounts read-only at `/Volumes/CONTENT` with its reason.
- The owner used the app on the real Seagate: a 30 GB copy from the Mac onto it in under 5 minutes.

**Left:**
- The owner's look at the menu, onboarding and Settings: eject from the menu, badges, wording, VoiceOver.
- `chkdsk /scan` of the Seagate on Windows after its first real write.
- Removing the stale `/Volumes/UnlatchTest` from the FSKit tests (`sudo rmdir`).

### Step 15: Speed on the disk path (re-scoped, D-62) — done 2026-11-01

Re-scoped: there is no kernel-offloaded I/O through macFUSE, so the step became I/O-size, caching and threading work on the disk path (D-62, D-64).

**Done:**
- Engine page cache (`Engine/src/bcache.c`, `UE_OPEN_CACHE`): 4 KiB pages, 32 MiB, read-ahead, coalesced write-back, direct transfers of 64 KiB and more, cold file data, clean-first eviction. Seam: `ue_writeback`, `ue_get_cache_stats`. A failed write-back is a device failure (D-46).
- `unlatch-fuse`: opens `/dev/rdiskN` for `/dev/diskN`; a write-back timer thread (pages 1 s old) under the request lock; the volume closes in FUSE `destroy`, so `umount` returns after everything is on the disk; cache statistics logged at unmount; `--no-cache`.
- Helper and `MountService` hand over the raw node.
- Ghosts are HIDDEN only (D-63); that was the cause of a 10× small-file regression with the cache.
- Tests: `test_cache_model` (20,000 random ops against an in-memory device with a 64-page cache and a short last page; cold mode; coalescing), `test_cache_engine`, and every write test runs a second time with the cache on. `Tests/Fuse/rw-suite.sh --disk` runs the suite on an attached disk.

**Verify:**
```sh
xcodebuild -quiet -scheme Unlatch -derivedDataPath build/DerivedData build
UNLATCH_SKIP_BIG=1 Scripts/test-engine.sh          # test_cache_model, test_cache_engine, "write tests again, with UE_OPEN_CACHE"
Scripts/fsx-soak.sh --regressions
Tests/Fuse/rw-suite.sh --runs 3                    # image files
Tests/Fuse/rw-suite.sh --runs 2 --disk             # attached disks: rdisk + cache
UNLATCH_INTEGRATION=1 swift test --package-path Shared; swift test --package-path CLI
Bench/baselines.sh --runs 3 unlatch                # numbers in docs/M0-gate.md
```

**Left:**
- The full benchmark rerun (all setups), deferred to the end at the owner's request.
- Windows chkdsk of a suite image written through the cache (needs the VM; asked for with Step 17).
- The installed helper on the owner's Mac still opens the buffered node until `sudo unlatch install-helper` is run again; `unlatch-fuse` works with either.

**Surprises:**
- The first cache version halved small-file speed. Tracing the misses showed a cyclic scan of 14,000 pages: ghosts flagged SYSTEM, re-read by libntfs-3g's readdir (D-63).
- Polling the macFUSE session fd delays requests (D-64).
- Without closing in `destroy`, `umount` returned while dirty pages were still being written; a probe right after it raced the daemon.

### Step 17: Robustness and the manual matrix — done 2026-11-01, except items needing owner hardware

Re-scoped for macFUSE and this Mac (D-65). Results are in `docs/test-matrix.md`: no open data-loss or corruption issue.

**Done:**
- Crash leftovers (D-65): root sweep of ghosts and rename temp names on read-write open; temp-name ids seeded from the clock. Fixture `leftover.img`, `test_crash_leftovers`.
- Native Windows symlinks for relative on-volume targets (D-67), file and directory; WSL links otherwise; directory symlinks remove and rename as links. `test_native_links`, reparse-tag checks in `test-engine.sh`, and Windows follows them (VM).
- Directory listings from a per-handle snapshot (D-66); `Tests/Fuse/readdir-writer.c` (in the rw-suite) went from 32 duplicates to 0.
- Dead mounts after a killed daemon are force-unmounted by `MountService.list()` (D-68); the daemon records its pid. `unmount` retries EBUSY for 3 s.
- Fixed on the way: the daemon's status records had `bsdName` "unlatch-fuse" (a `basename()` static buffer); images made in the same second shared a serial (`make-image.sh` now randomizes it); status records with equal timestamps sort mounted-first.
- `Tests/Fuse/interrupt.sh` (kill, force-eject), rw-suite "names" step (NFC/NFD, emoji, 255/256, reserved, case-only), directory and absolute symlinks in the suite, capacity runs, Windows chkdsk of a cache-written suite image (passed) and `windows-check.ps1` (fixed a stray line).

**Verify:**
```sh
UNLATCH_SKIP_BIG=1 Scripts/test-engine.sh          # test_crash_leftovers, test_native_links, reparse tags
Tests/Fuse/rw-suite.sh --runs 3; Tests/Fuse/rw-suite.sh --runs 2 --disk
Tests/Fuse/interrupt.sh                            # kill passes; detach reports SKIP for images
UNLATCH_INTEGRATION=1 swift test --package-path Shared   # deadDaemonMountIsReaped
export WINDOWS_SSH=vagrant@127.0.0.1 WINDOWS_SSH_PORT=2222
Tests/Fuse/rw-suite.sh --runs 1 && Scripts/chkdsk-vm.sh build/rw-suite/run.img rw-suite-step17
```

**Left (owner hardware):** USB pull mid-copy, sleep and wake, 50 GB file and 1M-file tree on a real disk, the 24-hour soak. All are open rows in `docs/test-matrix.md`.

**Surprises:**
- macOS will not force-eject an attached image while a process holds it, so surprise removal cannot be simulated with images.
- macFUSE keeps a dead daemon's mount; `statfs` still works on it, directory reads fail with ENXIO.
- Swift Testing runs the integration tests in parallel, which exposed the shared serials.

### Step 18: Packaging without a Developer ID (re-scoped, D-69) — done 2026-11-01, except a clean-Mac check

**Done:**
- `VERSION` plus `Scripts/set-version.sh` (and `--check`, now in CI).
- `Scripts/release.sh`: Release build, ad-hoc signing inside out with the hardened runtime (`Packaging/unlatch-fuse.entitlements`: library validation off for libfuse3), DMG (app, Applications link, Read Me First, Notices), source tarball, `SHA256SUMS`, rendered Homebrew files. Publishes nothing.
- Notices (`Scripts/make-notices.sh`): GPL-3.0, libntfs-3g COPYING, COPYING.LIB, AUTHORS, CREDITS, swift-argument-parser's license (`Packaging/notices/`), written source offer; in the app (Settings > About > Licenses and source), the DMG, and the formula's docs.
- Homebrew tap templates (`Packaging/homebrew/`): cask (depends on the macfuse cask; uninstall unmounts and removes the helper; zap) and formula (built with the new `Scripts/build-tools.sh`).
- `.github/workflows/release.yml`: on a `v*` tag, a draft release with the files and the CHANGELOG section. `docs/release.md`, `docs/install.md`, `docs/release-notes-footer.md`, `CHANGELOG.md`.
- Fixes found on the way: `unlatch --help` exited 1 with "CommandError error 1"; the CLI and `MountService` looked for `unlatch-fuse` next to `argv[0]`, which is only `unlatch` when run from PATH; new `unlatch unmount --all`.

**Verify:**
```sh
Scripts/set-version.sh --check
Scripts/release.sh --allow-dirty          # "Gatekeeper says: ... rejected" is expected
hdiutil attach -nobrowse -readonly build/release/0.0.0/Unlatch-0.0.0.dmg   # app, Notices, Read Me First
<dmg>/Unlatch.app/Contents/Helpers/unlatch-fuse <img> <dir>                # mounts read-write
```
Formula: built and installed from a `file://` tarball in a throwaway local tap; `brew test`, `brew style` and `brew audit` pass for the formula and the cask. The cask was audited but not installed, because its uninstall step would remove the helper installed on this Mac.

**Left:**
- "Works on a clean Mac": needs a second Mac or a macOS VM (none here). On this Mac the DMG's app starts and its daemon mounts read-write; Gatekeeper rejects the ad-hoc app as expected, and docs/install.md covers Open Anyway.
- Creating the tap repository and publishing anything: the owner's (docs/release.md).

**Surprises:**
- With the hardened runtime, an ad-hoc binary cannot load another team's library; Debug builds hid this because their helpers are re-signed without the runtime flag.
- `[ … ] && …` as the last command of a loop body stops a `set -e` script.

### Step 19: Updates and uninstall (re-scoped, D-70) — done 2026-11-01, except the owner's click-through

**Done:**
- `UnlatchCore.UpdateChecker` and `Version` (semantic-version order, pre-releases only for pre-release users); tests `versionOrder`, `updateOffer`.
- App (`App/Sources/Lifecycle.swift`): daily update check (Settings > Updates, "Check Now"); menu item "Unlatch X is available…", which offers to unmount first, then gives the Homebrew command or opens the release page; "Uninstall Unlatch…" (unmount, helper removal with the admin password, login item, status files, defaults, app to the Trash); `AppModel.stop()`.
- CLI `unmount --all`, used by the cask's uninstall step (so `brew upgrade` and `brew uninstall` unmount first).
- `docs/updating.md`, `docs/uninstall.md`.

**Verify:**
```sh
swift test --package-path Shared                         # versionOrder, updateOffer
xcodebuild -quiet -scheme Unlatch -derivedDataPath build/DerivedData build
U=CLI/.build/debug/unlatch; $U mount <a>; $U mount <b>; $U unmount --all   # both unmounted, exit 0
```

**Left:**
- The owner's click-through of the menu item and of "Uninstall Unlatch…" (it removes the helper installed on this Mac, so it was not run here). An update from vN to vN+1 can only be tried once two releases exist.

### Step 20: Beta and release preparation (re-scoped, D-71) — prepared 2026-11-01; publishing is the owner's

**Done:**
- README rewritten for users (requirements, install, use, what works, limitations, building, roadmap after 1.0, licenses).
- `docs/read-only.md` (every read-only reason and its fix; the app's notifications link to it), `docs/troubleshooting.md`, `SECURITY.md`, `docs/beta.md` (testers, disks, tasks, triage, 1.0 exit criteria), `.github/ISSUE_TEMPLATE/`, CONTRIBUTING's pre-PR checks.
- `CHANGELOG.md` with the `1.0.0-beta.1` section; version set to `1.0.0-beta.1` everywhere (`Scripts/set-version.sh`).
- Trial `Scripts/release.sh` at the new version: DMG, tarball, checksums, tap files.

**Verify:**
```sh
Scripts/set-version.sh --check                       # 1.0.0-beta.1
Scripts/release.sh --allow-dirty && ls build/release/1.0.0-beta.1
awk -v v=1.0.0-beta.1 '$0 ~ "^## \\[" v "\\]" {on=1; next} /^## \[/ {on=0} on' CHANGELOG.md   # the release notes
```

**Left (owner, in this order; docs/release.md):** push a signed tag `v1.0.0-beta.1` (the workflow makes a draft pre-release), publish the draft, create and push the tap `Ramanan-Bala/homebrew-unlatch`, enable private vulnerability reporting in the repository settings, recruit testers (docs/beta.md). v1.0.0 follows the beta's exit criteria.

## Carry-overs

- **Step 04+:** the ntfs-3g build sets no `--enable-debug`. Decide whether Debug engine builds want ntfs-3g debug logging. For now the log handler is null (D-16).
- **Step 07 (VM available now):** Windows-made golden images (`Tests/Images/README.md`).
- **Owner:** report the macFUSE FSKit read bug upstream with `Tests/Fuse/macfuse-repro/` (public post; needs the owner's OK). When fixed, rerun Step 08's checks with `--backend fskit` and switch the default back (D-43).
- **Step 14:** the app installs the helper (or tells the user to run `sudo unlatch install-helper`) and runs the auto-mount loop with `DiskWatcher(claim:)`.
- **Upstream:** send patches 0001 and 0002 to tuxera/ntfs-3g (a public PR or issue; needs the owner's OK).
- **Scripts (any step):** `hdiutil attach/detach` are deprecated. Move `attach-image.sh` to `diskutil image attach --noMount` if hdiutil goes away.
