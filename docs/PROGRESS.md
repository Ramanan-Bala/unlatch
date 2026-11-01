# Progress

## Status

| Field | Value |
|---|---|
| Current phase | 1 Engine on host |
| Last completed step | 03 Engine core: device bridge, probe, open/close, host harness (2026-11-01) |
| Next step | 04 Engine read path |
| libntfs-3g | 2026.9.28 (`Engine/vendor/VERSION`) |
| Tested on | macOS 27 (arm64), Xcode 27.2 (27B52), Swift 6.4.2, XcodeGen 2.46.0, Git LFS 3.8.1 |

### Risk register

| Risk | Where it is settled | Fallback | State |
|---|---|---|---|
| FSKit read-write throughput too low | Step 11 gate, Step 15 KOIO | Bring KOIO forward; NO-GO review | open |
| libntfs-3g custom device quirks | Step 03 | Patch in `Engine/vendor/patches/` | settled: works unpatched, one EROFS workaround (D-14) |
| Restricted entitlement on the CLI | Step 12 S1 | CLI talks to the app over XPC | open |
| Apple's NTFS driver wins the mount race | Step 12 S2 | Unmount-and-swap | open |
| Corruption | Steps 05–07, 10, 15, 17 | Read-only default for that scenario until fixed | open |
| Stale data exposed through KOIO | Step 15 explicit test | Inhibit KOIO for files with `init_size < size` | open |
| FSKit API changes in 27.x | every step re-checks the docs | pin the tested 27.x builds in README | open |

## Steps (log)

### Step 01: Repository scaffold — done 2026-11-01

**Done:**
- `git init` (branch `main`), `.gitignore`, Git LFS tracking `Tests/Images/*.img.zst` (`.gitattributes`).
- `LICENSE` (full GPL-3.0 text from gnu.org), `README.md`, `AGENTS.md`, `CONTRIBUTING.md`, `DCO` (v1.1).
- `project.yml` (XcodeGen) with targets `Unlatch` (menu-bar app, `LSUIElement`), `UnlatchFSModule` (ExtensionKit placeholder, extension point `com.apple.fskit.fsmodule`, embedded in the app), `UnlatchEngine` (static C library with `ue_version()`), `unlatch-engine` (C tool, `--version`). Schemes `Unlatch` (all four) and `unlatch-engine`.
- `Shared/` Swift package `UnlatchCore` (identifier and logger constants, one Swift Testing test). `CLI/` Swift package, executable `unlatch` on `swift-argument-parser`.
- `Scripts/bootstrap.sh`: checks Xcode 27+, XcodeGen, Git LFS; runs `git lfs install --local` and `xcodegen generate`.
- Empty layout dirs: `Tests/Images/`, `Bench/results/` (kept with `.gitkeep`).
- Committed locally as `step 01: repository scaffold`.

**Verify:**
```sh
Scripts/bootstrap.sh
xcodebuild -scheme Unlatch build
swift build --package-path Shared
swift test --package-path Shared
swift build --package-path CLI
swift run --package-path CLI unlatch --help
```
All pass from a clean checkout (also `xcodebuild -scheme unlatch-engine build`).

**Left:** nothing. The owner created the public repo `Ramanan-Bala/unlatch` and pushed it (task 7), because the session was not allowed to create a public repo.

**Surprises:**
- XcodeGen rewrites an entitlements file from `project.yml` whenever the target has an `entitlements:` key, so files with no `properties` come out empty.
- With App Group or FSKit entitlements applied, ad-hoc builds fail: "requires a provisioning profile". The entitlements are kept unapplied for now (D-06).
- `xcodebuild` prints a "CoreSimulator is out of date" warning on this machine. It is harmless for macOS builds.

### Step 02: Vendor and build libntfs-3g plus ntfsprogs — done 2026-11-01

**Done:**
- Pinned ntfs-3g **2026.9.28**, the latest stable release (D-09). The release tarball's SHA-256 matches the published value. It is extracted into `Engine/vendor/ntfs-3g/` (`COPYING` GPL-2.0, `COPYING.LIB`), with the pin in `Engine/vendor/VERSION`.
- `Scripts/build-ntfs3g.sh` builds out of tree into `build/ntfs3g/{lib,include,bin}`: arm64, macOS 27, no FUSE, no mount helper, no crypto, no plugins, no nfconv, static only (D-10, D-12). It applies `Engine/vendor/patches/*.patch` (none needed) and runs `autoreconf` only if `configure` is missing.
- `Scripts/make-image.sh <name> <size> [label]` (mkfile -n + mkntfs -F -Q -L, prints the path) and `Scripts/attach-image.sh <img>` (prints `diskN`).
- `UnlatchEngine` now has a private header path to `build/ntfs3g/include`, and `libntfs-3g.a` is merged into `libUnlatchEngine.a` (D-11). A pre-build check fails with a clear message if the library is missing. `Scripts/bootstrap.sh` builds it when missing.

**Verify:**
```sh
rm -rf build && Scripts/bootstrap.sh            # builds libntfs-3g from scratch (~20 s)
lipo -info build/ntfs3g/lib/libntfs-3g.a        # arm64
mkdir -p build/images && cd build/images
../../Scripts/make-image.sh t1 512m T1          # prints .../t1.img
../ntfs3g/bin/ntfsinfo -m t1.img                # Volume Name: T1, Version 3.1
../ntfs3g/bin/ntfsls -s -l t1.img               # root: $MFT, $LogFile, ... (plain ntfsls prints nothing: root is empty)
cd ../.. && xcodebuild -scheme Unlatch build
nm -g build/DerivedData/Build/Products/Debug/libUnlatchEngine.a | grep ' T _ntfs_device_mount'   # with -derivedDataPath build/DerivedData
ls Engine/vendor/ntfs-3g/COPYING*
```
I also tested write and read-back with `ntfscp`/`ntfscat`, and an `attach-image.sh` then `hdiutil detach` cycle on `t1.img`. diskutil reports the attached disk as NTFS, not mounted. I verified `build-ntfs3g.sh` on a fresh `git clone` of the step commit.

**Left:** nothing in scope.

**Surprises:**
- `tuxera.com` answers with a 301 redirect to `www.tuxera.com` and stalls when you follow it. Download from `https://www.tuxera.com/...` directly.
- On non-Linux hosts, configure's `install-exec-hook` moves `libntfs-3g.so*` to `/lib` unless you pass `--exec-prefix`. That flag fixes it without a patch.
- No macOS patches were needed. Upstream compiles with a few warnings: unused variables, packed-member addresses, and "Trimming not supported" in `ioctl.c`.
- `hdiutil attach -nomount` and `hdiutil detach` now print deprecation warnings that recommend `diskutil image attach --noMount` and `diskutil eject`. They still work.
- macOS identifies the attached image as NTFS (`File System Personality: NTFS`). With `-nomount`, Apple's driver does not mount it.

### Step 03: Engine core (device bridge, probe, open/close) and host harness — done 2026-11-01

**Done:**
- `Engine/include/unlatch_engine.h`: the full seam from the plan, with Step 03 functions plus declarations for the read side (Step 04) and write side (Step 05). Adds `ue_vol_state_name()` and documents what `ue_probe` and `ue_open` return.
- `Engine/src/internal.h`: the only file that includes ntfs-3g headers, after libntfs-3g's own `config.h` (D-13).
- `Engine/src/device.c`: `ntfs_device_operations` backed by `ue_io_t`. Seek position, bounds checks (seek past the end fails), `EROFS` on a read-write open of a read-only `ue_io_t`, `stat` reports a block device, and `ioctl` answers `DKIOCGETBLOCKSIZE` and `DKIOCGETBLOCKCOUNT`, the two queries libntfs-3g makes on Darwin.
- `Engine/src/volume.c`:
  - `ue_probe`: boot-sector check, read-only mount, dirty flag, hiberfile check, logfile check, mapped to a state with a readable reason (D-15). It also fills label, serial, version, cluster size and total/free bytes.
  - `ue_open`: read-only or strict read-write, then `ntfs_create_lru_caches`, `ntfs_set_ignore_case`, `ntfs_set_shown_files(vol, FALSE, TRUE, FALSE)`.
  - `ue_close` and `ue_is_readonly`.
- `EngineTool`: `unlatch-engine probe <img>` and `info [--rw] <img>`, using the file-backed `ue_io_t` in `file_io.c` (pread/pwrite loops, `F_FULLFSYNC`).
- `Tests/Engine/test_engine.c`, 8 tests:
  - clean image → CLEAN, with label, sizes and version
  - dirty image → DIRTY
  - zeroed boot sector → NOT_NTFS
  - truncated image → CORRUPT
  - active `hiberfil.sys` → HIBERNATED; read-write open fails with EPERM, read-only open works
  - read-only `ue_io_t` with a read-write open → EROFS
  - bad arguments → EINVAL
  - 100 read-write plus 100 read-only open/close cycles, and the volume is still CLEAN afterwards

  `Tests/Engine/set_dirty.c` is the dirty-flag fixture tool. `Scripts/test-engine.sh` runs everything (D-17).

**Verify:**
```sh
Scripts/test-engine.sh                       # ends with "test-engine: PASSED"
xcodebuild -scheme unlatch-engine -derivedDataPath build/DerivedData build
build/DerivedData/Build/Products/Debug/unlatch-engine probe build/test-engine/clean.img
grep -rnE '#\s*include\s*[<"]ntfs-3g/' --include='*.[ch]' --include='*.swift' . --exclude-dir=vendor --exclude-dir=build   # only Engine/src
xcodebuild -scheme Unlatch build             # still green
```
`leaks --atExit` over 100 read-write open/close cycles reports 0 leaks. For the dirty fixture, `ntfsinfo -f` independently reports `Volume Flags: 0x0001 DIRTY`.

**Left:** nothing in scope. The UNCLEAN_LOG and Fast Startup paths have no fixture yet; they need a Windows-made image (Step 07).

**Surprises:**
- Open question 3: the custom device path worked first time. The one trap is `ntfs_volume_startup`'s silent read-only retry on `EROFS` (D-14).
- `ntfs_volume_check_logfile` is `static`, so the probe rebuilds it from the exported functions. `ntfs_volume` doesn't keep the serial number, so the probe reads it from the boot sector.
- The installed ntfs-3g headers fail `-Wdocumentation` under `-Werror`, so they are included with `-isystem`.
- A failed `ntfs_device_mount` releases the volume and closes the device but does not free it; `ntfs_umount` frees both. `mount_volume` and `unmount_volume` in `volume.c` encode this.
- The Xcode project has to be regenerated (`Scripts/bootstrap.sh`) after adding a source file. `test-engine.sh` only generates the project when it's missing.

## Carry-overs

- **Step 08:** set a development team and reference `Extension/Resources/UnlatchFSModule.entitlements` and `App/Resources/Unlatch.entitlements` from `project.yml` (D-06). The extension cannot load without `com.apple.developer.fskit.fsmodule`. Keep an ad-hoc build path for CI (Step 07), for example an xcconfig override.
- **Step 08:** `Extension/Sources/UnlatchFSModule.swift` is a placeholder with no `@main`; the appex builds but is not a working extension.
- **Step 04+:** the ntfs-3g build sets no `--enable-debug`. Decide whether Debug engine builds want ntfs-3g debug logging. For now the log handler is null (D-16).
- **Step 07:** add fixtures for UNCLEAN_LOG and Fast Startup (`$LogFile` restart page 2.0), from Windows-made images. The probe code paths exist but are untested.
- **Step 07 (CI):** run `Scripts/test-engine.sh`. It needs `leaks`, which comes with Xcode command-line tools.
- **Step 08:** route libntfs-3g logging (`ntfs_log_set_handler`) to OSLog category `engine` (D-16).
- **Step 07 (CI):** cache `build/ntfs3g`, keyed on `Engine/vendor/VERSION` plus `Engine/vendor/patches/*` plus `Scripts/build-ntfs3g.sh`.
- **Step 18:** the notice bundle must include `Engine/vendor/ntfs-3g/COPYING`, `COPYING.LIB`, `AUTHORS` and `CREDITS`.
- **Scripts (any step):** `hdiutil attach/detach` are deprecated. Move `attach-image.sh` to `diskutil image attach --noMount` if hdiutil goes away.
- **Step 18:** `ENABLE_HARDENED_RUNTIME` is already on; Developer ID signing and notarization are not set up.
