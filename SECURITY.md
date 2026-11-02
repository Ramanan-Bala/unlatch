# Security

## Supported versions

Only the latest release, and the latest beta while a beta is running, receive fixes.

## Reporting a vulnerability

Please do **not** open a public issue. Use GitHub's private vulnerability reporting: the repository's **Security** tab > **Report a vulnerability** (https://github.com/Ramanan-Bala/unlatch/security/advisories/new). Include the version, what an attacker can do, and how to reproduce it; a crafted disk image is the most useful form. You should get an answer within a week. Fixes are released as soon as they are ready, with credit unless you prefer otherwise.

Problems that belong to libntfs-3g itself are also reported upstream (https://github.com/tuxera/ntfs-3g), after coordinating with you.

## What is in scope

Unlatch reads disks that may come from anyone, so the most important surface is **parsing an untrusted NTFS volume**:

- `unlatch-fuse` and the engine run **as the logged-in user**, never as root. A malicious disk that exploits a parser bug gets that user's rights, not the system's.
- The **privileged helper** (`/Library/PrivilegedHelperTools/Unlatch Helper`, runs as root) does one thing: for the console user, it opens the device node of a volume DiskArbitration reports as NTFS (name checked against `^disk[0-9]+(s[0-9]+)?$`) and passes the descriptor back. It never reads or parses the disk (D-57). Bugs that let another user, a non-NTFS device, or a crafted request through are in scope.
- The engine and libntfs-3g are fuzzed continuously in CI with AddressSanitizer and UndefinedBehaviorSanitizer (`Scripts/fuzz.sh`); crashing inputs are kept in `Tests/Fuzz/crashes/`.
- The app's update check sends one unauthenticated HTTPS request to api.github.com and parses the reply as data only; it never downloads or runs anything.

Out of scope: problems that need an attacker who already has administrator rights or physical access to an unlocked Mac, and the lack of a Developer ID signature (a known limitation, D-69).
