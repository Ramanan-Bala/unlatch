---
name: Bug report
about: Something does not work, or a disk did not behave as expected
labels: bug
---

**What happened** (and what you expected):

**Could data be lost or damaged?** (a file that reads back wrong, Windows `chkdsk` errors after Unlatch wrote to the disk) yes / no / not sure

**Steps to reproduce:**

**Versions:**
- `unlatch --version`:
- macOS (`sw_vers -productVersion` and build):
- macFUSE (`unlatch doctor`):
- Installed with: Homebrew / DMG / source

**The disk:** kind (USB HDD, SSD, stick, internal), size, how it was formatted (Windows version), and `unlatch status <disk>`:

**Logs:** attach the file from Settings > Troubleshooting > Export logs (or `log show --last 1h --info --predicate 'subsystem == "io.github.ramanan-bala.unlatch"'`).

Security problems: please use private reporting instead (SECURITY.md).
