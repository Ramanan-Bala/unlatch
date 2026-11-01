// SPDX-License-Identifier: GPL-3.0-or-later
import ArgumentParser
import UnlatchCore

@main
struct UnlatchCommand: ParsableCommand {
    static let configuration = CommandConfiguration(
        commandName: "unlatch",
        abstract: "Mount NTFS volumes read-write on macOS.",
        discussion: "Subcommands arrive in Step 13.",
        version: Unlatch.version
    )
}
