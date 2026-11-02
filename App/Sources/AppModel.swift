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

    func mount(_ bsdName: String, readOnly: Bool, automatic: Bool = false, dirtyReadWrite: Bool = false,
               markChecked: Bool = false) async {
        guard let service, !busy.contains(bsdName) else { return }
        busy.insert(bsdName)
        defer { busy.remove(bsdName) }
        do {
            let r = try await service.mount(bsdName, readOnly: readOnly, dirtyReadWrite: dirtyReadWrite, markChecked: markChecked)
            let name = (try? await service.disk(bsdName).label).flatMap { $0.isEmpty ? nil : $0 } ?? bsdName
            if r.readOnly && !readOnly {
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
    }

    /// Mounts a volume marked dirty read-write after the user confirms the risk (D-90).
    func mountDirtyReadWrite(_ v: Volume) async {
        let name = v.disk.label.isEmpty ? v.disk.bsdName : v.disk.label
        let alert = NSAlert()
        alert.alertStyle = .critical
        alert.messageText = "Write to \(name) without checking it?"
        alert.informativeText = """
            This disk was not ejected cleanly, so some of its records may be half-written. Writing to it \
            before it is checked can damage files on it, including files you do not touch.

            Copy anything important off it first. The disk stays marked as needing a check, so Windows \
            will still offer to check it.
            """
        alert.addButton(withTitle: "Cancel")
        let write = alert.addButton(withTitle: "Mount Read-Write")
        write.hasDestructiveAction = true
        NSApp.activate()
        guard alert.runModal() == .alertSecondButtonReturn else { return }
        log.notice("\(v.disk.bsdName, privacy: .public): mounting read-write although marked dirty, as confirmed")
        await mount(v.disk.bsdName, readOnly: false, dirtyReadWrite: true)
    }

    /// The user decides a disk marked dirty is fine (D-92): it is mounted read-write, and its
    /// next clean eject clears the mark, so it mounts read-write from then on.
    func markChecked(_ v: Volume) async {
        let name = v.disk.label.isEmpty ? v.disk.bsdName : v.disk.label
        let alert = NSAlert()
        alert.alertStyle = .critical
        alert.messageText = "Mark \(name) as checked?"
        alert.informativeText = """
            Do this only after copying anything important off the disk. It was not ejected cleanly, \
            and its records may still be half-written; marking it checked does not repair them.

            Unlatch mounts it read-write now. When you eject it from Unlatch, it is marked as checked: \
            it then mounts read-write every time, and Windows no longer offers to check it.
            """
        alert.addButton(withTitle: "Cancel")
        let mark = alert.addButton(withTitle: "Mark as Checked")
        mark.hasDestructiveAction = true
        NSApp.activate()
        guard alert.runModal() == .alertSecondButtonReturn else { return }
        log.notice("\(v.disk.bsdName, privacy: .public): marking checked at the next clean eject, as confirmed")
        await mount(v.disk.bsdName, readOnly: false, markChecked: true)
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
