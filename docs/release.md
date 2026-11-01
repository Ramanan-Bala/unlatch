# Making a release

Plan Steps 18–20, re-scoped by D-69: no Developer ID, no notarization, no Sparkle. A release is a DMG with an ad-hoc signed app, a source tarball, checksums, and the Homebrew tap files. **Every step that publishes something is the owner's** (pushing a tag, publishing the draft release, pushing the tap).

## 1. Prepare (any machine with the repo)

```sh
Scripts/set-version.sh 1.0.0          # VERSION, project.yml, UnlatchCore, engine, daemon; bumps the build number
Scripts/bootstrap.sh                  # regenerate the project
```

Move the `[Unreleased]` entries in `CHANGELOG.md` under `## [1.0.0] - YYYY-MM-DD`, then run the gates (AGENTS.md: engine tests, fsx regressions, rw-suite on files and disks, Swift tests, fuzz, a Windows chkdsk) and commit: `git commit -s -m "release 1.0.0"`.

## 2. Build and check locally

```sh
Scripts/release.sh                    # refuses a dirty tree; --allow-dirty for a trial run
ls build/release/1.0.0                # Unlatch-1.0.0.dmg, unlatch-1.0.0.tar.gz, SHA256SUMS, homebrew/
```

The script signs inside out (the three helpers, then the app) ad hoc with the hardened runtime. `unlatch-fuse` carries `com.apple.security.cs.disable-library-validation` (`Packaging/unlatch-fuse.entitlements`) so it can load macFUSE's libfuse3, and the script checks that it starts. `spctl` is expected to say "rejected". Then try the DMG: open it, copy the app somewhere, start it, run its self-test, and mount an image read-write with `Contents/Helpers/unlatch-fuse`.

## 3. Publish (owner)

1. `git push` and wait for CI to pass.
2. `git tag -s v1.0.0 -m "Unlatch 1.0.0"` and `git push origin v1.0.0`. The `release` workflow builds everything again on GitHub's runner and creates a **draft** release with the files and the changelog section as notes.
3. Read the draft, compare its `SHA256SUMS` with your local build if you like (they differ: the build is not bit-for-bit reproducible), then **Publish**. A version with a `-` (for example `1.0.0-beta.1`) is marked pre-release.
4. Tap: the repository `Ramanan-Bala/homebrew-unlatch` holds `Casks/unlatch.rb` and `Formula/unlatch.rb`. Copy the two files from the release (or `build/release/<v>/homebrew/`) into it, commit and push. Users then get it with `brew upgrade`.

## Checklist for the release notes

- What changed (from CHANGELOG.md).
- Tested on: macOS build, macFUSE version, disks.
- Known limitations (README).
- The install footer is appended automatically (`docs/release-notes-footer.md`).
