// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation

/// Which NTFS volumes Unlatch takes over when they appear (plan Step 12 task 4).
///
/// By default every external, removable or ejectable volume; never an internal one unless
/// allowed explicitly. Per-volume choices are keyed by the volume UUID DiskArbitration
/// derives from the NTFS serial, and kept in the app's defaults (no App Group, D-41).
public struct AutoMountPolicy: Sendable {
    public enum Choice: String, Sendable { case allow, deny }

    private let suite: String

    public init(suite: String = "\(Unlatch.bundlePrefix).Unlatch") {
        self.suite = suite
    }

    private var defaults: UserDefaults { UserDefaults(suiteName: suite) ?? .standard }
    private func key(_ volumeUUID: String) -> String { "automount.\(volumeUUID)" }

    public func choice(for volumeUUID: String) -> Choice? {
        defaults.string(forKey: key(volumeUUID)).flatMap(Choice.init(rawValue:))
    }

    public func set(_ choice: Choice?, for volumeUUID: String) {
        defaults.set(choice?.rawValue, forKey: key(volumeUUID))
    }

    /// True when Unlatch should mount this volume (and keep Apple's driver off it).
    public func shouldAutoMount(_ disk: DiskInfo) -> Bool {
        if let uuid = disk.volumeUUID, let c = choice(for: uuid) {
            return c == .allow
        }
        return !disk.isInternal || disk.isRemovable || disk.isEjectable
    }
}
