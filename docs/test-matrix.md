# Test matrix (plan Step 17)

Each row: what was tried, what should happen, what did, when and on which build. Build is the commit the row was run on; `step 17` means the Step 17 commit. Severity of open items: **S1** data loss or corruption, **S2** wrong behaviour without data loss, **S3** cosmetic or tooling.

Machine: Apple M2, 8 GiB, macOS 27.0.1, macFUSE 5.4.0 (kext backend), Windows 11 24H2 Arm VM in UTM for the Windows checks.

## Automated (re-run on every engine or daemon change)

| Scenario | Expected | Actual | Date | Build |
|---|---|---|---|---|
| Engine unit tests, both without and with the page cache (`Scripts/test-engine.sh`) | all pass, every written image CLEAN and `ntfsfix -n` clean, no leaks | pass (24 written images) | 2026-11-01 | step 17 |
| Page-cache model test: 20,000 random reads, writes, write-backs against an in-memory device, 64-page cache, short last page, cold mode | every read matches a shadow copy; device equals it after write-back; all transfers sector-aligned | pass | 2026-11-01 | step 17 |
| fsx regressions and random seeds (`Scripts/fsx-soak.sh`) | no model mismatch | pass | 2026-11-01 | step 17 |
| Fuzzers, 60 s each (`Scripts/fuzz.sh`) | no crash | pass | 2026-11-01 | step 15 |
| Read-write suite on image files, 3 runs (`Tests/Fuse/rw-suite.sh`) | every hash, xattr, link, name and listing check passes; CLEAN after unmount | pass | 2026-11-01 | step 17 |
| Same suite on attached disks (`--disk`: raw node and page cache) | same | pass | 2026-11-01 | step 17 |
| UnlatchCore integration tests (mount, unmount, eject, Apple's mount restored, watcher, dead-daemon reaping) | pass | 9 of 9 | 2026-11-01 | step 17 |
| CLI tests | pass | 4 of 4 | 2026-11-01 | step 17 |

## Real disk (`Scripts/test-real-disk.sh`)

SanDisk Cruzer Blade 16 GB, USB 2.0 (raw: 33 MB/s read, 6.7 MB/s write), GPT, NTFS made by Windows. Unlatch 1.0.0-beta.6 (build `636816e`), macFUSE 5.5.0, both backends. Reports in `build/real-disk/`.

| Scenario | Expected | Actual | Date | Build |
|---|---|---|---|---|
| Read-write suite on the stick, kext backend (`rw-suite.sh --device`, through the helper and the CLI) | every check passes; CLEAN after unmount | pass, 21 of 21 steps (3 min 47 s), incl. git clone + fsck, second mount through the helper refused | 2026-11-02 | 636816e |
| Same, FSKit backend | same | pass | 2026-11-02 | 636816e |
| 4.5 GiB file (over FAT32's limit) | same SHA-256 after a read-only remount | pass; written in 865 s (5 MiB/s) | 2026-11-02 | 636816e |
| 10,000 small files in 100 folders | same after a remount; space back after `rm -r` | pass | 2026-11-02 | 636816e |
| `kill -9` of the daemon mid-copy on the stick | probe not CORRUPT; remount read-write; earlier file intact | probe CLEAN, read-write, 208 files of the copy present, earlier file intact (before D-86: CLEAN was wrong, see below) | 2026-11-02 | 636816e |
| Same with the dirty mark (D-86, `--cut`) | DIRTY; read-only; earlier file intact; the check finds what chkdsk must fix | DIRTY, read-only ("run chkdsk on Windows before writing to it"), 180 files of the copy present, earlier file intact. The check found `unlatch-test/copy/2` and `copy/3` unreadable (index blocks never written): the stick needs chkdsk, as Windows would say | 2026-11-02 | 84a7224 |
| The owner pulled the stick mid-copy (by hand, before D-85/D-86) | daemon exits; next mount safe | **failed, fixed**: the daemon stayed 33 s and blocked the next mount (D-85); the stick probed CLEAN after libntfs-3g logged inconsistent metadata (D-86). A full check afterwards found that pull did no lasting damage | 2026-11-02 | 3d7abe9 |
| Snapshot of the whole stick after the kill, through the helper | `ntfsfix -n` clean; every file through a read-only mount matches the engine | pass: 116 directories, 1,144 files, 0 failures | 2026-11-02 | 636816e |
| App's claim against Apple's driver | `diskutil mount` refused while the app runs | refused ("Unlatch mounts this NTFS volume"). With the app **not** running, macOS mounts the stick read-only about 1.5 s after a dead daemon's mount is reaped, and an `unlatch mount` in that window leaves both mounted (carry-over, S2) | 2026-11-02 | 636816e |
| Replug, pull mid-copy, sleep and wake (`--only replug,sleep`, and `--cut --only pull`) | see the script | **open**: needs the owner at the Mac, after the stick is repaired | – | – |

## Interruption (`Tests/Fuse/interrupt.sh`)

| Scenario | Expected | Actual | Date | Build |
|---|---|---|---|---|
| `kill -9` of the daemon mid-copy (attached disk) | no stale mount; next probe not CORRUPT; next mount works | probe CLEAN, `ntfsfix -n` clean, remounted read-write, 1,218 files of the copy present, writes work, CLEAN after a normal unmount. macFUSE keeps the dead mount listed: `MountService.list()` now force-unmounts mounts whose daemon is gone (D-68); with plain `unlatch-fuse` it needs `umount -f` | 2026-11-01 | step 17 |
| Force-eject of the disk mid-copy | daemon exits, no stale mount, next probe not CORRUPT | **not testable with images**: macOS refuses to eject an attached image while the daemon holds it. The engine side is covered by `test_device_failure` (device turns read-only underneath; every later call EIO, nothing more written) | 2026-11-01 | step 17 |
| Pull a real USB stick mid-copy | as above | **open, S2**: needs a disposable USB stick from the owner | – | – |

## Crash leftovers (D-25, D-26, D-30, D-65)

| Scenario | Expected | Actual | Date | Build |
|---|---|---|---|---|
| Volume with a ghost `.unlatch-ghost-N` and a rename temp `.unlatch-rename-N` in the root, opened read-write | ghost deleted; temp name kept as visible "Unlatch recovered 1" with its data | pass (`test_crash_leftovers`) | 2026-11-01 | step 17 |
| Same volume opened read-only | nothing changes | pass | 2026-11-01 | step 17 |
| Temp name left in a subdirectory, then a rename there | hidden, harmless; new temp names never collide (ids seeded from the clock) | pass | 2026-11-01 | step 17 |
| Crash in the middle of a rename | volume consistent; at worst an extra hidden name | with the page cache, dirty pages reach the disk only between requests (the 1 s write-back timer takes the request lock) or when the cache is full, so a crash normally leaves the state before or after the rename. A torn write-back can leave both names or a temp name; the root sweep repairs root ones, others stay hidden | 2026-11-01 | step 17 |

## Capacity

| Scenario | Expected | Actual | Date | Build |
|---|---|---|---|---|
| Fill a 256 MiB volume | ENOSPC, then writes work after deleting; volume CLEAN | `dd` stops at the end (232 KiB left, small files still fit in the MFT); writes work after deleting; CLEAN, `ntfsfix -n` clean | 2026-11-01 | step 17 |
| 3.5 GiB file (the plan's 50 GB+ does not fit this Mac's 4 GiB image limit) | checksum matches after remount | copied in 4.0 s; SHA-256 matches after a read-only remount; CLEAN | 2026-11-01 | step 17 |
| 100,000-file tree (the plan's 1M is scaled to this Mac) | all files listed after remount; `rm -rf` works; CLEAN | created in 52 s (1–16 KiB each); 100,000 listed after remount; `rm -rf` in 77 s; CLEAN, `ntfsfix -n` clean | 2026-11-01 | step 17 |
| 50 GB+ file, 1M-file tree on a real disk | as above | **open, S3**: needs a large disposable disk | – | – |

## Names

| Scenario | Expected | Actual | Date | Build |
|---|---|---|---|---|
| NFD name created, opened by its NFC form | one file, found both ways | pass (rw-suite "names") | 2026-11-01 | step 17 |
| Emoji name; 255 UTF-16 units | accepted | pass | 2026-11-01 | step 17 |
| 256 units; `CON`, `aux.txt`, trailing dot or space, `:`, `?`, `<` | refused (ENAMETOOLONG, EINVAL) | pass | 2026-11-01 | step 17 |
| Case-only rename | the new case is shown | pass | 2026-11-01 | step 17 |
| Unicode names written by Windows, read on the Mac | shown correctly | **open, S3**: part of the Windows-made golden images (leftovers after Step 20) | – | – |

## Directory listings

| Scenario | Expected | Actual | Date | Build |
|---|---|---|---|---|
| Listing 300, 3,000 and 10,000 names while another process deletes a third and adds as many (`Tests/Fuse/readdir-writer.c`) | names present throughout appear exactly once | before: up to 32 names returned twice; after the per-handle snapshot (D-66): 0 | 2026-11-01 | step 17 |

## Windows (UTM VM, `Scripts/chkdsk-vm.sh`)

| Scenario | Expected | Actual | Date | Build |
|---|---|---|---|---|
| `chkdsk` of a full rw-suite volume written through the page cache | no problems | "found no problems" (`Tests/oracle/rw-suite-step17.chkdsk.txt`) | 2026-11-01 | step 17 |
| Symlinks made on the Mac, opened on Windows (`Tests/oracle/windows-check.ps1`) | on-volume targets followed; absolute ones not | `sym`, `cp/up` (through `..`) and `dirlink` (a directory) are SymbolicLink (tag 0xA000000C) and Windows reads through them; `abslink` stays a WSL reparse point | 2026-11-01 | step 17 |
| Streams, hard links, hidden flag, git clone | as in Step 16 | all as before (409 clone files, 4 streams, 2 links, Hidden) | 2026-11-01 | step 17 |
| Check Disk vs chkdsk (D-101): 140 cut-off images (`test_check_cut_copy` every 22nd write, 12; `test_cut_order` every 4th write, 128) and the 10 `chk-*` fixtures, each after `unlatch-engine check --repair`, then read-only `chkdsk` and `chkdsk /f` on a throwaway copy (`Scripts/chkdsk-vm.sh --scan` / `--detail`) | every image Check Disk leaves with no problems is clean for chkdsk | **fails: 39 of 100 are clean.** The other 61 have bits set for free space in `$MFT`'s bitmap and/or `$Bitmap` (60; Check Disk counts this as lost space, not as a problem, and does not see `$MFT` bitmap bits past `$MFT`'s end) or a missing security descriptor (1, `order-9`). Where Check Disk left problems (50), chkdsk found damage on every one: orphans relinked to their folders (31 cuts), `$MFTMirr` (13, chkdsk read-only: "Detected errors in the MFT mirror"), and the 6 unrepairable fixtures. Every cut Check Disk repaired (4) and 3 of 4 repaired fixtures are clean; `chk-dangling` keeps the lost clusters of its freed records. Carry-overs in PROGRESS | 2026-11-03 | 1.0.0-beta.13 |
| The same 140 cuts and 18 `chk-*` fixtures repaired by the D-98/D-99/D-102 Check Disk (`claude/check-disk-d98-d99` 92189b6) | every image Check Disk leaves with no problems is clean for chkdsk | **pass: 151 of 151** clean read-only and with `/f`, including 139 of 140 cuts. chkdsk corrects all 7 Check Disk leaves: `copy-245` (index growth, D-100; its 22 orphans are relinked) and 6 unrepairable fixtures | 2026-11-03 | 92189b6 |

## System integration

| Scenario | Expected | Actual | Date | Build |
|---|---|---|---|---|
| Spotlight | volume not indexed | `mdutil -s`: "unknown indexing state" (macFUSE volumes are not indexed, D-61) | 2026-11-01 | step 17 |
| Time Machine | Unlatch volumes not offered as destinations | Time Machine only offers APFS and HFS+ disks; not checked in its UI | 2026-11-01 | step 17 |
| Unmount right after heavy writes | succeeds | a system process can hold a fresh volume for a moment (EBUSY, nothing in `lsof`); `MountService.unmount` now retries for 3 s | 2026-11-01 | step 17 |
| Sleep and wake with a volume mounted; Power Nap | mount survives, data intact | **open, S2**: needs the owner at the Mac | – | – |

## Soak

| Scenario | Expected | Actual | Date | Build |
|---|---|---|---|---|
| fsx soak, 1,000 seeds × 2,000 ops (Step 06) | no mismatch | pass (Step 06 run; regressions re-run every gate) | 2026-11-01 | step 06+ |
| 24-hour soak: fuzz, fsx and a copy loop on a real disk | no failures | **open, S3**: needs a real disposable disk and a day of machine time | – | – |

## Distribution (Homebrew cask, 1.0.0-beta.1)

| Scenario | Expected | Actual | Date | Build |
|---|---|---|---|---|
| Fresh install on the dev Mac after removing Unlatch and macFUSE: `brew tap`, `brew trust --tap`, `brew install --cask` | macFUSE installed as a dependency, app in Applications, `unlatch` linked | as expected (macFUSE 5.5.0) | 2026-11-02 | 1.0.0-beta.1 |
| `unlatch` from Terminal before the app was opened | works | **killed** (Gatekeeper: quarantined, ad-hoc signed; no dialog for command-line programs). Documented in the README, troubleshooting and the cask's caveats | 2026-11-02 | 1.0.0-beta.1 |
| Same after opening the app once with Open Anyway | works | the approval marks the bundled programs too (quarantine `0181` → `01c1`); `unlatch doctor` passes every check | 2026-11-02 | 1.0.0-beta.1 |

## Open items

No open data-loss or corruption issue. Open, waiting on hardware or the owner: USB pull mid-copy (S2), sleep and wake (S2), 50 GB file and 1M-file tree on a real disk (S3), 24-hour soak (S3), Windows-made Unicode names (S3, with the golden images). Check Disk leaves every cut but one (`copy-245`, D-100) clean for Windows chkdsk once D-98/D-99/D-102 are merged (D-101).
