// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation
import Testing
@testable import UnlatchCLI
@testable import UnlatchCore

private func disk(_ bsd: String, label: String, mount: String? = nil) -> DiskInfo {
    DiskInfo(bsdName: bsd, wholeDisk: String(bsd.prefix { $0 != "s" || true }), label: label, size: 1_000_000,
             volumeUUID: "U-\(bsd)", isInternal: false, isRemovable: true, isEjectable: true,
             owner: mount == nil ? .none : .unlatch, mountPoint: mount)
}

@Test func parsesEverySubcommand() {
    for args in [["list"], ["list", "--json"], ["mount", "disk6s1"], ["mount", "-r", "disk6s1"], ["mount", "--read-only", "X"],
                 ["unmount", "disk6s1", "--force"], ["eject", "/dev/disk6"], ["status"], ["status", "disk6s1", "--json"],
                 ["doctor"], ["install-helper"], ["uninstall-helper"]] {
        #expect(UnlatchCLI.parseExit(args) == .success, "\(args)")
    }
    for args in [["mount"], ["frobnicate"], ["list", "--nope"], ["unmount"]] {
        #expect(UnlatchCLI.parseExit(args) == .usage, "\(args)")
    }
}

@Test func resolvesDiskArguments() throws {
    let disks = [disk("disk6s1", label: "Seagate", mount: "/Volumes/Seagate"), disk("disk7", label: "WORK"), disk("disk8", label: "work 2")]
    #expect(try resolve("disk6s1", in: disks).bsdName == "disk6s1")
    #expect(try resolve("/dev/disk7", in: disks).bsdName == "disk7")
    #expect(try resolve("/dev/rdisk7", in: disks).bsdName == "disk7")
    #expect(try resolve("/Volumes/Seagate/", in: disks).bsdName == "disk6s1")
    #expect(try resolve("seagate", in: disks).bsdName == "disk6s1")
    #expect(throws: CLIExit.self) { try resolve("disk9", in: disks) }
    let twins = [disk("disk7", label: "X"), disk("disk8", label: "x")]
    #expect(throws: CLIExit.self) { try resolve("X", in: twins) }
}

@Test func exitCodes() {
    #expect(Exit.of(MountError.macFUSEMissing) == .macFUSE)
    #expect(Exit.of(MountError.busy("x")) == .error)
    #expect(Exit.success.rawValue == 0 && Exit.error.rawValue == 1 && Exit.usage.rawValue == 2)
    #expect(Exit.readOnly.rawValue == 3 && Exit.macFUSE.rawValue == 4)
}

/// The schema-1 JSON is a contract: this snapshot must only ever gain fields.
@Test func jsonSnapshot() {
    let suite = "unlatch.cli.test.\(UUID().uuidString)"
    defer { UserDefaults().removePersistentDomain(forName: suite) }
    let st = VolumeStatus(bsdName: "disk6s1", serial: "54DDA118567EC0ED", label: "Seagate", state: .dirty,
                          reason: "volume is marked dirty", readOnly: true, mounted: true, engineABI: 1,
                          timestamp: Date(timeIntervalSince1970: 1_793_520_000))
    let v = JSONOut.volume(disk("disk6s1", label: "Seagate", mount: "/Volumes/Seagate"), status: st,
                           policy: AutoMountPolicy(suite: suite))
    let json = JSONOut.encode(JSONOut.Volumes(volumes: [v]))
    #expect(json == """
    {
      "schema" : 1,
      "volumes" : [
        {
          "autoMount" : true,
          "bsdName" : "disk6s1",
          "devicePath" : "/dev/disk6s1",
          "ejectable" : true,
          "internal" : false,
          "label" : "Seagate",
          "mountPoint" : "/Volumes/Seagate",
          "owner" : "unlatch",
          "removable" : true,
          "size" : 1000000,
          "status" : {
            "mounted" : true,
            "readOnly" : true,
            "reason" : "volume is marked dirty",
            "serial" : "54DDA118567EC0ED",
            "state" : "dirty",
            "timestamp" : "2026-11-01T08:00:00Z"
          },
          "volumeUUID" : "U-disk6s1"
        }
      ]
    }
    """)
}
