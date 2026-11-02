# Benchmarks

`Bench/run.sh <mountpoint> <label>` measures one mounted volume; `Bench/baselines.sh` sets up Unlatch and the baselines and runs it on each; `Bench/compare.py` turns `Bench/results/*.json` into a markdown table. Plan Step 11; the gate is in `docs/M0-gate.md`.

## Tests

| Test | What | Reported |
|---|---|---|
| `seq_write` | 1 GiB in 4 MiB `write`s, then `fsync` | MB/s |
| `seq_read_cold` | the same file after a remount, read with `F_NOCACHE` | MB/s |
| `seq_read_warm` | read again, through the page cache | MB/s |
| `small_files` | 30,000 files of 1–16 KiB in 100 directories | files/s |
| `metadata` | create, rename, then delete 10,000 empty files | ops/s (3 per file) |
| `raw_read` | `dd` of `/dev/rdiskN` with 4 MiB blocks: what the device itself delivers | MB/s |

Each is the median of 5 runs. `Bench/io.c` does the timed work, so no shell `fork` per file sits in the numbers.

## Setups (`Bench/baselines.sh`)

Every file system gets its own 4 GiB image on the internal SSD, attached as a disk:

| Label | File system | Mount |
|---|---|---|
| `apfs` | APFS (`hdiutil create -fs APFS`) | `/Volumes/BAPFS` |
| `exfat` | exFAT (Apple's driver) | `/Volumes/BEXFAT` |
| `ntfs3g` | stock ntfs-3g 2026.9.28 on macFUSE (`Scripts/build-ntfs3g.sh --driver`, `-o no_detach`), on the image **file**: it refuses block devices unless run as root | `build/bench/m` |
| `unlatch-file` | `unlatch-fuse` (kext backend) on the image file, like for like with `ntfs3g` | `build/bench/m` |
| `unlatch` | `unlatch-fuse` (kext backend) on the attached disk, as a real drive would be used | `build/bench/m` |

Image-backed volumes live in the host's page cache, so these numbers compare the file system stacks' overhead, not drives. The plan's disposable-SSD run uses the same scripts on a real disk; pass its mount point to `run.sh` and its `rdiskN` with `--device`.

## Real drive: USB 2.0 stick

SanDisk Cruzer Blade 16 GB (USB 2.0), GPT with one 15.8 GB NTFS partition (`mkntfs -f`), Apple M2, macOS 27.0.1, Unlatch 1.0.0-beta.3 (`Unlatch.app`, mounted with `unlatch mount`). `Bench/run.sh /Volumes/SANDISK unlatch-usb-sandisk --runs 3 --mib 512 --small 5000 --meta 5000 --remount "unlatch unmount disk6s2 && unlatch mount disk6s2"`, so the sizes differ from the image runs above. Result: [`results/2026-11-02-162fb9f-unlatch-usb-sandisk.json`](results/2026-11-02-162fb9f-unlatch-usb-sandisk.json).

The device's own speed is `sudo dd` of 512 MiB in 4 MiB blocks on the raw partition (`/dev/rdiskNs2`), median of 3:

| | Device (raw `dd`) | Unlatch | Unlatch / device |
|---|---|---|---|
| Sequential read (cold) | 32.9 MB/s | 33.1 MB/s | 100% |
| Sequential write | 6.7 MB/s | 5.5 MB/s | 82% |
| Small files (1–16 KiB, create + delete) | | 94 files/s | |
| Metadata (create, rename, delete) | | 1,405 ops/s | |

Reads run at the stick's full speed. Writes lose about a fifth, most likely to NTFS's own writes (MFT records, bitmap, log) on a device this slow; the exFAT run on the same stick will show how much any file system loses.

## Running

```sh
Scripts/build-ntfs3g.sh --driver          # once: the stock ntfs-3g baseline
Bench/baselines.sh                        # all four, ~10 min
Bench/baselines.sh unlatch                # one
Bench/compare.py                          # the newest result per label, as a table
```
