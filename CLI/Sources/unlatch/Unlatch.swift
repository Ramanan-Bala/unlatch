// SPDX-License-Identifier: GPL-3.0-or-later
// The Xcode target compiles this file into the UnlatchCLI module itself (UNLATCH_ONE_MODULE).
#if !UNLATCH_ONE_MODULE
import UnlatchCLI
#endif

@main
enum Main {
    static func main() async {
        await UnlatchCLI.run(CommandLine.arguments.dropFirst().map { $0 })
    }
}
