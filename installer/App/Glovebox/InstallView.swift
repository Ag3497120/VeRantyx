import SwiftUI
import AppKit
import GloveboxKit

/// Install: one button, then the glove ring for the whole install, with what's happening under
/// it. Then the game is on the headset.
struct InstallPage: View {
    @EnvironmentObject var model: InstallerModel

    var body: some View {
        if model.result != nil {
            DonePage()
        } else {
            CardScreen(layout: .center) { card }
        }
    }

    @ViewBuilder private var card: some View {
        #if DEBUG
        let _ = Tour.askedForCard(model)
        #endif
        if let err = model.installError {
            if model.guide.isRunning && model.guide.job == .signInAgain {
                GuideRunningCard()
            } else {
                InstallErrorCard(error: err)
            }
        } else if model.allowingSigning {
            VStack {
                if model.signingAsked {
                    // macOS's window is up, the guide under it.
                    wide {
                        Text("Finish in the macOS window").font(.system(size: 20, weight: .bold))
                        Spacer()
                        ProgressView().controlSize(.small)
                        Button("Cancel") { model.cancelInstall() }.secondaryButton()
                    }
                } else {
                    SigningCard()
                }
            }
            .onAppear {
                #if DEBUG
                Tour.card = "signing"
                #endif
            }
            .onDisappear {
                #if DEBUG
                Tour.card = "-"
                #endif
            }
        } else if model.waitingForHeadset {
            wide {
                Image(systemName: Symbols.available("visionpro.fill", fallback: "eyeglasses"))
                    .font(.system(size: 30))
                    .frame(width: 64, height: 64)
                    .background(Circle().fill(.white.opacity(0.14)))
                VStack(alignment: .leading, spacing: 4) {
                    Text("Put on your Vision Pro").font(.system(size: 20, weight: .bold))
                    Text("Wake it up and unlock it.").foregroundStyle(.white.opacity(0.72))
                }
                Spacer()
                ProgressView().controlSize(.small)
                Button("Cancel") { model.cancelInstall() }.secondaryButton()
            }
        } else if model.installing {
            // The news sign-up, in a quiet stretch: on the progress card's right, the same height.
            HStack(alignment: .bottom, spacing: 16) {
                VStack(spacing: 0) {
                    GloveRing(fraction: model.progress.overall)
                        .padding(.bottom, 16)
                    Text("Installing on \(model.headsetName)")
                        .font(.system(size: 22, weight: .bold))
                        .tracking(-0.3)
                    Text([stageText, Format.timeLeft(model.progress.secondsLeft)].compactMap { $0 }.joined(separator: " · "))
                        .foregroundStyle(.white.opacity(0.72))
                        .contentTransition(.numericText())
                        .fixedSize(horizontal: false, vertical: true)
                        .padding(.top, 5)
                    Button("Cancel") { model.cancelInstall() }
                        .secondaryButton()
                        .padding(.top, 18)
                }
                .multilineTextAlignment(.center)
                .foregroundStyle(.white)
                .padding(.horizontal, 28)
                .padding(.vertical, 24)
                .frame(width: 440)
                .frame(maxHeight: .infinity)
                .glass(RoundedRectangle(cornerRadius: 28, style: .continuous))
                .shadow(color: .black.opacity(0.3), radius: 20, y: 8)
                if model.showsNewsAsk {
                    NewsAskCard(width: 360, fill: true).transition(.move(edge: .trailing).combined(with: .opacity))
                }
            }
            .fixedSize(horizontal: false, vertical: true)
        } else if model.gettingReady {
            // Install, Update or Try Again: the steps are checked again before anything starts.
            wide {
                Text("Getting ready").font(.system(size: 20, weight: .bold))
                Spacer()
                ProgressView().controlSize(.small)
                Button("Cancel") { model.cancelInstall() }.secondaryButton()
            }
        } else if model.state(of: .install) == .waiting {
            wide {
                VStack(alignment: .leading, spacing: 4) {
                    Text("Almost there").font(.system(size: 20, weight: .bold))
                    Text("A few steps are left.").foregroundStyle(.white.opacity(0.72))
                }
                Spacer()
                Button("Take Me There") { model.go(model.firstOpenStep() ?? .xcode) }
                    .primaryButton()
                    .keyboardShortcut(.defaultAction)
            }
        } else {
            wide {
                VStack(alignment: .leading, spacing: 4) {
                    Text("Ready to install").font(.system(size: 20, weight: .bold))
                    Text("About 5 minutes. Keep your Vision Pro close.").foregroundStyle(.white.opacity(0.72))
                }
                Spacer()
                Button("Install") { model.requestInstall() }
                    .primaryButton()
                    .keyboardShortcut(.defaultAction)
            }
        }
    }

    /// The card across the bottom of the window, its parts in one row.
    private func wide<Content: View>(@ViewBuilder _ content: () -> Content) -> some View {
        HStack(spacing: 18) { content() }
            .foregroundStyle(.white)
            .padding(.horizontal, 24)
            .padding(.vertical, 20)
            .frame(maxWidth: .infinity, alignment: .leading)
            .glass(RoundedRectangle(cornerRadius: 26, style: .continuous))
            .shadow(color: .black.opacity(0.3), radius: 20, y: 8)
    }

    private var stageText: String? {
        switch model.progress.stage {
        case .download?: return "Downloading the game files"
        case .convert?: return "Converting the game for Vision Pro"
        case .build?: return "Making the app"
        case .install?: return "Sending it to your Vision Pro"
        case nil: return "Starting"
        }
    }
}

/// Before the one signature macOS asks about: what the player allows and why. macOS asks only after
/// Continue, and the guide then points up at Always Allow from under its window (XcodeGuide's keychain job).
private struct SigningCard: View {
    @EnvironmentObject var model: InstallerModel

    var body: some View {
        StepCard(title: "macOS will ask for your Mac password once",
                 subtitle: "It lets Glovebox use your Apple Account to put the game on your Vision Pro.",
                 width: 580) {
            EmptyView()
        } footer: {
            Spacer()
            Button("Cancel") { model.cancelInstall() }.secondaryButton()
            Button("Continue") { model.continueSigning() }
                .primaryButton()
                .keyboardShortcut(.defaultAction)
        }
    }
}

/// Something stopped the install: what, and the way on.
private struct InstallErrorCard: View {
    @EnvironmentObject var model: InstallerModel
    var error: EngineError

    var body: some View {
        StepCard(title: title, subtitle: detail, width: nil) {
            EmptyView()
        } footer: {
            Button("Copy Diagnostics") { model.copyDiagnostics() }.textLink()
            Spacer()
            if error.code == "steam-not-owned" {
                Button("Get It on Steam") { NSWorkspace.shared.open(model.recipe.storeURL) }.secondaryButton()
            } else if error.code == "signing-failed" || error.code == "signing-no-team" {
                Button("Check Account") { model.recover(at: .account) }.secondaryButton()
            }
            if error.code == "xcode-signin-expired" {
                Button("Try Again") { model.requestInstall() }.secondaryButton()
                Button("Sign In") { model.signInAgain() }
                    .primaryButton()
                    .keyboardShortcut(.defaultAction)
            } else if error.code == "apple-agreement-pending" {
                Button("Try Again") { model.requestInstall() }.secondaryButton()
                Button("Open Apple's Website") { model.acceptAgreement() }
                    .primaryButton()
                    .keyboardShortcut(.defaultAction)
            } else {
                Button("Try Again") { model.requestInstall() }
                    .primaryButton()
                    .keyboardShortcut(.defaultAction)
            }
        }
    }

    private var need: String? { error.data?["need"]?.uint64.map(Format.gigabytes) }

    private var title: String {
        switch error.code {
        case "steam-not-owned": return "This Steam account doesn't own the game"
        case "download-failed": return "The download stopped"
        case "disk-low": return "Make room on this Mac"
        case "signing-failed", "signing-no-team": return "Couldn't use your Apple Account"
        case "xcode-signin-expired": return "Sign in to Xcode again"
        case "apple-agreement-pending": return "Accept Apple's new terms"
        case "signing-allowed-once": return "Click Always Allow this time"
        case "signing-not-allowed": return "Glovebox can't install without this"
        case "signing-certificate-missing": return "This Mac is missing an Apple certificate"
        case "install-failed": return "Couldn't install on your Vision Pro"
        case "game-files-missing": return "Some game files didn't download"
        default: return "Something went wrong"
        }
    }

    private var detail: String {
        switch error.code {
        case "download-failed": return "Check your internet, then try again."
        case "disk-low": return need.map { "You need \($0) free." } ?? "Free up some space, then try again."
        case "signing-failed", "signing-no-team": return "Check that Xcode is still signed in to it."
        case "xcode-signin-expired": return "Xcode's sign-in to your Apple Account expired. It needs your password once more."
        case "apple-agreement-pending": return "Apple updated its developer agreement. Accept it on Apple's website, then come back."
        case "signing-allowed-once": return "So macOS won't ask again."
        case "signing-not-allowed": return "Try again and click Always Allow."
        case "signing-certificate-missing": return "Glovebox couldn't add it from Apple. Check your internet, then try again."
        case "game-files-missing": return "Try again to get them."
        case "install-failed": return "Keep it on and unlocked, then try again."
        case "steam-not-owned": return "Sign in with the account that owns it."
        default: return "Try again. If it keeps happening, copy the diagnostics and send them to us."
        }
    }
}

/// Installed: the app's icon as the headset shows it, and what to do there.
struct DonePage: View {
    @EnvironmentObject var model: InstallerModel

    var body: some View {
        CardScreen {
            HStack(alignment: .bottom, spacing: 16) {
                card
                // The news sign-up stays until it has an answer.
                if model.showsNewsAsk {
                    NewsAskCard(width: 360).transition(.move(edge: .trailing).combined(with: .opacity))
                }
            }
        }
    }

    private var card: some View {
        VStack(alignment: .leading, spacing: 18) {
            HStack(spacing: 18) {
                Image("GloveboxCircle").resizable()
                    .frame(width: 92, height: 92)
                    .shadow(color: .black.opacity(0.3), radius: 12, y: 6)
                    .accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 5) {
                    Text("Ready to play!").font(.system(size: 28, weight: .bold)).tracking(-0.4)
                    Text(model.trustFirst
                         ? "On your Vision Pro, open Settings › General › VPN & Device Management, tap your Apple Account, then Trust. Then open \(model.recipe.headsetAppName)."
                         : "Open \(model.recipe.headsetAppName) on your Vision Pro.")
                        .font(.system(size: 15))
                        .foregroundStyle(.white.opacity(0.75))
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            HStack {
                Spacer()
                Button("Done") { model.doneTapped() }
                    .primaryButton()
                    .keyboardShortcut(.defaultAction)
            }
        }
        .foregroundStyle(.white)
        .padding(.horizontal, 24)
        .padding(.vertical, 22)
        .frame(width: 470, alignment: .leading)
        .glass(RoundedRectangle(cornerRadius: 26, style: .continuous))
        .shadow(color: .black.opacity(0.3), radius: 20, y: 8)
    }
}

/// Coming back: the one thing to do now (update, renew, or nothing), and the reports switch.
struct HomePage: View {
    @EnvironmentObject var model: InstallerModel

    var body: some View {
        CardScreen {
            HStack(alignment: .bottom, spacing: 16) {
                card
                if model.showsNewsAsk {
                    NewsAskCard(width: 360).transition(.move(edge: .trailing).combined(with: .opacity))
                }
            }
        }
    }

    private var card: some View {
        StepCard(title: title, subtitle: subtitle) {
            Toggle("Help improve Glovebox", isOn: Binding(get: { model.shareStutters ?? false }, set: { model.setShareStutters($0) }))
                .toggleStyle(.switch)
        } footer: {
            Button("Copy Diagnostics") { model.copyDiagnostics() }.textLink()
            if !model.newsSignedUp && !model.askingForNews {
                Button("Get News by Email") { model.askForNews() }.textLink()
            }
            Spacer()
            if update {
                Button("Install Update") { model.update() }.primaryButton().keyboardShortcut(.defaultAction)
            } else if renew {
                Button("Renew") { model.update() }.primaryButton().keyboardShortcut(.defaultAction)
            } else {
                Button("Reinstall") { model.update() }.secondaryButton()
            }
        }
    }

    private var update: Bool { model.status?.updateAvailable == true }
    private var daysLeft: Int? { model.status?.daysLeft() }
    private var renew: Bool { model.status?.renewSoon() == true }

    // What these say is about the app on the headset (its name there), never this Mac's app or visionOS.
    private var app: String { model.recipe.headsetAppName }

    private var title: String {
        if update { return "A new version of \(app) is ready" }
        if renew { return (daysLeft ?? 0) < 0 ? "\(app) stopped opening" : "Renew \(app)" }
        return "You're all set"
    }

    private var subtitle: String? {
        if update { return "Install it on your Vision Pro. Keep the headset nearby and awake. Your saves stay." }
        if renew, let d = daysLeft {
            if d < 0 { return "Renew it to play again. It's free." }
            return "It stops opening on your Vision Pro \(d == 0 ? "today" : d == 1 ? "tomorrow" : "in \(d) days"). Renewing is free."
        }
        return model.status?.expires.map { "\(app) works on your Vision Pro until \($0.formatted(date: .long, time: .omitted))." }
    }
}
