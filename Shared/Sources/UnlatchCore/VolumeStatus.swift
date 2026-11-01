// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation

/// The extension's verdict on one NTFS volume, shared with the app and CLI through the
/// App Group container (`status/<serial>.json`). Written at check, mount and unmount.
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

    /// `<App Group container>/status`, or nil when the App Group entitlement is missing.
    public static func directory() -> URL? {
        FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: Unlatch.appGroup)?
            .appendingPathComponent("status", isDirectory: true)
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
