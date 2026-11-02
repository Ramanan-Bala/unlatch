# Changelog

Notable changes per release. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow [Semantic Versioning](https://semver.org/). The release workflow copies a version's section into its GitHub release notes.

## [Unreleased]

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
