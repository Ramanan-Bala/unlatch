# Patches to the vendored ntfs-3g

`Scripts/build-ntfs3g.sh` applies every `*.patch` here, in name order, with `patch -p1` against a copy of `Engine/vendor/ntfs-3g/`. The vendored tree itself is never edited.

Name patches `NNNN-short-description.patch` and say why each exists in its header.

| Patch | Why | Step |
|---|---|---|
| `0001-ntfs_readdir-fix-bitmap-offset-on-resume.patch` | Resuming `ntfs_readdir` in the middle of a directory tested the wrong index-bitmap bits: it skipped entries or failed with EIO (D-21). | 04 |
| `0002-bootsect-reject-overflowing-sector-count.patch` | `ntfs_boot_sector_parse` shifted an unchecked `number_of_sectors` left, which is UB and gives a garbage seek for huge or negative values. Found by `fuzz_probe` (D-33). | 07 |
