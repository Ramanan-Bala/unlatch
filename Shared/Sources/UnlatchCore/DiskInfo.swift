// SPDX-License-Identifier: GPL-3.0-or-later
import DiskArbitration
import Foundation
import IOKit

/// One NTFS volume as DiskArbitration and the mount table see it.
public struct DiskInfo: Sendable, Equatable, Identifiable {
    /// Who has the volume mounted.
    public enum Owner: Sendable, Equatable {
        case none
        case apple                 // Apple's read-only NTFS driver
        case unlatch               // unlatch-fuse (fsname "unlatch@<bsd>")
        case other(String)         // another driver, by f_fstypename
    }

    public var id: String { bsdName }
    public let bsdName: String
    public let wholeDisk: String?
    public let label: String
    public let size: Int64
    public let volumeUUID: String?
    public let isInternal: Bool
    public let isRemovable: Bool
    public let isEjectable: Bool
    public var owner: Owner
    public var mountPoint: String?

    public var devicePath: String { "/dev/\(bsdName)" }
}

/// Microsoft Basic Data GPT type and the MBR "Windows_NTFS" content hint: candidates when
/// DiskArbitration has no volume kind (an unrecognised or damaged volume).
private let ntfsContentHints: Set<String> = ["Windows_NTFS", "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"]

enum DA {
    /// The description of a disk, if it is an NTFS volume (or looks like one).
    static func ntfsDescription(_ disk: DADisk) -> [String: Any]? {
        guard let d = DADiskCopyDescription(disk) as? [String: Any] else { return nil }
        let kind = d[kDADiskDescriptionVolumeKindKey as String] as? String
        let content = d[kDADiskDescriptionMediaContentKey as String] as? String
        if kind == "ntfs" || (kind == nil && content.map(ntfsContentHints.contains) == true) {
            return d
        }
        return nil
    }

    static func info(_ disk: DADisk, mounts: [Mount]) -> DiskInfo? {
        guard let d = ntfsDescription(disk), let bsd = d[kDADiskDescriptionMediaBSDNameKey as String] as? String else {
            return nil
        }
        // Copy the name while the whole-disk object is alive: DADiskGetBSDName points into it.
        let whole = DADiskCopyWholeDisk(disk).flatMap { w in DADiskGetBSDName(w).map { String(cString: $0) } }
        let uuid = (d[kDADiskDescriptionVolumeUUIDKey as String]).map { CFUUIDCreateString(nil, ($0 as! CFUUID)) as String }
        var owner = DiskInfo.Owner.none
        var point: String?
        if let m = mounts.first(where: { $0.from == "unlatch@\(bsd)" || $0.from == "unlatch@r\(bsd)" }) {
            owner = .unlatch
            point = m.on
        } else if let m = mounts.first(where: { $0.from == "/dev/\(bsd)" }) {
            owner = m.type == "ntfs" ? .apple : .other(m.type)
            point = m.on
        }
        return DiskInfo(
            bsdName: bsd, wholeDisk: whole,
            label: d[kDADiskDescriptionVolumeNameKey as String] as? String ?? "",
            size: (d[kDADiskDescriptionMediaSizeKey as String] as? NSNumber)?.int64Value ?? 0,
            volumeUUID: uuid,
            isInternal: d[kDADiskDescriptionDeviceInternalKey as String] as? Bool ?? false,
            isRemovable: d[kDADiskDescriptionMediaRemovableKey as String] as? Bool ?? false,
            isEjectable: d[kDADiskDescriptionMediaEjectableKey as String] as? Bool ?? false,
            owner: owner, mountPoint: point)
    }

    /// Every leaf IOMedia's BSD name (partitions and unpartitioned disks).
    static func leafBSDNames() -> [String] {
        var iter: io_iterator_t = 0
        guard let match = IOServiceMatching("IOMedia") as NSMutableDictionary? else { return [] }
        match["Leaf"] = true
        guard IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter) == KERN_SUCCESS else { return [] }
        defer { IOObjectRelease(iter) }
        var names: [String] = []
        while case let entry = IOIteratorNext(iter), entry != 0 {
            if let n = IORegistryEntryCreateCFProperty(entry, "BSD Name" as CFString, kCFAllocatorDefault, 0)?
                .takeRetainedValue() as? String {
                names.append(n)
            }
            IOObjectRelease(entry)
        }
        return names.sorted()
    }
}

/// One line of the mount table.
struct Mount: Sendable {
    let from: String
    let on: String
    let type: String

    /// The mount table, with Unlatch's FSKit mounts named like its kext mounts. The kext shows
    /// unlatch-fuse's fsname, "unlatch@<bsd>"; FSKit shows "macfuse://<uuid>", so those are
    /// matched to the newest mounted status record with the same mount point (D-78).
    static func table(status: StatusStore = StatusStore()) -> [Mount] {
        let raw = rawTable()
        guard raw.contains(where: { $0.type == "macfuse" && !$0.from.hasPrefix("unlatch@") }) else { return raw }
        let records = status.all().filter { $0.mounted && $0.mountPoint != nil }
        return raw.map { m in
            guard m.type == "macfuse", !m.from.hasPrefix("unlatch@"),
                  let r = records.first(where: { $0.mountPoint == m.on }) else { return m }
            return Mount(from: "unlatch@\(r.bsdName)", on: m.on, type: m.type)
        }
    }

    static func rawTable() -> [Mount] {
        var buf: UnsafeMutablePointer<statfs>?
        let n = getmntinfo(&buf, MNT_NOWAIT)
        guard n > 0, let buf else { return [] }
        return (0..<Int(n)).map { i in
            var s = buf[i]
            let from = withUnsafeBytes(of: &s.f_mntfromname) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
            let on = withUnsafeBytes(of: &s.f_mntonname) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
            let type = withUnsafeBytes(of: &s.f_fstypename) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
            return Mount(from: from, on: on, type: type)
        }
    }
}
