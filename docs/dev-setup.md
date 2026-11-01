# Developer setup: signing, enabling and mounting the FSKit extension

Never use a drive that holds real data. Use image files (`Scripts/make-image.sh`, `Tests/Images/`) and disposable disks.

## 1. Signing (once per machine)

Without a team, the app and extension build ad-hoc signed with no entitlements. That's enough for CI and the engine work, but macOS won't load the FSKit extension (D-06). To sign for real:

1. Sign in to Xcode with your Apple ID: Xcode → Settings → Accounts. A paid Apple Developer Program team is needed: Xcode refuses the FSKit Module capability for a free Personal Team (D-38).
2. Copy `Config/Local.xcconfig.example` to `Config/Local.xcconfig` (git-ignored) and set `DEVELOPMENT_TEAM` to your team ID (Xcode → Settings → Accounts → the team, or developer.apple.com → Membership).
3. Regenerate and build:
   ```sh
   Scripts/bootstrap.sh
   xcodebuild -scheme Unlatch -derivedDataPath build/DerivedData -allowProvisioningUpdates build
   ```
   Automatic signing registers `io.github.ramanan-bala.Unlatch`, `….Unlatch.FSModule` and `group.io.github.ramanan-bala.Unlatch` under your team.
4. Check the entitlements:
   ```sh
   codesign -d --entitlements - build/DerivedData/Build/Products/Debug/Unlatch.app/Contents/Extensions/UnlatchFSModule.appex
   ```

## 2. Install and enable the extension

1. Copy the app to `/Applications`, so the system finds the embedded extension, and launch it once:
   ```sh
   ditto build/DerivedData/Build/Products/Debug/Unlatch.app /Applications/Unlatch.app
   open /Applications/Unlatch.app
   ```
2. Check that the system sees the extension:
   ```sh
   pluginkit -m -v -p com.apple.fskit.fsmodule
   ```
   It should list `io.github.ramanan-bala.Unlatch.FSModule`.
3. Enable it in System Settings → General → Login Items & Extensions → File System Extensions → **Unlatch**. The app will deep-link there in Step 14 (`FSClient.shared.openFileSystemExtensionsSettings()`). Enabling it is a security setting, so it's always done by a person.

## 3. Mount an image

```sh
Scripts/make-image.sh t1 512m T1                  # or: zstd -d Tests/Images/content-ntfs3g.img.zst -o c.img
disk=$(Scripts/attach-image.sh t1.img)            # prints diskN, not mounted
mkdir -p /tmp/unlatch
mount -F -t unlatch "$disk" /tmp/unlatch          # if it fails with a permission error: sudo (record which in DECISIONS)
ls -la /tmp/unlatch; df -h /tmp/unlatch
umount /tmp/unlatch
hdiutil detach "$disk"
```

Mounting read-only: `mount -F -t unlatch -o rdonly "$disk" /tmp/unlatch`.

**Apple's NTFS driver:** Apple's read-only NTFS driver may also claim the disk. `attach-image.sh` attaches with `-nomount`, so nothing mounts it first. Step 12 settles the auto-mount race.

## 4. Logs and status

```sh
log stream --level debug --predicate 'subsystem == "io.github.ramanan-bala.unlatch"'
ls ~/Library/Group\ Containers/group.io.github.ramanan-bala.Unlatch/status/   # one JSON record per volume serial
```

The extension writes `status/<serial>.json` (`VolumeStatus` in UnlatchCore) at check and load. It records the probe state (`clean`, `dirty`, `hibernated`, `unclean_log`, `corrupt`, `not_ntfs`), the reason, and whether the volume was loaded read-only.

## 5. Step 08 checks (once signed and enabled)

| Check | Command |
|---|---|
| Listing | `ls -la /tmp/unlatch` matches `unlatch-engine ls <img> /` |
| Times | `stat /tmp/unlatch/ReadMe.TXT` matches `unlatch-engine stat <img> /ReadMe.TXT` |
| Space | `df /tmp/unlatch` matches `unlatch-engine info <img>` |
| Content | `shasum /tmp/unlatch/docs/sub/report.bin` matches `unlatch-engine cat <img> /docs/sub/report.bin \| shasum` |
| Fast Startup image | `logfile-faststartup-synthetic` mounts read-only; its status record says `hibernated` |
