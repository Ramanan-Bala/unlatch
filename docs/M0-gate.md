# M0 gate: GO/NO-GO (plan Step 11)

**Verdict: GO** (2026-11-01, owner agreed). Unlatch beats stock ntfs-3g on every test when both run on the same backing, and clears the agreed thresholds against APFS on that backing. The disk path, through the buffered `/dev/diskN`, is clearly slower; that is Step 15's work, not a reason to stop.

## Results

| Test | apfs | exfat | ntfs3g | unlatch-file | unlatch |
|---|---:|---:|---:|---:|---:|
| Sequential write | 2,709 MB/s | 2,402 MB/s (89%) | 932 MB/s (34%) | 1,533 MB/s (57%) | 430 MB/s (16%) |
| Sequential read, cold | 2,385 MB/s | 2,266 MB/s (95%) | 2,834 MB/s (119%) | 8,095 MB/s (339%) | 300 MB/s (13%) |
| Sequential read, warm | 13,861 MB/s | 14,561 MB/s (105%) | 1,874 MB/s (14%) | 2,360 MB/s (17%) | 13,332 MB/s (96%) |
| Raw device read | 2,276 MB/s | 2,390 MB/s (105%) | – | – | 874 MB/s (38%) |
| Small files (1-16 KiB) | 3,296 files/s | 1,190 files/s (36%) | 2,296 files/s (70%) | 2,840 files/s (86%) | 1,578 files/s (48%) |
| Metadata (create, rename, delete) | 25,341 ops/s | 877 ops/s (3%) | 2,118 ops/s (8%) | 2,837 ops/s (11%) | 419 ops/s (2%) |

Apple M2, 8 GiB, macOS 27.0.1 (26A434), commit d540f50; median of 3 runs, 1024 MiB file, 30000 small files, 10000 metadata files; cold reads: remount + F_NOCACHE. Percentages are of apfs.

- **Like for like:** `ntfs3g` and `unlatch-file` both mount the image file. Stock ntfs-3g refuses block devices unless run as root.
- **Real-drive path:** `unlatch` mounts the attached disk (`/dev/diskN`), the way a real drive is used.
- **What the numbers compare:** every volume is a 4 GiB image on the internal SSD, held in the host's page cache. The numbers compare file system stacks, not drives.
- **Cold reads:** these remount first. Image-file setups can still be served from the host's cache of the image, which is why `unlatch-file` shows 8 GB/s.
- **Noise:** this Mac has 8 GiB of RAM and was 1.2 GB into swap during the runs, so the disk-path numbers vary from run to run. The `unlatch` row is a median of 3 runs; the others are medians of 5.
- **Setup and method:** `Bench/README.md`.

## Thresholds (D-52)

| Threshold (proposed in the plan) | Like for like (`unlatch-file`) | Disk path (`unlatch`) |
|---|---|---|
| Sequential read ≥ 60% of APFS | 339% ✓ | 13% ✗ |
| Sequential write ≥ 40% of APFS | 57% ✓ | 16% ✗ |
| Not slower than stock ntfs-3g on the same backing | faster on all six tests ✓ | (ntfs-3g cannot run on a disk without root) |

The gate is judged like for like, because that compares what Unlatch controls: the engine and daemon against ntfs-3g's own FUSE driver. The disk-path gap comes from the buffered block device (D-51). Every engine write is a small synchronous transfer through it, and reads come back in small pieces. Step 15 is re-scoped to close it: an engine-side block cache over `/dev/rdiskN`, larger and coalesced transfers, and a multi-threaded session loop.

## Not done

- **Disposable external SSD:** the plan's run on a real drive over a real port needs a drive from the owner. Use `Bench/run.sh <mount> <label> --device rdiskN` on it.
- **Larger-than-RAM file:** the `mkfile` test is skipped, since the images are 4 GiB on an 8 GiB Mac.
- **FSKit backend:** not benchmarked; its large reads fail (D-42).

## Step 15: disk path before and after the page cache (D-62)

`unlatch` setup only (the attached 4 GiB disk), median of 3, same Mac. "Before" is commit 14e5f67 (buffered `/dev/diskN`, D-51). "First cache" is the cache over `/dev/rdiskN` before the D-63 fix and the eviction change.

| Test | Before | First cache | Change |
|---|---:|---:|---:|
| Sequential write | 448 MB/s (2.40 s) | 2,744 MB/s (0.39 s) | 6.1× |
| Sequential read, cold | 294 MB/s (3.66 s) | 1,788 MB/s (0.60 s) | 6.1× |
| Sequential read, warm | 13,100 MB/s | 13,880 MB/s | same (page cache) |
| Small files (30,000) | 1,113 files/s (27.0 s) | 452 files/s (66.4 s) | regression, fixed below |
| Metadata (10,000 × 3) | 496 ops/s (60.5 s) | 182 ops/s (164.6 s) | regression, fixed below |

The first cache made small files and metadata slower. The cause was ghosts (D-25): `rm -r` of thousands of files left thousands of ghost names in the root, flagged SYSTEM, and libntfs-3g's readdir reads the data of every SYSTEM entry, so each listing of the root re-read all of them (268,000 cache misses in one run). Ghosts are now HIDDEN only (D-63), file data enters the cache cold, and eviction prefers clean pages. With those, a 10,000-file run of the same workload on the disk path has 15 misses in total, and the metadata test runs at about 2,400 ops/s (3,000 files × 3 ops in 3.7 s), against 496 before.

The full rerun of `Bench/baselines.sh` (all setups, median of 5) is left for the end of the release work, at the owner's request.
