# TODO

Open work, newest first. Details and history are in `docs/PROGRESS.md` and `docs/DECISIONS.md`.

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

- [ ] `brew upgrade --cask unlatch` removes the helper: Homebrew runs the old cask's whole `uninstall` stanza on upgrade (only `signal` is skipped; `Library/Homebrew/cask/artifact/abstract_uninstall.rb`), including `launchctl` and `delete` for the helper, so after every upgrade the helper must be installed again and, with ad-hoc signing, Full Disk Access granted again. Options: move the helper's removal to `zap` (a plain uninstall then leaves the root helper behind until `--zap` or Settings > Uninstall), or have the app detect a missing helper after an upgrade and offer to reinstall it in one click. Decide, then update the cask, `docs/updating.md` and D-75.
- [ ] The app's `CFBundleShortVersionString` reads `1.0` for `1.0.0-beta.2` (the build cuts off the pre-release part). Set it to the numeric `X.Y.Z` explicitly and keep the full version elsewhere (for example a custom Info.plist key, or `CFBundleVersion`), so Finder and About show the real version.
- [ ] Full benchmark rerun (`Bench/baselines.sh`, all setups) and record it in `docs/M0-gate.md`.
- [ ] Hardware tests in `docs/test-matrix.md`: USB stick pulled mid-copy, sleep and wake, a large real disk, the 24-hour soak, a clean-Mac install.
- [ ] Windows-made images still wanted: Fast Startup, dirty, fragmented `$MFT` (`Tests/Images/README.md`).
