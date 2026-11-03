// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation
import Testing
@testable import UnlatchCore

// MARK: unit tests

@Test func autoMountPolicyDefaultsAndChoices() {
    let suite = "unlatch.test.\(UUID().uuidString)"
    defer { UserDefaults().removePersistentDomain(forName: suite) }
    let policy = AutoMountPolicy(suite: suite)
    func disk(isInternal: Bool, removable: Bool = false, uuid: String? = "U1") -> DiskInfo {
        DiskInfo(bsdName: "disk9s1", wholeDisk: "disk9", label: "X", size: 1, volumeUUID: uuid, isInternal: isInternal,
                 isRemovable: removable, isEjectable: removable, owner: .none, mountPoint: nil)
    }
    #expect(policy.shouldAutoMount(disk(isInternal: false)))
    #expect(!policy.shouldAutoMount(disk(isInternal: true)))
    #expect(policy.shouldAutoMount(disk(isInternal: true, removable: true)))
    policy.set(.deny, for: "U1")
    #expect(!policy.shouldAutoMount(disk(isInternal: false)))
    policy.set(.allow, for: "U1")
    #expect(policy.shouldAutoMount(disk(isInternal: true)))
    policy.set(nil, for: "U1")
    #expect(policy.choice(for: "U1") == nil)
}

@Test func statusStoreReadsNewestRecord() throws {
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: dir) }
    let rec = { (serial: String, bsd: String, when: String, state: String) in
        """
        {"bsdName":"\(bsd)","engineABI":1,"label":"L","mounted":true,"readOnly":false,"reason":"",
         "serial":"\(serial)","state":"\(state)","timestamp":"\(when)","unlatchVersion":"0.0.0"}
        """
    }
    try rec("A", "disk7s1", "2026-11-01T10:00:00Z", "clean").write(to: dir.appendingPathComponent("A.json"), atomically: true, encoding: .utf8)
    try rec("B", "disk7s1", "2026-11-01T11:00:00Z", "dirty").write(to: dir.appendingPathComponent("B.json"), atomically: true, encoding: .utf8)
    try "not json".write(to: dir.appendingPathComponent("C.json"), atomically: true, encoding: .utf8)
    let store = StatusStore(directory: dir)
    #expect(store.all().count == 2)
    #expect(store.latest(bsdName: "disk7s1")?.state == .dirty)
    #expect(store.latest(bsdName: "disk1") == nil)
}

@Test func mountPointNames() {
    let d = DiskInfo(bsdName: "disk9s1", wholeDisk: nil, label: "a/b", size: 0, volumeUUID: nil, isInternal: false,
                     isRemovable: true, isEjectable: true, owner: .none, mountPoint: nil)
    #expect(MountService.mountPoint(for: d) == "/Volumes/a:b")
}

@Test func mountPointReusesLeftoverFolder() throws {
    let vols = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString).path
    defer { try? FileManager.default.removeItem(atPath: vols) }
    let d = DiskInfo(bsdName: "disk9s1", wholeDisk: nil, label: "L", size: 0, volumeUUID: nil, isInternal: false,
                     isRemovable: true, isEjectable: true, owner: .none, mountPoint: nil)
    try FileManager.default.createDirectory(atPath: "\(vols)/L", withIntermediateDirectories: true)
    #expect(MountService.mountPoint(for: d, volumes: vols) == "\(vols)/L")            // empty leftover: reused
    try "x".write(toFile: "\(vols)/L/f", atomically: true, encoding: .utf8)
    #expect(MountService.mountPoint(for: d, volumes: vols) == "\(vols)/L disk9s1")    // not empty
    try FileManager.default.removeItem(atPath: "\(vols)/L")
    try FileManager.default.createSymbolicLink(atPath: "\(vols)/L", withDestinationPath: "/tmp")
    #expect(MountService.mountPoint(for: d, volumes: vols) == "\(vols)/L disk9s1")    // a link
    #expect(!MountService.isLeftover("/"))                                              // a mount point
    #expect(MountService.isFSKitRefusal("macFUSE's file system extension did not take the mount; try again in a minute"))
    #expect(!MountService.isFSKitRefusal("this volume (serial 1) is already mounted; unmount it first"))
    #expect(MountService.isTransientRefusal("/dev/rdisk6s2: this volume (serial 1) is already mounted; unmount it first"))
    #expect(MountService.isTransientRefusal("/Volumes/SANDISK: Input/output error"))
    #expect(!MountService.isTransientRefusal("/dev/rdisk6s2: not mounting, corrupt"))
}

// MARK: integration (attached images; needs macFUSE's file system extension on and a built unlatch-fuse)

private let integration = ProcessInfo.processInfo.environment["UNLATCH_INTEGRATION"] == "1"

private func sh(_ args: String...) throws -> String { try run(args) }

private func run(_ args: [String]) throws -> String {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: args[0])
    p.arguments = Array(args.dropFirst())
    let out = Pipe()
    p.standardOutput = out
    p.standardError = FileHandle.nullDevice
    try p.run()
    let data = out.fileHandleForReading.readDataToEndOfFile()
    p.waitUntilExit()
    return String(decoding: data, as: UTF8.self)
}

private let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
    .deletingLastPathComponent().deletingLastPathComponent()

/// A fresh NTFS image, attached; returns the BSD name of the volume (whole disk, no partition map).
private func attachFreshImage(_ name: String, mount: Bool) throws -> String {
    let dir = root.appendingPathComponent("build/core-test")
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    let img = dir.appendingPathComponent("\(name).img")
    try? FileManager.default.removeItem(at: img)
    let p = Process()
    p.executableURL = root.appendingPathComponent("Scripts/make-image.sh")
    p.arguments = [name, "64m", name.uppercased()]
    p.currentDirectoryURL = dir
    p.standardOutput = FileHandle.nullDevice
    try p.run()
    p.waitUntilExit()
    var args = ["/usr/bin/hdiutil", "attach", "-nobrowse", img.path]
    if !mount { args.insert("-nomount", at: 2) }
    let out = try run(args)
    let dev = try #require(out.split(whereSeparator: { $0 == " " || $0 == "\t" || $0 == "\n" }).first)
    return String(dev.dropFirst(5))
}

@Test(.enabled(if: integration), .timeLimit(.minutes(2)))
func listMountWriteUnmountEject() async throws {
    let service = try MountService(daemon: root.appendingPathComponent("build/DerivedData/Build/Products/Debug/unlatch-fuse"))
    let bsd = try attachFreshImage("coremount", mount: false)
    #expect(await service.list().contains { $0.bsdName == bsd && $0.owner == .none })
    let r = try await service.mount(bsd)
    #expect(!r.readOnly && r.mountPoint.hasPrefix("/Volumes/"))
    #expect(try await service.disk(bsd).owner == .unlatch)
    try "hello".write(toFile: r.mountPoint + "/hello.txt", atomically: false, encoding: .utf8)
    try await service.unmount(bsd)
    #expect(try await service.disk(bsd).owner == .none)
    let again = try await service.mount(bsd, readOnly: true)
    #expect(try String(contentsOfFile: again.mountPoint + "/hello.txt", encoding: .utf8) == "hello")
    try await service.eject(bsd)
    await #expect(throws: MountError.self) { try await service.disk(bsd) }
}

@Test(.enabled(if: integration), .timeLimit(.minutes(2)))
func appleMountComesBackAfterAFailure() async throws {
    let broken = try MountService(daemon: URL(fileURLWithPath: "/usr/bin/false"))
    let bsd = try attachFreshImage("coreapple", mount: true)
    #expect(try await broken.disk(bsd).owner == .apple)
    await #expect(throws: MountError.self) { try await broken.mount(bsd) }
    #expect(try await broken.disk(bsd).owner == .apple)
    try await broken.eject(bsd)
}

/// A daemon that fails twice as macFUSE's FSKit backend does when it refuses a mount, then
/// runs the real one: MountService tries again and mounts (D-84).
@Test(.enabled(if: integration), .timeLimit(.minutes(2)))
func fskitRefusalIsRetried() async throws {
    let dir = root.appendingPathComponent("build/core-test")
    let count = dir.appendingPathComponent("flaky.count")
    let script = dir.appendingPathComponent("flaky-daemon.sh")
    let real = root.appendingPathComponent("build/DerivedData/Build/Products/Debug/unlatch-fuse")
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    try? FileManager.default.removeItem(at: count)
    try """
        #!/bin/sh
        n=$(cat '\(count.path)' 2>/dev/null || echo 0); echo $((n + 1)) > '\(count.path)'
        if [ "$n" -lt 2 ]; then
            echo "unlatch-fuse: macFUSE's file system extension did not take the mount; try again in a minute" >&2
            exit 1
        fi
        exec '\(real.path)' "$@"
        """.write(to: script, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: script.path)
    let service = try MountService(daemon: script)
    let bsd = try attachFreshImage("coreflaky", mount: false)
    let r = try await service.mount(bsd)
    #expect(try String(contentsOf: count, encoding: .utf8).trimmingCharacters(in: .whitespacesAndNewlines) == "3")
    #expect(try await service.disk(bsd).owner == .unlatch && r.mountPoint.hasPrefix("/Volumes/"))
    try await service.eject(bsd)
}

@Test(.enabled(if: integration), .timeLimit(.minutes(1)))
func watcherSeesAnAttachedVolume() async throws {
    let watcher = try #require(DiskWatcher())
    let events = watcher.events()
    let bsd = try attachFreshImage("corewatch", mount: false)
    var seen = false
    for await e in events {
        if case .appeared(let d) = e, d.bsdName == bsd { seen = true; break }
    }
    watcher.stop()
    #expect(seen)
    _ = try sh("/usr/bin/hdiutil", "detach", bsd)
}

@Test(.enabled(if: integration), .timeLimit(.minutes(2)))
func deadDaemonMountIsReaped() async throws {
    let service = try MountService(daemon: root.appendingPathComponent("build/DerivedData/Build/Products/Debug/unlatch-fuse"))
    let bsd = try attachFreshImage("corereap", mount: false)
    let r = try await service.mount(bsd)
    let pid = try #require(await service.status(bsd)?.pid)
    kill(pid, SIGKILL)
    try await Task.sleep(nanoseconds: 500_000_000)
    // macFUSE keeps the dead mount listed until it is force-unmounted; list() does that.
    let disks = await service.list()
    #expect(disks.first { $0.bsdName == bsd }?.owner == DiskInfo.Owner.none)
    // macOS removes the /Volumes folder shortly after the forced unmount.
    for _ in 0..<30 where FileManager.default.fileExists(atPath: r.mountPoint) {
        try await Task.sleep(nanoseconds: 100_000_000)
    }
    #expect(!FileManager.default.fileExists(atPath: r.mountPoint))
    #expect(await service.status(bsd)?.mounted == false)
    try await service.eject(bsd)
}

// MARK: Check Disk (D-93)

@Test func checkReportDecodesAndJudges() throws {
    // unlatch-fuse --check output for chk-index.img (Scripts/test-engine.sh), cut to three problems.
    let json = """
        {"schema": 1, "problems": [{"kind": "index_unreadable", "repairable": false, "record": 69, "path": "/big", \
        "detail": "the directory's index cannot be read"}, {"kind": "orphan", "repairable": false, "record": 70, \
        "path": "/big/f00000", "detail": "x"}, {"kind": "orphan", "repairable": false, "record": 71, "path": \
        "/big/f00001", "detail": "y"}], "state": "clean", "records": 370, "recordsInUse": 325, "directories": 4, \
        "entries": 20, "clusters": 8191, "clustersInUse": 743, "lostRecords": 0, "lostClusters": 0, "found": 3, \
        "repairable": 0, "repaired": 0, "remaining": 3, "repairRan": false, "dirtyCleared": false, "exitCode": 4}
        """
    let r = try JSONDecoder().decode(CheckReport.self, from: Data(json.utf8))
    #expect(r.verdict == .needsWindows && r.problems.count == 3 && r.problems[0].other == nil)
    #expect(r.kinds.map(\.kind) == ["index_unreadable", "orphan"] && r.kinds[1].count == 2)
    #expect(CheckReport.describe("orphan", count: 2) == "2 files no folder lists")

    func report(found: UInt64, repairable: UInt64, remaining: UInt64, ran: Bool) throws -> CheckReport {
        let j = """
            {"schema": 1, "problems": [], "state": "dirty", "records": 1, "recordsInUse": 1, "directories": 1, \
            "entries": 1, "clusters": 1, "clustersInUse": 1, "lostRecords": 0, "lostClusters": 0, "found": \(found), \
            "repairable": \(repairable), "repaired": 0, "remaining": \(remaining), "repairRan": \(ran), \
            "dirtyCleared": false, "exitCode": 0}
            """
        return try JSONDecoder().decode(CheckReport.self, from: Data(j.utf8))
    }
    #expect(try report(found: 0, repairable: 0, remaining: 0, ran: false).verdict == .clean)
    #expect(try report(found: 2, repairable: 2, remaining: 2, ran: false).verdict == .repairable)
    #expect(try report(found: 2, repairable: 2, remaining: 0, ran: true).verdict == .repaired)
    #expect(try report(found: 3, repairable: 2, remaining: 3, ran: false).verdict == .needsWindows)
    #expect(try report(found: 0, repairable: 0, remaining: 0, ran: false).entriesRemoved == nil)

    // A dangling folder entry (D-96), as a disk pulled mid-copy leaves it: repairable, and the
    // advice no longer sends anyone to chkdsk.
    func dangling(remaining: UInt64, removed: UInt64, ran: Bool) throws -> CheckReport {
        let j = """
            {"schema": 1, "problems": [{"kind": "entry_free_record", "repairable": true, "record": 64, "other": 280, \
            "path": "/unlatch-test/copy/2", "detail": "the entry points at record 280, which is not in use"}], \
            "state": "dirty", "records": 1, "recordsInUse": 1, "directories": 1, "entries": 1, "clusters": 1, \
            "clustersInUse": 1, "lostRecords": 0, "lostClusters": 0, "found": 1, "repairable": 1, "repaired": 0, \
            "remaining": \(remaining), "entriesRemoved": \(removed), "repairRan": \(ran), "dirtyCleared": \(ran), \
            "exitCode": 0}
            """
        return try JSONDecoder().decode(CheckReport.self, from: Data(j.utf8))
    }
    let found = try dangling(remaining: 1, removed: 0, ran: false)
    #expect(found.verdict == .repairable && found.advice.contains("folder entries") && !found.advice.contains("chkdsk"))
    #expect(!found.advice.contains("marks space"))
    let fixed = try dangling(remaining: 0, removed: 1, ran: true)
    #expect(fixed.verdict == .repaired && fixed.advice.contains("1 folder entry") && !fixed.advice.contains("space in use"))

    // An orphan and $MFTMirr out of step (D-98, D-99), as a cut-off write leaves them: repairable
    // without Windows, and the repair says what it did.
    func cutOff(remaining: UInt64, ran: Bool) throws -> CheckReport {
        let j = """
            {"schema": 1, "problems": [{"kind": "mft_mirror", "repairable": true, "record": 3, "path": "/$MFTMirr", \
            "detail": "x"}, {"kind": "orphan", "repairable": true, "record": 300, "path": "/a/b.txt", "detail": "y"}], \
            "state": "corrupt", "records": 1, "recordsInUse": 1, "directories": 1, "entries": 1, "clusters": 1, \
            "clustersInUse": 1, "lostRecords": 0, "lostClusters": 0, "found": 2, "repairable": 2, "repaired": 0, \
            "remaining": \(remaining), "entriesRemoved": 0, "orphansRelinked": \(ran ? 1 : 0), \
            "orphansFound": \(ran ? 1 : 0), "mirrorRepaired": \(ran), "repairRan": \(ran), "dirtyCleared": \(ran), \
            "exitCode": 0}
            """
        return try JSONDecoder().decode(CheckReport.self, from: Data(j.utf8))
    }
    let cut = try cutOff(remaining: 2, ran: false)
    #expect(cut.verdict == .repairable && !cut.advice.contains("chkdsk"))
    #expect(cut.advice.contains("no folder lists") && cut.advice.contains("backup copy of the file table"))
    let mended = try cutOff(remaining: 0, ran: true)
    #expect(mended.verdict == .repaired && mended.mirrorRepaired == true && mended.orphansFound == 1)
    #expect(mended.advice.contains("brought up to date") && mended.advice.contains("found.000"))
    #expect(try report(found: 0, repairable: 0, remaining: 0, ran: false).orphansRelinked == nil)
}

@Test(.enabled(if: integration), .timeLimit(.minutes(2)))
func checkUnmountsChecksAndRemounts() async throws {
    let service = try MountService(daemon: root.appendingPathComponent("build/DerivedData/Build/Products/Debug/unlatch-fuse"))
    let bsd = try attachFreshImage("corecheck", mount: false)
    let r = try await service.mount(bsd)
    try "hello".write(toFile: r.mountPoint + "/hello.txt", atomically: false, encoding: .utf8)
    let report = try await service.check(bsd)
    #expect(report.verdict == .clean && report.found == 0 && report.recordsInUse > 16)
    #expect(try await service.disk(bsd).owner == .unlatch)          // mounted again
    try await service.eject(bsd)
}

@Test(.enabled(if: integration), .timeLimit(.minutes(2)))
func checkRepairsBitmapsAndClearsTheMark() async throws {
    let mkfixture = root.appendingPathComponent("build/test-engine/mkfixture"), setDirty = root.appendingPathComponent("build/test-engine/set_dirty")
    try #require(FileManager.default.isExecutableFile(atPath: mkfixture.path), "run Scripts/test-engine.sh first")
    let service = try MountService(daemon: root.appendingPathComponent("build/DerivedData/Build/Products/Debug/unlatch-fuse"))
    let bsd = try attachFreshImage("corerepair", mount: false)
    try await service.eject(bsd)
    let img = root.appendingPathComponent("build/core-test/corerepair.img").path
    let p = Process()
    p.executableURL = mkfixture
    p.arguments = [img]
    let input = Pipe()
    p.standardInput = input
    try p.run()
    input.fileHandleForWriting.write(Data("file /a.bin 20000 1\nclear-record-bit /a.bin\nclear-cluster-bits /a.bin\n".utf8))
    input.fileHandleForWriting.closeFile()
    p.waitUntilExit()
    _ = try run([setDirty.path, img])
    let out = try run(["/usr/bin/hdiutil", "attach", "-nobrowse", "-nomount", img])
    let dev = String(try #require(out.split(whereSeparator: { $0 == " " || $0 == "\t" || $0 == "\n" }).first).dropFirst(5))
    let found = try await service.check(dev)
    #expect(found.verdict == .repairable && found.found == 2 && found.state == "dirty")
    let fixed = try await service.check(dev, repair: true)
    #expect(fixed.verdict == .repaired && fixed.dirtyCleared && fixed.remaining == 0)
    #expect(try await service.check(dev).verdict == .clean)
    try await service.eject(dev)
}
