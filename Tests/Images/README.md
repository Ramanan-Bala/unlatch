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

## Wanted (need Windows; see docs/testing.md for the VM)

These are the plan's Windows-made images. Add each with a README note on how it was made (Windows version, commands) and an `.expect` file:

- compressed (NTFS LZNT1), sparse and ADS files written by Windows
- a directory junction and a symbolic link (`mklink /J`, `mklink`)
- long paths created by Windows (`\\?\` prefix)
- Fast Startup: shut down with Fast Startup on, then take the image (expect HIBERNATED)
- dirty: unplug or kill the VM mid-write (expect DIRTY or UNCLEAN_LOG)
- fragmented MFT: create and delete many files until `$MFT` has many extents (`fsutil file queryextents C:\$MFT`)
