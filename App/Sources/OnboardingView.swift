// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI

struct OnboardingView: View {
    var model: AppModel
    @Bindable var checks: Checks
    @Environment(\.dismissWindow) private var dismissWindow
    @State private var copied = false

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Set up Unlatch").font(.title2.bold())
            Text("Unlatch lets this Mac write to NTFS disks from Windows. A few things need to be in place.")
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)

            CheckRow(title: "macOS 27 or later", state: checks.macOS)
            CheckRow(title: "macFUSE installed", state: checks.macFUSE) {
                Button("Get macFUSE") { NSWorkspace.shared.open(URL(string: "https://macfuse.github.io")!) }
            }
            CheckRow(title: "macFUSE allowed to load", state: checks.kext)
            CheckRow(title: "Self-test: mount a test disk", state: checks.selfTest) {
                Button("Run again") { Task { await checks.runSelfTest() } }
            }
            CheckRow(title: "Helper for real disks", state: checks.helper,
                     note: "After installing, allow it in Full Disk Access, or macOS keeps it from opening disks.") {
                HStack {
                    Button("Install…") { Task { await checks.installHelper() } }
                    Button("Open Full Disk Access") { Checks.openFullDiskAccess() }
                }
            }

            Divider()
            Toggle("Open Unlatch at login", isOn: $checks.launchAtLogin)
            Toggle("Show notifications", isOn: Binding(get: { model.notificationsEnabled },
                                                     set: { model.notificationsEnabled = $0 }))
            HStack(spacing: 6) {
                Text("Command line:").foregroundStyle(.secondary)
                Button(copied ? "Copied" : "Copy install command") {
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString("sudo ln -sf '\(Checks.bundledCLI.path)' /usr/local/bin/unlatch", forType: .string)
                    copied = true
                }
            }
            .font(.callout)

            HStack {
                Button("Check again") { Task { await checks.run(model: model) } }
                Spacer()
                Button("Done") {
                    checks.onboarded = true
                    model.requestNotificationPermission()
                    dismissWindow(id: "onboarding")
                }
                .keyboardShortcut(.defaultAction)
                .disabled(!checks.allPassed)
            }
        }
        .padding(20)
        .frame(width: 460)
        .task { await checks.run(model: model) }
    }
}

struct CheckRow<Actions: View>: View {
    let title: String
    let state: Checks.State
    var note: String?
    @ViewBuilder var actions: () -> Actions

    init(title: String, state: Checks.State, note: String? = nil, @ViewBuilder actions: @escaping () -> Actions = { EmptyView() }) {
        self.title = title
        self.state = state
        self.note = note
        self.actions = actions
    }

    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            icon.frame(width: 18)
            VStack(alignment: .leading, spacing: 3) {
                Text(title)
                if case .failed(let why) = state {
                    Text(why).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                }
                if let note, state != .ok {
                    Text(note).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                }
                if state != .ok { actions().controlSize(.small) }
            }
        }
        .accessibilityElement(children: .contain)
        .accessibilityLabel("\(title): \(label)")
    }

    private var label: String {
        switch state {
        case .ok: "done"
        case .running: "checking"
        case .unknown: "not checked"
        case .failed(let why): "needs attention. \(why)"
        }
    }

    @ViewBuilder private var icon: some View {
        switch state {
        case .ok: Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
        case .running: ProgressView().controlSize(.small)
        case .unknown: Image(systemName: "circle").foregroundStyle(.secondary)
        case .failed: Image(systemName: "exclamationmark.circle.fill").foregroundStyle(.orange)
        }
    }
}
