<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/brand/wordmark-dark.svg">
    <img src="docs/brand/wordmark.svg" alt="Unlatch" width="368">
  </picture>
</p>

# Unlatch

Unlatch mounts NTFS volumes read-write on macOS 27+ (Apple Silicon). It is an FSKit file system extension with [libntfs-3g](https://github.com/tuxera/ntfs-3g) running in-process, plus a menu-bar app and a command-line tool called `unlatch`.

## Status

Early development. Nothing works yet: the repository holds the project scaffold only. Do not use Unlatch on a drive that holds data you care about.

The work follows [`docs/Unlatch-Implementation-Plan.md`](docs/Unlatch-Implementation-Plan.md). Progress is logged in [`docs/PROGRESS.md`](docs/PROGRESS.md).

## Requirements

- macOS 27 or later on Apple Silicon
- Xcode 27+, [XcodeGen](https://github.com/yonaskolb/XcodeGen) and Git LFS to build (`brew install xcodegen git-lfs`)

## Building

```sh
Scripts/bootstrap.sh                     # checks tools, generates Unlatch.xcodeproj
xcodebuild -scheme Unlatch build         # app, FSKit extension, engine, unlatch-engine
swift build --package-path Shared        # UnlatchCore
swift run --package-path CLI unlatch --help
```

See [`AGENTS.md`](AGENTS.md) for the full list of build and test commands.

## License

Unlatch is free software, licensed under the GNU General Public License, version 3 or (at your option) any later version (GPL-3.0-or-later). See [`LICENSE`](LICENSE). It links libntfs-3g, which is licensed under the GPL v2 or later.

Contributions are welcome under the Developer Certificate of Origin; see [`CONTRIBUTING.md`](CONTRIBUTING.md).
