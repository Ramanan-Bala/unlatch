// SPDX-License-Identifier: GPL-3.0-or-later
import ArgumentParser
import Foundation
import UnlatchCore

/// Runs the CLI with the exit codes of docs/cli.md (ArgumentParser's own are 64 for usage).
public enum UnlatchCLI {
    public static func run(_ args: [String]) async -> Never {
        let command: ParsableCommand
        do {
            command = try UnlatchCommand.parseAsRoot(args)
        } catch {
            if UnlatchCommand.exitCode(for: error) == .success {
                UnlatchCommand.exit(withError: error)       // --help, --version: print and exit 0
            }
            FileHandle.standardError.write(Data((UnlatchCommand.fullMessage(for: error) + "\n").utf8))
            exit(Exit.usage.rawValue)
        }
        do {
            if var c = command as? AsyncParsableCommand {
                try await c.run()
            } else {
                var c = command
                try c.run()
            }
            exit(Exit.success.rawValue)
        } catch let e as CLIExit {
            exit(e.code.rawValue)
        } catch {
            // The root command with no subcommand asks for help by throwing (CleanExit).
            if UnlatchCommand.exitCode(for: error) == .success {
                UnlatchCommand.exit(withError: error)
            }
            FileHandle.standardError.write(Data("unlatch: \(error.localizedDescription)\n".utf8))
            exit(Exit.of(error).rawValue)
        }
    }

    /// Exit code for a parse of `args` (tests): .usage when the arguments do not parse.
    static func parseExit(_ args: [String]) -> Exit {
        (try? UnlatchCommand.parseAsRoot(args)) == nil ? .usage : .success
    }
}

/// Thrown by commands that have already printed their message.
struct CLIExit: Error {
    let code: Exit
}

func fail(_ message: String, _ code: Exit = .error) -> CLIExit {
    FileHandle.standardError.write(Data("unlatch: \(message)\n".utf8))
    return CLIExit(code: code)
}

public struct UnlatchCommand: AsyncParsableCommand {
    public static let configuration = CommandConfiguration(
        commandName: "unlatch",
        abstract: "Mount NTFS volumes read-write on macOS.",
        discussion: """
        <disk> is a BSD name (disk6s1), a device path (/dev/disk6s1), a mount path or a volume label.

        Exit codes: 0 success, 1 error, 2 usage, 3 mounted but forced read-only (the reason is printed),
        4 macFUSE missing or not allowed, 5 check found problems it did not repair. JSON output is versioned ("schema": 1); see docs/cli.md.
        """,
        version: Unlatch.version,
        subcommands: [List.self, MountCmd.self, Unmount.self, Eject.self, Status.self, Check.self, Doctor.self,
                      SetupCmd.self, InstallHelper.self, UninstallHelper.self]
    )

    public init() {}
}

// MARK: resolving <disk>

/// BSD name, /dev path (raw or not), mount path or label (case-insensitive, must be unique).
func resolve(_ arg: String, in disks: [DiskInfo]) throws -> DiskInfo {
    var name = arg
    if name.hasPrefix("/dev/") { name = String(name.dropFirst(5)) }
    if name.hasPrefix("rdisk") { name = String(name.dropFirst()) }
    if let d = disks.first(where: { $0.bsdName == name }) { return d }
    let path = (arg as NSString).standardizingPath
    if let d = disks.first(where: { $0.mountPoint.map { ($0 as NSString).standardizingPath } == path }) { return d }
    let byLabel = disks.filter { $0.label.caseInsensitiveCompare(arg) == .orderedSame }
    if byLabel.count == 1 { return byLabel[0] }
    if byLabel.count > 1 {
        throw fail("\"\(arg)\" names \(byLabel.count) volumes (\(byLabel.map(\.bsdName).joined(separator: ", "))); use a BSD name")
    }
    throw fail("no NTFS volume \"\(arg)\" (see unlatch list)")
}

func service() throws -> MountService {
    do { return try MountService(daemon: Paths.daemon) } catch { throw fail(error.localizedDescription) }
}

// MARK: commands

struct List: AsyncParsableCommand {
    static let configuration = CommandConfiguration(abstract: "List NTFS volumes and who has them mounted.")
    @Flag(help: "JSON output (schema 1).") var json = false

    func run() async throws {
        let s = try service()
        let disks = await s.list()
        let policy = AutoMountPolicy()
        if json {
            print(JSONOut.encode(JSONOut.Volumes(volumes: disks.map { JSONOut.volume($0, status: nil, policy: policy) })))
            return
        }
        if disks.isEmpty {
            print("No NTFS volumes.")
            return
        }
        for d in disks {
            let where_ = d.mountPoint.map { " on \($0)" } ?? ""
            let label = d.label.isEmpty ? "(no label)" : d.label
            print("\(d.bsdName)\t\(label)\t\(humanSize(d.size))\t\(JSONOut.owner(d.owner))\(where_)")
        }
    }
}

struct MountCmd: AsyncParsableCommand {
    static let configuration = CommandConfiguration(
        commandName: "mount", abstract: "Mount a volume with Unlatch (read-write when it is safe).")
    @Argument(help: "The volume.") var disk: String
    @Flag(name: .shortAndLong, help: "Mount read-only.") var readOnly = false
    @Flag(help: ArgumentHelp("Enable read-write without checking a volume marked dirty; a clean unmount or eject clears the mark.",
                             discussion: """
                                 It repairs nothing: prefer `unlatch check --repair <disk>`, which checks first. Writing to \
                                 a disk that was cut off mid-write can damage files on it. See docs/read-only.md.
                                 """))
    var markChecked = false
    @Flag(help: "JSON output (schema 1).") var json = false

    func run() async throws {
        let s = try service()
        let d = try resolve(disk, in: await s.list())
        if markChecked && !readOnly {
            FileHandle.standardError.write(Data(("unlatch: warning: this repairs nothing; writing to a disk marked dirty can damage "
                + "files on it. Ejecting it cleanly clears the mark; Windows will no longer offer to check it.\n").utf8))
        }
        let r: MountService.Result
        do { r = try await s.mount(d.bsdName, readOnly: readOnly, markChecked: markChecked) } catch {
            if case MountError.daemonFailed(let why) = error, why.contains("corrupt") {
                // Often only $MFTMirr behind $MFT after a cut-off write, which Check Disk repairs (D-98).
                FileHandle.standardError.write(Data(
                    "unlatch: to check it and repair what is safe: unlatch check --repair \(d.bsdName)\n".utf8))
            }
            throw fail(error.localizedDescription, Exit.of(error))
        }
        let forced = r.readOnly && !readOnly
        if json {
            print(JSONOut.encode(JSONOut.Mounted(bsdName: d.bsdName, mountPoint: r.mountPoint, readOnly: r.readOnly,
                                                 state: r.status?.state.rawValue, reason: r.status?.reason)))
        } else {
            print("Mounted \(d.bsdName) on \(r.mountPoint) \(r.readOnly ? "read-only" : "read-write").")
        }
        if forced {
            fflush(stdout)
            let why = r.status.map { "\($0.state.rawValue): \($0.reason)" } ?? "the volume could not be opened for writing"
            FileHandle.standardError.write(Data("unlatch: read-only because \(why)\n".utf8))
            if r.status?.state == .dirty && !markChecked {
                FileHandle.standardError.write(Data(("unlatch: to check it and enable read-write: unlatch check --repair \(d.bsdName)\n"
                    + "unlatch: to enable read-write without checking: unlatch mount --mark-checked \(d.bsdName)\n").utf8))
            }
            throw CLIExit(code: .readOnly)
        }
    }
}

struct Unmount: AsyncParsableCommand {
    static let configuration = CommandConfiguration(abstract: "Unmount a volume (whoever mounted it).")
    @Argument(help: "The volume.") var disk: String?
    @Flag(help: "Unmount every volume Unlatch has mounted (before an update or uninstall).") var all = false
    @Flag(help: "Unmount even if files are open.") var force = false

    func validate() throws {
        if all == (disk != nil) { throw ValidationError("Give a volume or --all.") }
    }

    func run() async throws {
        let s = try service()
        let disks = await s.list()
        if all {
            var failed = false
            for d in disks where d.owner == .unlatch {
                do {
                    try await s.unmount(d.bsdName, force: force)
                    print("Unmounted \(d.bsdName).")
                } catch {
                    _ = fail("\(d.bsdName): \(error.localizedDescription)")
                    failed = true
                }
            }
            if failed { throw CLIExit(code: .error) }
            return
        }
        let d = try resolve(disk!, in: disks)
        do { try await s.unmount(d.bsdName, force: force) } catch { throw fail(error.localizedDescription) }
        print("Unmounted \(d.bsdName).")
    }
}

struct Eject: AsyncParsableCommand {
    static let configuration = CommandConfiguration(abstract: "Unmount and eject the disk holding a volume.")
    @Argument(help: "The volume.") var disk: String
    @Flag(help: "Unmount even if files are open.") var force = false

    func run() async throws {
        let s = try service()
        let d = try resolve(disk, in: await s.list())
        do { try await s.eject(d.bsdName, force: force) } catch { throw fail(error.localizedDescription) }
        print("Ejected \(d.wholeDisk ?? d.bsdName).")
    }
}

struct Status: AsyncParsableCommand {
    static let configuration = CommandConfiguration(abstract: "Show Unlatch's verdict on volumes (clean, dirty, hibernated, ...).")
    @Argument(help: "The volume; all NTFS volumes if omitted.") var disk: String?
    @Flag(help: "JSON output (schema 1).") var json = false

    func run() async throws {
        let s = try service()
        let all = await s.list()
        let disks = try disk.map { [try resolve($0, in: all)] } ?? all
        let policy = AutoMountPolicy()
        var vols: [JSONOut.Volume] = []
        for d in disks { vols.append(JSONOut.volume(d, status: await s.status(d.bsdName), policy: policy)) }
        if json {
            print(JSONOut.encode(JSONOut.Volumes(volumes: vols)))
            return
        }
        for v in vols {
            print("\(v.bsdName) (\(v.label.isEmpty ? "no label" : v.label)): \(v.owner)\(v.mountPoint.map { " on \($0)" } ?? "")")
            if let st = v.status {
                print("  \(st.state)\(st.readOnly ? ", read-only" : "")\(st.reason.isEmpty ? "" : ": \(st.reason)") (\(st.timestamp))")
            } else {
                print("  not checked by Unlatch yet")
            }
        }
    }
}

struct Check: AsyncParsableCommand {
    static let configuration = CommandConfiguration(
        abstract: "Check a volume for damage (Check Disk), and repair what is safe to repair.",
        discussion: """
        The volume is unmounted first and mounted again afterwards. The check reads only. --repair sets the \
        bitmap bits of records and clusters in use (nothing is overwritten), removes folder entries that point \
        at no file and puts files no folder lists back in their folder (or in /found.000), as a disk unplugged \
        mid-copy leaves them, when that is all that is wrong, and clears the volume's dirty mark when nothing is \
        left. A volume that cannot be opened because the file table's backup copy ($MFTMirr) differs is first \
        brought up to date from the file table, then checked. Anything else needs Windows' chkdsk.
        """)
    @Argument(help: "The volume.") var disk: String
    @Flag(help: "Repair what is safe to repair, and clear the dirty mark when nothing is left.") var repair = false
    @Flag(help: "JSON output (schema 1).") var json = false
    @Flag(help: "Leave the volume unmounted afterwards.") var noRemount = false

    func run() async throws {
        let s = try service()
        let d = try resolve(disk, in: await s.list())
        let r: CheckReport
        do { r = try await s.check(d.bsdName, repair: repair, remount: !noRemount) } catch {
            throw fail(error.localizedDescription, Exit.of(error))
        }
        let left: Exit = r.remaining > 0 ? .problems : .success
        if json {
            print(JSONOut.encode(JSONOut.Checked(bsdName: d.bsdName, verdict: JSONOut.verdict(r.verdict), advice: r.advice,
                                                 report: r)))
            if left != .success { throw CLIExit(code: left) }
            return
        }
        let name = d.label.isEmpty ? d.bsdName : "\(d.label) (\(d.bsdName))"
        print("Checked \(name): \(r.recordsInUse) records in use, \(r.directories) folders, \(r.entries) folder entries.")
        if r.found == 0 {
            print("No problems found.")
        } else {
            print("Found \(r.found) problem\(r.found == 1 ? "" : "s"):")
            for k in r.kinds { print("  \(CheckReport.describe(k.kind, count: k.count))") }
            let shown = 20
            for p in r.problems.prefix(shown) {
                print("    \(p.path.isEmpty ? "record \(p.record)" : p.path): \(p.detail)")
            }
            if r.problems.count > shown { print("    ... and \(r.problems.count - shown) more (--json lists them all)") }
        }
        if r.repairRan {
            let removed = r.entriesRemoved ?? 0, relinked = r.orphansRelinked ?? 0, moved = r.orphansFound ?? 0
            print("Repaired \(r.repaired) bitmap bit\(r.repaired == 1 ? "" : "s"); removed \(removed) folder "
                  + "entr\(removed == 1 ? "y" : "ies"); put back \(relinked) file\(relinked == 1 ? "" : "s") no folder listed"
                  + (moved > 0 ? " (\(moved) in /found.000)" : "") + (r.mirrorRepaired == true ? "; updated $MFTMirr" : "")
                  + ".")
        }
        print(r.advice)
        if r.verdict == .repairable && !repair { print("To repair: unlatch check --repair \(d.bsdName)") }
        if r.lostClusters > 0 || r.lostRecords > 0 {
            print("(\(r.lostClusters) clusters and \(r.lostRecords) records are marked used but belong to nothing: space only.)")
        }
        if left != .success { throw CLIExit(code: left) }
    }
}
