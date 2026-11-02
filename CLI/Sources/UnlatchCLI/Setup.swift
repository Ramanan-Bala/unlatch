// SPDX-License-Identifier: GPL-3.0-or-later
import ArgumentParser
import Foundation
import UnlatchCore

/// Where the other Unlatch binaries are: $UNLATCH_FUSE / $UNLATCH_HELPER, next to this
/// executable (an installed layout), or the repository's Debug build (development).
enum Paths {
    private static let devProducts = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        .appendingPathComponent("build/DerivedData/Build/Products/Debug")

    private static func find(env: String, name: String) -> URL {
        if let p = ProcessInfo.processInfo.environment[env] { return URL(fileURLWithPath: p) }
        let beside = Unlatch.executableDirectory.appendingPathComponent(name)
        if FileManager.default.isExecutableFile(atPath: beside.path) { return beside }
        return devProducts.appendingPathComponent(name)
    }

    static var daemon: URL { find(env: "UNLATCH_FUSE", name: "unlatch-fuse") }
    static var helper: URL { find(env: "UNLATCH_HELPER", name: HelperOpener.label) }
    static let installedHelper = HelperOpener.installedPath
    static let launchdPlist = "/Library/LaunchDaemons/\(HelperOpener.label).plist"
}

/// The helper's launchd job (Helper/io.github.ramanan-bala.unlatch.helper.plist).
let helperPlist = """
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>\(HelperOpener.label)</string>
    <key>AssociatedBundleIdentifiers</key>
    <string>\(Unlatch.bundlePrefix).Unlatch</string>
    <key>Program</key>
    <string>\(Paths.installedHelper)</string>
    <key>Sockets</key>
    <dict>
        <key>Listeners</key>
        <dict>
            <key>SockPathName</key>
            <string>\(HelperOpener.socketPath)</string>
            <key>SockPathMode</key>
            <integer>438</integer>
        </dict>
    </dict>
</dict>
</plist>

"""

@discardableResult
func shell(_ args: [String]) -> (Int32, String) {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: args[0])
    p.arguments = Array(args.dropFirst())
    let out = Pipe()
    p.standardOutput = out
    p.standardError = out
    do { try p.run() } catch { return (127, error.localizedDescription) }
    let data = out.fileHandleForReading.readDataToEndOfFile()
    p.waitUntilExit()
    return (p.terminationStatus, String(decoding: data, as: UTF8.self))
}

struct InstallHelper: ParsableCommand {
    static let configuration = CommandConfiguration(
        commandName: "install-helper",
        abstract: "Install the privileged helper that opens real disks (run with sudo, once).",
        discussion: "Real disks' device nodes are readable only by root. The helper only opens an NTFS volume's node for the console user and hands it back; Unlatch itself runs as you.")

    func run() throws {
        guard getuid() == 0 else { throw fail("run it with sudo: sudo unlatch install-helper") }
        let src = Paths.helper
        guard FileManager.default.isExecutableFile(atPath: src.path) else { throw fail("no helper binary at \(src.path)") }
        shell(["/bin/launchctl", "bootout", "system/\(HelperOpener.label)"])          // an older one, if any
        let fm = FileManager.default
        try? fm.createDirectory(atPath: "/Library/PrivilegedHelperTools", withIntermediateDirectories: true)
        try? fm.removeItem(atPath: Paths.installedHelper)
        try? fm.removeItem(atPath: HelperOpener.legacyInstalledPath)
        try fm.copyItem(atPath: src.path, toPath: Paths.installedHelper)
        try fm.setAttributes([.posixPermissions: 0o755, .ownerAccountID: 0, .groupOwnerAccountID: 0],
                             ofItemAtPath: Paths.installedHelper)
        // The copy keeps the download's quarantine flag, but not the approval the user gave the app,
        // so Gatekeeper would block every launch (and launchd retry it every 10 s). The user is
        // installing Unlatch's own helper with administrator rights here, so the flag goes (D-75).
        removexattr(Paths.installedHelper, "com.apple.quarantine", 0)
        try helperPlist.write(toFile: Paths.launchdPlist, atomically: true, encoding: .utf8)
        try fm.setAttributes([.posixPermissions: 0o644, .ownerAccountID: 0, .groupOwnerAccountID: 0],
                             ofItemAtPath: Paths.launchdPlist)
        let (rc, out) = shell(["/bin/launchctl", "bootstrap", "system", Paths.launchdPlist])
        guard rc == 0 else { throw fail("launchctl bootstrap failed: \(out)") }
        print("""
            Installed the Unlatch helper (\(Paths.installedHelper)).
            Allow it once in System Settings > Privacy & Security > Full Disk Access: drag it there from
            Finder (open -R "\(Paths.installedHelper)"), or click +, press ⌘⇧G and enter that path.
            macOS does not let a background helper open removable disks otherwise.
            """)
    }
}

struct UninstallHelper: ParsableCommand {
    static let configuration = CommandConfiguration(
        commandName: "uninstall-helper", abstract: "Remove the privileged helper (run with sudo).")

    func run() throws {
        guard getuid() == 0 else { throw fail("run it with sudo: sudo unlatch uninstall-helper") }
        shell(["/bin/launchctl", "bootout", "system/\(HelperOpener.label)"])
        try? FileManager.default.removeItem(atPath: Paths.launchdPlist)
        try? FileManager.default.removeItem(atPath: Paths.installedHelper)
        try? FileManager.default.removeItem(atPath: HelperOpener.legacyInstalledPath)
        try? FileManager.default.removeItem(atPath: HelperOpener.socketPath)
        print("Removed the Unlatch helper.")
    }
}

struct Doctor: AsyncParsableCommand {
    static let configuration = CommandConfiguration(abstract: "Check that everything Unlatch needs is in place.")

    func run() async throws {
        var worst = Exit.success
        func check(_ ok: Bool, _ what: String, _ fix: String, _ failure: Exit = .error) {
            print("\(ok ? "ok  " : "FAIL") \(what)\(ok ? "" : "\n     \(fix)")")
            if !ok && (worst == .success || failure == .macFUSE) { worst = failure }
        }
        let v = ProcessInfo.processInfo.operatingSystemVersion
        check(v.majorVersion >= 27, "macOS \(v.majorVersion).\(v.minorVersion).\(v.patchVersion)", "Unlatch needs macOS 27 or later.")
        let fuse = MountService.macFUSEInstalled
        let info = NSDictionary(contentsOfFile: "/Library/Filesystems/macfuse.fs/Contents/Info.plist")
        let fuseVersion = (info?["CFBundleShortVersionString"] ?? info?["CFBundleVersion"]) as? String ?? "?"
        check(fuse && !MacFUSE.needsInstall, "macFUSE installed\(fuse ? " (\(fuseVersion))" : "")",
              fuse ? "macFUSE \(MacFUSE.minimumVersion) or later is needed: unlatch setup" : "unlatch setup (or brew install --cask macfuse)", .macFUSE)
        // The loaded bundle carries a macOS-major suffix (io.macfuse.filesystems.macfuse.25).
        let kext = shell(["/usr/bin/kmutil", "showloaded", "--list-only"]).1.contains("io.macfuse.filesystems.macfuse")
        check(kext || !fuse, "macFUSE kernel extension loaded",
              "Allow it once: System Settings > Privacy & Security (Apple Silicon: Reduced Security in Recovery first). It loads on the first mount.", .macFUSE)
        let daemon = Paths.daemon
        check(FileManager.default.isExecutableFile(atPath: daemon.path), "unlatch-fuse at \(daemon.path)",
              "Build it (xcodebuild -scheme Unlatch build) or set UNLATCH_FUSE.")
        check(HelperOpener.installed, "privileged helper (\(HelperOpener.socketPath))",
              "Needed for real disks, not for disk images: sudo unlatch install-helper")
        let status = StatusStore().directory
        try? FileManager.default.createDirectory(at: status, withIntermediateDirectories: true)
        check(FileManager.default.isWritableFile(atPath: status.path), "status directory \(status.path)",
              "Make it writable by you.")
        if worst != .success { throw CLIExit(code: worst) }
    }
}

/// Runs a command attached to this terminal (so sudo can ask for the password); its exit status.
@discardableResult
func interactive(_ args: [String]) -> Int32 {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: args[0])
    p.arguments = Array(args.dropFirst())
    do { try p.run() } catch { return 127 }
    p.waitUntilExit()
    return p.terminationStatus
}

/// `unlatch setup`: everything a new Mac needs, in order (D-73). It installs or updates macFUSE
/// (downloaded from macFUSE's GitHub releases, checksum, signature and notarization checked),
/// explains the one-time approval of macFUSE's kernel extension, installs the helper, and points
/// at Full Disk Access. Each step that changes the system asks first, unless --yes.
struct SetupCmd: AsyncParsableCommand {
    static let configuration = CommandConfiguration(
        commandName: "setup",
        abstract: "Install and check everything Unlatch needs (macFUSE, the helper), step by step.")
    @Flag(name: [.short, .long], help: "Do every step without asking.") var yes = false
    @Flag(help: "Skip the helper (only disk images will be mounted).") var noHelper = false

    private func confirm(_ question: String) -> Bool {
        if yes { return true }
        print("\(question) [Y/n] ", terminator: "")
        guard let a = readLine()?.trimmingCharacters(in: .whitespaces).lowercased() else { return false }
        return a.isEmpty || a == "y" || a == "yes"
    }

    private func sudo(_ args: [String]) -> Int32 {
        interactive(getuid() == 0 ? args : ["/usr/bin/sudo"] + args)
    }

    func run() async throws {
        let v = ProcessInfo.processInfo.operatingSystemVersion
        guard v.majorVersion >= 27 else { throw fail("Unlatch needs macOS 27 or later (this Mac: \(v.majorVersion).\(v.minorVersion)).") }

        // 1. macFUSE.
        if MacFUSE.needsInstall {
            let have = MacFUSE.installedVersion.map { "macFUSE \($0) is older than \(MacFUSE.minimumVersion)" } ?? "macFUSE is not installed"
            print("1. \(have). Unlatch mounts through macFUSE (free; installed separately, never bundled).")
            let r: MacFUSE.Release
            do { r = try await MacFUSE.latestRelease() } catch { throw fail(error.localizedDescription) }
            guard confirm("   Download macFUSE \(r.version) from github.com/macfuse/macfuse and install it?") else {
                throw fail("macFUSE is required. Install it from https://macfuse.github.io, then run unlatch setup again.", .macFUSE)
            }
            print("   Downloading and checking macFUSE \(r.version)...")
            let p: MacFUSE.Prepared
            do { p = try await MacFUSE.prepare(r) } catch { throw fail(error.localizedDescription) }
            defer { p.cleanup() }
            print("   Checked: SHA-256 as published, signed by macFUSE's developer (team \(MacFUSE.teamID)), notarized by Apple.")
            print("   Installing (macOS asks for your password)...")
            guard sudo(p.installCommand) == 0 else { throw fail("macFUSE's installer failed; run it by hand: open '\(p.package.path)'") }
            print("   macFUSE \(MacFUSE.installedVersion ?? r.version) installed.")
        } else {
            print("1. macFUSE \(MacFUSE.installedVersion ?? "") is installed.")
        }

        // 2. macFUSE's kernel extension: approval cannot be automated (Recovery and System Settings).
        if shell(["/usr/bin/kmutil", "showloaded", "--list-only"]).1.contains("io.macfuse.filesystems.macfuse") {
            print("2. macFUSE's kernel extension is allowed and loaded.")
        } else {
            print("2. macFUSE's kernel extension has to be allowed once. If it already is, it simply loads at the first mount.")
            print("   On Apple Silicon, if you have not done this before:")
            for (i, s) in MacFUSE.approvalSteps.enumerated() { print("   \(Character(UnicodeScalar(UInt8(97 + i))))) \(s)") }
        }

        // 3. The helper, for real disks.
        if noHelper {
            print("3. Helper skipped: disk images work; real disks need `sudo unlatch install-helper` later.")
        } else if HelperOpener.installed {
            print("3. The helper for real disks is installed.")
        } else if confirm("3. Install the helper that opens real disks for you (needs your password)?") {
            let me = Unlatch.executableDirectory.appendingPathComponent("unlatch").path
            guard sudo([me, "install-helper"]) == 0 else { throw fail("installing the helper failed") }
            print("""
                   Last step: give the helper Full Disk Access. System Settings opens at Full Disk Access, and a
                   Finder window shows the helper: drag it into the list and turn it on. (macOS checks this
                   permission for the helper, which opens the disks, not for the app; D-74.)
                """)
            interactive(["/usr/bin/open", "x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles"])
            interactive(["/usr/bin/open", "-R", HelperOpener.installedPath])
        } else {
            print("3. Helper skipped: disk images work; real disks need `sudo unlatch install-helper` later.")
        }
        print("\nDone. `unlatch doctor` checks everything again; plug in an NTFS disk, or `unlatch mount <disk>`.")
    }
}
