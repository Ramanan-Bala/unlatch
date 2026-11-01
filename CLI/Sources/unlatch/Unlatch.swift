// SPDX-License-Identifier: GPL-3.0-or-later
import UnlatchCLI

@main
enum Main {
    static func main() async {
        await UnlatchCLI.run(CommandLine.arguments.dropFirst().map { $0 })
    }
}
