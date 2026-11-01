// SPDX-License-Identifier: GPL-3.0-or-later
import AppKit
import Observation
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

    private let service: MountService?
    private let statusStore = StatusStore()
    private var watcher: DiskWatcher?
    private var tasks: [Task<Void, Never>] = []
    private let log = Unlatch.logger("app")

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
        watcher = DiskWatcher(claim: { $0.label != selfTest && policy.shouldAutoMount($0) })
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
        defer { busy.remove(bsdName) }
        do {
            let r = try await service.mount(bsdName, readOnly: readOnly)
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
            if automatic { notify("Could not mount \(bsdName)", error.localizedDescription) }
        }
        await refresh()
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
