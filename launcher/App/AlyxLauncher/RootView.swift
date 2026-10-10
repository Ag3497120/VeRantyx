import SwiftUI
import UIKit
import SteamLauncherKit

/// Hands the system's background download events to the chunk fetcher.
final class AppDelegate: NSObject, UIApplicationDelegate {
    func application(_ application: UIApplication,
                     didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil) -> Bool {
        LauncherModel.backgroundFetcher.activate()
        return true
    }

    func application(_ application: UIApplication, handleEventsForBackgroundURLSession identifier: String,
                     completionHandler: @escaping () -> Void) {
        guard identifier == LauncherModel.backgroundFetcher.identifier else { return completionHandler() }
        LauncherModel.backgroundFetcher.handleEvents(completion: completionHandler)
    }
}

/// Picks the screen for the current phase. Embeddable: the full app passes its own `onPlay`
/// that opens the game's ImmersiveSpace.
struct RootView: View {
    @EnvironmentObject var model: LauncherModel
    var onPlay: () -> Void

    var body: some View {
        ZStack {
            if model.phase == .library {
                LibraryView(onPlay: onPlay).transition(.opacity)
            } else {
                LandingView().transition(.opacity)
            }
        }
        .frame(minWidth: 1100, minHeight: 760)
        .animation(.easeInOut(duration: 0.5), value: model.phase == .library)
        .tint(Brand.orange)
        .preferredColorScheme(.dark)
    }
}
