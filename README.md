<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/brand/wordmark-dark.svg">
    <img src="docs/brand/wordmark.svg" alt="Unlatch" width="368">
  </picture>
</p>

# Unlatch

Unlatch lets your Mac write to NTFS disks: Windows-formatted USB drives, external SSDs and internal partitions. It runs [libntfs-3g](https://github.com/tuxera/ntfs-3g) behind a small mount daemon on [macFUSE](https://macfuse.github.io), with a menu-bar app that mounts disks as you plug them in and a command-line tool, `unlatch`. It is free software under the GPL.

**Status: beta.** It is tested heavily on disk images (randomized model tests, fuzzing, Windows `chkdsk` of every kind of volume it writes; see [docs/test-matrix.md](docs/test-matrix.md)) and has been used on real disks, but it is new. Keep a backup of anything that matters on the disks you use it with.

## Requirements

- macOS 27 or later on Apple Silicon.
- [macFUSE](https://macfuse.github.io) 5.4 or later, with its system extension allowed (on Apple Silicon this needs Reduced Security, set once in Recovery). Unlatch does not include macFUSE; install it first.

## Install

```sh
brew install --cask ramanan-bala/unlatch/unlatch
```

or download the DMG from [Releases](https://github.com/Ramanan-Bala/unlatch/releases). Unlatch is **not signed with an Apple Developer ID**, so the first time macOS asks: right-click Unlatch in Applications and choose **Open**. [docs/install.md](docs/install.md) walks through all of it, including the helper that real disks need.

## Use

- **Plug in a disk.** Unlatch mounts NTFS disks in Finder, under `/Volumes`, read-write when Windows left them in a clean state. Settings choose which disks are mounted automatically, and the menu has per-disk choices.
- **Eject** from the menu, from Finder, or with `unlatch eject <disk>` before unplugging.
- **Read-only when it is not safe to write.** If Windows was hibernated, used Fast Startup, or did not finish writing, Unlatch mounts the disk read-only and says why. [docs/read-only.md](docs/read-only.md) explains each reason and the fix.
- **Command line:** `unlatch list`, `mount`, `unmount`, `eject`, `status`, `doctor`; see [docs/cli.md](docs/cli.md).

## What works

- Reading and writing files and folders of any size, sparse and compressed files, Unicode names (stored in Windows' normal form, NFC).
- Extended attributes, Finder tags and comments, kept as NTFS named streams, so no `._` files appear; Windows sees them as alternate data streams.
- Symlinks: links within the disk are native Windows symlinks that Windows follows; other links use the WSL format. Windows symlinks and junctions are readable. Hard links.
- Hidden files, and the Windows read-only attribute as a missing write permission.

## Limitations

- **Windows Fast Startup** keeps the disk "in use" when Windows only appears to shut down. Unlatch then mounts read-only. Turn Fast Startup off in Windows (Control Panel > Power Options > "Choose what the power buttons do"), or hold Shift while choosing Shut Down.
- **Windows name rules apply.** Names cannot contain `\ / : * ? " < > |`, end with a dot or space, or be a reserved name such as `CON` or `aux.txt`; the Mac reports "invalid argument" for those.
- **Encrypted (EFS) files** cannot be read or written; BitLocker volumes are not supported.
- **No journal replay.** Like ntfs-3g, Unlatch does not replay the NTFS journal. Always eject before unplugging; if a disk was pulled out mid-write, run `chkdsk` on Windows before relying on it.
- **Not indexed by Spotlight**, and not usable for Time Machine.
- Permissions: files belong to the user who mounted the disk; NTFS access lists are not mapped.
- Speed depends on the disk; on this project's test Mac, sequential copies run at about 1.5–2.7 GB/s on a fast SSD image ([docs/M0-gate.md](docs/M0-gate.md)).

When something goes wrong: [docs/troubleshooting.md](docs/troubleshooting.md). Security issues: [SECURITY.md](SECURITY.md).

## Building

Xcode 27, [XcodeGen](https://github.com/yonaskolb/XcodeGen), Git LFS, and macFUSE (for its headers):

```sh
Scripts/bootstrap.sh                       # checks tools, builds libntfs-3g, generates Unlatch.xcodeproj
xcodebuild -scheme Unlatch build           # app, unlatch-fuse, helper, engine, test harness
swift build --package-path CLI             # the unlatch command
Scripts/release.sh --allow-dirty           # a local DMG in build/release/
```

[AGENTS.md](AGENTS.md) lists every build and test command; [docs/dev-setup.md](docs/dev-setup.md) covers macFUSE and logs.

## How it is built

- `Engine/`: a small C engine over libntfs-3g with one public header (`unlatch_engine.h`), a write-back page cache, and crash-leftover repair.
- `Fuse/`: `unlatch-fuse`, the mount daemon, on libfuse's low-level API.
- `Helper/`: a tiny privileged helper that only opens NTFS disk devices for the logged-in user and hands them to the daemon, which runs as you.
- `Shared/`, `CLI/`, `App/`: Swift: disk detection and mounting, the `unlatch` command, the menu-bar app.

The design and every decision behind it are in [docs/Unlatch-Implementation-Plan.md](docs/Unlatch-Implementation-Plan.md), [docs/DECISIONS.md](docs/DECISIONS.md) and [docs/PROGRESS.md](docs/PROGRESS.md).

## After 1.0

- A native FSKit module, once the project can sign one (needs a paid Apple Developer membership), replacing macFUSE.
- A repair assistant for disks that need `chkdsk`, and a format command (mkntfs).
- Engine replacement behind `unlatch_engine.h` if a better NTFS implementation becomes available.
- Localization.

## License

Unlatch is free software: GPL-3.0-or-later ([LICENSE](LICENSE)). It includes libntfs-3g (GPL-2.0-or-later) and uses swift-argument-parser (Apache-2.0); the app's Settings > About > Licenses and source has every notice and the source offer. macFUSE is separate software with its own license and is never included.

Contributions are welcome under the Developer Certificate of Origin; see [CONTRIBUTING.md](CONTRIBUTING.md).
