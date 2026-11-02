# TODO

Open work, newest first. Details and history are in `docs/PROGRESS.md` and `docs/DECISIONS.md`.

## Check Disk: check and repair a disk marked dirty on the Mac (D-90, D-92)

Today a Mac-only user's choices for a disk marked dirty are read-only, "Mount Read-Write Anyway" or "Mark as Checked", none of which looks at the disk. Goal: Unlatch checks it and clears the mark only when it is consistent. macOS has no NTFS checker (ntfs-3g's ntfsck is 2006 code, quarantined upstream, check-only).

- [ ] A read-only checker in the engine (`ue_check`): every in-use MFT record is marked in `$MFT`'s bitmap; every cluster a file's runlist uses is marked in `$Bitmap`; no cluster belongs to two files; every directory index can be read and points at in-use records with matching sequence numbers.
- [ ] Safe repair only: set bitmap bits for records and clusters in use (at worst space is lost, nothing is overwritten). Anything else (an unreadable index, a shared cluster) is reported, never repaired.
- [ ] Clear the mark (`UE_OPEN_CLEAR_DIRTY`) only when the check finds nothing left.
- [ ] `unlatch check <disk> [--repair] [--json]` and a "Check Disk…" item in the app's ⋯ menu with a report of what was found.
- [ ] Tests: fixture images with each kind of damage (`mkfixture`), a SIGKILL mid-copy image from `interrupt.sh`, and the owner's SanDisk stick snapshot (two folders whose index blocks were never written).

## Full Disk Access on the app instead of the helper (D-74)

Today macOS asks for Full Disk Access for `io.github.ramanan-bala.unlatch.helper`, because the helper is what opens the root-only disk devices. Goal: the user grants it to **Unlatch** once.

- [ ] Ship the helper inside the app as a bare executable in `Unlatch.app/Contents/MacOS/` (no bundle of its own), with its launchd plist in `Contents/Library/LaunchDaemons/` (`BundleProgram`, the socket, `AssociatedBundleIdentifiers`).
- [ ] Register it with `SMAppService.daemon(plistName:)` from the app's setup window (and unregister it in Uninstall); the user approves it in Login Items. Check Apple's ServiceManagement docs first (AGENTS.md).
- [ ] Find out whether SMAppService registers a daemon of an **ad-hoc signed** app at all.
- [ ] Test with a **disposable USB stick**: grant Full Disk Access to Unlatch only, then mount the stick read-write. The only removable disk on the dev Mac holds real data and must not be used.
- [ ] Check that the grant survives an update (ad-hoc signatures change every build).
- [ ] If it works: replace `unlatch install-helper`'s `/Library/PrivilegedHelperTools` install, update `HelperOpener`, the README's one-time setup, the cask's uninstall step and D-57/D-60/D-74. If not: keep the current helper and record why.

Background: Apple DTS says a daemon generally does not inherit its app's TCC grants, even via SMAppService ([forum 853725](https://developer.apple.com/forums/thread/853725)); the bare-executable layout is undocumented and was reported to work for Accessibility, not tested for Full Disk Access.

## Other open items

- [ ] Full benchmark rerun (`Bench/baselines.sh`, all setups) and record it in `docs/M0-gate.md`.
- [ ] Hardware tests in `docs/test-matrix.md`: USB stick pulled mid-copy, sleep and wake, a large real disk, the 24-hour soak, a clean-Mac install.
- [ ] Windows-made images still wanted: Fast Startup, dirty, fragmented `$MFT` (`Tests/Images/README.md`).
