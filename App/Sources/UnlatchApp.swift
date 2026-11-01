// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI
import UserNotifications

@main
struct UnlatchApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @State private var model: AppModel
    @State private var checks = Checks()

    init() {
        // Watch disks from launch: the menu's content only exists while the menu is open.
        let m = AppModel()
        m.start()
        _model = State(initialValue: m)
    }

    var body: some Scene {
        MenuBarExtra {
            MenuView(model: model, checks: checks)
        } label: {
            Image("MenuBarIcon")
                .accessibilityLabel("Unlatch")
        }
        .menuBarExtraStyle(.window)

        Window("Set up Unlatch", id: "onboarding") {
            OnboardingView(model: model, checks: checks)
        }
        .windowResizability(.contentSize)
        // Shown at launch until setup was finished once; the menu offers it again when a check fails.
        .defaultLaunchBehavior(UserDefaults.standard.bool(forKey: "onboarded") ? .suppressed : .presented)

        Settings {
            SettingsView(model: model)
        }
    }
}

/// Starts the model, opens onboarding on first run or when a check fails, and routes
/// notification taps ("Learn more").
final class AppDelegate: NSObject, NSApplicationDelegate, UNUserNotificationCenterDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        UNUserNotificationCenter.current().delegate = self
    }

    func userNotificationCenter(_ center: UNUserNotificationCenter, didReceive response: UNNotificationResponse) async {
        if let s = response.notification.request.content.userInfo["learnMore"] as? String, let url = URL(string: s) {
            await MainActor.run { _ = NSWorkspace.shared.open(url) }
        }
    }

    func userNotificationCenter(_ center: UNUserNotificationCenter, willPresent notification: UNNotification)
        async -> UNNotificationPresentationOptions {
        [.banner, .sound]
    }
}
