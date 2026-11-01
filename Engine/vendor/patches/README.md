# Patches to the vendored ntfs-3g

`Scripts/build-ntfs3g.sh` applies every `*.patch` here, in name order, with `patch -p1` against a copy of `Engine/vendor/ntfs-3g/`. The vendored tree itself is never edited.

Name patches `NNNN-short-description.patch` and say why each exists in its header.

None are needed for 2026.9.28 (Step 02).
