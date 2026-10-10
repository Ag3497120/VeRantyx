import SwiftUI
import UIKit
import SteamLauncherKit

@main
struct AlyxLauncherApp: App {
    @UIApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @StateObject private var model = LauncherModel()
    @State private var showPlayNote = false
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup {
            RootView(onPlay: { showPlayNote = true })
                .environmentObject(model)
                .alert("Launcher preview: the game isn't in this build", isPresented: $showPlayNote) {
                    Button("OK", role: .cancel) {}
                }
        }
        .defaultSize(width: 1280, height: 860)
        .windowResizability(.contentSize)
        // The whole app's phase: background once every window is closed or out of sight.
        .onChange(of: scenePhase, initial: true) { _, phase in model.scenePhaseChanged(phase) }
    }
}
