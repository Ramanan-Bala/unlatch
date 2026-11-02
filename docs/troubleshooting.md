# Troubleshooting

Start with the menu bar's **Setup & Health…** window, or `unlatch doctor` in a terminal: it checks macOS, macFUSE and its system extension, `unlatch-fuse`, the helper, and the status directory, and says what to fix.

## The disk does not mount, or Finder shows it read-only

- **Read-only with a reason:** see [read-only.md](read-only.md).
- **"macFUSE is not installed" / exit code 4:** install it (`brew install --cask macfuse`), then allow its system extension: on Apple Silicon, Reduced Security in Recovery once, then System Settings > Privacy & Security > Allow. Restart if macOS asks.
- **"macOS blocked the Unlatch helper"** (real disks only): give the helper Full Disk Access: System Settings > Privacy & Security > Full Disk Access > + > ⌘⇧G > `/Library/PrivilegedHelperTools/io.github.ramanan-bala.unlatch.helper`.
- **"needs the helper":** run Unlatch's setup again (menu > Finish setting up), or `sudo unlatch install-helper`.
- **Apple's read-only NTFS driver got the disk first:** `unlatch mount <disk>` takes it over (and gives it back if mounting fails). Turning auto-mount on in Settings makes Unlatch claim NTFS disks as they appear.
- **The disk is not listed at all:** `diskutil list` should show a `Windows_NTFS` or `Microsoft Basic Data` partition. BitLocker-encrypted partitions are not supported.

## "Unlatch can't be opened because Apple cannot check it"

Unlatch is not signed with an Apple Developer ID. Right-click Unlatch in Applications > Open, then Open; or System Settings > Privacy & Security > Open Anyway. See [install.md](install.md).

## `unlatch` in Terminal says "killed"

macOS stops the programs inside a downloaded, unsigned app until the app itself has been opened once. Open Unlatch from Applications with right-click > Open (or Open Anyway in Privacy & Security), then run `unlatch` again. Needed once per version.

## Unmount or eject says the disk is busy

A program still has a file open on it. Close it, or find it with `lsof +D /Volumes/<name>`. Right after a large copy, macOS itself may hold the disk for a moment; Unlatch retries for a few seconds. `unlatch unmount --force <disk>` unmounts anyway, which can lose the open program's unsaved changes.

## A folder shows up as empty, or a file name is missing

- Names that break Windows' rules (`a:b`, `CON`, a trailing dot) cannot be created; programs get "invalid argument".
- A damaged directory entry is skipped and logged instead of failing the whole folder (D-54). Run `chkdsk X: /f` on Windows.

## Files named "Unlatch recovered N"

A rename was cut off by a crash or an unplug; Unlatch kept the file under this name the next time it mounted the disk. Check it and rename or delete it.

## The disk was unplugged without ejecting

Plug it back in: Unlatch mounts it read-only if Windows' records say it was in use, and it repairs its own leftovers. To be sure the disk is consistent, run `chkdsk X: /f` on Windows. Always eject first next time.

## It is slow

Unlatch keeps a write-back cache in memory and writes it out at least every second. Very slow USB 2 sticks and SMR hard disks are slow at small-file work under any file system. A benchmark you can run: `Bench/run.sh /Volumes/<name> mydisk` (see Bench/README.md).

## Reporting a problem

1. Settings > Troubleshooting > **Export logs…** saves the last hour of Unlatch's log.
2. Note `unlatch --version`, macOS (`sw_vers`), macFUSE's version (`unlatch doctor`), and the disk (`diskutil info <disk>`; no personal data).
3. Open an issue at https://github.com/Ramanan-Bala/unlatch/issues with what you did, what you expected and what happened. For anything that might be a security problem, see [SECURITY.md](../SECURITY.md) instead.
