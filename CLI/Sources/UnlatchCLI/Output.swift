// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation
import UnlatchCore

/// Exit codes (docs/cli.md). Fixed: scripts depend on them.
public enum Exit: Int32, Sendable {
    case success = 0
    case error = 1
    case usage = 2
    case readOnly = 3          // mounted, but forced read-only (dirty, hibernated, ...)
    case macFUSE = 4           // macFUSE missing or too old
    case problems = 5          // check: problems are left on the volume

    static func of(_ error: Error) -> Exit {
        if case MountError.macFUSEMissing = error { return .macFUSE }
        return .error
    }
}

/// JSON output, schema 1. Field names and meanings never change within a schema version;
/// new fields may be added.
public enum JSONOut {
    public static let schema = 1

    struct Volume: Codable, Equatable {
        let bsdName: String
        let devicePath: String
        let label: String
        let size: Int64
        let volumeUUID: String?
        let `internal`: Bool
        let removable: Bool
        let ejectable: Bool
        let owner: String           // none | apple | unlatch | other:<fstype>
        let mountPoint: String?
        let autoMount: Bool
        let status: Status?
    }

    struct Status: Codable, Equatable {
        let state: String
        let reason: String
        let readOnly: Bool
        let mounted: Bool
        let serial: String
        let timestamp: String
    }

    struct Volumes: Codable, Equatable {
        var schema = JSONOut.schema
        let volumes: [Volume]
    }

    struct Mounted: Codable, Equatable {
        var schema = JSONOut.schema
        let bsdName: String
        let mountPoint: String
        let readOnly: Bool
        let state: String?
        let reason: String?
    }

    struct Checked: Encodable {
        var schema = JSONOut.schema
        let bsdName: String
        let verdict: String         // clean | repaired | repairable | needs_windows
        let advice: String
        let report: CheckReport
    }

    static func verdict(_ v: CheckReport.Verdict) -> String {
        switch v {
        case .clean: "clean"
        case .repaired: "repaired"
        case .repairable: "repairable"
        case .needsWindows: "needs_windows"
        }
    }

    static func owner(_ o: DiskInfo.Owner) -> String {
        switch o {
        case .none: "none"
        case .apple: "apple"
        case .unlatch: "unlatch"
        case .other(let t): "other:\(t)"
        }
    }

    static func status(_ s: VolumeStatus?) -> Status? {
        s.map {
            Status(state: $0.state.rawValue, reason: $0.reason, readOnly: $0.readOnly, mounted: $0.mounted,
                   serial: $0.serial, timestamp: ISO8601DateFormatter().string(from: $0.timestamp))
        }
    }

    static func volume(_ d: DiskInfo, status s: VolumeStatus?, policy: AutoMountPolicy) -> Volume {
        Volume(bsdName: d.bsdName, devicePath: d.devicePath, label: d.label, size: d.size, volumeUUID: d.volumeUUID,
               internal: d.isInternal, removable: d.isRemovable, ejectable: d.isEjectable, owner: owner(d.owner),
               mountPoint: d.mountPoint, autoMount: policy.shouldAutoMount(d), status: status(s))
    }

    static func encode<T: Encodable>(_ v: T) -> String {
        let e = JSONEncoder()
        e.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
        return String(decoding: (try? e.encode(v)) ?? Data("{}".utf8), as: UTF8.self)
    }
}

/// Human-readable sizes, decimal like Finder.
func humanSize(_ bytes: Int64) -> String {
    ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file)
}
