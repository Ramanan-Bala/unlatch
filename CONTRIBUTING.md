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
