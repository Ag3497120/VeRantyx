import SwiftUI
import AppKit
import Combine
import Sparkle
import GloveboxKit

@main
struct GloveboxApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @StateObject private var model: InstallerModel
    @StateObject private var updater = Updater()

    init() {
        KeychainSelfTest.runIfAsked()  // release checks: quits before anything here reads the Steam session
        let m: InstallerModel
        if let name = Demo.current { m = InstallerModel.demo(name) } else { m = InstallerModel() }
        AppDelegate.model = m
        _model = StateObject(wrappedValue: m)
    }

    var body: some Scene {
        Window("Glovebox", id: "main") {
            RootView()
                .environmentObject(model)
                .environmentObject(model.steam)
                .environmentObject(model.guide)
                .task {
                    Demo.snapshotIfAsked(model)
                    #if DEBUG
                    Tour.startIfAsked(model)
                    #endif
                }
        }
        .windowStyle(.hiddenTitleBar)
        .windowResizability(.contentSize)
        .commands {
            CommandGroup(after: .appInfo) {
                LicensesCommand()
                Button("Check for Updates…") { updater.check() }.disabled(!updater.canCheck)
            }
            CommandGroup(replacing: .newItem) {}
            CommandGroup(replacing: .help) {
                Button("Copy Diagnostics") { model.copyDiagnostics() }
                Button("Glovebox Website") { NSWorkspace.shared.open(URL(string: "https://glovebox.vision")!) }
            }
        }
        Window("Licenses", id: "licenses") {
            ScrollView {
                Text(LicensesText.all)
                    .font(.system(.body, design: .monospaced))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding()
            }
            .frame(minWidth: 560, minHeight: 420)
        }
        .defaultSize(width: 720, height: 640)
    }
}

/// The app menu's "Licenses…": a plain window with the license texts of everything the app carries (tools/licenses.py).
private struct LicensesCommand: View {
    @Environment(\.openWindow) private var openWindow
    var body: some View { Button("Licenses…") { openWindow(id: "licenses") } }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    @MainActor static weak var model: InstallerModel?

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    @MainActor func applicationWillFinishLaunching(_ notification: Notification) {
        Demo.applyAppearance()
    }

    /// Quitting mid-install stops the engine (and its xcodebuild and downloads) cleanly; the next
    /// Install picks up where it stopped. Nothing of Glovebox's outlives it (Children).
    @MainActor func applicationWillTerminate(_ notification: Notification) {
        Self.model?.cancelInstall()
        Self.model?.guide.stop()
        Children.stopAll(within: 3)
    }
}

/// Sparkle 2: checks the appcast on the latest GitHub release daily, and installs an update only if it's
/// signed with our EdDSA key and by the same Developer ID. Off until a key is configured.
@MainActor
final class Updater: ObservableObject {
    @Published var canCheck = false
    private let controller: SPUStandardUpdaterController?

    init() {
        let key = Bundle.main.object(forInfoDictionaryKey: "SUPublicEDKey") as? String ?? ""
        guard !key.isEmpty, Demo.current == nil else {
            controller = nil
            return
        }
        let c = SPUStandardUpdaterController(startingUpdater: true, updaterDelegate: nil, userDriverDelegate: nil)
        controller = c
        c.updater.publisher(for: \.canCheckForUpdates).receive(on: RunLoop.main).assign(to: &$canCheck)
    }

    func check() { controller?.checkForUpdates(nil) }
}
