// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation
import os

/// Fixed identifiers shared by the app, extension and CLI.
public enum Unlatch {
    public static let version = "1.0.0-beta.1"
    public static let bundlePrefix = "io.github.ramanan-bala"
    public static let appGroup = "group.\(bundlePrefix).Unlatch"
    public static let logSubsystem = "\(bundlePrefix).unlatch"

    /// The directory of the running executable, symlinks resolved. `argv[0]` is only the
    /// command name when it is run from `PATH` (a Homebrew install links bin/unlatch).
    public static var executableDirectory: URL {
        let exe = Bundle.main.executableURL ?? URL(fileURLWithPath: CommandLine.arguments[0])
        return exe.resolvingSymlinksInPath().deletingLastPathComponent()
    }

    /// Logger for one of the categories `engine`, `io`, `mount`, `app`, `cli`.
    public static func logger(_ category: String) -> Logger {
        Logger(subsystem: logSubsystem, category: category)
    }
}
