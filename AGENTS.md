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
| Build engine and harness only | `xcodebuild -scheme unlatch-engine build` |
| Build UnlatchCore | `swift build --package-path Shared` |
| Test UnlatchCore | `swift test --package-path Shared` |
| Build CLI | `swift build --package-path CLI` |
| Run CLI | `swift run --package-path CLI unlatch --help` |
| Engine tests (unit, harness, leaks, written-image checks, include boundary; ~2 min, ~18 GB of sparse images, ~8 GB really written) | `Scripts/test-engine.sh` |
| Engine tests without the 4 GiB `cat` and `put`/`get` checks (~40 s) | `UNLATCH_SKIP_BIG=1 Scripts/test-engine.sh` |
| Probe an image | `build/DerivedData/Build/Products/Debug/unlatch-engine probe t1.img` (after `xcodebuild -scheme unlatch-engine -derivedDataPath build/DerivedData build`) |
| Open an image and print it | `unlatch-engine info [--rw] t1.img` |
| Browse an image | `unlatch-engine ls [--stats] <img> [path]`, `stat <img> <path>`, `cat <img> <path>`, `tree <img> [path]` |
| fsx randomized model test | `unlatch-engine fsx <empty.img> --seed N --ops M [--verbose]`; soak: `Scripts/fsx-soak.sh --seeds 1000`; regressions: `Scripts/fsx-soak.sh --regressions` |
| Windows chkdsk oracle | `Scripts/chkdsk-vm.sh --fsx <seed>` (needs `WINDOWS_SSH`; see `docs/testing.md`) |
| Change an image | `unlatch-engine put <img> <local\|-> <path>`, `get <img> <path> <local\|->`, `mkdir`, `mv <img> <from> <to>`, `rm [-d]`, `truncate <img> <path> <size>`, `touch` |
| Build a fixture image | `build/test-engine/mkfixture <img> < script` (see the header of `Engine/src/fixtures/mkfixture.c`) |
| Make a test NTFS image (prints its path) | `Scripts/make-image.sh t1 512m T1` |
| Inspect an image | `build/ntfs3g/bin/ntfsinfo -m t1.img`, `build/ntfs3g/bin/ntfsls -s t1.img` |
| Attach an image as a block device, unmounted (prints `diskN`) | `Scripts/attach-image.sh t1.img`; detach with `hdiutil detach diskN` |

ntfsprogs tools in `build/ntfs3g/bin/`: `mkntfs ntfscat ntfsclone ntfscluster ntfscmp ntfscp ntfsfix ntfsinfo ntfslabel ntfsls ntfsresize ntfsundelete`. Put images under `build/` (ignored) or anywhere else outside the repo; `*.img` is ignored everywhere.

`Unlatch.xcodeproj` is generated from `project.yml` and is not committed. After changing `project.yml`, run `Scripts/bootstrap.sh` again.

Builds are ad-hoc signed (`CODE_SIGN_IDENTITY = -`). The entitlements files in `App/Resources/` and `Extension/Resources/` exist but are not applied yet, because restricted entitlements need a provisioning profile (D-06).

## Layout

```
Engine/      C engine: include/unlatch_engine.h (public seam), src/*.c,
             vendor/ntfs-3g (pinned in vendor/VERSION), vendor/patches/
EngineTool/  unlatch-engine host harness (works on image files); file_io.c is the file-backed
             ue_io_t, walk.c the host-side path walk
Extension/   Swift FSKit module
Shared/      Swift package UnlatchCore
CLI/         Swift package, the `unlatch` executable
App/         SwiftUI menu-bar app (LSUIElement), embeds the extension
Tests/       Engine/ C tests (run by Scripts/test-engine.sh), Images/ (golden images, Git LFS)
Bench/       benchmark scripts and results
Scripts/     build and release scripts
```

## Code conventions

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
