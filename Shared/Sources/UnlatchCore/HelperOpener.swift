// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation

/// Asks the privileged helper (`Helper/main.c`, D-57) to open a device node and hand the
/// descriptor back over its Unix socket.
public struct HelperOpener: DeviceOpener {
    public static let label = "\(Unlatch.bundlePrefix).unlatch.helper"
    public static let socketPath = "/var/run/\(label).sock"
    public static var installed: Bool { FileManager.default.fileExists(atPath: socketPath) }

    public init() {}

    public func open(_ path: String, writable: Bool) throws -> Int32 {
        let bsd = path.hasPrefix("/dev/") ? String(path.dropFirst(5)) : path
        let s = socket(AF_UNIX, SOCK_STREAM, 0)
        guard s >= 0 else { throw MountError.needsHelper(path) }
        defer { close(s) }
        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        withUnsafeMutableBytes(of: &addr.sun_path) { buf in
            Self.socketPath.utf8CString.withUnsafeBytes { buf.copyBytes(from: $0.prefix(buf.count - 1)) }
        }
        let rc = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { connect(s, $0, socklen_t(MemoryLayout<sockaddr_un>.size)) }
        }
        guard rc == 0 else { throw MountError.needsHelper(path) }
        let req = "open \(bsd) \(writable ? "rw" : "ro")\n"
        guard req.withCString({ write(s, $0, strlen($0)) }) > 0 else { throw MountError.needsHelper(path) }

        var line = [UInt8](repeating: 0, count: 300)
        var control = [UInt8](repeating: 0, count: 64)
        var fd: Int32 = -1
        let n = line.withUnsafeMutableBytes { lb in
            control.withUnsafeMutableBytes { cb -> Int in
                var iov = iovec(iov_base: lb.baseAddress, iov_len: lb.count)
                return withUnsafeMutablePointer(to: &iov) { iovp in
                    var m = msghdr(msg_name: nil, msg_namelen: 0, msg_iov: iovp, msg_iovlen: 1,
                                   msg_control: cb.baseAddress, msg_controllen: socklen_t(cb.count), msg_flags: 0)
                    let r = recvmsg(s, &m, 0)
                    if r > 0, m.msg_controllen >= MemoryLayout<cmsghdr>.size {
                        let c = cb.baseAddress!.assumingMemoryBound(to: cmsghdr.self)
                        if c.pointee.cmsg_level == SOL_SOCKET && c.pointee.cmsg_type == SCM_RIGHTS {
                            // CMSG_DATA: the descriptor follows the header, aligned to 4 bytes.
                            let off = (MemoryLayout<cmsghdr>.size + 3) & ~3
                            fd = cb.load(fromByteOffset: off, as: Int32.self)
                        }
                    }
                    return r
                }
            }
        }
        let text = String(decoding: line.prefix(max(n, 0)), as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
        if text == "ok", fd >= 0 { return fd }
        if fd >= 0 { close(fd) }
        let parts = text.split(separator: " ", maxSplits: 2)
        let code = parts.count > 1 ? Int32(parts[1]) ?? EIO : EIO
        let msg = parts.count > 2 ? String(parts[2]) : "the helper did not answer"
        switch code {
        case EBUSY: throw MountError.busy(path)
        case EPERM where msg.contains("NTFS"): throw MountError.notNTFS(bsd)
        default: throw MountError.refused("The Unlatch helper refused \(path): \(msg).")
        }
    }
}

/// Opens directly when the node is the user's (attached images), else through the helper.
public struct AutoOpener: DeviceOpener {
    public init() {}

    public func open(_ path: String, writable: Bool) throws -> Int32 {
        do {
            return try DirectOpener().open(path, writable: writable)
        } catch MountError.needsHelper(let p) where HelperOpener.installed {
            return try HelperOpener().open(p, writable: writable)
        }
    }
}
