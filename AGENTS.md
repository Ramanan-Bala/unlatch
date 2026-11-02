# AGENTS.md

Guidance for anyone (human or agent) working in this repository. The plan is [`docs/Unlatch-Implementation-Plan.md`](docs/Unlatch-Implementation-Plan.md); where this file and the plan disagree, the plan wins.

## Session rules

1. **Scope.** Do only the step you were given. If you find a problem that belongs to a later step, write it under "Carry-overs" in `docs/PROGRESS.md` and leave it unfixed.
2. **Handoff.** Before the session ends, add an entry to `docs/PROGRESS.md` with: what was done, the exact commands that verify it, what is left, and any surprises.
3. **Decisions.** Any choice another step depends on goes in `docs/DECISIONS.md`, one line each: `D-NN: decision — reason — step`.
4. **Real data.** Never use a drive that holds real data. Use image files and disposable disks only.
5. **Commits.** One commit per step at least, with the message `step NN: <summary>`, signed off (`git commit -s`).
6. **Apple APIs.** Before writing any FSKit, DiskArbitration or ServiceManagement code, check the current signatures in Apple's docs (`https://developer.apple.com/documentation/fskit/<symbol>`; adding `.md` gives text). The signatures in the plan show the shape of the code and may not match the SDK exactly.

## Build and test

Each step updates this list.

| What | Command |
|---|---|
| Check tools, build libntfs-3g if missing, generate `Unlatch.xcodeproj` | `Scripts/bootstrap.sh` |
| Rebuild libntfs-3g and ntfsprogs into `build/ntfs3g/` | `Scripts/build-ntfs3g.sh` |
| Build app, extension, engine, harness | `xcodebuild -scheme Unlatch build` |
| Mount an image through macFUSE (read-write when CLEAN; `-r` forces read-only; the default backend is fskit directly under `/Volumes`, kext elsewhere) | `build/DerivedData/Build/Products/Debug/unlatch-fuse [-f] [-d] [-r] [--mark-checked] [--no-cache] [--backend auto\|fskit\|kext] <img-or-/dev/diskN> <dir>`; `umount <dir>` |
| Interruption tests (kill the daemon mid-copy, force-eject) | `Tests/Fuse/interrupt.sh [--backend kext\|fskit] [kill\|detach]` |
| Test matrix (Step 17 results) | `docs/test-matrix.md` |
| Set or check the version (VERSION and every copy) | `Scripts/set-version.sh X.Y.Z`; `Scripts/set-version.sh --check` |
| Build a release: DMG, source tarball, SHA256SUMS, Homebrew files (publishes nothing) | `Scripts/release.sh [--allow-dirty]` → `build/release/<version>/`; steps in `docs/release.md` |
| Notices bundle (licenses, source offer) | `Scripts/make-notices.sh <dir> [version]` |
| Build the project website (GitHub Pages; `.github/workflows/pages.yml` deploys it from `main`) | `Scripts/build-site.sh [outdir]` → `build/site/`; preview: `python3 -m http.server -d build/site 8000` |
| Build unlatch-fuse and the helper with clang only (the Homebrew formula's path) | `Scripts/build-tools.sh <outdir>` |
| Daemon and engine logs | `log show --last 5m --info --predicate 'subsystem == "io.github.ramanan-bala.unlatch"'` (or `-f` for stderr) |
| Test on a real, disposable NTFS disk (both backends, big and small files, kill, snapshot check; prompts for replug, pull and sleep) | `Scripts/test-real-disk.sh <disk> [--quick] [--no-physical] [--fill] [--bench] [--cut] [--only s1,s2]` → `build/real-disk/` |
| Check Disk (D-93): check an image or device; `--repair` sets missing bitmap bits and clears the dirty mark when nothing is left. Exit 0 clean, 1 repaired, 4 problems left, 8 could not check | `unlatch-engine check [--repair] [--json] <img\|/dev/fd/N>`; through the helper: `unlatch-fuse --check [--repair] --fd N <dev>`; `unlatch check <disk> [--repair] [--json]` |
| Clear the dirty mark of a checked volume (D-86, D-89) | `unlatch-engine mark-clean <img\|/dev/fd/N>` |
| Run a command on a real disk through the helper (fd 3), or snapshot it | `Tests/Fuse/with-device.py <diskNsM> ro\|rw <cmd> {}`; `... ro --snapshot <out.img>` |
| Compare a read-only mount (`unlatch-fuse -r`) with the harness | `Tests/Fuse/check-readonly.sh <img> <mountpoint>` |
| Read-write suite through a mount (fresh images; xattrs, links, git clone; `--disk` mounts an attached disk, `--device` a real one in its `rw-suite/` folder) | `Tests/Fuse/rw-suite.sh --runs 3 [--backend kext\|fskit] [--no-git] [--disk \| --device diskNsM]` |
| Build engine and harness only | `xcodebuild -scheme unlatch-engine build` |
| Build UnlatchCore | `swift build --package-path Shared` |
| Test UnlatchCore | `swift test --package-path Shared`; with attached images and macFUSE: `UNLATCH_INTEGRATION=1 swift test --package-path Shared --no-parallel` (in parallel, ejects of several images at once can fail "busy") |
| Build CLI | `swift build --package-path CLI` |
| Run CLI | `swift run --package-path CLI unlatch --help`; commands in `docs/cli.md` |
| Test CLI | `swift test --package-path CLI` |
| One-time setup: install or update macFUSE (verified download), approval steps, the helper | `CLI/.build/debug/unlatch setup [--yes] [--no-helper]` |
| Install the privileged helper (real disks only) | `sudo CLI/.build/debug/unlatch install-helper` |
| Engine tests (unit, harness, leaks, written-image checks, include boundary; ~2 min, ~18 GB of sparse images, ~8 GB really written) | `Scripts/test-engine.sh` |
| Engine tests without the 4 GiB `cat` and `put`/`get` checks (~40 s) | `UNLATCH_SKIP_BIG=1 Scripts/test-engine.sh` |
| Probe an image | `build/DerivedData/Build/Products/Debug/unlatch-engine probe t1.img` (after `xcodebuild -scheme unlatch-engine -derivedDataPath build/DerivedData build`) |
| Open an image and print it | `unlatch-engine info [--rw] t1.img` |
| Browse an image | `unlatch-engine ls [--stats] <img> [path]`, `stat <img> <path>` (with a link's target), `cat <img> <path>`, `xattr <img> <path> [name]`, `tree <img> [path]` |
| fsx randomized model test | `unlatch-engine fsx <empty.img> --seed N --ops M [--verbose]`; soak: `Scripts/fsx-soak.sh --seeds 1000`; regressions: `Scripts/fsx-soak.sh --regressions` |
| Windows chkdsk oracle | `Scripts/chkdsk-vm.sh --fsx <seed>` (needs `WINDOWS_SSH`; see `docs/testing.md`) |
| Golden images | `Scripts/test-golden.sh`; regenerate with `Tests/Images/make-golden.sh` (changes serials and times); the Windows-made one with `Tests/Images/windows/make.sh` (needs `WINDOWS_SSH`) |
| Fuzz (needs Homebrew LLVM) | `Scripts/fuzz.sh --time 60`; replay saved crashes: `Scripts/fuzz.sh --regress` |
| Benchmarks (baselines incl. stock ntfs-3g; `--driver` build once) | `Scripts/build-ntfs3g.sh --driver`; `Bench/baselines.sh [--runs N] [apfs exfat ntfs3g unlatch-file unlatch]`; `Bench/compare.py` |
| CI | `.github/workflows/engine.yml` (`xcode-27` runner); `gh run list --workflow engine` |
| Change an image | `unlatch-engine put <img> <local\|-> <path>`, `get <img> <path> <local\|->`, `mkdir`, `mv <img> <from> <to>`, `rm [-d]`, `truncate <img> <path> <size>`, `touch` |
| Build a fixture image | `build/test-engine/mkfixture <img> < script` (see the header of `Engine/src/fixtures/mkfixture.c`) |
| Make a test NTFS image (prints its path) | `Scripts/make-image.sh t1 512m T1` |
| Inspect an image | `build/ntfs3g/bin/ntfsinfo -m t1.img`, `build/ntfs3g/bin/ntfsls -s t1.img` |
| Attach an image as a block device, unmounted (prints `diskN`) | `Scripts/attach-image.sh t1.img`; detach with `hdiutil detach diskN` |

Fixture reparse points: `mkfixture` `reparse PATH symlink-rel|symlink-abs|junction TARGET`. Planted damage for Check Disk: `clear-record-bit`, `clear-cluster-bits`, `share-clusters PATH OTHER`, `run-past`, `bump-seq`, `tear-record`, `zero-index DIR`, `zero-record` (header of `mkfixture.c`).

ntfsprogs tools in `build/ntfs3g/bin/`: `mkntfs ntfscat ntfsclone ntfscluster ntfscmp ntfscp ntfsfix ntfsinfo ntfslabel ntfsls ntfsresize ntfsundelete`. Put images under `build/` (ignored) or anywhere else outside the repo; `*.img` is ignored everywhere.

`Unlatch.xcodeproj` is generated from `project.yml` and is not committed. After changing `project.yml`, run `Scripts/bootstrap.sh` again.

Mounting goes through macFUSE (D-39), through its FSKit backend for mounts under `/Volumes` (D-78) and its kext backend elsewhere (tests, benchmarks): install macFUSE 5.5+ (`brew install --cask macfuse`), turn on its file system extension, and, for the kext-backed test mounts, allow its kernel extension. Test scripts that attach disks race the installed app's auto-mount: turn it off while they run. `unlatch-fuse` builds against `/usr/local` (`MACFUSE_PREFIX`). Builds are ad-hoc signed (`Config/Base.xcconfig`); the dormant FSKit module in `Extension/` needs a paid team (D-37, D-38). `docs/dev-setup.md` covers macFUSE, mounting and logs.

## Layout

```
Engine/      C engine: include/unlatch_engine.h (public seam), src/*.c,
             vendor/ntfs-3g (pinned in vendor/VERSION), vendor/patches/
EngineTool/  unlatch-engine host harness (works on image files); file_io.c is the file-backed
             ue_io_t, walk.c the host-side path walk
Helper/      the privileged helper: opens NTFS device nodes for the console user (D-57)
Fuse/        unlatch-fuse: the mount daemon, libfuse3 low-level ops over the engine (D-40)
Extension/   dormant Swift FSKit module (needs a paid team, D-38); imports the engine as
             module UnlatchEngineC (Engine/include/module.modulemap)
Shared/      Swift package UnlatchCore
CLI/         Swift package, the `unlatch` executable
App/         SwiftUI menu-bar app (LSUIElement), embeds the extension
Tests/       Engine/ C tests (run by Scripts/test-engine.sh), Images/ (golden images, Git LFS)
Bench/       benchmark scripts and results
Scripts/     build and release scripts
site/        the GitHub Pages website (built by Scripts/build-site.sh)
```

## Code conventions

- **FSKit:** check signatures in the SDK headers (`FSKit.framework/Headers`) or Apple's docs before writing FSKit code. Every engine call goes through the volume's `EngineQueue`, and replies are sent from there. Names from FSKit go through `engineName()` (NFC), and errno goes through `posixError()`.
- **Swift:** Swift 6 language mode with strict concurrency (`SWIFT_STRICT_CONCURRENCY = complete` in Xcode; `swift-tools-version: 6.0` in packages).
- **C:** C11, compiled with `-Wall -Wextra -Werror` in `Engine/` and `EngineTool/`.
- **Engine boundary:** no ntfs-3g headers outside `Engine/src`. Everything else includes only `Engine/include/unlatch_engine.h` (prefix `ue_`). Only the `UnlatchEngine` target has `build/ntfs3g/include` on its header search path.
- **Vendored code:** never edit `Engine/vendor/ntfs-3g/`. Fixes go in `Engine/vendor/patches/`. To bump the version, replace the tree from the release tarball and update `Engine/vendor/VERSION`.
- **Errors:** engine functions return 0 or a POSIX `errno` value.
- **Engine internals:** `Engine/src/internal.h` is the one place that includes `config.h` and ntfs-3g headers; every engine `.c` includes it first. The ntfs-3g headers come in through `-isystem` (D-13).
- **Engine inodes:** never keep an `ntfs_inode` open between engine calls; open with `ue_iget`, close with `ue_iput` before returning (D-24). `ntfs_delete` consumes both inodes it is given.
- **Engine threading:** not thread-safe. The host serializes every call on an engine, and `ue_probe` and `ue_open` calls with each other, because libntfs-3g has globals.
- **Architecture:** arm64 only, macOS 27+.
- **License header:** new source files start with `SPDX-License-Identifier: GPL-3.0-or-later`.
- **Safety:** when in doubt, mount read-only. Never pass `NTFS_MNT_RECOVER` or `NTFS_MNT_IGNORE_HIBERFILE` unless the user explicitly confirms.

## Identifiers

| Item | Value |
|---|---|
| Bundle prefix (`BUNDLE_PREFIX` in `project.yml`) | `io.github.ramanan-bala` |
| App / extension | `io.github.ramanan-bala.Unlatch` / `io.github.ramanan-bala.Unlatch.FSModule` |
| App Group | `group.io.github.ramanan-bala.Unlatch` |
| FS short name | `unlatch` |

In Swift, use the constants in `UnlatchCore.Unlatch` rather than repeating these strings.

## Logging

One `OSLog` subsystem, `io.github.ramanan-bala.unlatch`, with categories `engine`, `io`, `mount`, `app`, `cli`. In Swift: `Unlatch.logger("mount")`.

```sh
log stream --level debug --predicate 'subsystem == "io.github.ramanan-bala.unlatch"'
log stream --level debug --predicate 'subsystem == "io.github.ramanan-bala.unlatch" AND category == "engine"'
```
