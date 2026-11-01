# Unlatch: Review of the Build Guide

Review of `docs/NTFS for Mac Complete Build Guide.md`. These decisions are fixed (confirmed 2026-11-01):

- **Name:** Unlatch. Covers the app, CLI, bundle IDs and docs.
- **License:** GPL-3.0-or-later, open source, source in a public GitHub repo. Not on the Mac App Store.
- **Distribution:** free Developer ID DMG on GitHub Releases. Anyone may use, modify and redistribute it under the GPL.
- **Minimum OS:** macOS 27. This lets us use the FSKit Handler protocols, kernel-offloaded I/O, `DataCacheHandler` and `FSClient.mountSingleVolume`.

All ten recommendations in `docs/Unlatch-Libraries-and-Licenses.html` §9 are accepted. Contributors sign off with a DCO; no CLA. Paid support, sponsorship or paid builds may be added later without changing the license.

The implementation plan in `docs/Unlatch-Implementation-Plan.md` already includes every change below. Where the guide and the plan disagree, follow the plan.

---

## A. Errors in the guide (they would block or break the build)

| # | What the guide says | What is true | Fix |
|---|---|---|---|
| A1 | The libntfs-3g library is "LGPL, as far as I know" | The libntfs-3g sources (for example `libntfs-3g/volume.c`) are **GPL v2 or later**. Anything linking it in-process is a combined work under the GPL. | The project is GPL-3.0-or-later (now decided). Drop the "closed source with relinking" discussion. Ship the full source and license texts. |
| A2 | The engine header uses `ntfs_create`, `ntfs_readdir`, `ntfs_open`, … | libntfs-3g already exports symbols with these names (`ntfs_create`, `ntfs_readdir`, `ntfs_mount`, …). They would collide at link time. | Use the prefix `ue_` (Unlatch Engine) for the seam. |
| A3 | "The app or CLI runs a safety probe through the engine" before mounting | Only root can read `/dev/diskN` (`root:operator 0640`). An unprivileged app or CLI cannot read the volume. | The safety decision belongs in the **extension**, which gets the device from FSKit, during `probeResource` / `startCheck` / `loadResource`. The extension writes the verdict to a shared App Group container, and the app and CLI read it there. |
| A4 | Not mentioned | FSKit: *"The system doesn't mount block device file systems until they pass a file system check."* The `FSUnaryFileSystem` must conform to `FSManageableResourceMaintenanceOperations` (`startCheck`, `startFormat`). | Implement `startCheck` as the safety probe. Make `startFormat` return `ENOTSUP` for v1. |
| A5 | Volume ops use the `FS…Operations` protocols from macOS 15.4 | Those protocols are deprecated on macOS 27. The replacements are `FSVolume.Handler`, `ReadWriteHandler`, `OpenCloseHandler`/`DataCacheHandler`, `XattrHandler`, `KernelOffloadedIOHandler` and others, which return `FSVolumeHandlerResult` objects. | Write against the Handler protocols only. |
| A6 | Build universal (arm64 + x86_64) | macOS 26 was the last release for Intel Macs, so macOS 27 is Apple Silicon only. | Build arm64 only. Remove every universal-build step and the Intel row from the test matrix. |
| A7 | "Check whether mounting needs a privileged helper" | macOS 27 has `FSClient.shared.mountSingleVolume(resource:bundleID:options:)` (entitlement `com.apple.developer.fskit.mount`). It loads, activates and creates the `/Volumes` mount point. | Use it. No `SMAppService` daemon. Keep `mount -F -t unlatch` only as a developer fallback. |
| A8 | "Unmount Apple's read-only mount, then mount ours" | There is a race: Apple's driver auto-mounts first, and Spotlight or Finder can open files before the swap. | Register a DiskArbitration **mount-approval callback** for NTFS disks the user opted in. Dissent Apple's mount, then mount with Unlatch. Keep unmount-and-swap as the fallback for manual mounts. |
| A9 | `ntfs_ino_t` = "MFT record number" | MFT records are reused. A record number alone gives stale handles after delete and recreate. | Use the full 64-bit NTFS file reference: 48-bit record plus 16-bit sequence number. The sequence number becomes the FSKit item generation. |
| A10 | CLI named `ntfs`, symlinked into `/usr/local/bin` | That name is generic and likely to collide. `/usr/local/bin` needs admin rights and is not on PATH by default on Apple Silicon. | Name the CLI `unlatch`. Offer the symlink in `~/.local/bin`, or print the command for `/usr/local/bin`. |
| A11 | "`ntfs-3g.probe`", "ntfs-3g's check tool" | There is no real fsck. `ntfsfix` only resets the journal and dirty flag, and `ntfs-3g.probe` needs the FUSE driver build. | Build `ntfsprogs` (`mkntfs`, `ntfsfix`, `ntfsinfo`, `ntfscluster`, `ntfsls`) from the pinned source with no FUSE. Use **Windows `chkdsk` in a VM** as the real consistency oracle. |

## B. Design improvements

### B1. Kernel-offloaded I/O is the main speed lever, not one item in a list
macOS 27's `KernelOffloadedIOHandler` lets the volume give the kernel **extent maps** (`blockmapFile`, `completeIO`, `preallocateSpace`, KOIO `createFile`/`lookupItem`). The kernel then moves file data straight to the device through the unified buffer cache. The FSKit user-space round trip leaves the data path entirely, and so does the serial engine queue.

- This replaces most of the guide's §7 lever table (read-ahead, write coalescing, buffer reuse). The kernel's buffer cache already does that work.
- The engine only has to map file offsets to device extents and allocate clusters. Both are metadata operations on the one serial queue.
- Files the kernel cannot handle get `inhibitKernelOffloadedIO` and go through `ReadWriteHandler`. That covers resident (small) data, compressed, encrypted and sparse-hole regions, and files whose `initialized_size` is less than `data_size`.
- Plan: build the plain `ReadWriteHandler` first so the code is correct, then add KOIO as the performance milestone, and measure both.

### B2. A host-side engine harness before FSKit
Any bug that shows up only through FSKit is slow to debug: the extension has to be approved and signed, and crashes are hard to reach. So add a plain command-line tool, `unlatch-engine`, that links the engine and runs against **image files**: `probe | ls | stat | cat | put | mkdir | mv | rm | truncate | fsx`. All engine work, the fuzzers and CI use it. FSKit work starts only after the engine passes its tests on the host.

### B3. A fuller engine seam
The guide's header can't support a working macOS volume. Missing:
- `ue_statfs`: free and total space. Without it Finder shows no free space and refuses copies.
- `ue_setattr`: times, size, DOS flags (hidden and read-only map to `UF_HIDDEN` and `UF_IMMUTABLE`).
- `ue_readdir` that returns **attributes with each entry**. FSKit's `enumerateDirectory` asks for them, and N extra `getattr` calls make large folders slow. It also needs stable cookies and a directory verifier.
- An inode lifetime: an open-inode cache in the engine, released by `ue_release` when FSKit calls `reclaimItem`.
- Removal that tells files from directories (`ENOTEMPTY`, `ENOTDIR`, `EISDIR`).
- Rename that can replace an existing target atomically.
- Later steps add xattrs (NTFS named streams), symlinks (reparse points), hard links and `ue_blockmap` for KOIO.
- **Errors use plain POSIX `errno` ints.** Swift already has `POSIXError(POSIXErrorCode(rawValue:))`, so the guide's custom enum and mapping layer go away. Volume state (dirty, hibernated) goes in the probe struct, not in error codes.
- A declared ABI version and a threading contract: "not thread-safe; the host serializes all calls per engine".

### B4. Name semantics (missing from the guide, and they decide whether Finder works)
- **Case:** report the volume as case-insensitive and case-preserving, which matches Windows. Build the upcase table with `ntfs_set_ignore_case`.
- **Unicode normalization:** macOS may send NFD names. Windows writes NFC. Normalize to NFC at the seam, in Swift, so a file named `é` created on Windows can be found on the Mac.
- **Forbidden Win32 characters** (`\ : * ? " < > |`, trailing dot or space, reserved names like `CON`): reject them with `EINVAL` in v1, and document that. A Services-for-Mac-style remap could come later.
- **Hidden and system files:** hide `$MFT`, `$LogFile` and the other metafiles. Show `hiberfil.sys` and `pagefile.sys` with `UF_HIDDEN`.

### B5. macOS side effects of a writable volume
When a volume mounts read-write, macOS writes `.Spotlight-V100`, `.fseventsd`, `.Trashes` and `._*` AppleDouble files. Plan for it:
- Implement `XattrHandler` backed by NTFS named streams, so Finder metadata goes into the file and not into `._` files.
- Add a per-volume option to turn Spotlight indexing off (`mdutil -i off`).
- Make sure Time Machine does not offer the volume as a backup disk.

### B6. Safety model, made precise using libntfs-3g's own checks
For a read-write mount, libntfs-3g already refuses:
- a hibernated `hiberfil.sys` (`EPERM`)
- an unclean `$LogFile` (`EOPNOTSUPP`)
- a Fast Startup log, restart page v2.0 (`EPERM`)

The engine probe mounts read-only, records the volume dirty flag, then runs the hiberfile and logfile checks explicitly. It maps the result to `clean | dirty | hibernated | unclean_log | corrupt | not_ntfs`. Never pass `NTFS_MNT_RECOVER` or `NTFS_MNT_IGNORE_HIBERFILE` by default. Repair (`ntfsfix`-equivalent) is an explicit, confirmed action, and its text recommends `chkdsk` on Windows.

### B7. How status reaches the user
The extension writes one JSON record per volume to the App Group container: state, reason, read-only or read-write, engine version. The app and CLI read it there. This replaces the guide's in-process probe, which can't work (A3).

### B8. Testing realism
- **Golden images made by Windows:** compressed files, ADS, junctions and symlinks, long paths, Unicode, Fast Startup hibernation, a dirty unplug, a full disk, a heavily fragmented MFT. Keep them small, compress them, and store them in `Tests/Images/` (Git LFS) along with the script that made them.
- **`fsx`-style randomized model test** in the host harness: random ops against the image and an in-memory model, compare after each op, and run `chkdsk` in the VM when the run ends.
- **Fuzz** `ue_probe` and `ue_open` with libFuzzer on mutated images.
- **CI** runs only the engine (host harness, unit tests, fuzz smoke). FSKit needs user approval, so FSKit tests run on a self-hosted or manual runner.
- **Cache-honest benchmarks:** `purge` does not flush the drive's own cache, so also use `F_NOCACHE` reads and files larger than RAM. Running macFUSE as a baseline on macOS 27 may need reduced security. Use a separate benchmark Mac or volume.

### B9. Operability
- One `OSLog` subsystem, `<bundle-prefix>.unlatch`, with categories `engine`, `io`, `mount`, `app`. Document the `log stream --predicate` commands.
- An extension crash kills the mount. The app detects the vanished mount and tells the user the volume may now be dirty.
- Uninstall: unmount everything, then guide the user to disable the extension, then remove the app. Document it.
- Sparkle 2 with EdDSA-signed feeds. Never replace the extension while a volume is mounted: the updater asks the user to unmount first.

### B10. Milestone order
Order: engine on the host → FSKit read-only → FSKit read-write and the M0 speed gate → mount management → CLI → app → KOIO → robustness → ship. The guide's M0 tries to prove FSKit, libntfs-3g custom devices and speed all at once. Splitting them means each failure has a single cause.

## C. Smaller fixes to the guide's text
- Remove the hedges ("as far as I know", "15.4 as far as I know"). The facts are settled above.
- Remove the "Out of scope: older macOS" bullet and say "macOS 27+ on Apple Silicon".
- §8's sample `DiskWatcher` force-unwraps `DASessionCreate`, and its callbacks capture `self` unretained without a lifetime guarantee. The plan specifies the watcher as owned by `MountService`, with `stop()` that unregisters the callbacks.
- §9's onboarding should use `FSClient.shared.openFileSystemExtensionsSettings()` and `fetchInstalledExtensions` to deep-link to the setting and detect whether the extension is enabled, instead of written instructions.
- §11: hardened runtime and notarization stay. Remove universal, and add the GPL source and notice bundle.
- The FS short name must not be `ntfs`, which is Apple's. Use `unlatch`.

## D. Open questions (each is settled by a spike in the plan)
1. Can an unbundled CLI binary hold `com.apple.developer.fskit.mount`, or does it have to ask the app over XPC? (Step 12)
2. When Apple's NTFS module and Unlatch both probe a disk as usable, which one wins, and does the mount-approval dissent work for FSKit mounts? (Step 12)
3. Does libntfs-3g's custom `ntfs_device_operations` path work fully with `ntfs_device_mount` plus `ntfs_create_lru_caches`? (Step 3)
4. How fast is the `ReadWriteHandler` path compared with KOIO in practice? (Steps 11 and 15)
