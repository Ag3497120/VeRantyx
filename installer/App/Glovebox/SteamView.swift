import SwiftUI
import AppKit
import CoreImage.CIFilterBuiltins
import GloveboxKit
import SteamLauncherKit

/// Steam: scan a QR code with the Steam app on the phone, then Approve, the way the Steam app
/// itself signs in. Account and password (then Approve) is the other way. It proves the player
/// owns the game, and the game files start downloading right away.
struct SteamPage: View {
    @EnvironmentObject var model: InstallerModel
    @EnvironmentObject var steam: SteamAccount
    @State private var account = ""
    @State private var password = ""
    /// Show Password: off until the player asks, and off again whenever Glovebox leaves the front.
    @State private var reveal = false
    @FocusState private var focus: Field?
    enum Field { case account, password }

    var body: some View {
        CardScreen { card }
            .onAppear {
                if account.isEmpty { account = steam.accountName }
                // The demo screens for the too-long note: a paste far longer than any password, hidden and shown.
                if Demo.current == "steam-password-toolong" || Demo.current == "steam-password-revealed" {
                    account = "gordon_freeman"
                    password = String(repeating: "x", count: 300)
                    reveal = Demo.current == "steam-password-revealed"
                }
                focus = account.isEmpty ? .account : .password
            }
            .task(id: steam.method == .qr && steam.step == .form) { steam.startQR() }
            .onDisappear { steam.stopQR() }
    }

    @ViewBuilder private var card: some View {
        switch steam.step {
        case .form, .working:
            if steam.method == .qr { qr } else { form }
        case .approve(let canEnterCode): approve(canEnterCode: canEnterCode)
        case .code(let email): code(email: email)
        case .checkingOwnership:
            StepCard(title: "Checking your library") { ProgressView().controlSize(.small) }
        case .owned: owned
        case .notOwned: notOwned
        }
    }

    // MARK: QR code

    private var qr: some View {
        StepCard(title: "Sign in to Steam", subtitle: "We use it to download the game for you.") {
            HStack(spacing: 22) {
                QRCard(text: steam.qrURL, failed: steam.qrURL == nil && steam.error != nil, size: 148) {
                    steam.error = nil
                    steam.startQR()
                }
                VStack(alignment: .leading, spacing: 12) {
                    ScanStep(number: 1, text: Text("Open the Steam app"))
                    ScanStep(number: 2, text: Text("Tap \(Image(systemName: "shield.lefthalf.filled")) Steam Guard"))
                    ScanStep(number: 3, text: Text("Scan this code"))
                }
            }
            // A tester scanned with the iPhone's camera: Steam took him to setting up Steam Guard, which failed.
            // Steam turns down this code from anywhere but its own app, and the app scans only once Steam Guard is set up.
            Text("Scan with the Steam app, not your camera. It needs Steam Guard set up.")
                .font(.callout)
                .foregroundStyle(.white.opacity(0.72))
                .fixedSize(horizontal: false, vertical: true)
            if let e = steam.error { CardError(text: e) }
        } footer: {
            Spacer()
            Button("Sign In with Password") { steam.usePassword() }.secondaryButton()
        }
    }

    // MARK: Account and password

    private var form: some View {
        StepCard(title: "Sign in to Steam", subtitle: "Steam may email you a code, or ask you to approve in the Steam app.") {
            VStack(alignment: .leading, spacing: 10) {
                VStack(spacing: 10) {
                    TextField("Account name", text: $account)
                        .textContentType(.username)
                        .focused($focus, equals: .account)
                        .onSubmit { focus = .password }
                    passwordField
                }
                .textFieldStyle(.roundedBorder)
                .controlSize(.large)
                // The Mac's own way to show a password (Wi-Fi's join sheet): a checkbox under the field. An eye
                // inside the field sat on top of the text, which a rounded field can't keep clear of.
                Toggle("Show password", isOn: Binding(get: { reveal }, set: { on in
                    // Typing goes on in the field that takes its place.
                    let typing = focus == .password
                    reveal = on
                    if typing { DispatchQueue.main.async { focus = .password } }
                }))
                .toggleStyle(.checkbox)
            }
            .disabled(steam.step == .working)
            if tooLong {
                CardError(text: "The password is too long. Please make sure you entered it correctly.")
            } else if let e = steam.error {
                CardError(text: e)
            }
        } footer: {
            Button("Use the QR code instead") { steam.useQR() }.textLink().disabled(steam.step == .working)
            Spacer()
            Button(action: submit) {
                if steam.step == .working { ProgressView().controlSize(.small) } else { Text("Sign In") }
            }
            .primaryButton()
            .keyboardShortcut(.defaultAction)
            .disabled(account.isEmpty || password.isEmpty || tooLong || steam.step == .working)
        }
    }

    /// The password: dots unless the player asks to see it (Show password, under it). Shown, it's still a password
    /// to the system: no autocorrect, and AutoFill knows it.
    private var passwordField: some View {
        Group {
            if reveal {
                TextField("Password", text: $password)
                    .autocorrectionDisabled()
            } else {
                SecureField("Password", text: $password)
            }
        }
        .textContentType(.password)
        .focused($focus, equals: .password)
        .onSubmit(submit)
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didResignActiveNotification)) { _ in reveal = false }
    }

    /// More in the password field than any Steam password: shown as it happens, and Sign In waits.
    private var tooLong: Bool { SteamAuth.passwordTooLong(password) }

    private func submit() {
        guard !account.isEmpty, !password.isEmpty, !tooLong, steam.step != .working else { return }
        steam.signIn(account: account.trimmingCharacters(in: .whitespaces), password: password)
        password = ""
        reveal = false
    }

    // MARK: Steam Guard

    private func approve(canEnterCode: Bool) -> some View {
        StepCard(title: "Approve on your phone", subtitle: "Tap Approve in the Steam app.") {
            HStack(spacing: 18) {
                PhoneApproveArt().frame(width: 74, height: 110)
                ProgressView().controlSize(.small)
            }
        } footer: {
            if canEnterCode { Button("Enter a code instead") { steam.useCode() }.textLink() }
            Spacer()
            Button("Cancel") { steam.cancel() }.secondaryButton()
        }
    }

    private func code(email: String?) -> some View {
        StepCard(title: "Enter the Steam Guard code",
                 subtitle: email.map { "Steam emailed it to \($0)." } ?? "It's in the Steam app.") {
            CodeBoxes(length: 5) { steam.submitCode($0) }
            if let e = steam.error { CardError(text: e) }
        } footer: {
            Spacer()
            Button("Cancel") { steam.cancel() }.secondaryButton()
        }
    }

    // MARK: Signed in

    /// Owned: the Mac's part of the game downloads in the background meanwhile.
    private var owned: some View {
        StepCard(title: "You own \(model.recipe.title)!", subtitle: ownedSubtitle) {
            if model.download.error != nil {
                HStack {
                    CardError(text: "The download stopped.")
                    Spacer()
                    Button("Try Again") { model.startDownload() }
                }
            } else if model.download.running {
                VStack(alignment: .leading, spacing: 6) {
                    ProgressView(value: model.download.fraction)
                    if let left = Format.timeLeft(model.download.secondsLeft) {
                        Text(left).font(.callout).foregroundStyle(.white.opacity(0.72)).contentTransition(.numericText())
                    }
                }
            }
        } footer: {
            Button("Not \(steam.accountName)?") { model.useAnotherSteamAccount() }.textLink()
            Spacer()
            Button("Continue") { model.continueOn() }.primaryButton().keyboardShortcut(.defaultAction)
        }
    }

    private var ownedSubtitle: String? {
        if model.download.done { return "Downloaded." }
        if model.download.running { return "Downloading. You can continue." }
        return nil
    }

    private var notOwned: some View {
        StepCard(title: "\(steam.accountName) doesn't own \(model.recipe.title)",
                 subtitle: "Buy it, or use the account that owns it.") {
            EmptyView()
        } footer: {
            Button("Use Another Account") { model.useAnotherSteamAccount() }.secondaryButton()
            Spacer()
            Button("Get It on Steam") { NSWorkspace.shared.open(model.recipe.storeURL) }
                .primaryButton()
                .keyboardShortcut(.defaultAction)
        }
    }
}

/// Boxes for a code, filled from one hidden field. Sends at the last character.
struct CodeBoxes: View {
    var length: Int
    var onComplete: (String) -> Void
    @State private var code = ""
    @FocusState private var focused: Bool

    var body: some View {
        ZStack {
            TextField("", text: $code)
                .textFieldStyle(.plain)
                .focused($focused)
                .opacity(0.01)
                .onChange(of: code) { _, new in
                    let clean = String(new.uppercased().filter { $0.isLetter || $0.isNumber }.prefix(length))
                    if clean != new { code = clean; return }
                    if clean.count == length { onComplete(clean) }
                }
            HStack(spacing: 8) {
                ForEach(0..<length, id: \.self) { i in
                    let chars = Array(code)
                    Text(i < chars.count ? String(chars[i]) : "")
                        .font(.system(size: 26, weight: .semibold, design: .rounded))
                        .frame(width: 44, height: 54)
                        .background(RoundedRectangle(cornerRadius: 10, style: .continuous).fill(.background))
                        .overlay(RoundedRectangle(cornerRadius: 10, style: .continuous)
                            .strokeBorder(i == min(chars.count, length - 1) && focused ? Color.accentColor : Color.secondary.opacity(0.35),
                                          lineWidth: i == min(chars.count, length - 1) && focused ? 2 : 1))
                }
            }
            .allowsHitTesting(false)
        }
        .contentShape(Rectangle())
        .onTapGesture { focused = true }
        .onAppear { focused = true }
    }
}

/// The sign-in QR code on white, the way the Steam app scans it best. A spinner until Steam sends
/// the code; Try Again when it couldn't.
struct QRCard: View {
    var text: String?
    var failed: Bool
    var size: CGFloat = 176
    var retry: () -> Void

    var body: some View {
        RoundedRectangle(cornerRadius: 16, style: .continuous)
            .fill(.white)
            .shadow(color: .black.opacity(0.12), radius: 6, y: 2)
            .overlay {
                if let text, let image = Self.image(text) {
                    Image(decorative: image, scale: 1)
                        .interpolation(.none)
                        .resizable()
                        .padding(size * 0.07)
                        .transition(.opacity)
                } else if failed {
                    VStack(spacing: 10) {
                        Image(systemName: "wifi.exclamationmark").font(.title).foregroundStyle(.secondary)
                        Button("Try Again", action: retry)
                    }
                    .environment(\.colorScheme, .light)
                } else {
                    ProgressView().environment(\.colorScheme, .light)
                }
            }
            .frame(width: size, height: size)
            .animation(.easeInOut(duration: 0.25), value: text)
    }

    static func image(_ text: String) -> CGImage? {
        let filter = CIFilter.qrCodeGenerator()
        filter.message = Data(text.utf8)
        filter.correctionLevel = "M"
        guard let out = filter.outputImage else { return nil }
        return CIContext().createCGImage(out, from: out.extent)
    }
}

/// One numbered step on the phone.
struct ScanStep: View {
    var number: Int
    var text: Text

    var body: some View {
        HStack(spacing: 10) {
            Text("\(number)")
                .font(.callout.weight(.semibold).monospacedDigit())
                .foregroundStyle(.white.opacity(0.8))
                .frame(width: 22, height: 22)
                .background(Circle().fill(.white.opacity(0.14)))
            text
        }
    }
}
