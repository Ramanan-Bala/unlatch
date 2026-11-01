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
        let exe = URL(fileURLWithPath: CommandLine.arguments[0]).resolvingSymlinksInPath()
        let beside = exe.deletingLastPathComponent().appendingPathComponent(name)
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
        try fm.copyItem(atPath: src.path, toPath: Paths.installedHelper)
        try fm.setAttributes([.posixPermissions: 0o755, .ownerAccountID: 0, .groupOwnerAccountID: 0],
                             ofItemAtPath: Paths.installedHelper)
        try helperPlist.write(toFile: Paths.launchdPlist, atomically: true, encoding: .utf8)
        try fm.setAttributes([.posixPermissions: 0o644, .ownerAccountID: 0, .groupOwnerAccountID: 0],
                             ofItemAtPath: Paths.launchdPlist)
        let (rc, out) = shell(["/bin/launchctl", "bootstrap", "system", Paths.launchdPlist])
        guard rc == 0 else { throw fail("launchctl bootstrap failed: \(out)") }
        print("""
            Installed the Unlatch helper (\(Paths.installedHelper)).
            Allow it once in System Settings > Privacy & Security > Full Disk Access (+, then that path):
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
        check(fuse, "macFUSE installed\(fuse ? " (\(fuseVersion))" : "")", "brew install --cask macfuse", .macFUSE)
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
