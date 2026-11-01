// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI
import UnlatchCore
import UniformTypeIdentifiers

struct SettingsView: View {
    @Bindable var model: AppModel
    @State private var includeInternal = AutoMountPolicy().includeInternal
    @State private var exportResult: String?

    var body: some View {
        Form {
            Section("Auto-mount") {
                Toggle("Mount NTFS disks with Unlatch when they are connected", isOn: $model.autoMount)
                Picker("Disks", selection: $includeInternal) {
                    Text("External disks only").tag(false)
                    Text("All disks, including internal ones").tag(true)
                }
                .onChange(of: includeInternal) { model.policy.includeInternal = includeInternal }
                Text("Unlatch volumes are not indexed by Spotlight.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("Notifications") {
                Toggle("Show notifications", isOn: Binding(get: { model.notificationsEnabled },
                                                         set: { model.notificationsEnabled = $0 }))
            }
            Section("Troubleshooting") {
                Button("Export logs…") { exportLogs() }
                if let exportResult { Text(exportResult).font(.caption).foregroundStyle(.secondary) }
            }
            Section("Updates") {
                Toggle("Check for new versions once a day", isOn: Binding(get: { model.checkForUpdates },
                                                                       set: { model.checkForUpdates = $0 }))
                HStack {
                    Button("Check Now") { Task { await model.checkNow() } }
                    if let u = model.update {
                        Text("Unlatch \(u.version) is available.").font(.caption)
                    } else if model.lastUpdateCheck != nil {
                        Text("Unlatch \(Unlatch.version) is the newest version.").font(.caption).foregroundStyle(.secondary)
                    }
                }
                Text("Asks github.com for the list of releases; nothing about this Mac is sent. Updating is Homebrew's (brew upgrade --cask unlatch) or a new download.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("About") {
                Text("Unlatch \(Unlatch.version). Free software under the GPL, version 3 or later; it includes libntfs-3g. macFUSE is installed separately.")
                    .font(.caption).foregroundStyle(.secondary)
                // Contents/Resources/Notices: licenses and the written source offer (Step 18).
                Button("Licenses and source…") {
                    if let url = Bundle.main.url(forResource: "NOTICES", withExtension: "md", subdirectory: "Notices") {
                        NSWorkspace.shared.activateFileViewerSelecting([url])
                    }
                }
                Button("Uninstall Unlatch…", role: .destructive) { Task { await model.uninstall() } }
            }
        }
        .formStyle(.grouped)
        .frame(width: 440)
    }

    /// The last hour of Unlatch's log (all categories) into a file to attach to a bug report.
    private func exportLogs() {
        let panel = NSSavePanel()
        panel.nameFieldStringValue = "Unlatch-log.txt"
        panel.allowedContentTypes = [.plainText]
        guard panel.runModal() == .OK, let url = panel.url else { return }
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/log")
        p.arguments = ["show", "--last", "1h", "--info", "--predicate", "subsystem == \"\(Unlatch.logSubsystem)\""]
        FileManager.default.createFile(atPath: url.path, contents: nil)
        guard let h = try? FileHandle(forWritingTo: url) else { return }
        p.standardOutput = h
        p.standardError = h
        do {
            try p.run()
            p.waitUntilExit()
            exportResult = "Saved to \(url.lastPathComponent)."
        } catch {
            exportResult = error.localizedDescription
        }
        try? h.close()
    }
}
