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
