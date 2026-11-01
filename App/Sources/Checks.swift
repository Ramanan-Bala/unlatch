// SPDX-License-Identifier: GPL-3.0-or-later
import AppKit
import Observation
import ServiceManagement
import UnlatchCore

/// Onboarding checks (plan Step 14 task 3, re-scoped for macFUSE: D-39, D-57, D-60).
@MainActor @Observable
final class Checks {
    enum State: Equatable { case unknown, running, ok, failed(String) }

    var macOS: State = .unknown
    var macFUSE: State = .unknown
    var kext: State = .unknown
    var helper: State = .unknown
    var selfTest: State = .unknown

    var onboarded: Bool {
        get { UserDefaults.standard.bool(forKey: "onboarded") }
        set { UserDefaults.standard.set(newValue, forKey: "onboarded") }
    }

    /// Everything needed for disk images; the helper is only needed for real disks.
    var allPassed: Bool { macOS == .ok && macFUSE == .ok && selfTest == .ok }

    static let selfTestLabel = "UNLATCHTEST"
    static var bundledCLI: URL { Bundle.main.bundleURL.appendingPathComponent("Contents/Helpers/unlatch") }

    func run(model: AppModel) async {
        let v = ProcessInfo.processInfo.operatingSystemVersion
        macOS = v.majorVersion >= 27 ? .ok : .failed("Unlatch needs macOS 27 or later; this Mac has \(v.majorVersion).\(v.minorVersion).")
        macFUSE = MountService.macFUSEInstalled ? .ok : .failed("Install macFUSE, then come back here.")
        kext = Self.kextLoaded() ? .ok : .failed("It loads on the first mount; macOS may ask you to allow it in Privacy & Security.")
        helper = HelperOpener.installed ? .ok : .failed("Needed for real disks, not for disk images.")
        if macFUSE == .ok, selfTest != .ok { await runSelfTest() }
    }

    static func kextLoaded() -> Bool {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/kmutil")
        p.arguments = ["showloaded", "--list-only"]
        let out = Pipe()
        p.standardOutput = out
        p.standardError = FileHandle.nullDevice
        guard (try? p.run()) != nil else { return false }
        let data = out.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        return String(decoding: data, as: UTF8.self).contains("io.macfuse.filesystems.macfuse")
    }

    /// Mounts the bundled tiny NTFS image read-only, reads its marker file, and ejects it.
    func runSelfTest() async {
        selfTest = .running
        guard let img = Bundle.main.url(forResource: "selftest", withExtension: "img") else {
            selfTest = .failed("The test image is missing from the app.")
            return
        }
        do {
            let service = try MountService(daemon: Bundle.main.bundleURL.appendingPathComponent("Contents/Helpers/unlatch-fuse"))
            let bsd = try Self.attach(img)
            defer { Self.detach(bsd) }
            let r = try await service.mount(bsd, readOnly: true)
            let marker = try String(contentsOfFile: r.mountPoint + "/unlatch-selftest.txt", encoding: .utf8)
            try await service.eject(bsd)
            selfTest = marker.hasPrefix("Unlatch self-test") ? .ok : .failed("The test volume read back wrong.")
            kext = Self.kextLoaded() ? .ok : kext
        } catch {
            selfTest = .failed(error.localizedDescription)
        }
    }

    private static func attach(_ img: URL) throws -> String {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/hdiutil")
        p.arguments = ["attach", "-nomount", "-readonly", "-nobrowse", img.path]
        let out = Pipe()
        p.standardOutput = out
        p.standardError = FileHandle.nullDevice
        try p.run()
        let data = out.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        guard let dev = String(decoding: data, as: UTF8.self).split(whereSeparator: \.isWhitespace).first,
              dev.hasPrefix("/dev/") else { throw MountError.refused("Could not attach the test image.") }
        return String(dev.dropFirst(5))
    }

    private static func detach(_ bsd: String) {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/hdiutil")
        p.arguments = ["detach", "-quiet", bsd]
        try? p.run()
        p.waitUntilExit()
    }

    /// Installs the helper with an administrator prompt (it runs `unlatch install-helper`).
    func installHelper() async {
        helper = .running
        let cmd = "'\(Self.bundledCLI.path)' install-helper"
        let script = "do shell script \"\(cmd.replacingOccurrences(of: "\"", with: "\\\""))\" with administrator privileges"
        var err: NSDictionary?
        _ = NSAppleScript(source: script)?.executeAndReturnError(&err)
        helper = HelperOpener.installed ? .ok : .failed(err?[NSAppleScript.errorMessage] as? String ?? "The helper was not installed.")
    }

    static func openFullDiskAccess() {
        NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles")!)
    }

    var launchAtLogin: Bool {
        get { SMAppService.mainApp.status == .enabled }
        set { try? newValue ? SMAppService.mainApp.register() : SMAppService.mainApp.unregister() }
    }
}
