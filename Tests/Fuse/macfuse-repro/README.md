# macFUSE FSKit backend: large uncached reads fail

A minimal libfuse3 file system (`hello.c`, no Unlatch code) that serves one 4 MiB file from memory. On a fresh mount through the FSKit backend, any read of 256 KiB or more of data that isn't cached yet returns `EIO` (`dd` reports "Input/output error", 0 bytes). The daemon's `FUSE_READ` reply is complete, `fuse_reply_buf` returns 0, and macFUSE's module logs `readFrom:atOffset:intoBuffer:...:reply:error:5`. Reads of 4 KiB work, and once data is cached, reads of any size return it correctly.

Seen with macFUSE 5.4.0 and 5.5.0 on macOS 27 (arm64), 2026-11-01. With 5.4.0 there was also silent corruption: a 256 MiB sparse file read back with ~100 MiB of junk, because the kernel never sent `FUSE_READ` for those ranges. Mounting and unmounting several times in quick succession also leaves the extension "not found" or "not enabled" until it is re-enabled.

```sh
cc -O1 -Wall -isystem /usr/local/include/fuse3 hello.c -L/usr/local/lib -lfuse3 -o hello
./hello /Volumes/HelloFS /tmp/ref.bin           # mounts, writes the expected bytes to ref.bin
dd if=/Volumes/HelloFS/f bs=1m | shasum          # fresh mount: EIO, da39a3ee (empty)
umount /Volumes/HelloFS
./hello /Volumes/HelloFS /tmp/ref.bin
dd if=/Volumes/HelloFS/f bs=4k | shasum          # matches shasum < /tmp/ref.bin
umount /Volumes/HelloFS
```

The FSKit backend only mounts under `/Volumes`; elsewhere it fails with "Failed to create mount point".
