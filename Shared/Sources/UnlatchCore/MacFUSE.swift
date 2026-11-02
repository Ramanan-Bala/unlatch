// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation

/// macFUSE, the one thing Unlatch needs that it may not ship (its license forbids bundling it).
/// Unlatch installs it the way a user would: it downloads the official release from macFUSE's
/// own GitHub releases, checks the SHA-256 that release publishes, checks that the installer
/// package is signed by macFUSE's developer and notarized by Apple, and only then hands it to
/// Apple's `installer` (D-73). Nothing of macFUSE is redistributed.
public enum MacFUSE {
    /// The oldest macFUSE Unlatch is tested with: 5.5.0 is the first whose FSKit backend
    /// honours read-only mounts and survives empty read replies (D-78).
    public static let minimumVersion = "5.5.0"
    /// Benjamin Fleischer's Developer ID team, which signs macFUSE's installer.
    public static let teamID = "3T5GSNBU6W"
    public static let releasesAPI = URL(string: "https://api.github.com/repos/macfuse/macfuse/releases/latest")!
    public static let bundle = "/Library/Filesystems/macfuse.fs"

    public enum Problem: Error, LocalizedError, Equatable {
        case noRelease
        case checksum(String)
        case signature(String)
        case download(String)

        public var errorDescription: String? {
            switch self {
            case .noRelease: return "macFUSE's GitHub releases did not list an installer."
            case .checksum(let f): return "\(f) did not match the checksum macFUSE published; it was not installed."
            case .signature(let why): return "The macFUSE installer is not signed and notarized as expected (\(why)); it was not installed."
            case .download(let why): return "Could not download macFUSE: \(why)"
            }
        }
    }

    /// The installed version, from macfuse.fs's Info.plist; nil when macFUSE is missing.
    public static var installedVersion: String? {
        let plist = URL(fileURLWithPath: bundle + "/Contents/Info.plist")
        guard let d = NSDictionary(contentsOf: plist) else { return nil }
        return d["CFBundleShortVersionString"] as? String
    }

    /// True when macFUSE is missing or older than `minimumVersion`.
    public static var needsInstall: Bool { needsInstall(installed: installedVersion) }

    static func needsInstall(installed: String?) -> Bool {
        guard let s = installed, let v = Version(s) else { return true }
        return v < Version(minimumVersion)!
    }

    public struct Release: Sendable, Equatable {
        public let version: String
        public let dmg: URL
        public let checksums: URL
    }

    public static func latestRelease(session: URLSession = .shared) async throws -> Release {
        var req = URLRequest(url: releasesAPI)
        req.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        req.setValue("Unlatch/\(Unlatch.version)", forHTTPHeaderField: "User-Agent")
        let (data, response) = try await session.data(for: req)
        guard (response as? HTTPURLResponse)?.statusCode == 200, let r = release(from: data) else { throw Problem.noRelease }
        return r
    }

    struct GitHubRelease: Decodable {
        struct Asset: Decodable { let name: String; let browser_download_url: URL }
        let tag_name: String
        let assets: [Asset]
    }

    /// Picks `macfuse-X.dmg` and `macfuse-X.sha256` from the API's JSON (tests feed it directly).
    static func release(from json: Data) -> Release? {
        guard let r = try? JSONDecoder().decode(GitHubRelease.self, from: json) else { return nil }
        let v = r.tag_name.hasPrefix("macfuse-") ? String(r.tag_name.dropFirst(8)) : r.tag_name
        guard let dmg = r.assets.first(where: { $0.name == "macfuse-\(v).dmg" }),
              let sums = r.assets.first(where: { $0.name == "macfuse-\(v).sha256" }) else { return nil }
        return Release(version: v, dmg: dmg.browser_download_url, checksums: sums.browser_download_url)
    }

    /// A downloaded, verified installer, on an attached disk image. Install it with
    /// `installCommand`, then call `cleanup()`.
    public struct Prepared: Sendable {
        public let version: String
        public let package: URL
        let mountPoint: URL
        let directory: URL

        /// Apple's command-line installer; it needs administrator rights.
        public var installCommand: [String] { ["/usr/sbin/installer", "-pkg", package.path, "-target", "/"] }

        public func cleanup() {
            _ = try? run("/usr/bin/hdiutil", ["detach", "-quiet", "-force", mountPoint.path])
            try? FileManager.default.removeItem(at: directory)
        }
    }

    /// Downloads the release, checks its SHA-256 against the published list, attaches it read-only,
    /// and checks that the package is signed by macFUSE's team and notarized.
    public static func prepare(_ r: Release, session: URLSession = .shared) async throws -> Prepared {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("unlatch-macfuse-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        do {
            let dmg = dir.appendingPathComponent("macfuse-\(r.version).dmg")
            let (tmp, resp) = try await session.download(from: r.dmg)
            guard (resp as? HTTPURLResponse)?.statusCode == 200 else { throw Problem.download("HTTP error for \(r.dmg)") }
            try FileManager.default.moveItem(at: tmp, to: dmg)
            let (sums, _) = try await session.data(from: r.checksums)
            let want = expectedSHA256(String(decoding: sums, as: UTF8.self), file: dmg.lastPathComponent)
            let have = try run("/usr/bin/shasum", ["-a", "256", dmg.path]).split(separator: " ").first.map(String.init)
            guard let want, have == want else { throw Problem.checksum(dmg.lastPathComponent) }

            let mnt = dir.appendingPathComponent("volume")
            _ = try run("/usr/bin/hdiutil", ["attach", "-quiet", "-nobrowse", "-readonly", "-mountpoint", mnt.path, dmg.path])
            let prepared = Prepared(version: r.version, package: mnt.appendingPathComponent("Install macFUSE.pkg"),
                                    mountPoint: mnt, directory: dir)
            do {
                try verifySignature(prepared.package)
            } catch {
                prepared.cleanup()
                throw error
            }
            return prepared
        } catch {
            try? FileManager.default.removeItem(at: dir)
            throw error
        }
    }

    /// The hash for `file` in a `shasum -a 256` listing.
    static func expectedSHA256(_ listing: String, file: String) -> String? {
        for line in listing.split(whereSeparator: \.isNewline) {
            let f = line.split(separator: " ", omittingEmptySubsequences: true)
            if f.count == 2, f[1] == file || f[1] == "*" + file, f[0].count == 64 { return f[0].lowercased() }
        }
        return nil
    }

    /// pkgutil: a Developer ID Installer certificate of macFUSE's team, notarized; spctl agrees.
    static func verifySignature(_ pkg: URL) throws {
        let sig = (try? run("/usr/sbin/pkgutil", ["--check-signature", pkg.path])) ?? ""
        guard sig.contains("Developer ID Installer:"), sig.contains("(\(teamID))") else {
            throw Problem.signature("not signed by macFUSE's developer, team \(teamID)")
        }
        guard sig.contains("Notarization: trusted") else { throw Problem.signature("not notarized") }
        guard (try? run("/usr/sbin/spctl", ["-a", "-t", "install", pkg.path])) != nil else {
            throw Problem.signature("Gatekeeper rejected it")
        }
    }

    /// Runs a tool; its standard output, or an error when it fails.
    @discardableResult
    static func run(_ tool: String, _ args: [String]) throws -> String {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: tool)
        p.arguments = args
        let out = Pipe()
        p.standardOutput = out
        p.standardError = FileHandle.nullDevice
        try p.run()
        let data = out.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        guard p.terminationStatus == 0 else { throw Problem.download("\(tool) exited with \(p.terminationStatus)") }
        return String(decoding: data, as: UTF8.self)
    }

    /// The one-time step that lets macFUSE mount: turning on its file system extension (FSKit
    /// backend, D-78). No kernel extension, no Recovery, no restart.
    public static let approvalSteps = [
        "Open System Settings > General > Login Items & Extensions.",
        "Under Extensions, click the info button next to File System Extensions.",
        "Turn on macFUSE, then click Done.",
    ]
    /// System Settings > General > Login Items & Extensions.
    public static let extensionSettingsURL = URL(string: "x-apple.systempreferences:com.apple.LoginItems-Settings.extension")!
}
