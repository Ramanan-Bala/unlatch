// SPDX-License-Identifier: GPL-3.0-or-later
import AppKit
import ServiceManagement
import UnlatchCore

/// Update notices and uninstall (plan Step 19, D-70). There is no signed updater: when GitHub
/// has a newer release the menu says so, and installing it is `brew upgrade` or a new DMG.
/// Before either, Unlatch's volumes are unmounted, so no write is cut off by the app quitting.
@MainActor
extension AppModel {
    var checkForUpdates: Bool {
        get { UserDefaults.standard.object(forKey: "checkForUpdates") as? Bool ?? true }
        set {
            UserDefaults.standard.set(newValue, forKey: "checkForUpdates")
            if !newValue { update = nil }
        }
    }

    /// Installed by the Homebrew cask (which keeps its own record of the app).
    static var installedWithHomebrew: Bool {
        ["/opt/homebrew/Caskroom/unlatch", "/usr/local/Caskroom/unlatch"].contains { FileManager.default.fileExists(atPath: $0) }
    }

    /// Checks GitHub at most once a day while the setting is on.
    func startUpdateChecks() {
        updateTask?.cancel()
        updateTask = Task { [weak self] in
            while !Task.isCancelled {
                if let self, self.checkForUpdates { await self.checkNow() }
                try? await Task.sleep(for: .seconds(24 * 60 * 60))
            }
        }
    }

    func checkNow() async {
        do {
            update = try await UpdateChecker().check()
            lastUpdateCheck = Date()
        } catch {
            Unlatch.logger("app").info("update check failed: \(error.localizedDescription, privacy: .public)")
        }
    }

    private var unlatchVolumes: [Volume] { volumes.filter { $0.disk.owner == .unlatch } }

    /// Unmounts every Unlatch volume; false (with lastError set) if one stays mounted.
    func unmountAll() async -> Bool {
        await refresh()
        for v in unlatchVolumes { await unmount(v.id) }
        await refresh()
        return unlatchVolumes.isEmpty
    }

    /// Asks to unmount Unlatch's volumes first; false when the user cancels or one is busy.
    private func unmountFirst(for action: String) async -> Bool {
        await refresh()
        let mounted = unlatchVolumes
        guard !mounted.isEmpty else { return true }
        let names = mounted.map { $0.disk.label.isEmpty ? $0.id : $0.disk.label }.joined(separator: ", ")
        let alert = NSAlert()
        alert.messageText = "Unmount \(mounted.count == 1 ? "1 volume" : "\(mounted.count) volumes") before \(action)?"
        alert.informativeText = "Unlatch has \(names) mounted. They are unmounted first, so no copy is cut off."
        alert.addButton(withTitle: "Unmount and Continue")
        alert.addButton(withTitle: "Cancel")
        NSApp.activate()
        guard alert.runModal() == .alertFirstButtonReturn else { return false }
        if await unmountAll() { return true }
        let fail = NSAlert()
        fail.messageText = "A volume is still in use"
        fail.informativeText = (lastError ?? "Close the files that are open on it and try again.")
        fail.runModal()
        return false
    }

    /// The menu's "Update available" action.
    func installUpdate() async {
        guard let update else { return }
        guard await unmountFirst(for: "updating") else { return }
        if Self.installedWithHomebrew {
            let cmd = "brew upgrade --cask unlatch"
            NSPasteboard.general.clearContents()
            NSPasteboard.general.setString(cmd, forType: .string)
            let a = NSAlert()
            a.messageText = "Update with Homebrew"
            a.informativeText = "Unlatch was installed with Homebrew. Run this in Terminal (it is on the clipboard):\n\n\(cmd)"
            a.addButton(withTitle: "OK")
            a.addButton(withTitle: "Open Release Notes")
            if a.runModal() == .alertSecondButtonReturn { NSWorkspace.shared.open(update.page) }
        } else {
            NSWorkspace.shared.open(update.page)
        }
    }

    /// Settings > Uninstall: unmount everything, remove the helper (admin password), the login
    /// item, status records and settings, then move the app to the Trash and quit. macFUSE stays.
    func uninstall() async {
        let confirm = NSAlert()
        confirm.messageText = "Uninstall Unlatch?"
        confirm.informativeText = """
            Unlatch's volumes are unmounted, the helper is removed (macOS asks for your password), \
            and Unlatch's settings and status files are deleted. The app goes to the Trash. \
            macFUSE stays installed; remove it separately if nothing else needs it.
            """
        confirm.addButton(withTitle: "Uninstall")
        confirm.addButton(withTitle: "Cancel")
        confirm.alertStyle = .warning
        NSApp.activate()
        guard confirm.runModal() == .alertFirstButtonReturn else { return }
        guard await unmountFirst(for: "uninstalling") else { return }
        stop()

        var problems: [String] = []
        if FileManager.default.fileExists(atPath: HelperOpener.installedPath) ||
            FileManager.default.fileExists(atPath: HelperOpener.legacyInstalledPath) || HelperOpener.installed {
            let cmd = "'\(Checks.bundledCLI.path)' uninstall-helper"
            let script = "do shell script \"\(cmd.replacingOccurrences(of: "\"", with: "\\\""))\" with administrator privileges"
            var err: NSDictionary?
            _ = NSAppleScript(source: script)?.executeAndReturnError(&err)
            if FileManager.default.fileExists(atPath: HelperOpener.installedPath) {
                problems.append("The helper is still installed (\(err?[NSAppleScript.errorMessage] as? String ?? "cancelled")). Remove it with: sudo unlatch uninstall-helper")
            }
        }
        try? await SMAppService.mainApp.unregister()
        if let dir = VolumeStatus.directory()?.deletingLastPathComponent() {   // ~/Library/Application Support/Unlatch
            try? FileManager.default.removeItem(at: dir)
        }
        if let id = Bundle.main.bundleIdentifier {
            UserDefaults.standard.removePersistentDomain(forName: id)
        }
        let app = Bundle.main.bundleURL
        do {
            try await NSWorkspace.shared.recycle([app])
        } catch {
            problems.append("Move \(app.path) to the Trash yourself (\(error.localizedDescription)).")
        }
        let done = NSAlert()
        done.messageText = problems.isEmpty ? "Unlatch was uninstalled" : "Unlatch was mostly uninstalled"
        done.informativeText = problems.isEmpty
            ? (Self.installedWithHomebrew ? "Also run: brew uninstall --cask unlatch" : "macFUSE is still installed.")
            : problems.joined(separator: "\n\n")
        done.runModal()
        NSApp.terminate(nil)
    }
}
