# NTFS for Mac: Complete Build Guide

*Stack: Swift, FSKit and libntfs-3g (C). Architecture: an FSKit filesystem extension with the NTFS engine running in-process, plus a menu-bar app and a CLI. No FUSE and no root daemon.*

## 1. Goal and scope

macOS mounts NTFS volumes read-only. This project ships a fast, safe NTFS read-write filesystem for macOS, plus a CLI and a menu-bar app to manage it. Large-file copy speed is a first-class requirement, so the design removes the FUSE layer entirely.

**In scope**

- A native FSKit filesystem extension that mounts NTFS read-write, with the NTFS engine running in-process.
- Detection of NTFS volumes and optional auto-mount.
- Safety checks that refuse read-write on hibernated or dirty volumes.
- CLI: `ntfs list | mount | unmount | eject | status`.
- Menu-bar app with the same actions, notifications, and onboarding.
- Signed, notarized, distributable build.
- A benchmark harness that proves the speed claim.

**Out of scope for v1**

- A from-scratch NTFS engine (it can replace the first engine later, section 5).
- Support for macOS releases older than the minimum FSKit version.
- Non-NTFS filesystems (exFAT and others already work natively).

**Speed target.** Set it from measurement, not guesswork: sequential large-file throughput should land close to the raw drive speed measured with `dd`, and clearly above ntfs-3g on macFUSE. Section 10 defines how to measure and what to compare.

## 2. Key decisions

The filesystem runs inside Apple's FSKit with libntfs-3g linked in-process, which removes FUSE and the root daemon. The engine sits behind one narrow C interface, and that interface is the only part designed to be swapped.

| Decision | Choice | Reason |
| --- | --- | --- |
| Filesystem host | FSKit extension | User-space, no kernel extension, no FUSE round-trips |
| NTFS engine | libntfs-3g (C), linked in-process | Mature write logic; faster than a separate ntfs-3g process over FUSE |
| Engine seam | One C header (`ntfs_engine.h`) | Lets a later engine replace libntfs-3g without touching the extension, app, or CLI |
| Languages | Swift for extension, app, CLI; C for the engine boundary | Swift is the only supported route into FSKit |
| Privilege model | No root daemon | The system mounts FSKit volumes; the app and CLI stay unprivileged |
| Detection | DiskArbitration | Native appear and disappear events |
| CLI | Swift Argument Parser | Native, no extra runtime |
| Minimum macOS | The first release with block-device FSKit support (15.4 as far as I know) | Verify in Apple's current FSKit docs |
| Not swappable | Everything above the engine seam | Keeps the foundation simple and fast to build |

## 3. Prerequisites and licensing

- A Mac on the minimum macOS release, with the current Xcode and command-line tools.
- A paid Apple Developer Program account. The FSKit module capability, Developer ID signing, and notarization all depend on it. Check Apple's current requirements for the FSKit entitlement.
- The libntfs-3g source, built as a universal static library (arm64 and x86\_64) from a pinned release.
- `mkntfs`, `ntfsfix`, and `ntfs-3g.probe` for development and testing, plus `fio` or `dd` for benchmarks.
- Disposable disks and NTFS images. **Never develop against a drive holding data you care about.**

**Licensing, decide before writing code.** The ntfs-3g project ships a library and a driver under different licenses (the library is LGPL, as far as I know, and the driver is GPL). Linking the library into a closed-source extension has obligations, such as allowing the library to be relinked and keeping notices. Confirm the exact terms for the release you pin, and get legal review if you plan to sell or close the source. Keep all ntfs-3g code behind the engine seam so the licensing surface stays small and isolated.

## 4. Architecture

The data path stays inside one process: Finder or `cp` talks to the kernel, the kernel talks to the FSKit extension, and the extension calls the engine directly. The app and CLI only manage mounts.

```
  Finder / cp / apps
         |
   kernel VFS
         |
  FSKit extension (Swift)  <-------- manages mounts ------ NTFSKit.app (menu bar)
         |                                                  ntfs (CLI)
  =======|=========  engine seam: ntfs_engine.h (C ABI)
         |
  Engine (libntfs-3g now)
         |
  FSBlockDeviceResource (the disk)
```

**Mount flow**

1. DiskArbitration reports a disk with volume kind `ntfs` (Apple's read-only driver mounts it).
2. The app or CLI runs a safety probe through the engine: is the volume clean and not hibernated?
3. If safe, it unmounts Apple's read-only mount and mounts the volume with the FSKit module.
4. The extension opens the block device through FSKit, hands it to the engine, and serves reads and writes.
5. If unsafe, the volume stays read-only and the user sees the reason.

**What changed from the FUSE design.** No root daemon, no XPC, no FUSE-T or macFUSE, and no separate ntfs-3g process. Fewer moving parts, fewer boundary crossings per I/O, and nothing extra for users to install.

## 5. Repository layout and the engine boundary

```
NTFSKit/
├── NTFSKit.xcodeproj
├── Engine/                    C target: the seam and its implementation
│   ├── include/ntfs_engine.h  the only interface the extension sees
│   ├── ntfs3g_engine.c        implementation on libntfs-3g
│   └── vendor/libntfs-3g/     pinned source or prebuilt universal .a
├── Extension/                 FSKit extension (Swift), links Engine
├── Shared/                    Swift package: models, mount helpers, DiskArbitration wrapper
├── CLI/                       `ntfs` executable (Argument Parser)
├── App/                       SwiftUI menu-bar app, embeds the extension
├── Bench/                     benchmark scripts and result templates
└── Scripts/                   build, sign, notarize, test-image helpers
```

### The engine boundary

This header is the whole contract. A future engine (for example a Rust implementation exposing a C ABI) replaces `ntfs3g_engine.c` and nothing else.

```c
// ntfs_engine.h
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct ntfs_engine ntfs_engine_t;   // opaque
typedef uint64_t ntfs_ino_t;                // MFT record number

typedef enum {
    NTFS_OK = 0, NTFS_ERR_IO, NTFS_ERR_NOENT, NTFS_ERR_EXIST, NTFS_ERR_NOSPC,
    NTFS_ERR_INVAL, NTFS_ERR_ROFS, NTFS_ERR_DIRTY, NTFS_ERR_HIBERNATED
} ntfs_status_t;

// The engine never opens devices. The host supplies block I/O.
typedef struct {
    void    *ctx;
    int    (*read)(void *ctx, uint64_t offset, void *buf, size_t len);
    int    (*write)(void *ctx, uint64_t offset, const void *buf, size_t len);
    int    (*flush)(void *ctx);
    uint64_t size_bytes;
    uint32_t sector_size;
} ntfs_io_t;

typedef struct { bool clean; bool hibernated; char label[128]; uint8_t uuid[16]; } ntfs_probe_t;
typedef struct { uint64_t size; uint32_t mode; uint64_t mtime_ns, ctime_ns, atime_ns; bool is_dir; } ntfs_attr_t;

ntfs_status_t ntfs_probe(const ntfs_io_t *io, ntfs_probe_t *out);
ntfs_status_t ntfs_open(const ntfs_io_t *io, bool read_only, ntfs_engine_t **out);
void          ntfs_close(ntfs_engine_t *e);          // flushes and unmounts cleanly

ntfs_status_t ntfs_lookup (ntfs_engine_t *e, ntfs_ino_t dir, const char *name, ntfs_ino_t *out);
ntfs_status_t ntfs_getattr(ntfs_engine_t *e, ntfs_ino_t ino, ntfs_attr_t *out);
ntfs_status_t ntfs_readdir(ntfs_engine_t *e, ntfs_ino_t dir, uint64_t cookie,
                           bool (*emit)(void *ctx, const char *name, ntfs_ino_t ino, bool is_dir), void *ctx);
ntfs_status_t ntfs_read   (ntfs_engine_t *e, ntfs_ino_t ino, uint64_t off, void *buf, size_t len, size_t *done);
ntfs_status_t ntfs_write  (ntfs_engine_t *e, ntfs_ino_t ino, uint64_t off, const void *buf, size_t len, size_t *done);
ntfs_status_t ntfs_create (ntfs_engine_t *e, ntfs_ino_t dir, const char *name, bool is_dir, uint32_t mode, ntfs_ino_t *out);
ntfs_status_t ntfs_unlink (ntfs_engine_t *e, ntfs_ino_t dir, const char *name);
ntfs_status_t ntfs_rename (ntfs_engine_t *e, ntfs_ino_t odir, const char *oname, ntfs_ino_t ndir, const char *nname);
ntfs_status_t ntfs_truncate(ntfs_engine_t *e, ntfs_ino_t ino, uint64_t size);
ntfs_status_t ntfs_sync   (ntfs_engine_t *e);
```

**Boundary rules**

- The engine does no path handling and opens no devices. All I/O goes through `ntfs_io_t`, so the host controls caching and alignment.
- Names are UTF-8 across the boundary. The engine converts to and from NTFS's UTF-16.
- Errors are a small status enum. The extension maps them to POSIX errors.
- Threading is stated in the header: either the engine is internally serialized or the host serializes calls. Pick one and document it, because libntfs-3g is not built for concurrent callers.
- Nothing above the boundary includes an ntfs-3g header.

## 6. FSKit extension

The extension is a file-system module bundled inside the app. FSKit is young and its API changes between releases, so treat the signatures below as the shape of the code and copy the exact ones from Apple's current FSKit documentation and sample project.

### Structure

```swift
import FSKit

@main
struct NTFSModule: UnaryFileSystemExtension {
    var fileSystem: FSUnaryFileSystem & FSUnaryFileSystemOperations { NTFSFileSystem() }
}

final class NTFSFileSystem: FSUnaryFileSystem, FSUnaryFileSystemOperations {
    // Called when the system asks whether this module can handle a disk.
    func probeResource(resource: FSResource,
                       replyHandler: @escaping (FSProbeResult?, Error?) -> Void) {
        guard let dev = resource as? FSBlockDeviceResource else {
            return replyHandler(nil, POSIXError(.ENODEV))
        }
        var info = ntfs_probe_t()
        var io = BlockIO.make(for: dev)          // wraps the resource as ntfs_io_t
        let status = ntfs_probe(&io, &info)
        // reply with usable / notRecognized, including label and UUID
    }

    // Called to mount: open the engine and return the volume.
    func loadResource(resource: FSResource, options: FSTaskOptions,
                      replyHandler: @escaping (FSVolume?, Error?) -> Void) {
        // read-only if the user asked for it or the probe said unsafe
        // ntfs_open(&io, readOnly, &engine) -> NTFSVolume(engine:)
    }

    func unloadResource(resource: FSResource, options: FSTaskOptions,
                        replyHandler: @escaping (Error?) -> Void) {
        // ntfs_close(engine): flush, mark clean, release the device
    }
}
```

### Volume operations

The volume class adopts FSKit's volume operation protocols and forwards each call to the engine. Each FSKit item wraps a `ntfs_ino_t`.

| FSKit operation | Engine call |
| --- | --- |
| lookup item by name | `ntfs_lookup`, then `ntfs_getattr` |
| enumerate directory | `ntfs_readdir` with a cookie |
| read file data | `ntfs_read` |
| write file data | `ntfs_write` |
| create file or directory | `ntfs_create` |
| remove item | `ntfs_unlink` |
| rename item | `ntfs_rename` |
| set size | `ntfs_truncate` |
| synchronize | `ntfs_sync` |
| unmount | `ntfs_close` |

### Block I/O bridge

`ntfs_io_t` has synchronous C callbacks, while FSKit's block-device calls may be asynchronous. Use the synchronous read and write calls if the resource offers them. Otherwise run engine work on one dedicated serial queue and bridge each call with a semaphore, never on the main thread. Keep I/O sizes large and aligned to the device's block size. This bridge is the hottest code in the project, so keep it allocation-free.

### Info.plist

The extension declares the FSKit extension point (`com.apple.fskit.fsmodule`), the file system's short name used with `mount -t`, and that it supports block-device resources. The exact keys are easiest to copy from Apple's sample project and may change between macOS releases.

### Extension rules

- All engine calls go through one serial queue, since libntfs-3g is not safe for concurrent callers.
- Map status codes to POSIX errors in one place.
- Never trust names or offsets from the system without bounds checks.
- Log with `OSLog`, but keep logging out of the read and write fast path.

## 7. Engine on libntfs-3g and the performance design

### Integrating the library

- Build libntfs-3g as a pinned, universal static library and link it into `Engine/` only.
- The library supports custom device operations, so it can read and write through the callbacks in `ntfs_io_t` instead of opening `/dev/diskXsY` itself. Allocate the device with those operations and mount through the library's device-based mount call. Check the exact function and flag names in the headers of the release you pin.
- Mount flags: read-only when the probe says the volume is unsafe or the user asked for it, and never ignore the hibernation file unless the user explicitly overrides.
- Wrap every library call so errors become the status enum from section 5, and no library type leaks upward.

### Where speed comes from

With FUSE gone, the remaining cost is how I/O reaches the disk and how the engine allocates space. These are the levers, in the order I would pursue them. Measure after each one (section 10).

| Lever | What it does | Risk |
| --- | --- | --- |
| Large, aligned I/O | Pass big reads and writes straight to the block device instead of cluster-sized calls | Low |
| Sequential read-ahead | Detect streaming reads and prefetch the next large chunk | Low |
| Write coalescing | Merge adjacent file-data writes into large device writes within a bounded dirty window | Medium: must flush on sync, close, and unmount |
| Grow files in big steps | Extend allocation by large runs when a file grows, not per write, to cut run-list updates and fragmentation | Medium |
| Metadata policy | Write metadata through promptly and batch only file data | Keeps corruption risk low |
| No access-time updates | Skip updating access times on reads | Low |
| Buffer reuse and zero-copy | Read straight into the buffer FSKit provides, and reuse scratch buffers | Low |
| Serialization cost | Keep lock hold times short; do not hold the engine queue while waiting on the device if it can be avoided | Medium: libntfs-3g is not concurrent |

### Rules that protect data

- The dirty window is bounded and every barrier (`fsync`, close, unmount, eject) drains it.
- Metadata ordering is never changed by an optimization.
- On surprise removal, fail pending I/O quickly, never retry writes to a vanished device, and leave the volume marked dirty so the next mount is cautious.
- Every optimization ships behind a measurement showing a gain and a test run showing no corruption.

### Profiling

Use Instruments (Time Profiler, System Trace, File Activity) during large copies. Look first at time spent waiting on the device, then time in buffer copies, then lock contention on the engine queue.

## 8. Detection, mounting and safety checks

### Detection (Shared package)

DiskArbitration reports NTFS volumes as they appear and disappear. The watcher lives in the Shared package so the app and CLI use the same code.

```swift
import DiskArbitration

final class DiskWatcher {
    private let session = DASessionCreate(kCFAllocatorDefault)!
    var onAppear: ((String, [String: Any]) -> Void)?
    var onDisappear: ((String) -> Void)?

    func start() {
        let ctx = Unmanaged.passUnretained(self).toOpaque()

        DARegisterDiskAppearedCallback(session, nil, { disk, ctx in
            let me = Unmanaged<DiskWatcher>.fromOpaque(ctx!).takeUnretainedValue()
            guard let desc = DADiskCopyDescription(disk) as? [String: Any],
                  (desc[kDADiskDescriptionVolumeKindKey as String] as? String) == "ntfs",
                  let name = DADiskGetBSDName(disk) else { return }
            me.onAppear?(String(cString: name), desc)
        }, ctx)

        DARegisterDiskDisappearedCallback(session, nil, { disk, ctx in
            let me = Unmanaged<DiskWatcher>.fromOpaque(ctx!).takeUnretainedValue()
            if let name = DADiskGetBSDName(disk) { me.onDisappear?(String(cString: name)) }
        }, ctx)

        DASessionSetDispatchQueue(session, DispatchQueue(label: "ntfskit.disks"))
    }
}
```

Restrict auto-mount to external volumes by default, using the removable and internal flags in the disk description.

### Mount sequence

1. Detect the volume (Apple's read-only mount is active).
2. Run the safety probe (below). The probe reads the volume through the engine, not through a mount.
3. Unmount Apple's read-only mount (`DADiskUnmount`, or `diskutil unmount`).
4. Mount through the FSKit module, selecting it by the short name declared in the extension's Info.plist (see `man mount` on the target macOS release for the flag that selects an FSKit module).
5. Confirm the mount, then report state to the caller.
6. If step 4 fails, remount Apple's read-only mount so the user is never left with an unmounted drive.

**Check early:** whether the mount step needs elevated privileges for external block devices on your target macOS. If it does, add one small privileged helper registered with `SMAppService`, used only for that single step. Everything else stays unprivileged.

### Safety checks

| Check | Action when it fails |
| --- | --- |
| Volume dirty (unclean shutdown) | Mount read-only; offer `ntfsfix` only as an explicit, confirmed action, and recommend running chkdsk on Windows |
| Hibernated or Fast Startup state | Mount read-only and tell the user to shut Windows down fully or disable Fast Startup |
| Unsupported or damaged structures | Refuse read-write, show the reason |
| Surprise removal while mounted | Fail pending I/O, unmount, remove the stale mount point |
| Eject requested | Drain the write window, sync, close the engine, then eject |

The safety rule is simple: **if in doubt, read-only.**

## 9. CLI and menu-bar app

Both are thin clients of one `MountService` in the Shared package (detection, probe, unmount, mount, eject). With no daemon there is no XPC layer, so the CLI and app call the same code directly.

### CLI

```swift
import ArgumentParser

@main
struct NTFS: AsyncParsableCommand {
    static let configuration = CommandConfiguration(
        commandName: "ntfs",
        subcommands: [List.self, Mount.self, Unmount.self, Eject.self, Status.self])
}

struct Mount: AsyncParsableCommand {
    @Argument(help: "BSD name, for example disk4s1") var disk: String
    @Flag(help: "Mount read-only") var readOnly = false

    func run() async throws {
        let result = try await MountService().mount(bsdName: disk, readOnly: readOnly)
        print(result.summary)   // state, mount point, reason if forced read-only
    }
}
```

- `--json` on `list` and `status` for scripting.
- Non-zero exit codes on failure, with a plain reason when a volume falls back to read-only.
- The app can offer to symlink `/usr/local/bin/ntfs` to the copy inside the app bundle.

### Menu-bar app

```swift
import SwiftUI

@main
struct NTFSKitApp: App {
    @StateObject private var model = VolumesModel()

    var body: some Scene {
        MenuBarExtra("NTFS", systemImage: "externaldrive.badge.checkmark") {
            ForEach(model.volumes) { VolumeRow(volume: $0, model: model) }
            Divider()
            Toggle("Auto-mount NTFS drives", isOn: $model.autoMount)
            Button("Quit") { NSApplication.shared.terminate(nil) }
        }
    }
}
```

**Behavior**

- List volumes with a state badge: read-only, read-write, dirty (with reason), error.
- Per-volume actions: mount read-write, unmount, eject, open in Finder.
- Notifications through UserNotifications when a drive mounts or falls back to read-only.
- `LSUIElement` set to true so there is no Dock icon; optional launch at login with `SMAppService.mainApp`.
- Live updates come straight from the DiskArbitration watcher.

**First-run onboarding**

1. Check the macOS version against the minimum.
2. Guide the user to enable the file system extension in System Settings. The exact location differs by macOS release, so write the instructions from the current OS and test them on each release.
3. Confirm the extension is active by probing a test image bundled with the app.
4. Offer the optional CLI install and auto-mount setting.

If the extension is not enabled, every mount attempt should fail with a clear message pointing back to onboarding, never a silent read-only fallback.

## 10. Benchmarking and testing

### Benchmark method

Speed is a claim, so prove it the same way every time. Run the same workloads on the same drive, same cable, same Mac, with caches cleared between runs.

| Baseline | Why |
| --- | --- |
| Raw device read (`dd` from the raw disk node) | The ceiling the drive can deliver |
| Same drive formatted exFAT or APFS, large copy | What a native macOS filesystem achieves on this hardware |
| ntfs-3g on macFUSE | The old design, the number to beat |
| ntfs-3g on FUSE-T | Second FUSE data point |
| This project (FSKit + in-process engine) | The result |

| Workload | Command sketch |
| --- | --- |
| Large sequential write | `cp` or `dd bs=4m` of a multi-GB file to the volume, then `sync` |
| Large sequential read | `dd if=<file> of=/dev/null bs=4m` after `purge` |
| Many small files | Extract a source tree of tens of thousands of files |
| Mixed and random I/O | `fio` with 4k random read/write (optional) |
| Metadata heavy | Create, rename, and delete thousands of files and directories |

**Rules:** repeat each run several times and keep the median, record the drive model and connection, run `purge` between runs, and store results in `Bench/results/` with the commit hash.

**Acceptance gate (set the numbers after your first baseline run):** large-file throughput within an agreed fraction of the raw device speed, and a clear margin over ntfs-3g on macFUSE. If the gate fails, profile (section 7) before adding features.

### Testing

**Unit tests**

- Status-to-POSIX error mapping, name conversion, path and label sanitizing.
- The block I/O bridge, using a mock `ntfs_io_t` backed by a file.
- Probe logic against canned boot sectors.

**Integration tests on disk images (never real drives first)**

```bash
# create a blank image and format it (verify flags against your mkntfs version)
mkfile -n 2g test.img
mkntfs -F -Q -L TestNTFS test.img

# attach as a raw disk image without mounting
hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount test.img
```

Then mount through the extension and run: create, write, read back and compare checksums, rename, delete, truncate, unmount, re-attach, and verify again.

**Corruption and robustness**

- After every write test, check the image with ntfs-3g's check tool and, periodically, with Windows chkdsk.
- Fuzz the engine boundary with malformed images, and keep crashes as regression cases.
- Interrupt writes (kill the extension, detach the disk mid-copy) and confirm the volume mounts cleanly or read-only afterwards.

**Manual matrix**

| Scenario | Expected |
| --- | --- |
| Clean NTFS drive | Mounts read-write |
| Windows Fast Startup hibernation | Stays read-only with a clear message |
| Unclean unplug | Read-only, offers repair |
| Unplug while copying | No crash, no stale mount point |
| Sleep and wake during use | Volume stays usable or remounts cleanly |
| 50 GB or larger single file | Completes, checksum matches |
| Names with Unicode, long names, reserved Windows names | Handled or rejected clearly |
| Apple Silicon and Intel | Both pass |
| Each macOS point release | Re-run the suite |

## 11. Packaging, signing and notarization

1. **Build universal.** Compile libntfs-3g and every target for arm64 and x86\_64.
2. **Entitlements.** The extension needs Apple's FSKit module capability, enabled on your App ID in the Developer portal. Confirm the exact entitlement name and requirements in Apple's current FSKit documentation. The app and extension share one Team ID.
3. **Embed.** Put the extension in the app's `Contents/PlugIns/` (or the location Xcode's template uses), and put the CLI inside the app bundle.
4. **Sign inside out** with a Developer ID Application certificate and the hardened runtime:

   ```bash
   codesign --force --options runtime --timestamp \
     --entitlements Extension.entitlements \
     --sign "Developer ID Application: Your Name (TEAMID)" path/to/Extension.appex
   ```

   Then sign the CLI, then the app.
5. **Package** as a DMG or a signed installer package.
6. **Notarize and staple:**

   ```bash
   xcrun notarytool submit NTFSKit.dmg --keychain-profile "notary" --wait
   xcrun stapler staple NTFSKit.dmg
   ```
7. **Third-party notices.** Ship the ntfs-3g license text and attributions, plus whatever relinking information your license analysis requires (section 3).
8. **Updates.** Choose an updater early (for example Sparkle) and sign update feeds.
9. **Automate** steps 1 to 6 in `Scripts/` and run them in CI on a macOS runner.

Test the installed, notarized build on a clean Mac, not only the build from Xcode. Extension discovery and approval behave differently for signed, installed apps.

## 12. Risks, milestones and roadmap

### Risks

| Risk | Mitigation |
| --- | --- |
| FSKit throughput is lower than hoped | M0 spike measures it before anything else is built; the engine path is the part to tune |
| FSKit API or macOS behavior changes | Pin supported macOS versions, isolate FSKit code in `Extension/`, re-test every release |
| Data corruption on write | Probe first, read-only when in doubt, bounded dirty window, fuzzing, chkdsk cross-checks, images before real drives |
| libntfs-3g is not concurrent or fully custom-device friendly | One serial queue; confirm custom device support in M0 |
| Licensing conflicts | Resolve section 3 before shipping; keep all ntfs-3g code behind the engine seam |
| Mount needs elevated rights | Check in M0; if needed, one small privileged helper for the mount step only |
| Extension approval confuses users | Onboarding flow with a self-test, clear error messages |
| Surprise removal | Fail pending I/O fast, leave volume dirty, clean stale mount points |

### Milestones

| # | Milestone | Done when |
| --- | --- | --- |
| M0 | **Spike and speed gate** | A minimal FSKit extension mounts an NTFS image through libntfs-3g with custom device I/O, reads and writes a multi-GB file, and the numbers are compared with the baselines in section 10 |
| M1 | Engine boundary | `ntfs_engine.h` implemented on libntfs-3g, unit tested against images, no ntfs-3g types outside `Engine/` |
| M2 | Volume operations | All operations in section 6 work: lookup, readdir, read, write, create, remove, rename, truncate, sync |
| M3 | Detection and mounting | `MountService` detects volumes, probes, swaps Apple's mount for ours, and recovers on failure |
| M4 | CLI | All subcommands with `--json` and exit codes |
| M5 | Menu-bar app | Volume list, actions, notifications, onboarding |
| M6 | Performance pass | Levers from section 7 applied one at a time, each backed by a measurement and a clean corruption run |
| M7 | Safety and robustness | Fuzzing, interruption tests, chkdsk cross-checks pass |
| M8 | Packaging | Signed, notarized build installs and works on a clean Mac |
| M9 | Beta | Real drives, multiple Macs, multiple macOS releases |
| M10 | Release | Documentation, third-party notices, support process |

**M0 is a go or no-go gate.** If it fails because of FSKit itself, stop and reassess before building M1 onward.

### Roadmap after v1

- Replace the engine behind `ntfs_engine.h` if profiling shows libntfs-3g is the limit. A Rust implementation exposing the same C ABI is the likely candidate, and only `Engine/` changes.
- Per-volume settings (always read-only, always auto-mount).
- Repair assistant.
- Localization and accessibility pass.
- Auto-update.

### First week

1. Build libntfs-3g as a universal static library and mount a test image from a small C program using custom device callbacks.
2. Create the FSKit extension target from Apple's template and make it probe the image.
3. Wire the engine into `loadResource`, then do read-only listing and file reads.
4. Add `ntfs_write`, copy a multi-GB file, and record the first benchmark.

### References

- Apple documentation: FSKit, DiskArbitration, ServiceManagement, SwiftUI `MenuBarExtra`, UserNotifications, OSLog, notarization.
- Apple's FSKit sample project (the best source for Info.plist keys and current signatures).
- ntfs-3g library headers and documentation.
- Swift Argument Parser.
