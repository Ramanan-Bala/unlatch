# Installing Unlatch

Unlatch needs **macOS 27 or later on Apple Silicon** and **[macFUSE](https://macfuse.github.io)** (free, installed separately; Unlatch never bundles it). Unlatch builds are **not signed with an Apple Developer ID** and are not notarized (there is no paid Apple membership behind the project), so macOS asks once before opening the app.

## With Homebrew (recommended)

```sh
brew install --cask ramanan-bala/unlatch/unlatch
```

This installs macFUSE (the `macfuse` cask) if needed, puts Unlatch in Applications, and links the `unlatch` command. For the command-line tools only, built from source (needs Xcode 27):

```sh
brew install ramanan-bala/unlatch/unlatch
```

## From the DMG

1. Install macFUSE from https://macfuse.github.io (or `brew install --cask macfuse`).
2. Download `Unlatch-<version>.dmg` from the [releases](https://github.com/Ramanan-Bala/unlatch/releases) and check it against `SHA256SUMS`: `shasum -a 256 -c SHA256SUMS`.
3. Drag Unlatch to Applications.

## The first start

1. **Open the app anyway.** macOS says it cannot verify the developer. Right-click Unlatch in Applications, choose **Open**, and confirm. If that button is missing: System Settings > Privacy & Security > **Open Anyway**. Only needed once per version.
2. **Allow macFUSE's system extension.** On Apple Silicon this needs Reduced Security in Recovery once, then approval in System Settings > Privacy & Security. Unlatch's setup window checks it and links to macFUSE's instructions.
3. **Real disks: install the helper.** Disk images work at once. For USB and internal disks, the setup window installs a small privileged helper (it asks for your password), and you give it **Full Disk Access**: System Settings > Privacy & Security > Full Disk Access > + > ⌘⇧G > `/Library/PrivilegedHelperTools/io.github.ramanan-bala.unlatch.helper`. From a terminal: `sudo unlatch install-helper`.
4. The setup window runs a self-test: it mounts a tiny built-in volume and reads it back.

From then on, NTFS disks you connect are mounted read-write when Windows left them clean, and read-only (with the reason) otherwise. See the [README](../README.md) for what that means and [troubleshooting](troubleshooting.md) when something goes wrong.

## From source

See the README's Building section. `Scripts/release.sh` makes the same DMG locally.

## Updating and uninstalling

See [docs/uninstall.md](uninstall.md) and [docs/updating.md](updating.md).
