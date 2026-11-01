# Developer setup: macFUSE, mounting, logs

Never use a drive that holds real data. Use image files (`Scripts/make-image.sh`, `Tests/Images/`) and disposable disks.

Unlatch mounts through **macFUSE** (D-39): macFUSE forwards file system calls to the `unlatch-fuse` daemon, which runs the engine. Nothing of ours needs signing. macFUSE has two backends:

- **kext** (the default, D-43): macFUSE's kernel extension. Mature, mounts anywhere, honours read-only. It must be allowed once: on Apple Silicon, Reduced Security in Recovery, then approval in System Settings → Privacy & Security, then a restart.
- **fskit** (`--backend fskit`): no kernel extension, mounts only under `/Volumes`. On macOS 27 its uncached reads of 256 KiB or more fail (D-42, `Tests/Fuse/macfuse-repro/`), so don't use it for real data yet.

## 1. Install and enable macFUSE (once per machine)

1. Install macFUSE 5.5 or later: `brew install --cask macfuse`, or the installer from macfuse.github.io.
2. For the kext backend, allow the kernel extension as the installer describes. Check with `kmutil showloaded | grep macfuse` after the first mount.
3. For the FSKit backend, enable its file system extension: System Settings → General → Login Items & Extensions → File System Extensions → **macFUSE**. If the switch is greyed out, view the list "By Category". Quick mount/unmount cycles can switch it off again (D-42).

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

Options: `-f` stays in the foreground, `-d` prints FUSE debug output, `--backend fskit` uses the FSKit backend (mount point under `/Volumes`), and `-o opt,...` passes extra macFUSE options. A block device needs no sudo when the user owns it, as with `hdiutil`-attached images. Step 08 always mounts read-only; dirty, hibernated and unclean-log volumes will stay read-only once writing arrives in Step 10.

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
| Everything below at once | `Tests/Fuse/check-readonly.sh build/c.img build/m` |
| Listing | `ls -la build/m` matches `unlatch-engine ls <img> /` |
| Times | `stat build/m/ReadMe.TXT` matches `unlatch-engine stat <img> /ReadMe.TXT` |
| Space | `df build/m` matches `unlatch-engine info <img>` |
| Content | `shasum build/m/docs/sub/report.bin` matches `unlatch-engine cat <img> /docs/sub/report.bin \| shasum` |
| Fast Startup image | `logfile-faststartup-synthetic` mounts read-only; its status record says `hibernated` |

## 6. The dormant FSKit module

`Extension/` holds a native FSKit module (Step 08's first version). macOS loads it only when it is signed by a paid Apple Developer team: Xcode refuses the FSKit Module capability for a free Personal Team (D-38). To try it with a paid team, copy `Config/Local.xcconfig.example` to `Config/Local.xcconfig`, set `DEVELOPMENT_TEAM`, build with `-allowProvisioningUpdates`, copy the app to `/Applications`, enable **Unlatch** under File System Extensions, and mount with `mount -F -t unlatch diskN <dir>`.
