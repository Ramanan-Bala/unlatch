# Engine fuzzing

`Scripts/fuzz.sh` builds two libFuzzer targets with ASan and UBSan against a sanitized libntfs-3g (`Scripts/build-ntfs3g.sh --fuzz`, Homebrew LLVM):

- `fuzz_probe`: `ue_probe`.
- `fuzz_open_ls`: read-only `ue_open`, then a bounded walk with readdir, lookup, getattr, reads and statfs.

**Input format ("sector list"):** a `u32le` image size in 512-byte sectors (2 to 8 MiB), then records of `{ u32le sector index, 512 bytes }`. Sectors not listed are zero. This keeps mutations on the volume's real metadata instead of megabytes of zeros. Seeds are built from real images (fresh, populated, dirty, hiberfil) by `Tests/Fuzz/mkseed.c`.

**Crashes:** every crash input is saved in `crashes/<target>/` and replayed first on every run. UBSan's `alignment` and `shift-base` checks are off: libntfs-3g reads packed on-disk structures and sign-extends with `(s8)x << 8` by design (D-33).
