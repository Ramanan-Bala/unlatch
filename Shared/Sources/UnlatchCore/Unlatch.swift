// SPDX-License-Identifier: GPL-3.0-or-later
import os

/// Fixed identifiers shared by the app, extension and CLI.
public enum Unlatch {
    public static let version = "0.0.0"
    public static let bundlePrefix = "io.github.ramanan-bala"
    public static let appGroup = "group.\(bundlePrefix).Unlatch"
    public static let logSubsystem = "\(bundlePrefix).unlatch"

    /// Logger for one of the categories `engine`, `io`, `mount`, `app`, `cli`.
    public static func logger(_ category: String) -> Logger {
        Logger(subsystem: logSubsystem, category: category)
    }
}
