# Developer setup: macFUSE, mounting, logs

Never use a drive that holds real data. Use image files (`Scripts/make-image.sh`, `Tests/Images/`) and disposable disks.

Unlatch mounts through **macFUSE's FSKit backend** (D-39): macFUSE's own signed FSKit extension forwards file system calls to the `unlatch-fuse` daemon, which runs the engine. Nothing of ours needs signing, and no kernel extension is loaded.

## 1. Install and enable macFUSE (once per machine)

1. Install macFUSE 5.4 or later: `brew install --cask macfuse`, or the installer from macfuse.github.io. Skip the kernel-extension approval it offers; the FSKit backend doesn't need it.
2. Enable its file system extension: System Settings → General → Login Items & Extensions → File System Extensions → turn on **macFUSE**. Enabling it is a security setting, so a person always does it.
3. Check: `pluginkit -m -v -p com.apple.fskit.fsmodule` lists `io.macfuse.app.fsmodule.macfuse`.

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
$fuse build/c.img build/m            # an image file directly
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

Options: `-f` stays in the foreground, `-d` prints FUSE debug output, `--backend kext` uses macFUSE's kernel extension instead (it needs approval in Recovery), and `-o opt,...` passes extra macFUSE options. Step 08 always mounts read-only; dirty, hibernated and unclean-log volumes will stay read-only once writing arrives in Step 10.

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
| Listing | `ls -la build/m` matches `unlatch-engine ls <img> /` |
| Times | `stat build/m/ReadMe.TXT` matches `unlatch-engine stat <img> /ReadMe.TXT` |
| Space | `df build/m` matches `unlatch-engine info <img>` |
| Content | `shasum build/m/docs/sub/report.bin` matches `unlatch-engine cat <img> /docs/sub/report.bin \| shasum` |
| Fast Startup image | `logfile-faststartup-synthetic` mounts read-only; its status record says `hibernated` |

## 6. The dormant FSKit module

`Extension/` holds a native FSKit module (Step 08's first version). macOS loads it only when it is signed by a paid Apple Developer team: Xcode refuses the FSKit Module capability for a free Personal Team (D-38). To try it with a paid team, copy `Config/Local.xcconfig.example` to `Config/Local.xcconfig`, set `DEVELOPMENT_TEAM`, build with `-allowProvisioningUpdates`, copy the app to `/Applications`, enable **Unlatch** under File System Extensions, and mount with `mount -F -t unlatch diskN <dir>`.
