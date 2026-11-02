// SPDX-License-Identifier: GPL-3.0-or-later
import Testing
import Foundation
@testable import UnlatchCore

@Test func identifiersDeriveFromBundlePrefix() {
    #expect(Unlatch.appGroup == "group.io.github.ramanan-bala.Unlatch")
    #expect(Unlatch.logSubsystem == "io.github.ramanan-bala.unlatch")
}

/// The record `Fuse/status.c` writes must decode as `VolumeStatus` (D-41).
@Test func decodesDaemonStatusRecord() throws {
    let json = """
    {
      "bsdName" : "disk6", "engineABI" : 1, "label" : "LOG \\"q\\"", "mounted" : true,
      "readOnly" : true, "reason" : "Windows Fast Startup", "serial" : "54DDA118567EC0ED",
      "state" : "hibernated", "timestamp" : "2026-11-01T10:48:26Z", "unlatchVersion" : "0.0.0"
    }
    """
    let decoder = JSONDecoder()
    decoder.dateDecodingStrategy = .iso8601
    let s = try decoder.decode(VolumeStatus.self, from: Data(json.utf8))
    #expect(s.state == .hibernated)
    #expect(s.label == "LOG \"q\"")
    #expect(s.readOnly && s.mounted)
}

@Test func versionOrder() throws {
    let v = { (s: String) in try #require(Version(s)) }
    #expect(try v("1.0.0") > v("0.9.9"))
    #expect(try v("1.0.0") > v("1.0.0-rc.1"))
    #expect(try v("1.0.0-beta.10") > v("1.0.0-beta.2"))
    #expect(try v("1.0.0-beta") < v("1.0.0-beta.1"))
    #expect(try v("1.10.0") > v("1.9.0"))
    #expect(Version("1.0") == nil && Version("v1.0.0") == nil)
}

@Test func updateOffer() {
    let json = Data("""
    [{"tag_name":"v1.1.0-beta.1","html_url":"https://example.org/b","draft":false,"prerelease":true},
     {"tag_name":"v1.0.1","html_url":"https://example.org/r","draft":false,"prerelease":false},
     {"tag_name":"v2.0.0","html_url":"https://example.org/d","draft":true,"prerelease":false},
     {"tag_name":"v0.9.0","html_url":"https://example.org/o","draft":false,"prerelease":false}]
    """.utf8)
    #expect(UpdateChecker.newest(in: json, current: "1.0.0")?.version == "1.0.1")       // no drafts, no betas
    #expect(UpdateChecker.newest(in: json, current: "1.0.0-beta.3")?.version == "1.1.0-beta.1")
    #expect(UpdateChecker.newest(in: json, current: "1.0.1") == nil)
    #expect(UpdateChecker.newest(in: Data("{}".utf8), current: "1.0.0") == nil)
}

@Test func macFUSERelease() throws {
    let json = Data("""
    {"tag_name":"macfuse-5.5.0","assets":[
      {"name":"macfuse-5.5.0-debug.tbz","browser_download_url":"https://example.org/d"},
      {"name":"macfuse-5.5.0.dmg","browser_download_url":"https://example.org/m.dmg"},
      {"name":"macfuse-5.5.0.sha256","browser_download_url":"https://example.org/s"}]}
    """.utf8)
    let r = try #require(MacFUSE.release(from: json))
    #expect(r.version == "5.5.0" && r.dmg.absoluteString == "https://example.org/m.dmg")
    let listing = """
    92c21ef2db4199bcf5bddc8e6efb57b23ecb9715f74ab38cba2c77ab226d3830  macfuse-5.5.0.dmg
    57a7be08cf33630bc1345afab0687ae345cdf558a3748bf4aec8aa5715d50f3e  macfuse-5.5.0-debug.tbz
    """
    #expect(MacFUSE.expectedSHA256(listing, file: "macfuse-5.5.0.dmg") == "92c21ef2db4199bcf5bddc8e6efb57b23ecb9715f74ab38cba2c77ab226d3830")
    #expect(MacFUSE.expectedSHA256(listing, file: "macfuse-5.6.0.dmg") == nil)
    #expect(MacFUSE.needsInstall(installed: nil))
    #expect(MacFUSE.needsInstall(installed: "4.8.0"))
    #expect(!MacFUSE.needsInstall(installed: "5.4.0") && !MacFUSE.needsInstall(installed: "5.5.0"))
}

/// Downloads the real latest macFUSE release and verifies it, without installing anything.
@Test(.enabled(if: ProcessInfo.processInfo.environment["UNLATCH_INTEGRATION"] == "1"), .timeLimit(.minutes(3)))
func macFUSEDownloadVerifies() async throws {
    let r = try await MacFUSE.latestRelease()
    let p = try await MacFUSE.prepare(r)
    defer { p.cleanup() }
    #expect(FileManager.default.fileExists(atPath: p.package.path))
    #expect(p.installCommand.first == "/usr/sbin/installer")
}

/// A package not signed by macFUSE's developer is refused.
@Test func macFUSEUnsignedPackageRefused() throws {
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent("unlatch-pkgtest-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: dir.appendingPathComponent("root"), withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: dir) }
    let pkg = dir.appendingPathComponent("Install macFUSE.pkg")
    try MacFUSE.run("/usr/bin/pkgbuild", ["--root", dir.appendingPathComponent("root").path, "--identifier",
                                          "test.fake.macfuse", "--version", "1", pkg.path])
    #expect(throws: MacFUSE.Problem.self) { try MacFUSE.verifySignature(pkg) }
}
