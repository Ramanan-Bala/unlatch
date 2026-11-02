# Beta plan (plan Step 20)

Goal: confidence that Unlatch does not lose or corrupt data on real disks before 1.0. A release blocker is **any data-integrity issue**: a file that reads back different from what was written, a volume Windows' `chkdsk` reports problems on after Unlatch wrote to it, or anything lost. Everything else is filed and triaged normally.

## Who and what

- At least **3 Macs** (Apple Silicon, 8 GB and up) on at least **2 macOS 27.x builds**.
- A mix of disks: USB hard disk, USB or Thunderbolt SSD, a Windows-formatted USB stick, an SD card, and if possible an internal NTFS partition. At least one should be shared with a Windows PC in daily use.
- Release: `1.0.0-beta.N`, published as a pre-release; testers install with Homebrew (`brew tap ramanan-bala/unlatch`, `brew trust --tap ramanan-bala/unlatch`, `brew install --cask ramanan-bala/unlatch/unlatch`) or the DMG. The app offers newer betas to beta users only.

## What testers do

1. Install (docs/install.md), finish setup, note any step that was unclear.
2. Daily use for two weeks: copy photos and videos both ways, edit documents in place, unzip and delete large trees, use Finder tags, keep a git repository on the disk.
3. Once a week: eject, attach to Windows, run `chkdsk X:` (no `/f`), and report the summary line.
4. Deliberately: unplug once without ejecting (on a disk whose content is backed up), put the Mac to sleep with a disk mounted, and use a disk Windows left in Fast Startup.
5. Report with Settings > Troubleshooting > Export logs, plus the issue template's questions.

## Triage

| Kind | Example | Action |
|---|---|---|
| Data integrity (blocker) | checksum differs, `chkdsk` errors after Unlatch wrote, file lost | fix before 1.0; add a regression test (fsx seed, golden image or rw-suite step) |
| Safety | a dirty or hibernated disk mounted read-write | blocker |
| Functional | a mount fails, a Finder action errors, wrong read-only reason | fix or document; not a blocker unless common |
| Cosmetic, wording | menu text, onboarding | file |

## Exit criteria for 1.0

- No open blocker; every fixed blocker has a regression test.
- Each tester reports at least one clean `chkdsk` after two weeks of use.
- `docs/test-matrix.md`'s open hardware rows (USB pull, sleep and wake, large disks, 24-hour soak) are run on a tester's disk.
- The benchmark (`Bench/baselines.sh`) is rerun and recorded in docs/M0-gate.md.
