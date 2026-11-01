// SPDX-License-Identifier: GPL-3.0-or-later
import DiskArbitration
import Foundation

/// Opens a volume's device node for the daemon (D-57). Disk images attached by the user are
/// theirs and open directly; real disks are root:operator 0640, so the privileged helper
/// opens those and hands the descriptor back (Step 13).
public protocol DeviceOpener: Sendable {
    /// Returns an open descriptor the caller owns, or throws `MountError.needsHelper`.
    func open(_ path: String, writable: Bool) throws -> Int32
}

/// Opens the node itself; real disks need the helper instead.
public struct DirectOpener: DeviceOpener {
    public init() {}

    public func open(_ path: String, writable: Bool) throws -> Int32 {
        let fd = Darwin.open(path, writable ? O_RDWR : O_RDONLY)
        if fd >= 0 { return fd }
        if errno == EACCES || errno == EPERM { throw MountError.needsHelper(path) }
        if errno == EBUSY { throw MountError.busy(path) }
        throw MountError.daemonFailed("\(path): \(String(cString: strerror(errno)))")
    }
}

/// Detects, mounts, unmounts and ejects NTFS volumes for the CLI and the app (plan Step 12).
public actor MountService {
    public struct Result: Sendable {
        public let mountPoint: String
        public let readOnly: Bool
        public let status: VolumeStatus?
    }

    private let daemon: URL
    private let opener: DeviceOpener
    private let statusStore: StatusStore
    // DASession is not Sendable; it is only used from this actor and its own queue.
    nonisolated(unsafe) private let session: DASession
    private let queue = DispatchQueue(label: "\(Unlatch.bundlePrefix).unlatch.mountservice")
    private let log = Unlatch.logger("mount")

    /// `daemon` is the `unlatch-fuse` binary; by default the one next to this executable.
    public init(daemon: URL? = nil, opener: DeviceOpener = AutoOpener(), statusStore: StatusStore = StatusStore()) throws {
        guard let s = DASessionCreate(kCFAllocatorDefault) else { throw MountError.refused("DiskArbitration is unavailable.") }
        session = s
        DASessionSetDispatchQueue(s, queue)
        self.daemon = daemon ?? Self.defaultDaemon()
        self.opener = opener
        self.statusStore = statusStore
    }

    deinit { DASessionSetDispatchQueue(session, nil) }

    static func defaultDaemon() -> URL {
        if let env = ProcessInfo.processInfo.environment["UNLATCH_FUSE"] { return URL(fileURLWithPath: env) }
        return Unlatch.executableDirectory.appendingPathComponent("unlatch-fuse")
    }

    public static var macFUSEInstalled: Bool { FileManager.default.fileExists(atPath: "/Library/Filesystems/macfuse.fs") }

    // MARK: listing

    /// Every NTFS volume DiskArbitration knows, with who has it mounted. Mounts left behind
    /// by a daemon that died are cleaned up first (`reapDeadMounts`).
    public func list() -> [DiskInfo] {
        reapDeadMounts()
        let mounts = Mount.table()
        return DA.leafBSDNames().compactMap { name in
            DADiskCreateFromBSDName(kCFAllocatorDefault, session, name).flatMap { DA.info($0, mounts: mounts) }
        }
    }

    /// Force-unmounts Unlatch mounts whose daemon is gone (killed or crashed): macFUSE keeps
    /// such a mount listed, and every directory read on it fails with ENXIO (D-68). The daemon
    /// records its pid in the status record; a mount without one is left alone.
    @discardableResult
    public func reapDeadMounts() -> [String] {
        var reaped: [String] = []
        for m in Mount.table() where m.type == "macfuse" && m.from.hasPrefix("unlatch@") {
            let bsd = String(m.from.dropFirst("unlatch@".count))
            // Records claiming this disk is mounted (timestamps have 1 s resolution, so a record
            // of an earlier volume on the same diskN can tie with the current one).
            let recs = statusStore.all().filter { $0.bsdName == bsd && $0.mounted && $0.pid != nil }
            let dead = { (r: VolumeStatus) in kill(r.pid!, 0) != 0 && errno == ESRCH }
            guard !recs.isEmpty, recs.allSatisfy(dead) else { continue }
            if Darwin.unmount(m.on, MNT_FORCE) == 0 {
                reaped.append(m.on)
                for var r in recs {
                    r.mounted = false
                    try? r.write()
                }
                Unlatch.logger("mount").notice("removed \(m.on, privacy: .public): its daemon is gone")
            }
        }
        return reaped
    }

    public func disk(_ bsdName: String) throws -> DiskInfo {
        let name = bsdName.hasPrefix("/dev/") ? String(bsdName.dropFirst(5)) : bsdName
        guard let d = DADiskCreateFromBSDName(kCFAllocatorDefault, session, name) else { throw MountError.notFound(name) }
        guard let info = DA.info(d, mounts: Mount.table()) else {
            throw DADiskCopyDescription(d) == nil ? MountError.notFound(name) : MountError.notNTFS(name)
        }
        return info
    }

    public func status(_ bsdName: String) -> VolumeStatus? { statusStore.latest(bsdName: bsdName) }

    // MARK: mount

    /// Mounts with unlatch-fuse under /Volumes. If Apple's driver had the volume, it is
    /// unmounted first and mounted again when Unlatch fails, so the user never loses access.
    @discardableResult
    public func mount(_ bsdName: String, readOnly: Bool = false) async throws -> Result {
        guard Self.macFUSEInstalled else { throw MountError.macFUSEMissing }
        var info = try disk(bsdName)
        if info.owner == .unlatch, let p = info.mountPoint {
            return Result(mountPoint: p, readOnly: readOnly, status: status(info.bsdName))
        }
        let hadApple = info.owner == .apple
        if hadApple || info.owner != .none {
            try await daUnmount(info.bsdName, force: false)
            info.owner = .none
        }
        do {
            // The raw node (/dev/rdiskN): unlatch-fuse caches in front of it itself (D-62).
            let fd = try opener.open(Self.rawPath(info.devicePath), writable: !readOnly)
            defer { close(fd) }
            let point = Self.mountPoint(for: info)
            try runDaemon(fd: fd, device: info.devicePath, mountPoint: point, readOnly: readOnly)
            let st = status(info.bsdName)
            log.notice("mounted \(info.bsdName, privacy: .public) on \(point, privacy: .public)")
            return Result(mountPoint: point, readOnly: st?.readOnly ?? readOnly, status: st)
        } catch {
            if hadApple {
                log.notice("giving \(info.bsdName, privacy: .public) back to Apple's driver after: \(error.localizedDescription, privacy: .public)")
                try? await daMount(info.bsdName)
            }
            throw error
        }
    }

    /// /Volumes/<label>, or /Volumes/<label> <bsd> when the name is taken.
    /// `/dev/diskN` → `/dev/rdiskN`; other paths unchanged.
    static func rawPath(_ path: String) -> String {
        path.hasPrefix("/dev/disk") ? "/dev/r" + path.dropFirst(5) : path
    }

    static func mountPoint(for info: DiskInfo) -> String {
        let base = (info.label.isEmpty ? "NTFS" : info.label).replacingOccurrences(of: "/", with: ":")
        let first = "/Volumes/\(base)"
        return FileManager.default.fileExists(atPath: first) ? "/Volumes/\(base) \(info.bsdName)" : first
    }

    /// Runs `unlatch-fuse --fd 3 [-r] <device> <mountpoint>`; it returns once mounted.
    private func runDaemon(fd: Int32, device: String, mountPoint: String, readOnly: Bool) throws {
        var args = [daemon.path, "--fd", "3"]
        if readOnly { args.append("-r") }
        args += [device, mountPoint]
        var errPipe: [Int32] = [0, 0]
        guard pipe(&errPipe) == 0 else { throw MountError.daemonFailed("pipe: \(String(cString: strerror(errno)))") }
        // Move every descriptor the spawn juggles above 9 first. Otherwise one of them can be
        // 2 or 3 itself (the helper's socket just freed fd 3, and the pipe took it), and the
        // dup2/close actions below would close the device they had just put on fd 3.
        let dev = fcntl(fd, F_DUPFD_CLOEXEC, 10)
        let errRead = fcntl(errPipe[0], F_DUPFD_CLOEXEC, 10)
        let errWrite = fcntl(errPipe[1], F_DUPFD_CLOEXEC, 10)
        close(errPipe[0])
        close(errPipe[1])
        defer { close(dev) }
        guard dev >= 0, errRead >= 0, errWrite >= 0 else {
            throw MountError.daemonFailed("descriptors: \(String(cString: strerror(errno)))")
        }
        var actions: posix_spawn_file_actions_t?
        posix_spawn_file_actions_init(&actions)
        defer { posix_spawn_file_actions_destroy(&actions) }
        // dup2 clears close-on-exec on the copies, so only fds 2 and 3 reach the daemon
        // (plus 0 and 1); a stray copy of the pipe's write end would keep the read below open.
        posix_spawn_file_actions_adddup2(&actions, dev, 3)
        posix_spawn_file_actions_adddup2(&actions, errWrite, 2)
        var pid: pid_t = 0
        let cargs = args.map { strdup($0) } + [nil]
        defer { cargs.forEach { free($0) } }
        let rc = posix_spawn(&pid, daemon.path, &actions, nil, cargs, environ)
        close(errWrite)
        guard rc == 0 else {
            close(errRead)
            throw MountError.daemonFailed("\(daemon.path): \(String(cString: strerror(rc)))")
        }
        let errText = String(decoding: FileHandle(fileDescriptor: errRead, closeOnDealloc: true).readDataToEndOfFile(),
                             as: UTF8.self)
        var st: Int32 = 0
        waitpid(pid, &st, 0)
        guard (st & 0x7f) == 0 && ((st >> 8) & 0xff) == 0 else {
            let why = errText.split(separator: "\n").last.map { $0.replacingOccurrences(of: "unlatch-fuse: ", with: "") }
            throw MountError.daemonFailed(why ?? "unlatch-fuse exited with status \(st)")
        }
    }

    // MARK: unmount / eject

    /// Unmounts whoever has the volume mounted. `force` only on explicit request.
    public func unmount(_ bsdName: String, force: Bool = false) async throws {
        let info = try disk(bsdName)
        switch info.owner {
        case .none:
            return
        case .unlatch:
            guard let p = info.mountPoint else { return }
            // Right after heavy writes, a system indexer can hold the volume for a moment
            // (EBUSY with nothing in lsof); retry for up to 3 s before reporting busy.
            var tries = 0
            while Darwin.unmount(p, force ? MNT_FORCE : 0) != 0 {
                let err = errno
                if err == EBUSY && tries < 12 {
                    tries += 1
                    try await Task.sleep(nanoseconds: 250_000_000)
                    continue
                }
                throw err == EBUSY ? MountError.busy(p) : MountError.unmountFailed(p, err)
            }
        default:
            try await daUnmount(info.bsdName, force: force)
        }
    }

    /// Unmounts, then ejects the whole disk.
    public func eject(_ bsdName: String, force: Bool = false) async throws {
        let info = try disk(bsdName)
        try await unmount(info.bsdName, force: force)
        guard let whole = DADiskCreateFromBSDName(kCFAllocatorDefault, session, info.wholeDisk ?? info.bsdName) else {
            throw MountError.notFound(info.bsdName)
        }
        // Other volumes on the same disk go too, as with Finder's eject.
        try await withDACallback { box in
            DADiskUnmount(whole, DADiskUnmountOptions(kDADiskUnmountOptionWhole), daDone, box)
        }
        try await withDACallback { box in
            DADiskEject(whole, DADiskEjectOptions(kDADiskEjectOptionDefault), daDone, box)
        }
    }

    // MARK: DiskArbitration with async/await

    private func daUnmount(_ bsdName: String, force: Bool) async throws {
        guard let d = DADiskCreateFromBSDName(kCFAllocatorDefault, session, bsdName) else { throw MountError.notFound(bsdName) }
        let opts = force ? kDADiskUnmountOptionForce : kDADiskUnmountOptionDefault
        try await withDACallback { box in DADiskUnmount(d, DADiskUnmountOptions(opts), daDone, box) }
    }

    private func daMount(_ bsdName: String) async throws {
        guard let d = DADiskCreateFromBSDName(kCFAllocatorDefault, session, bsdName) else { throw MountError.notFound(bsdName) }
        try await withDACallback { box in DADiskMount(d, nil, DADiskMountOptions(kDADiskMountOptionDefault), daDone, box) }
    }

    private func withDACallback(_ start: (UnsafeMutableRawPointer) -> Void) async throws {
        try await withCheckedThrowingContinuation { (c: CheckedContinuation<Void, Error>) in
            start(Unmanaged.passRetained(DABox(c)).toOpaque())
        }
    }
}

/// Carries a continuation through DiskArbitration's C callback context.
private final class DABox: @unchecked Sendable {
    let continuation: CheckedContinuation<Void, Error>
    init(_ c: CheckedContinuation<Void, Error>) { continuation = c }
}

private let daDone: DADiskUnmountCallback = { disk, dissenter, ctx in
    let box = Unmanaged<DABox>.fromOpaque(ctx!).takeRetainedValue()
    guard let dissenter else {
        box.continuation.resume()
        return
    }
    let name = DADiskGetBSDName(disk).map { String(cString: $0) } ?? "disk"
    let status = DADissenterGetStatus(dissenter)
    let why = DADissenterGetStatusString(dissenter).map { $0 as String } ?? String(format: "DiskArbitration status 0x%x", status)
    box.continuation.resume(throwing: status == kDAReturnBusy ? MountError.busy(name) : MountError.ejectFailed(name, why))
}
