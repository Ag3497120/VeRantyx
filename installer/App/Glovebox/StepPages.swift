import SwiftUI
import AppKit
import GloveboxKit

// MARK: Xcode

/// Xcode: get it, finish its setup, add visionOS. The card shows whichever of these is next; only
/// the first two need the player, and Glovebox adds visionOS by itself.
struct XcodePage: View {
    @EnvironmentObject var model: InstallerModel

    var body: some View {
        CardScreen {
            if let p = model.macProblem {
                macCard(p)
            } else if model.xcodeInstallState != .done {
                getCard
            } else if model.xcodeSetupState != .done {
                setupCard
            } else if model.visionOSState != .done {
                visionOSCard
            } else {
                StepCard(title: "Xcode is ready") {
                    XcodeBadge(text: model.setup.results[.xcode]?.value ?? "Xcode", done: true)
                } footer: {
                    Spacer()
                    continueButton
                }
            }
        }
    }

    private var continueButton: some View {
        Button("Continue") { model.continueOn() }
            .primaryButton()
            .keyboardShortcut(.defaultAction)
    }

    @ViewBuilder private var getCard: some View {
        let tooOld = model.setup.results[.xcode]?.code == "xcode-too-old"
        if model.guide.isRunning, model.guide.job == .appStore {
            GuideRunningCard()
        } else if model.xcodeInstallState == .working {
            StepCard(title: model.checkedOnce ? "Xcode is downloading" : "Looking for Xcode",
                     subtitle: model.checkedOnce ? "You can continue while it downloads." : nil) {
                HStack(spacing: 14) {
                    XcodeIcon(size: 56, installed: model.xcodeIcon)
                    ProgressView().controlSize(.small)
                }
            } footer: {
                Button("Open App Store") { model.getXcode() }.textLink()
                Spacer()
                continueButton
            }
        } else {
            StepCard(title: tooOld ? "Update Xcode" : "Get Xcode",
                     subtitle: tooOld ? "We need Xcode 26 or later to build the game." : "We'll use it to build the game for your Vision Pro.") {
                HStack(spacing: 14) {
                    XcodeIcon(size: 56, installed: model.xcodeIcon)
                    Text("Free, from the App Store").foregroundStyle(.white.opacity(0.75))
                }
            } footer: {
                Spacer()
                Button(tooOld ? "Update Xcode" : "Get Xcode") { model.getXcode() }
                    .primaryButton()
                    .keyboardShortcut(.defaultAction)
            }
        }
    }

    @ViewBuilder private var setupCard: some View {
        if model.guide.isRunning, model.guide.job == .setup {
            GuideRunningCard()
        } else {
            setupStartCard
        }
    }

    private var setupStartCard: some View {
        StepCard(title: "Finish setting up Xcode", subtitle: "It takes a few clicks. We'll show you where.") {
            XcodeBadge(text: "Xcode", done: false)
        } footer: {
            Spacer()
            if model.xcodeSetupState == .working {
                CardStatus(text: "Waiting for Xcode", working: true)
            } else if let r = model.setup.results[.xcode] {
                Button("Open Xcode") { model.fix(r) }
                    .primaryButton()
                    .keyboardShortcut(.defaultAction)
            }
        }
    }

    private var visionOSCard: some View {
        let fix = model.autoFix?.code == "visionos-missing" ? model.autoFix : nil
        return StepCard(title: "Adding visionOS to Xcode", subtitle: "You can continue while we do this.") {
            if model.visionOSState == .attention, let r = model.setup.results[.visionOS] {
                HStack {
                    CardError(text: "The download stopped.")
                    Spacer()
                    Button("Try Again") { model.fix(r) }
                }
            } else if let f = fix?.fraction {
                VStack(alignment: .leading, spacing: 6) {
                    ProgressView(value: f)
                    Text(Format.timeLeft(fix?.secondsLeft) ?? "\(Int(f * 100))%")
                        .font(.callout).foregroundStyle(.white.opacity(0.72))
                        .contentTransition(.numericText())
                }
            } else {
                ProgressView().controlSize(.small)
            }
        } footer: {
            Spacer()
            continueButton
        }
    }

    @ViewBuilder private func macCard(_ r: CheckResult) -> some View {
        if r.code == "mac-unsupported" {
            StepCard(title: "This Mac can't build the game", subtitle: "It needs a Mac with Apple silicon.") {
                EmptyView()
            }
        } else {
            StepCard(title: "Make room on this Mac", subtitle: r.detail ?? "You need 15 GB free.") {
                EmptyView()
            } footer: {
                Spacer()
                Button("Open Storage Settings") {
                    NSWorkspace.shared.open(URL(string: r.fix?.url ?? "x-apple.systempreferences:com.apple.settings.Storage")!)
                }
                .primaryButton()
            }
        }
    }
}

/// Xcode's icon with its version, ticked once it's ready.
private struct XcodeBadge: View {
    @EnvironmentObject var model: InstallerModel
    var text: String
    var done: Bool

    var body: some View {
        HStack(spacing: 14) {
            XcodeIcon(size: 56, installed: model.xcodeIcon)
            Text(text).font(.headline)
            if done { StatusMark(state: .done).font(.title3) }
        }
    }
}

// MARK: Apple Account

/// The Apple Account Xcode signs with: a free one works for 7 days at a time, the paid Developer
/// Program for a year. Signing in happens in Xcode, with the guide floating beside it.
struct AccountPage: View {
    @EnvironmentObject var model: InstallerModel
    @EnvironmentObject var guide: XcodeGuide

    var body: some View {
        CardScreen { card }
    }

    @ViewBuilder private var card: some View {
        let teams = model.setup.usableTeams
        if !model.xcodeReady {
            WaitingCard(title: "Sign in to Xcode")
        } else if model.signInState == .attention {
            StepCard(title: "Sign in to Xcode", subtitle: "We'll show you how.") {
                EmptyView()
            } footer: {
                Spacer()
                Button("Sign In") { guide.start(.signIn) }
                    .primaryButton()
                    .keyboardShortcut(.defaultAction)
            }
        } else if model.signInState == .working {
            if guide.isRunning {
                GuideRunningCard()
            } else {
                StepCard(title: "Checking your Apple Account") {
                    ProgressView().controlSize(.small)
                }
            }
        } else if model.programState == .attention, model.setup.openChoice != nil, teams.count > 1 {
            StepCard(title: "Which team should we use?") {
                Picker("Team", selection: Binding(get: { model.setup.team?.id ?? "" }, set: { model.chooseTeam($0) })) {
                    ForEach(teams) { t in Text(t.name).tag(t.id) }
                }
                .labelsHidden()
                .pickerStyle(.radioGroup)
            } footer: {
                Spacer()
                Button("Continue") { model.continueOn() }
                    .primaryButton()
                    .keyboardShortcut(.defaultAction)
                    .disabled(model.state(of: .account) != .done)
            }
        } else if model.programState == .attention {
            StepCard(title: "Join the Apple Developer Program",
                     subtitle: "$99 a year. It unlocks the RAM the game needs. Apple usually approves within a day.") {
                if let name = model.accountName {
                    Label("Signed in as \(name)", systemImage: "person.crop.circle.fill")
                        .foregroundStyle(.white.opacity(0.72))
                }
            } footer: {
                Button("Check Again") { guide.start(.refresh) }.secondaryButton()
                Spacer()
                Button("Join") { NSWorkspace.shared.open(URL(string: "https://developer.apple.com/programs/enroll/")!) }
                    .primaryButton()
                    .keyboardShortcut(.defaultAction)
            }
        } else {
            StepCard(title: "You're signed in", subtitle: model.setup.team?.name ?? model.accountName) {
                if model.setup.team?.paid == false {
                    Label("Free account: reinstall every 7 days", systemImage: "clock.arrow.circlepath")
                        .foregroundStyle(.white.opacity(0.85))
                } else {
                    Label("Apple Developer Program", systemImage: "checkmark.seal.fill")
                        .foregroundStyle(.white.opacity(0.85))
                }
            } footer: {
                Spacer()
                Button("Continue") { model.continueOn() }
                    .primaryButton()
                    .keyboardShortcut(.defaultAction)
            }
        }
    }
}

// MARK: Vision Pro

/// Pair the headset with this Mac, then turn on Developer Mode. Pairing has the guide beside
/// Device Hub; Developer Mode happens only on the headset, so the card shows what the player will
/// see there.
struct HeadsetPage: View {
    @EnvironmentObject var model: InstallerModel
    @EnvironmentObject var guide: XcodeGuide

    var body: some View {
        CardScreen { card }
    }

    @ViewBuilder private var card: some View {
        if !model.xcodeReady {
            WaitingCard(title: "Pair your Vision Pro")
        } else if model.pairState == .attention {
            if guide.isRunning && guide.job == .pair {
                GuideRunningCard()
            } else {
                StepCard(title: "Pair your Vision Pro", subtitle: "We'll show you how to connect it over Wi-Fi.") {
                    HStack(spacing: 14) {
                        Image(systemName: "laptopcomputer").font(.system(size: 40, weight: .light))
                        Image(systemName: "wifi").font(.system(size: 18, weight: .semibold)).foregroundStyle(.white.opacity(0.5))
                        Image(systemName: Symbols.available("visionpro", fallback: "eyeglasses")).font(.system(size: 40, weight: .light))
                    }
                    .foregroundStyle(.white.opacity(0.85))
                } footer: {
                    Spacer()
                    Button("Pair") { model.openPairing() }
                        .primaryButton()
                        .keyboardShortcut(.defaultAction)
                }
            }
        } else if model.pairState == .working {
            StepCard(title: "Checking your Vision Pro") {
                ProgressView().controlSize(.small)
            }
        } else if model.developerModeState != .done {
            StepCard(title: "Turn on Developer Mode", subtitle: "On your Vision Pro.", width: 600) {
                if model.developerModeState == .attention {
                    DeveloperModeGuide()
                } else {
                    ProgressView().controlSize(.small)
                }
            } footer: {
                CardStatus(text: "We'll continue once it's on.", working: true)
                Spacer()
            }
        } else {
            StepCard(title: "\(model.headsetName) is ready") {
                EmptyView()
            } footer: {
                Spacer()
                Button("Continue") { model.continueOn() }
                    .primaryButton()
                    .keyboardShortcut(.defaultAction)
            }
        }
    }
}

// MARK: Shared cards

/// A step that needs Xcode first.
struct WaitingCard: View {
    @EnvironmentObject var model: InstallerModel
    var title: String

    var body: some View {
        StepCard(title: title, subtitle: model.xcodeInstallState == .working ? "Once Xcode finishes downloading." : "Xcode comes first.") {
            EmptyView()
        } footer: {
            Spacer()
            Button("Go to Xcode") { model.go(.xcode) }.secondaryButton()
        }
    }
}

/// While the guide runs beside Xcode or Device Hub. Nothing to do here: both opened by
/// themselves, so bringing them back is only a quiet link, in case the player lost them.
struct GuideRunningCard: View {
    @EnvironmentObject var guide: XcodeGuide

    var body: some View {
        StepCard(title: "Follow the steps next to \(app)", subtitle: "We'll come back here when you're done.") {
            EmptyView()
        } footer: {
            Button("Stop") { guide.stop() }.textLink()
            Spacer()
            Button("Show It Again") { guide.bringForward() }.textLink()
        }
    }

    private var app: String {
        switch guide.job {
        case .appStore: return "the App Store"
        case .pair where guide.xcodeMajor >= 27: return "Device Hub"
        default: return "Xcode"
        }
    }
}
