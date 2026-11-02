# Updating Unlatch

Unlatch has no automatic updater: a signed updater (Sparkle) needs an Apple Developer ID, which the project does not have (D-70). Instead:

- **The app tells you.** Once a day (Settings > Updates, on by default) it asks GitHub for the list of releases. Nothing about your Mac or disks is sent. When a newer version exists, the menu shows **"Unlatch X is available…"**. Beta builds are offered only to people already running a beta.
- **Choosing that item** first offers to unmount Unlatch's volumes, so no copy is cut off when the app quits. Then:
  - installed with Homebrew: it puts `brew upgrade --cask unlatch` on the clipboard for you to run in Terminal;
  - installed from the DMG: it opens the release page; download the new DMG and drag the app over the old one.

## With Homebrew

```sh
brew upgrade --cask unlatch        # the app; its uninstall step runs `unlatch unmount --all` first
brew upgrade unlatch               # the command-line formula, if you use that instead
```

## By hand

1. `unlatch unmount --all` (or eject the volumes from the menu), then quit Unlatch.
2. Install the new DMG's app over the old one; open it once with right-click > Open (it is not signed with a Developer ID).
3. If the release notes say the helper changed, run `sudo unlatch install-helper` again. Full Disk Access is kept.

Your settings, the per-volume auto-mount choices and the helper survive an update, including `brew upgrade` (from 1.0.0-beta.4 on; D-76). When a new version changes the helper itself, Setup & Health shows it: click **Install…** there, and allow **Unlatch Helper** in Full Disk Access again if macOS asks. Reinstalling an unchanged helper keeps it and its Full Disk Access.

**Upgrading from 1.0.0-beta.3 or earlier with Homebrew** still removes the helper once, because the old version's uninstall step runs during the upgrade. Afterwards open **Setup & Health…**, click **Install…** next to the helper, and allow **Unlatch Helper** in Full Disk Access.
