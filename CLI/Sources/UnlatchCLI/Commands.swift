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
        4 macFUSE missing or not allowed. JSON output is versioned ("schema": 1); see docs/cli.md.
        """,
        version: Unlatch.version,
        subcommands: [List.self, MountCmd.self, Unmount.self, Eject.self, Status.self, Doctor.self,
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
    @Flag(help: "JSON output (schema 1).") var json = false

    func run() async throws {
        let s = try service()
        let d = try resolve(disk, in: await s.list())
        let r: MountService.Result
        do { r = try await s.mount(d.bsdName, readOnly: readOnly) } catch { throw fail(error.localizedDescription, Exit.of(error)) }
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
