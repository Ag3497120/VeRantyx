import SwiftUI
import GloveboxKit

/// One window: the game's art, the steps along the top, and the current step's card.
struct RootView: View {
    @EnvironmentObject var model: InstallerModel
    @EnvironmentObject var steam: SteamAccount

    var body: some View {
        ZStack {
            Backdrop(appID: model.recipe.steamAppID)
            page
                .id(pageID)
                .transition(.asymmetric(insertion: .opacity.combined(with: .offset(y: 14)), removal: .opacity))
                .padding(.horizontal, 24)
                .padding(.bottom, 24)
                .padding(.top, 58)
        }
        .overlay(alignment: .topLeading) {
            GloveboxChip().padding(.leading, 84).padding(.top, 9)
        }
        .overlay(alignment: .topTrailing) {
            if model.mode == .setup {
                StepBar().padding(.top, 9).padding(.trailing, 14)
            }
        }
        .overlay(alignment: .top) { toast }
        .overlay {
            if model.askingToShare {
                ZStack {
                    Color.black.opacity(0.45)
                    ShareAskCard { model.setShareStutters($0) }
                        .transition(.scale(scale: 0.92).combined(with: .opacity))
                }
                .transition(.opacity)
            }
        }
        .overlay { if model.engineMissing { EngineMissingView() } }
        .ignoresSafeArea()
        .frame(width: Theme.windowSize.width, height: Theme.windowSize.height)
        .tint(Theme.accent)
        .preferredColorScheme(.dark)
    }

    @ViewBuilder private var page: some View {
        switch model.step {
        case .welcome: if model.mode == .home { HomePage() } else { WelcomePage() }
        case .xcode: XcodePage()
        case .steam: SteamPage()
        case .account: AccountPage()
        case .headset: HeadsetPage()
        case .install: InstallPage()
        }
    }

    /// The page changes with the step, and with Install's own finish.
    private var pageID: String { "\(model.step.rawValue)-\(model.mode == .home)-\(model.result != nil)" }

    @ViewBuilder private var toast: some View {
        if let t = model.toast {
            Label(t, systemImage: "checkmark.circle.fill")
                .font(.callout.weight(.medium))
                .foregroundStyle(.white)
                .padding(.horizontal, 14).padding(.vertical, 8)
                .glass(Capsule(), tint: 0.35)
                .padding(.top, 54)
                .transition(.move(edge: .top).combined(with: .opacity))
        }
    }
}

/// Only reachable from a broken build: the app has no engine inside it.
struct EngineMissingView: View {
    @EnvironmentObject var model: InstallerModel
    var body: some View {
        ZStack {
            Rectangle().fill(.background).ignoresSafeArea()
            ContentUnavailableView {
                Label("This copy of Glovebox is incomplete", systemImage: "shippingbox")
            } description: {
                Text("Download it again from GitHub.")
            } actions: {
                Button("Copy Diagnostics") { model.copyDiagnostics() }
            }
        }
    }
}
