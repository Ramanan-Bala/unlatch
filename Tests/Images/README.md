# Golden images

Each `<name>.img.zst` (Git LFS, zstd, sparse) has a `<name>.expect` file that `Scripts/test-golden.sh` checks. The expectations are written from how each image is built, never copied from the engine's output: file checksums come from the `mkfixture gen` stream, and probe states from the construction.

## Present (made without Windows)

`Tests/Images/make-golden.sh` regenerates all of them. Regenerating changes serial numbers and times, so only regenerate when an expectation has to change.

| Image | How it is made | Expect |
|---|---|---|
| `fresh-ntfs3g` | `mkntfs -Q`, 16 MB | CLEAN, empty root |
| `content-ntfs3g` | mkfixture: nested dirs; Unicode names (Latin-1, CJK); a 300+ character path with a 255-character name; hidden and resident file; 256 MB sparse file; `init_size < size`; an ADS (`Zone.Identifier`); a 300-entry directory | CLEAN, sizes, SHA-256s, flags |
| `dirty-ntfs3g` | `content-ntfs3g` with `VOLUME_IS_DIRTY` set in `$MFT` and `$MFTMirr` (`Tests/Engine/set_dirty.c`) | DIRTY, same contents |
| `hiberfil-ntfs3g` | `hiberfil.sys` starting with `HIBR`, copied in with `ntfscp` | HIBERNATED; read-write open is EPERM |
| `logfile-clean-synthetic` | `$LogFile` restart pages v1.1 with the clean flag (`mkfixture logfile clean`) | CLEAN; read-write open works |
| `logfile-unclean-synthetic` | restart pages v1.1, one open client, no clean flag | UNCLEAN_LOG; read-write open is EOPNOTSUPP |
| `logfile-faststartup-synthetic` | restart pages v2.0, the version Windows Fast Startup leaves | HIBERNATED; read-write open is EPERM |

## Made by Windows

`Tests/Images/windows/make.sh` (needs `WINDOWS_SSH`, docs/testing.md) runs `make-windows-golden.ps1` in the Windows VM: diskpart creates a 64 MB fixed VHD, Windows formats it NTFS and writes everything with its own tools, and `manifest.tsv` records what Windows reports. `manifest-to-expect.py` turns that report into the `.expect` file (tab-separated, since names contain spaces), so no expectation comes from Unlatch.

| Image | How it is made | Expect |
|---|---|---|
| `windows-content` | Windows 11 24H2 (10.0.26100.1742), `format fs=ntfs quick`, label WINGOLD: LZNT1-compressed folder (`compact /c`) with a text and a random file; sparse file (`fsutil sparse`, 8 MiB with 64 KiB of data); two alternate data streams; names in Japanese, NFC and NFD `é`, emoji, 255 characters, and a path over 260 characters (`\\?\`); hidden, read-only and system files; a 500-entry folder; `mklink` relative file and directory symlinks, an absolute symlink and a junction (`mklink /J`) | CLEAN, every size and SHA-256 as Windows computed them, stream sizes and hashes, flags, link targets (absolute ones under the mount point, D-49), read-write open |

Also checked once (not part of the suite): a copy mounted read-write by Unlatch, with new files in the compressed folder (they inherit compression), a renamed folder, a deleted 500-entry folder and an append, passes Windows `chkdsk` (`Tests/oracle/windows-content-written.chkdsk.txt`).

## Wanted (need Windows; see docs/testing.md for the VM)

These are the plan's Windows-made images. Add each with a README note on how it was made (Windows version, commands) and an `.expect` file:

- Fast Startup: shut down with Fast Startup on, then take the image (expect HIBERNATED)
- dirty: unplug or kill the VM mid-write (expect DIRTY or UNCLEAN_LOG)
- fragmented MFT: create and delete many files until `$MFT` has many extents (`fsutil file queryextents C:\$MFT`)
