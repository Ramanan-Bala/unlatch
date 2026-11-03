# Changelog

Notable changes per release. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow [Semantic Versioning](https://semver.org/). The release workflow copies a version's section into its GitHub release notes.

## [Unreleased]

### Changed
- **Check Disk now repairs the last things a disk pulled out mid-copy leaves, without Windows:**
  - **Files no folder lists** (a new file whose folder was not written yet): Unlatch puts them back in their folder. If that folder is gone or another file now has the name, it puts them in a `found.000` folder at the top of the disk, as Windows' chkdsk does. Their contents are not touched.
  - **A disk that cannot be opened because the backup copy of its file table no longer matches the table** ("NTFS metadata is damaged"): Unlatch brings the backup copy up to date from the table, then checks and repairs the rest in the same run. It does this only when the table itself is whole.
  - **Check Disk and Enable Read-Write**, **Check Disk…** and `unlatch check --repair` do both.

### Fixed
- Check Disk now finds a folder whose index points at a part of it that was never written, which a cut-off write can leave. Before, it reported such a folder as healthy, but some names in it could not be looked up. It is reported as a folder that cannot be read.

## [1.0.0-beta.13] - 2026-11-03

### Changed
- **A disk pulled out mid-copy no longer ends up with folders listing files that were never written.** Unlatch now writes each new file's record before the folder entry that points at it, so a cut-off leaves at worst a file no folder lists yet. Writing many small files is about 16% slower. README: "If a disk is unplugged while writing".
- **Check Disk repairs what a disk pulled out mid-copy most often leaves:** a folder entry that points at a file that was never written ("the entry points at record N, which is not in use"). Unlatch removes such names, as Windows' chkdsk does, instead of sending you to Windows; the file itself was already lost, and no other file is touched. **Check Disk and Enable Read-Write** and `unlatch check --repair` now fix it.

## [1.0.0-beta.12] - 2026-11-03

### Fixed
- Check Disk no longer reports damage ("record 16 has a name but no directory lists it") on a healthy, nearly full disk, where NTFS keeps part of its own file table in a spare record.

### Changed
- **A disk pulled out while idle no longer needs checking.** Unlatch now marks a disk as "in use" only while it is writing to it, and clears the mark about two seconds after the last change, once everything is on the disk. Pulling a disk out mid-copy still marks it.
- **A disk that needs checking is checked automatically.** When it is plugged in, Unlatch checks it first; if nothing is wrong, it clears the mark and mounts the disk read-write without asking. You are asked only when the check finds something.
- **Unlatch asks what to do when a disk needs checking.** When a disk marked as needing a check is mounted, it comes up read-only and Unlatch asks: **Check Disk and Enable Read-Write** (checks it, repairs what is safe, and mounts it read-write when nothing is left; otherwise it stays read-only and Unlatch says what was found), or **Enable Read-Write Without Checking** (repairs nothing; the mark is cleared when you eject it). It asks once per plug-in; **Enable Read-Write…** in the disk's ⋯ menu asks again. This replaces **Mount Read-Write Anyway** and **Mark as Checked** in the menu, and `unlatch mount --dirty-rw` is gone: writing to a disk that stays marked is no longer offered. `unlatch mount --mark-checked` and `unlatch check --repair` remain.

## [1.0.0-beta.11] - 2026-11-02

### Added
- **Check Disk** for a disk marked as needing a check, so you no longer need a Windows PC to find out whether it is damaged: in the disk's ⋯ menu, or `unlatch check <disk>`. Unlatch reads the disk's file table, allocation maps and every folder, and says what it found. When nothing is wrong it clears the mark, and the disk mounts read-write. When files use space marked free, it can repair that safely (it marks the space used; nothing is overwritten). Anything else (an unreadable folder, files no folder lists, a damaged file record) it reports and leaves for Windows' chkdsk, with what to do. See "Without Windows" in docs/read-only.md.

## [1.0.0-beta.10] - 2026-11-02

### Fixed
- After **Mark as Checked**, the disk shows as "Read-write" right away; it showed "Read-write, unchecked" and offered Mark as Checked again until it was ejected.

## [1.0.0-beta.9] - 2026-11-02

### Added
- **Mark as Checked** for a disk marked as needing a check that you have decided to trust, for example after copying its files off: in the disk's ⋯ menu, after a warning, or `unlatch mount --mark-checked <disk>`. Unlatch mounts it read-write, and ejecting it from Unlatch clears the mark, so it mounts read-write from then on. It repairs nothing, and Windows no longer offers to check the disk.

## [1.0.0-beta.8] - 2026-11-02

### Fixed
- **A disk pulled out without ejecting disappears from Finder at once.** A disk mounted read-only (for example one marked as needing a check) stayed listed after it was unplugged, and every replug added another copy ("SANDISK", "SANDISK disk7s2", ...). Unlatch now unmounts as soon as macOS reports the disk gone.

### Added
- **Mount Read-Write Anyway** for a disk marked as needing a check, for people without a Windows PC to run chkdsk: in the disk's ⋯ menu, after a warning, or `unlatch mount --dirty-rw <disk>`. Writing to such a disk risks damaging files on it; the disk stays marked, so Windows still checks it later. Hibernated disks and disks with unfinished Windows transactions stay read-only. See "Without Windows" in docs/read-only.md.

## [1.0.0-beta.7] - 2026-11-02

### Fixed
- **A disk unplugged or a Mac that crashed while writing is no longer reported as healthy.** Unlatch now marks a disk "in use" while it is mounted for writing, as Windows does, and clears the mark only after everything is safely written at eject. A disk cut off mid-write mounts read-only next time and asks for a Windows check (chkdsk), instead of being written to again and risking damage to files.
- **A disk pulled out without ejecting can be mounted again right away.** Before, plugging it back in within about half a minute failed with "already mounted" or an I/O error.
- **A disk Unlatch cannot mount no longer disappears.** If macFUSE or the helper is missing or not allowed, Unlatch leaves the disk to macOS (read-only) instead of hiding it; after a failed automatic mount it hands the disk back to macOS.
- A mount that took too long to start could carry on in the background after reporting failure; it is now stopped.
- The helper gives write access only to volumes macOS recognises as NTFS, never to other partitions (exFAT, FAT32, BitLocker) that share NTFS's partition type.
- `unlatch mount` on a disk already mounted read-only reports read-only, not read-write.
- `unlatch-fuse` run by name (as Homebrew installs it) without `-f` starts its background daemon.
- The menu stays responsive while a slow disk mounts.
- A read that hits a bad sector part-way returns the data read before it.

## [1.0.0-beta.6] - 2026-11-02

### Fixed
- **A disk can no longer be mounted read-write twice at once** (for example by automatic mounting and `unlatch mount` at the same moment), which could have damaged it. The second mount is refused with "already mounted".
- **Deleting files frees their space right away.** Before, space freed by deleting files on a mounted disk came back only after ejecting it, so a full disk stayed full. A file that is still open keeps its space until it is closed.
- Files whose names Windows stored with separate accent characters (for example from some Mac-to-Windows copies) are listed and can be opened, renamed and deleted; before, they were missing from Finder and `ls`.
- When macFUSE's file system extension refuses a mount for a moment (right after an eject, or after macFUSE restarts), Unlatch tries again after a few seconds instead of failing. A failed mount no longer pushes the next one to a folder named `<name> diskN`.
- Finder's Get Info shows the app's real version (it showed 1.0).

## [1.0.0-beta.5] - 2026-11-02

### Changed
- **No kernel extension, Recovery or restart needed.** Without macFUSE's kernel extension, Unlatch mounts through macFUSE's FSKit backend: the one-time setup step is turning on macFUSE in System Settings > General > Login Items & Extensions > File System Extensions.
- **If macFUSE's kernel extension is already allowed** (as for beta.4 and earlier), Unlatch keeps using it: FSKit is up to 8 times slower with many small files and at renames and deletes, and 20–40% slower with large files on a fast SSD (README, "What FSKit costs"). `unlatch doctor` says which one mounts use. The README's "Faster small files" explains how to allow it.
- Needs macFUSE 5.5 or later; the setup window and `unlatch setup` offer the update.

### Fixed
- Symbolic links report their target's length as their size, as POSIX expects.
- A damaged or crafted disk whose bitmap claims to be larger than the volume no longer keeps Unlatch busy for minutes while it checks the disk; it is reported as corrupt.

## [1.0.0-beta.4] - 2026-11-02

### Fixed
- `brew upgrade` no longer removes the helper (and with it its Full Disk Access): the cask removes it only with `brew uninstall --zap` or Settings > Uninstall. Upgrading from beta.3 or earlier still removes it once.
- `unlatch install-helper` keeps an installed helper that has not changed, with its Full Disk Access.

## [1.0.0-beta.3] - 2026-11-02

### Fixed
- After an upgrade, macOS asked every 10 seconds whether to move the helper to the Trash ("Apple could not verify…"): the installed helper kept the download's quarantine flag. `install-helper` now clears it.

### Changed
- The helper is installed as **Unlatch Helper** (`/Library/PrivilegedHelperTools/Unlatch Helper`), the name Full Disk Access, Gatekeeper and Activity Monitor show; the old file is removed.

### Added
- **Setup & Health…** in the menu-bar dropdown, always available, with a green or orange status icon; it opens the setup window, which reruns every check.

## [1.0.0-beta.2] - 2026-11-02

### Changed
- The helper is listed under Unlatch in Login Items, and "Allow in Full Disk Access" (setup window, `unlatch setup`) opens the right list and shows the helper in Finder to drag in.
- Install docs and the Homebrew cask's message: open the app once before using `unlatch` in Terminal (until then macOS stops the bundled programs), and trust the tap first (Homebrew 7).

### Fixed
- Release workflow: the cask and the formula are attached under distinct names.

## [1.0.0-beta.1] - 2026-11-01

The first public beta. Not signed with an Apple Developer ID; see docs/install.md.

### Added
- Read-write NTFS on macOS 27+ (Apple Silicon) through macFUSE: the `unlatch-fuse` mount daemon over libntfs-3g 2026.9.28 (plus two patches), safe by default: read-write only when Windows left the volume clean, read-only with the reason otherwise (hibernated, Fast Startup, unfinished journal, dirty).
- Menu-bar app: mounts NTFS disks as they are connected (claiming them from Apple's read-only driver), per-disk choices, eject, notifications, onboarding with a self-test, log export, update notices, uninstall.
- Setup that installs macFUSE for you (downloaded from macFUSE's releases; checksum, signature and notarization checked) and walks through the one-time approvals, in the app and as `unlatch setup`.
- `unlatch` command: `list`, `mount`, `unmount` (`--all`), `eject`, `status` (JSON, schema 1), `doctor`, `setup`, `install-helper`, `uninstall-helper`.
- A privileged helper that only opens NTFS disk devices for the logged-in user; the daemon itself runs as the user.
- Extended attributes and Finder tags as NTFS named streams (no `._` files); symlinks (native Windows symlinks within the disk, WSL symlinks otherwise; reads Windows symlinks and junctions); hard links; hidden and read-only attributes.
- A write-back page cache in the engine: large copies on real disks several times faster than through the block device's cache.
- Repair of Unlatch's own crash leftovers when a volume is opened read-write.
- Homebrew cask and formula, DMG, notices with every license and the source offer.

### Known limitations
- No journal replay; eject before unplugging. Encrypted (EFS) files and BitLocker are not supported. Not indexed by Spotlight. See the README.
