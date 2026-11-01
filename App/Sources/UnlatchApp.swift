// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI

@main
struct UnlatchApp: App {
    var body: some Scene {
        MenuBarExtra("Unlatch", systemImage: "externaldrive") {
            Text("Unlatch (placeholder)")
            Divider()
            Button("Quit Unlatch") { NSApplication.shared.terminate(nil) }
                .keyboardShortcut("q")
        }
    }
}
