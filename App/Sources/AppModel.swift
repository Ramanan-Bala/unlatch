// SPDX-License-Identifier: GPL-3.0-or-later
import AppKit
import Observation
import Synchronization
import UnlatchCore
import UserNotifications

/// The app's state: NTFS volumes, their verdicts, and the auto-mount loop (plan Step 14).
@MainActor @Observable
final class AppModel {
    struct Volume: Identifiable, Equatable {
        var disk: DiskInfo
        var status: VolumeStatus?
        var id: String { disk.bsdName }
    }

    private(set) var volumes: [Volume] = []
    private(set) var busy: Set<String> = []
    var lastError: String?
    let policy = AutoMountPolicy()
    var autoMount: Bool {
        didSet { policy.enabled = autoMount }
    }

    // Step 19 (Lifecycle.swift): the newest release when it is newer than this one.
    var update: UpdateChecker.Release?
    var lastUpdateCheck: Date?
    var updateTask: Task<Void, Never>?

    private let service: MountService?
    private let statusStore = StatusStore()
    private var watcher: DiskWatcher?
    private var tasks: [Task<Void, Never>] = []
    private let log = Unlatch.logger("app")
    /// Volumes whose automatic mount failed and that were handed to macOS: no longer claimed
    /// until they are unplugged, so they stay visible, read-only, through Apple's driver.
    private let released = BSDNames()
    /// Volumes already asked about since they were plugged in (D-94).
    private var asked: Set<String> = []

    init() {
        service = try? MountService(daemon: Bundle.main.bundleURL.appendingPathComponent("Contents/Helpers/unlatch-fuse"))
        autoMount = AutoMountPolicy().enabled
    }

    /// Starts watching disks; volumes the policy covers are claimed from Apple's driver and mounted.
    func start() {
        guard watcher == nil else { return }
        let policy = self.policy
        // The onboarding self-test image is the self-test's to mount, never auto-mounted.
        let selfTest = Checks.selfTestLabel
        let released = self.released
        watcher = DiskWatcher(claim: {
            $0.label != selfTest && policy.shouldAutoMount($0) && !released.contains($0.bsdName) && Self.canMount($0)
        })
        guard let events = watcher?.events() else { return }
        tasks.append(Task { [weak self] in
            for await e in events {
                await self?.handle(e)
            }
        })
        tasks.append(Task { [weak self] in
            guard let changes = self?.statusStore.changes() else { return }
            for await _ in changes { await self?.refresh() }
        })
        Task { await refresh() }
        startUpdateChecks()
    }

    /// Stops the auto-mount loop (and with it the claim on new NTFS disks) and the update checks.
    func stop() {
        tasks.forEach { $0.cancel() }
        tasks = []
        updateTask?.cancel()
        watcher?.stop()
        watcher = nil
    }

    /// Whether Unlatch can mount the volume at all: refusing Apple's driver is only safe then.
    /// A macOS update or a revoked permission otherwise made claimed drives vanish.
    nonisolated static func canMount(_ d: DiskInfo) -> Bool {
        MountService.macFUSEInstalled && (HelperOpener.installed || Darwin.access(d.devicePath, R_OK) == 0)
    }

    func refresh() async {
        guard let service else { return }
        let disks = await service.list()
        var next: [Volume] = []
        for d in disks { next.append(Volume(disk: d, status: await service.status(d.bsdName))) }
        volumes = next
    }

    private func handle(_ event: DiskWatcher.Event) async {
        switch event {
        case .appeared(let d):
            await refresh()
            if d.label != Checks.selfTestLabel, policy.shouldAutoMount(d), d.owner != .unlatch {
                await mount(d.bsdName, readOnly: false, automatic: true)
            }
        case .disappeared(let name):
            released.remove(name)
            asked.remove(name)
            let was = volumes.first { $0.id == name }
            await refresh()
            if let was, was.disk.owner == .unlatch {
                // Gone while Unlatch had it mounted: the cable came out before an eject.
                notify("\(was.disk.label.isEmpty ? name : was.disk.label) was removed without ejecting",
                       "Windows may report it needs checking. Eject from the Unlatch menu before unplugging.")
            }
        case .changed:
            await refresh()
        }
    }

    // MARK: actions

    func mount(_ bsdName: String, readOnly: Bool, automatic: Bool = false) async {
        guard let service, !busy.contains(bsdName) else { return }
        busy.insert(bsdName)
        var askDirty = false
        do {
            defer { busy.remove(bsdName) }
            let r = try await service.mount(bsdName, readOnly: readOnly)
            let name = (try? await service.disk(bsdName).label).flatMap { $0.isEmpty ? nil : $0 } ?? bsdName
            if r.readOnly && !readOnly && r.status?.state == .dirty {
                // Marked as needing a check: ask instead of notifying, once per plug-in for
                // automatic mounts; a mount the user asked for asks again (D-94).
                askDirty = !automatic || !asked.contains(bsdName)
            } else if r.readOnly && !readOnly {
                let why = r.status?.reason ?? "it could not be opened for writing"
                notify("\(name) is read-only", "\(why.prefix(1).uppercased() + why.dropFirst()).", learnMore: true)
            } else if automatic || !readOnly {
                notify("\(name) is ready", r.readOnly ? "Mounted read-only." : "Mounted read-write. Eject it here before unplugging.")
            }
            lastError = nil
        } catch {
            lastError = error.localizedDescription
            log.error("mount \(bsdName, privacy: .public): \(error.localizedDescription, privacy: .public)")
            if automatic {
                // The claim turned Apple's driver away: hand the volume back so it stays visible.
                released.insert(bsdName)
                try? await service.mountWithMacOS(bsdName)
                notify("Could not mount \(bsdName)", error.localizedDescription)
            }
        }
        await refresh()
        // In a task of its own: the disk events behind this mount are not held up by the dialog.
        if askDirty, let v = volumes.first(where: { $0.id == bsdName }) { Task { await enableReadWrite(v) } }
    }

    /// A disk marked as needing a check came up read-only (D-94): the user may check it and
    /// enable read-write, enable read-write without checking, or keep it read-only. Both
    /// choices clear the mark (the check only when nothing is left).
    func enableReadWrite(_ v: Volume) async {
        asked.insert(v.disk.bsdName)
        let name = v.disk.label.isEmpty ? v.disk.bsdName : v.disk.label
        let alert = NSAlert()
        alert.messageText = "\(name) needs checking"
        alert.informativeText = """
            It was not ejected cleanly, so some of its records may be half-written, and it was mounted read-only.

            Check Disk looks for damage, repairs what is safe to repair, and enables read-write when nothing is left. \
            Enabling read-write without checking repairs nothing: writing to a damaged disk can damage files on it, \
            and Windows will no longer offer to check it.
            """
        alert.addButton(withTitle: "Keep Read-Only")
        alert.addButton(withTitle: "Check Disk and Enable Read-Write")
        let unchecked = alert.addButton(withTitle: "Enable Read-Write Without Checking")
        unchecked.hasDestructiveAction = true
        NSApp.activate()
        switch alert.runModal() {
        case .alertSecondButtonReturn:
            log.notice("\(v.disk.bsdName, privacy: .public): checking to enable read-write, as asked")
            await checkAndEnable(v, name: name)
        case .alertThirdButtonReturn:
            log.notice("\(v.disk.bsdName, privacy: .public): read-write without checking, mark cleared at the clean eject, as asked")
            await mountMarkChecked(v.disk.bsdName, name: name)
        default:
            return
        }
    }

    /// Check Disk with the safe repair; it clears the mark and mounts read-write when nothing is
    /// left, and says what was found otherwise.
    private func checkAndEnable(_ v: Volume, name: String) async {
        guard let r = await runCheck(v.disk.bsdName, repair: true) else { return }
        if r.remaining == 0 {
            if v.disk.owner == .none { await mount(v.disk.bsdName, readOnly: false) }
            let rw = volumes.first { $0.id == v.disk.bsdName }.map { $0.disk.owner == .unlatch && $0.status?.readOnly == false } ?? false
            notify(rw ? "\(name) is ready" : "\(name) was checked",
                   (r.found == 0 ? "No problems found. " : "Repaired. ") + (rw ? "Mounted read-write." : "The mark is cleared."))
            return
        }
        let done = NSAlert()
        done.alertStyle = .critical
        done.messageText = r.problems.contains { $0.kind == "mft_mirror" }
            ? "\(name) cannot be opened until Windows repairs it" : "\(name) needs Windows to repair it"
        done.informativeText = Self.summary(r)
        done.addButton(withTitle: "OK")
        done.addButton(withTitle: "Copy Report")
        NSApp.activate()
        if done.runModal() == .alertSecondButtonReturn {
            NSPasteboard.general.clearContents()
            NSPasteboard.general.setString(Self.fullReport(r, name: name), forType: .string)
        }
    }

    /// Mounts read-write without checking; the clean eject clears the mark (D-92).
    private func mountMarkChecked(_ bsdName: String, name: String) async {
        guard let service, !busy.contains(bsdName) else { return }
        busy.insert(bsdName)
        do {
            defer { busy.remove(bsdName) }
            let r = try await service.mount(bsdName, markChecked: true)
            if !r.readOnly { notify("\(name) is ready", "Mounted read-write. Eject it here before unplugging.") }
            lastError = nil
        } catch {
            lastError = error.localizedDescription
            log.error("mount \(bsdName, privacy: .public): \(error.localizedDescription, privacy: .public)")
        }
        await refresh()
    }

    /// Check Disk (D-93): unmounts the disk, checks it (read-only), mounts it again, and says
    /// what was found. When everything found is safe to repair, or nothing was found on a disk
    /// marked dirty, it offers the repair, which also clears the mark when nothing is left.
    func checkDisk(_ v: Volume) async {
        let name = v.disk.label.isEmpty ? v.disk.bsdName : v.disk.label
        guard let r = await runCheck(v.disk.bsdName, repair: false) else { return }
        let dirty = r.state == "dirty"
        let alert = NSAlert()
        alert.informativeText = Self.summary(r)
        switch r.verdict {
        case .clean, .repaired:
            alert.messageText = "No problems found on \(name)"
            if dirty {
                alert.informativeText = """
                    \(r.recordsInUse) records and \(r.directories) folders were checked. The disk is still marked as \
                    needing a check, so it mounts read-only. Unlatch can clear the mark; it then mounts read-write.
                    """
                alert.addButton(withTitle: "Clear Mark")
                alert.addButton(withTitle: "Not Now")
            }
        case .repairable:
            alert.messageText = "\(name) has problems Unlatch can repair"
            alert.addButton(withTitle: "Repair")
            alert.addButton(withTitle: "Not Now")
        case .needsWindows:
            alert.alertStyle = .critical
            alert.messageText = r.problems.contains { $0.kind == "mft_mirror" }
                ? "\(name) cannot be opened until Windows repairs it" : "\(name) needs Windows to repair it"
            alert.addButton(withTitle: "OK")
            alert.addButton(withTitle: "Copy Report")
        }
        NSApp.activate()
        let answer = alert.runModal()
        if r.verdict == .needsWindows {
            if answer == .alertSecondButtonReturn {
                NSPasteboard.general.clearContents()
                NSPasteboard.general.setString(Self.fullReport(r, name: name), forType: .string)
            }
            return
        }
        guard answer == .alertFirstButtonReturn, r.verdict == .repairable || ((r.verdict == .clean || r.verdict == .repaired) && dirty)
        else { return }
        log.notice("\(v.disk.bsdName, privacy: .public): repairing, as confirmed")
        guard let fixed = await runCheck(v.disk.bsdName, repair: true) else { return }
        let done = NSAlert()
        done.messageText = fixed.remaining == 0 ? "\(name) is repaired" : "\(name) still has problems"
        done.informativeText = fixed.remaining == 0 ? fixed.advice : Self.summary(fixed)
        if fixed.remaining > 0 { done.alertStyle = .critical }
        NSApp.activate()
        done.runModal()
    }

    private func runCheck(_ bsdName: String, repair: Bool) async -> CheckReport? {
        guard let service, !busy.contains(bsdName) else { return nil }
        busy.insert(bsdName)
        defer { busy.remove(bsdName) }
        do {
            let r = try await service.check(bsdName, repair: repair)
            lastError = nil
            await refresh()
            return r
        } catch {
            lastError = error.localizedDescription
            log.error("check \(bsdName, privacy: .public): \(error.localizedDescription, privacy: .public)")
            await refresh()
            return nil
        }
    }

    /// What was found, by kind, then a few paths and the advice.
    static func summary(_ r: CheckReport) -> String {
        guard r.found > 0 else { return r.advice }
        var lines = r.kinds.map { "• " + CheckReport.describe($0.kind, count: $0.count) }
        let paths = r.problems.compactMap { $0.path.isEmpty ? nil : $0.path }
        if !paths.isEmpty {
            lines.append("")
            lines += paths.prefix(5).map { "  \($0)" }
            if paths.count > 5 { lines.append("  and \(paths.count - 5) more") }
        }
        lines += ["", r.advice]
        return lines.joined(separator: "\n")
    }

    static func fullReport(_ r: CheckReport, name: String) -> String {
        (["Unlatch Check Disk: \(name) (\(r.state)), \(r.found) problem(s), \(r.remaining) left"]
            + r.problems.map { "\($0.kind)\t\($0.path)\t\($0.detail)" } + [r.advice]).joined(separator: "\n")
    }

    func unmount(_ bsdName: String) async {
        await run(bsdName) { try await $0.unmount(bsdName) }
    }

    func eject(_ bsdName: String) async {
        let name = volumes.first { $0.id == bsdName }.map { $0.disk.label.isEmpty ? bsdName : $0.disk.label } ?? bsdName
        if await run(bsdName, { try await $0.eject(bsdName) }) {
            notify("\(name) can be unplugged", "It was ejected safely.")
        }
    }

    @discardableResult
    private func run(_ bsdName: String, _ op: (MountService) async throws -> Void) async -> Bool {
        guard let service, !busy.contains(bsdName) else { return false }
        busy.insert(bsdName)
        defer { busy.remove(bsdName) }
        do {
            try await op(service)
            lastError = nil
            await refresh()
            return true
        } catch {
            lastError = error.localizedDescription
            await refresh()
            return false
        }
    }

    func reveal(_ v: Volume) {
        guard let p = v.disk.mountPoint else { return }
        NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: p)])
    }

    func setChoice(_ choice: AutoMountPolicy.Choice?, for v: Volume) {
        guard let uuid = v.disk.volumeUUID else { return }
        policy.set(choice, for: uuid)
    }

    func choice(for v: Volume) -> AutoMountPolicy.Choice? {
        v.disk.volumeUUID.flatMap { policy.choice(for: $0) }
    }

    // MARK: notifications

    var notificationsEnabled: Bool {
        get { UserDefaults.standard.object(forKey: "notifications") as? Bool ?? true }
        set { UserDefaults.standard.set(newValue, forKey: "notifications") }
    }

    func requestNotificationPermission() {
        UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound]) { _, _ in }
    }

    private func notify(_ title: String, _ body: String, learnMore: Bool = false) {
        guard notificationsEnabled else { return }
        let content = UNMutableNotificationContent()
        content.title = title
        content.body = body
        if learnMore { content.userInfo = ["learnMore": Self.readOnlyHelp.absoluteString] }
        UNUserNotificationCenter.current().add(UNNotificationRequest(identifier: UUID().uuidString, content: content, trigger: nil))
    }

    static let readOnlyHelp = URL(string: "https://github.com/Ramanan-Bala/unlatch/blob/main/docs/read-only.md")!
}

/// A set of BSD names shared with DiskArbitration's queue.
final class BSDNames: Sendable {
    private let names = Mutex<Set<String>>([])

    func contains(_ name: String) -> Bool { names.withLock { $0.contains(name) } }
    func insert(_ name: String) { _ = names.withLock { $0.insert(name) } }
    func remove(_ name: String) { _ = names.withLock { $0.remove(name) } }
}
