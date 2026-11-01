# Contributing to Unlatch

Thank you for helping. Unlatch is GPL-3.0-or-later; by contributing you agree your work is released under that license.

## Developer Certificate of Origin

There is no CLA. Every commit must carry a DCO sign-off, which certifies the statements in [`DCO`](DCO):

```sh
git commit -s
```

This adds a line like `Signed-off-by: Your Name <you@example.com>` using your Git name and email. Commits without it cannot be merged.

## Pull requests

- Keep PRs small and focused.
- One implementation-plan step per PR. Steps are defined in [`docs/Unlatch-Implementation-Plan.md`](docs/Unlatch-Implementation-Plan.md).
- Commit messages for plan steps use `step NN: <summary>`.
- Update [`docs/PROGRESS.md`](docs/PROGRESS.md), and [`docs/DECISIONS.md`](docs/DECISIONS.md) when you decide something other steps depend on.
- Follow the conventions in [`AGENTS.md`](AGENTS.md).
- Never test against a drive that holds real data. Use image files and disposable disks.

## Before you open a pull request

Run the checks for what you changed (AGENTS.md has every command):

- Engine or daemon: `UNLATCH_SKIP_BIG=1 Scripts/test-engine.sh`, `Scripts/fsx-soak.sh --regressions`, `Tests/Fuse/rw-suite.sh --runs 3` (and `--disk`), and `Scripts/fuzz.sh --time 60` when parsing code changed.
- Swift: `swift test --package-path Shared` (`UNLATCH_INTEGRATION=1` with macFUSE), `swift test --package-path CLI`.
- Anything that changes what is written to disk: a Windows `chkdsk` of a written image if you can (`Scripts/chkdsk-vm.sh`, docs/testing.md).
- Add a line to `CHANGELOG.md` under `[Unreleased]`.

Bug reports: use the issue template; security problems go to private reporting (SECURITY.md).
