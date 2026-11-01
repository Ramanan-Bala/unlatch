# Progress

## Status

| Field | Value |
|---|---|
| Current phase | 0 Foundations |
| Last completed step | 01 Repository scaffold (2026-11-01) |
| Next step | 02 Vendor and build libntfs-3g plus ntfsprogs |
| Tested on | macOS 27 (arm64), Xcode 27.2 (27B52), Swift 6.4.2, XcodeGen 2.46.0, Git LFS 3.8.1 |

### Risk register

| Risk | Where it is settled | Fallback | State |
|---|---|---|---|
| FSKit read-write throughput too low | Step 11 gate, Step 15 KOIO | Bring KOIO forward; NO-GO review | open |
| libntfs-3g custom device quirks | Step 03 | Patch in `Engine/vendor/patches/` | open |
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

**Left:** create the public GitHub repo `Ramanan-Bala/unlatch` and push (task 7). The session was not permitted to create a public repo, so the owner has to run `gh repo create Ramanan-Bala/unlatch --public --source . --remote origin --push`.

**Surprises:**
- XcodeGen rewrites an entitlements file from `project.yml` whenever the target has an `entitlements:` key, so files with no `properties` come out empty.
- With App Group or FSKit entitlements applied, ad-hoc builds fail: "requires a provisioning profile". The entitlements are kept unapplied for now (D-06).
- `xcodebuild` prints a "CoreSimulator is out of date" warning on this machine. It is harmless for macOS builds.

## Carry-overs

- **Step 08:** set a development team and reference `Extension/Resources/UnlatchFSModule.entitlements` and `App/Resources/Unlatch.entitlements` from `project.yml` (D-06). The extension cannot load without `com.apple.developer.fskit.fsmodule`. Keep an ad-hoc build path for CI (Step 07), for example an xcconfig override.
- **Step 08:** `Extension/Sources/UnlatchFSModule.swift` is a placeholder with no `@main`; the appex builds but is not a working extension.
- **Step 18:** `ENABLE_HARDENED_RUNTIME` is already on; Developer ID signing and notarization are not set up.
