# Homebrew tap files

Templates for the tap repository `Ramanan-Bala/homebrew-unlatch` (plan Step 18, D-69). `Scripts/release.sh` fills in the version and checksums and writes the finished files to `build/release/<version>/homebrew/`; the release workflow attaches them to the draft release. Publishing the tap is the owner's step (docs/release.md).

| File | Installs | Notes |
|---|---|---|
| `Casks/unlatch.rb.in` | `Unlatch.app` from the release DMG, and links `unlatch` | `depends_on cask: "macfuse"`; uninstall unmounts Unlatch volumes, removes the helper; `zap` removes status files and settings |
| `Formula/unlatch.rb.in` | `unlatch`, `unlatch-fuse` and the helper, built from the source tarball | formulae cannot depend on casks, so it stops with a message when macFUSE's headers are missing; built with `Scripts/build-tools.sh` (clang) and SwiftPM, because xcodebuild's package resolution cannot run in Homebrew's sandbox |

Users:

```sh
brew install --cask ramanan-bala/unlatch/unlatch      # the app (and macFUSE)
brew install ramanan-bala/unlatch/unlatch             # or only the command-line tools
```

Checked locally with a throwaway tap: `brew style` and `brew audit` pass for both; the formula builds, installs and passes `brew test` from a `file://` tarball.
