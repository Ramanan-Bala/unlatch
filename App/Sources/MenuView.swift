// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI
import UnlatchCore

struct MenuView: View {
    @Bindable var model: AppModel
    var checks: Checks
    @Environment(\.openWindow) private var openWindow
    @Environment(\.openSettings) private var openSettings

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text("NTFS volumes")
                .font(.headline)
                .padding(.horizontal, 12)
                .padding(.vertical, 8)
            if model.volumes.isEmpty {
                Text("No NTFS disks connected.")
                    .foregroundStyle(.secondary)
                    .padding(.horizontal, 12)
                    .padding(.bottom, 8)
            }
            ForEach(model.volumes) { v in
                VolumeRow(model: model, volume: v)
            }
            if let err = model.lastError {
                Text(err)
                    .font(.caption)
                    .foregroundStyle(.red)
                    .fixedSize(horizontal: false, vertical: true)
                    .padding(.horizontal, 12)
                    .padding(.vertical, 6)
            }
            if let u = model.update {
                Button("Unlatch \(u.version) is available…") { Task { await model.installUpdate() } }
                    .buttonStyle(.link)
                    .padding(.horizontal, 12)
                    .padding(.vertical, 6)
            }
            if !checks.allPassed {
                Button("Finish setting up Unlatch…") { open("onboarding") }
                    .buttonStyle(.link)
                    .padding(.horizontal, 12)
                    .padding(.vertical, 6)
            }
            Divider().padding(.vertical, 4)
            Toggle("Auto-mount NTFS disks", isOn: $model.autoMount)
                .toggleStyle(.switch)
                .padding(.horizontal, 12)
                .help("Mount NTFS disks with Unlatch as soon as they are connected.")
            // Always reachable, not only while setup is unfinished: the same checks double as a
            // health view (macFUSE, its extension, the helper, the self-test).
            Button { open("onboarding") } label: {
                HStack(spacing: 6) {
                    Image(systemName: checks.allPassed ? "checkmark.circle.fill" : "exclamationmark.circle.fill")
                        .foregroundStyle(checks.allPassed ? .green : .orange)
                    Text("Setup & Health…")
                }
            }
            .buttonStyle(.plain)
            .padding(.horizontal, 12)
            .padding(.top, 8)
            .help(checks.allPassed ? "Everything Unlatch needs is in place." : "Something needs attention.")
            .accessibilityLabel(checks.allPassed ? "Setup and health: all checks pass" : "Setup and health: needs attention")
            HStack {
                Button("Settings…") { NSApp.activate(); openSettings() }
                Spacer()
                Button("Quit Unlatch") { NSApp.terminate(nil) }
                    .keyboardShortcut("q")
            }
            .padding(12)
        }
        .frame(width: 340)
        .task { await checks.run(model: model) }
    }

    private func open(_ id: String) {
        NSApp.activate()
        openWindow(id: id)
    }
}

/// One volume: label, size, state badge, and its actions.
struct VolumeRow: View {
    var model: AppModel
    var volume: AppModel.Volume

    private var badge: (text: String, color: Color, detail: String?) {
        let d = volume.disk, st = volume.status
        switch d.owner {
        case .unlatch:
            if st?.readOnly == true {
                return ("Read-only", .orange, st.map { $0.reason.isEmpty ? $0.state.rawValue : $0.reason })
            }
            return ("Read-write", .green, nil)
        case .apple:
            return ("Read-only (macOS)", .secondary, "macOS's own driver can only read NTFS.")
        case .other(let fs):
            return ("Mounted by \(fs)", .secondary, nil)
        case .none:
            switch st?.state {
            case .dirty?: return ("Needs checking", .orange, st?.reason)
            case .hibernated?: return ("Windows hibernated", .orange, st?.reason)
            case .uncleanLog?: return ("Not shut down cleanly", .orange, st?.reason)
            case .corrupt?: return ("Damaged", .red, st?.reason)
            default: return ("Not mounted", .secondary, nil)
            }
        }
    }

    var body: some View {
        let d = volume.disk, b = badge
        let busy = model.busy.contains(d.bsdName)
        HStack(alignment: .top, spacing: 10) {
            Image(systemName: d.isInternal ? "internaldrive" : "externaldrive")
                .font(.title2)
                .foregroundStyle(.secondary)
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 2) {
                Text(d.label.isEmpty ? d.bsdName : d.label).font(.body.weight(.medium))
                HStack(spacing: 6) {
                    Text(ByteCountFormatter.string(fromByteCount: d.size, countStyle: .file))
                        .foregroundStyle(.secondary)
                    Text(b.text)
                        .font(.caption.weight(.semibold))
                        .padding(.horizontal, 6)
                        .padding(.vertical, 1)
                        .background(b.color.opacity(0.18), in: Capsule())
                        .foregroundStyle(b.color == .secondary ? Color.secondary : b.color)
                }
                .font(.caption)
                if let detail = b.detail {
                    Text(detail).font(.caption2).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            Spacer()
            if busy {
                ProgressView().controlSize(.small).accessibilityLabel("Working")
            } else {
                actions
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .accessibilityElement(children: .combine)
        .accessibilityLabel("\(d.label.isEmpty ? d.bsdName : d.label), \(ByteCountFormatter.string(fromByteCount: d.size, countStyle: .file)), \(b.text)\(b.detail.map { ". \($0)" } ?? "")")
    }

    private var actions: some View {
        let v = volume, d = volume.disk
        return Menu {
            if d.owner == .unlatch {
                Button("Show in Finder") { model.reveal(v) }
                Button("Unmount") { Task { await model.unmount(d.bsdName) } }
            } else {
                Button("Mount read-write") { Task { await model.mount(d.bsdName, readOnly: false) } }
                Button("Mount read-only") { Task { await model.mount(d.bsdName, readOnly: true) } }
            }
            // A disk marked dirty that is not mounted read-write: the mount-time question again (D-94).
            if v.status?.state == .dirty, d.owner != .unlatch || v.status?.readOnly == true {
                Button("Enable Read-Write…") { Task { await model.enableReadWrite(v) } }
            }
            if v.status?.state == .dirty || v.status?.state == .corrupt {
                Button("Check Disk…") { Task { await model.checkDisk(v) } }
            }
            if d.isEjectable || d.isRemovable || !d.isInternal {
                Button("Eject") { Task { await model.eject(d.bsdName) } }
            }
            Divider()
            Picker("Auto-mount this disk", selection: Binding(
                get: { model.choice(for: v) },
                set: { model.setChoice($0, for: v) })) {
                Text("Default").tag(AutoMountPolicy.Choice?.none)
                Text("Always").tag(AutoMountPolicy.Choice?.some(.allow))
                Text("Never").tag(AutoMountPolicy.Choice?.some(.deny))
            }
        } label: {
            Image(systemName: "ellipsis.circle")
        }
        .menuStyle(.borderlessButton)
        .menuIndicator(.hidden)
        .fixedSize()
        .accessibilityLabel("Actions for \(d.label.isEmpty ? d.bsdName : d.label)")
    }
}
