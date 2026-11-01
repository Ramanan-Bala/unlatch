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
