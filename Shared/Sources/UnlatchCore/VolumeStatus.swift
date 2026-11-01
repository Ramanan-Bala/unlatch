// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation

/// The mount daemon's verdict on one NTFS volume, shared with the app and CLI through
/// `~/Library/Application Support/Unlatch/status/<serial>.json` (D-41). Written at probe,
/// mount and unmount; `Fuse/status.c` writes the same JSON.
public struct VolumeStatus: Codable, Equatable, Sendable {
    public enum State: String, Codable, Sendable {
        case clean, dirty, hibernated, uncleanLog = "unclean_log", corrupt, notNTFS = "not_ntfs"
    }

    public var bsdName: String
    public var serial: String          // 16 hex digits
    public var label: String
    public var state: State
    public var reason: String
    public var readOnly: Bool
    public var mounted: Bool
    public var engineABI: Int
    public var unlatchVersion: String
    public var timestamp: Date
    /// The daemon's process ID while mounted (absent in records from before Step 17).
    public var pid: Int32?

    public init(bsdName: String, serial: String, label: String, state: State, reason: String,
                readOnly: Bool, mounted: Bool, engineABI: Int, timestamp: Date = Date()) {
        self.bsdName = bsdName
        self.serial = serial
        self.label = label
        self.state = state
        self.reason = reason
        self.readOnly = readOnly
        self.mounted = mounted
        self.engineABI = engineABI
        self.unlatchVersion = Unlatch.version
        self.timestamp = timestamp
    }

    /// `~/Library/Application Support/Unlatch/status`.
    public static func directory() -> URL? {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first?
            .appendingPathComponent("Unlatch/status", isDirectory: true)
    }

    /// Writes this record atomically to `status/<serial>.json`.
    public func write() throws {
        guard let dir = Self.directory() else {
            throw CocoaError(.fileWriteNoPermission)
        }
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        encoder.dateEncodingStrategy = .iso8601
        try encoder.encode(self).write(to: dir.appendingPathComponent("\(serial).json"), options: .atomic)
    }
}
