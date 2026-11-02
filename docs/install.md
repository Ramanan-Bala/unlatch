# Installing Unlatch

Unlatch needs **macOS 27 or later on Apple Silicon** and **[macFUSE](https://macfuse.github.io)** (free, installed separately; Unlatch never bundles it). Unlatch builds are **not signed with an Apple Developer ID** and are not notarized (there is no paid Apple membership behind the project), so macOS asks once before opening the app.

## With Homebrew (recommended)

```sh
brew tap ramanan-bala/unlatch
brew trust --tap ramanan-bala/unlatch       # Homebrew 7 loads casks from other taps only once trusted
brew install --cask ramanan-bala/unlatch/unlatch
```

This installs macFUSE (the `macfuse` cask) if needed, puts Unlatch in Applications, and links the `unlatch` command. For the command-line tools only, built from source (needs Xcode 27; Homebrew formulae cannot install casks, so macFUSE comes first):

```sh
brew install --cask macfuse
brew tap ramanan-bala/unlatch
brew trust --tap ramanan-bala/unlatch
brew install ramanan-bala/unlatch/unlatch
unlatch setup
```

## From the DMG

1. Download `Unlatch-<version>.dmg` from the [releases](https://github.com/Ramanan-Bala/unlatch/releases) and check it against `SHA256SUMS`: `shasum -a 256 -c SHA256SUMS`.
2. Drag Unlatch to Applications. You do not need to install macFUSE first: the setup window does it.

## The first start

The README's [one-time setup](../README.md#one-time-setup) has every step. In short:

1. **Open the app anyway** (right-click > Open; it is not signed with a Developer ID).
2. **Install macFUSE** with the setup window's button, or `unlatch setup`. Unlatch fetches the newest release from github.com/macfuse/macfuse, compares its SHA-256 with the list macFUSE publishes next to it, requires the package to be signed by macFUSE's developer (team 3T5GSNBU6W) and notarized by Apple, and runs Apple's `installer` with your password. Prefer to do it yourself? Install it from https://macfuse.github.io or with `brew install --cask macfuse`; Unlatch then finds it.
3. **Turn on macFUSE's file system extension** once: System Settings > General > Login Items & Extensions > File System Extensions (info button) > **macFUSE**. No kernel extension, no Recovery, no restart. Only you can switch it on, so Unlatch shows the steps and opens the pane. If macFUSE's kernel extension is already allowed on your Mac, Unlatch uses it instead (faster with many small files; see the README's "Faster small files").
4. **Real disks: the helper** (setup window or `unlatch setup`; password), then **Full Disk Access** for `/Library/PrivilegedHelperTools/Unlatch Helper`.
5. **Self-test**: the setup window mounts a tiny built-in volume and reads it back.

`unlatch doctor` checks everything at any time.

From then on, NTFS disks you connect are mounted read-write when Windows left them clean, and read-only (with the reason) otherwise. See [read-only.md](read-only.md) and [troubleshooting.md](troubleshooting.md).

## From source

See the README's Building section. `Scripts/release.sh` makes the same DMG locally.

## Updating and uninstalling

See [docs/uninstall.md](uninstall.md) and [docs/updating.md](updating.md).
