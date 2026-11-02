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
| `unlatch-file` | `unlatch-fuse` on the image file, like for like with `ntfs3g` | `/Volumes/BUNLATCH` |
| `unlatch` | `unlatch-fuse` on the attached disk, as a real drive would be used | `/Volumes/BUNLATCH` |

Unlatch's runs use macFUSE's FSKit backend (`unlatch-fuse`'s default under `/Volumes`). `UNLATCH_BACKEND=kext Bench/baselines.sh unlatch-file unlatch` runs them through the kernel extension instead, labelled `unlatch-file-kext` and `unlatch-kext` (D-79).

### Results, 2026-11-02 (commit 484a20a, Unlatch 1.0.0-beta.5)

Apple M2, 8 GiB, macOS 27.0.1, macFUSE 5.5.0; `Bench/compare.py` on the `2026-11-02-484a20a-*` files.

| Test | APFS | exFAT | stock ntfs-3g (kext) | Unlatch file, kext | Unlatch file, FSKit | Unlatch disk, kext | Unlatch disk, FSKit |
|---|---:|---:|---:|---:|---:|---:|---:|
| Sequential write | 2,054 MB/s | 1,876 | 840 | 2,624 | 1,517 | 2,747 | 2,183 |
| Sequential read, cold | 2,309 MB/s | 2,407 | 2,942 | 4,407* | 2,642* | 1,891 | 1,109 |
| Sequential read, warm | 13,905 MB/s | 13,133 | 1,849 | 2,585 | 1,178 | 15,811 | 16,356 |
| Small files (1–16 KiB) | 2,231 files/s | 1,131 | 2,232 | 3,916 | 386 | 2,891 | 368 |
| Metadata (create, rename, delete) | 26,845 ops/s | 833 | 2,031 | 4,532 | 511 | 4,358 | 510 |

\* Image-file setups can be served from the host's cache of the image even after a remount.

FSKit against the kernel extension, attached disk: small files 7.9× slower, metadata 8.5× slower, cold reads 41% and writes 21% slower, warm (cached) reads the same. Every file operation passes through `fskitd` and macFUSE's extension process (D-78, D-79).

Image-backed volumes live in the host's page cache, so these numbers compare the file system stacks' overhead, not drives. The plan's disposable-SSD run uses the same scripts on a real disk; pass its mount point to `run.sh` and its `rdiskN` with `--device`.

## Real drive: USB 2.0 stick

SanDisk Cruzer Blade 16 GB (USB 2.0), GPT with one 15.8 GB NTFS partition (`mkntfs -f`), Apple M2, macOS 27.0.1, Unlatch 1.0.0-beta.5 (commit 484a20a's build plus the backend choice, mounted with `unlatch mount`). `UNLATCH_BACKEND=kext|fskit Bench/run.sh /Volumes/SANDISK <label> --runs 3 --mib 512 --small 5000 --meta 5000 --remount "unlatch unmount disk10s2 && unlatch mount disk10s2"` with labels `unlatch-usb-sandisk` (kext) and `unlatch-fskit-usb-sandisk`, so the sizes differ from the image runs above. The beta.3 run ([`unlatch`](results/2026-11-02-162fb9f-unlatch-usb-sandisk.json), kext) measured 5.5 MB/s writes, 94 files/s and 1,405 ops/s. The exFAT baseline is the same stick erased with `diskutil partitionDisk diskN 1 GPT ExFAT SANDISK R` (same layout), run with `--remount "diskutil unmount diskNs2 && diskutil mount diskNs2"`. The device's own speed is `sudo dd` of 512 MiB in 4 MiB blocks on the raw partition (`/dev/rdiskNs2`), median of 3. Results: [`unlatch`](results/2026-11-02-484a20a-unlatch-usb-sandisk.json), [`unlatch-fskit`](results/2026-11-02-484a20a-unlatch-fskit-usb-sandisk.json), [`exfat`](results/2026-11-02-c0452df-exfat-usb-sandisk.json), [`raw`](results/2026-11-02-raw-usb-sandisk.json); `Bench/usb-chart.py` draws `usb.svg` and `usb-dark.svg` (the chart in the top-level README) from them.

| | Device (raw `dd`) | Unlatch, FSKit | Unlatch, kernel ext. | exFAT (Apple) |
|---|---|---|---|---|
| Sequential read (cold) | 32.9 MB/s | 33.0 MB/s | 33.4 MB/s | 32.5 MB/s |
| Sequential write | 6.7 MB/s | 6.1 MB/s (91%) | 6.1 MB/s (91%) | 6.2 MB/s (93%) |
| Small files (1–16 KiB, create + delete) | | 82 files/s | 89 files/s | 61 files/s |
| Metadata (create, rename, delete) | | 400 ops/s | 1,157 ops/s | 122 ops/s |

Reads run at the stick's full speed and writes lose about a tenth with either backend, most likely to NTFS's own writes (MFT records, bitmap, log) on a device this slow; exFAT, with less metadata, loses 7%. Here FSKit costs 8% on small files and 2.9× on metadata: the stick hides most of FSKit's per-operation overhead. Small files and metadata beat exFAT with either backend, helped by the engine's write-back page cache (D-62).

## Real drive: a real copy to a USB hard drive

Seagate Expansion 1 TB portable hard drive (2.5", 5400 rpm, USB 3.0 at 5 Gb/s), NTFS, a third full, Apple M2, macOS 27.0.1, Unlatch 1.0.0-beta.5 installed app (kernel extension backend). A Finder copy of a GoPro footage folder (364 files, 123.1 GB, 19 files over 1 GiB) from the internal SSD. Not a `run.sh` benchmark: `df -k` of the mounted volume every 5 s, from 6.5 minutes into the copy until it finished; the whole-copy time runs from the copied folder's creation time on the drive (`stat -f %B`) to the last sample that grew. The drive holds real data, so there is no raw `dd` baseline; the comparison is the maker's "up to 130 MB/s". Result: [`unlatch-seagate-gopro`](results/2026-11-02-unlatch-seagate-gopro.json), 123.1 GB in 1,161 s, 106.1 MB/s (sampled stretch: 104.3 MB/s, 30 s averages 92–112 MB/s). `Bench/seagate-chart.py --from-log <log>` writes the result file from the samples, and `Bench/seagate-chart.py` draws `seagate.svg` and `seagate-dark.svg` (the chart in the top-level README) from it.

## Running

```sh
Scripts/build-ntfs3g.sh --driver          # once: the stock ntfs-3g baseline
Bench/baselines.sh                        # all five, ~35 min (FSKit small-file runs are slow)
UNLATCH_BACKEND=kext Bench/baselines.sh unlatch-file unlatch   # Unlatch through the kernel extension
Bench/baselines.sh unlatch                # one
Bench/compare.py                          # the newest result per label, as a table
```
