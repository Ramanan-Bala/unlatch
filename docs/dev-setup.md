# Developer setup: macFUSE, mounting, logs

Never use a drive that holds real data. Use image files (`Scripts/make-image.sh`, `Tests/Images/`) and disposable disks.

Unlatch mounts through **macFUSE** (D-39): macFUSE forwards file system calls to the `unlatch-fuse` daemon, which runs the engine. Nothing of ours needs signing. macFUSE has two backends:

- **fskit** (what the app and CLI use unless the kext is allowed, D-78, D-79): no kernel extension, so users never visit Recovery; they only turn on macFUSE's file system extension. Up to 8 times slower than the kext with small files and metadata, 20–40% with large files on a fast SSD (README, "What FSKit costs"). It mounts only directly under `/Volumes`. macFUSE 5.5.0 loses `fuse_reply_buf` replies to reads of 64 KiB or more (macfuse/macfuse#1225, fixed in 5.6.0), so `unlatch-fuse` replies with `fuse_reply_data`.
- **kext** (`--backend kext`): macFUSE's kernel extension. Mounts anywhere, so the test and benchmark scripts use it for mount points outside `/Volumes`. It must be allowed once: on Apple Silicon, Reduced Security in Recovery, then approval in System Settings → Privacy & Security, then a restart.

`unlatch-fuse`'s default, `--backend auto`, is fskit for a mount point directly under `/Volumes` and kext anywhere else. The app and CLI choose for themselves (`MacFUSE.preferredBackend`: the kext when it is loaded or in the auxiliary kernel collection at the installed version, per `kmutil inspect`) and pass `--backend`; `UNLATCH_BACKEND=kext|fskit` overrides that, and `Bench/baselines.sh` takes it too.

## 1. Install and enable macFUSE (once per machine)

1. Install macFUSE 5.5 or later: `brew install --cask macfuse`, or the installer from macfuse.github.io.
2. Enable its file system extension (FSKit backend): System Settings → General → Login Items & Extensions → File System Extensions → **macFUSE**. If the switch is greyed out, view the list "By Category". With 5.4.0, quick mount/unmount cycles switched it off again (D-42); 15 quick cycles with 5.5.0 did not.
3. Only for test and benchmark mounts outside `/Volumes`: allow the kernel extension as the installer describes. Check with `kmutil showloaded | grep macfuse` after the first mount.

Allowing the kext and enabling extensions are security settings, so a person always does them.

`unlatch-fuse` builds against `/usr/local/include/fuse3` and `/usr/local/lib/libfuse3.dylib`. For another prefix, set `MACFUSE_PREFIX` in `Config/Local.xcconfig`.

## 2. Build

```sh
Scripts/bootstrap.sh
xcodebuild -quiet -scheme Unlatch -derivedDataPath build/DerivedData build
fuse=build/DerivedData/Build/Products/Debug/unlatch-fuse
```

## 3. Mount an image

```sh
zstd -d Tests/Images/content-ntfs3g.img.zst -o build/c.img
mkdir -p build/m
$fuse build/c.img build/m            # an image file directly (kext backend)
ls -la build/m; df -h build/m
umount build/m
```

Through a block device, as a real disk would be:

```sh
disk=$(Scripts/attach-image.sh build/c.img)   # diskN, not mounted
$fuse /dev/$disk build/m
umount build/m
hdiutil detach "$disk"
```

Options: `-f` stays in the foreground, `-d` prints FUSE debug output, `--backend fskit` uses the FSKit backend (mount point under `/Volumes`), and `-o opt,...` passes extra macFUSE options. A block device needs no sudo when the user owns it, as with `hdiutil`-attached images. A volume mounts read-write only when it probes clean and the image or disk opens writable; `-r` forces read-only. Dirty, hibernated and unclean-log volumes always mount read-only. Extended attributes are stored as NTFS named streams (D-48), so macOS writes no `._` files; symlinks are created in the WSL format (D-49).

**Apple's NTFS driver:** Apple's read-only NTFS driver may also claim a disk. `attach-image.sh` attaches with `-nomount`, so nothing mounts it first. Step 12 settles the auto-mount race.

## 4. Logs and status

```sh
$fuse -f build/c.img build/m          # errors go to stderr in the foreground
ls ~/Library/Application\ Support/Unlatch/status/   # one JSON record per volume serial
```

The daemon writes `status/<serial>.json` (`VolumeStatus` in UnlatchCore, D-41) at probe, mount and unmount. It records the probe state (`clean`, `dirty`, `hibernated`, `unclean_log`, `corrupt`, `not_ntfs`), the reason, whether the volume is read-only, and whether it is mounted.

## 5. Step 08 checks

| Check | Command |
|---|---|
| Everything below at once | `$fuse -r build/c.img build/m && Tests/Fuse/check-readonly.sh build/c.img build/m` |
| Listing | `ls -la build/m` matches `unlatch-engine ls <img> /` |
| Times | `stat build/m/ReadMe.TXT` matches `unlatch-engine stat <img> /ReadMe.TXT` |
| Space | `df build/m` matches `unlatch-engine info <img>` |
| Content | `shasum build/m/docs/sub/report.bin` matches `unlatch-engine cat <img> /docs/sub/report.bin \| shasum` |
| Fast Startup image | `logfile-faststartup-synthetic` mounts read-only; its status record says `hibernated` |

## 6. The dormant FSKit module

`Extension/` holds a native FSKit module (Step 08's first version). macOS loads it only when it is signed by a paid Apple Developer team: Xcode refuses the FSKit Module capability for a free Personal Team (D-38). To try it with a paid team, copy `Config/Local.xcconfig.example` to `Config/Local.xcconfig`, set `DEVELOPMENT_TEAM`, build with `-allowProvisioningUpdates`, copy the app to `/Applications`, enable **Unlatch** under File System Extensions, and mount with `mount -F -t unlatch diskN <dir>`.
