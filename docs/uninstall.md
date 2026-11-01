# Uninstalling Unlatch

## From the app

Settings > About > **Uninstall Unlatch…**: it unmounts Unlatch's volumes (asking first), removes the privileged helper (macOS asks for your password), removes the login item, deletes Unlatch's status files and settings, moves the app to the Trash and quits.

## With Homebrew

```sh
brew uninstall --cask unlatch          # unmounts, removes the helper and the app
brew uninstall --cask --zap unlatch    # also deletes status files and settings
brew uninstall unlatch                 # the command-line formula; run `sudo unlatch uninstall-helper` first
```

## By hand

```sh
unlatch unmount --all
sudo unlatch uninstall-helper          # the launchd job, the helper binary and its socket
rm -rf ~/Library/Application\ Support/Unlatch
defaults delete io.github.ramanan-bala.Unlatch
```

Then quit Unlatch and move it from Applications to the Trash. If you added it under System Settings > General > Login Items, it disappears from there with the app. The helper's entry under Privacy & Security > Full Disk Access can be removed with the **−** button.

## What stays

- **macFUSE.** Other software may use it, so Unlatch never removes it. To remove it too: `brew uninstall --cask macfuse`, or macFUSE's own uninstaller in System Settings > macFUSE.
- **Your NTFS disks.** Nothing on them belongs to Unlatch. Files named `Unlatch recovered N` exist only if a rename was interrupted by a crash (D-65); they are ordinary files you can keep or delete.
