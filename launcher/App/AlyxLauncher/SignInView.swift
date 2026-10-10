import SwiftUI

/// The sign-in card's contents: account name and password (Password AutoFill fills both from
/// Apple's Passwords), then Steam Guard, which for most people is one tap of Approve in the
/// Steam app on their phone, the way a Steam Deck signs in. (Steam refuses its QR sign-in links
/// unless they're scanned inside its app, and nothing can show a QR code to a phone from a
/// headset, so password and Approve is the one way in.) The card's size and glass come from
/// `LandingView`, so every state fills the same frame.
struct SignInCard: View {
    @EnvironmentObject var model: LauncherModel
    @State private var account = ""
    @State private var password = ""
    @State private var slowApproval = false
    @FocusState private var focus: Field?
    enum Field { case account, password }

    var body: some View {
        // One container, not a Group: modifiers on a Group apply to each branch separately.
        ZStack {
            switch model.signInStep {
            case .approveInApp(let canEnterCode): approve(canEnterCode: canEnterCode)
            case .enterCode: enterCode
            default: form
            }
        }
        .animation(.spring(duration: 0.45), value: model.signInStep)
        // If no request shows up for a while, say what usually fixes it.
        .task(id: model.signInStep) {
            slowApproval = false
            guard case .approveInApp = model.signInStep else { return }
            try? await Task.sleep(nanoseconds: 30_000_000_000)
            withAnimation { slowApproval = true }
        }
    }

    // MARK: Account and password

    private var form: some View {
        VStack(spacing: 0) {
            // Where the guide steps have their header, so the titles line up across the card.
            JourneySteps(current: 0).padding(.bottom, 20)
            VStack(alignment: .leading, spacing: 8) {
                Text("Sign in to Steam").font(.largeTitle.weight(.bold))
                Text("To confirm you own the game and download it.")
                    .font(.title3)
                    .foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity, alignment: .leading)

            Spacer(minLength: 24)

            VStack(spacing: 12) {
                TextField("Steam account name", text: $account)
                    .textContentType(.username)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .focused($focus, equals: .account)
                    .submitLabel(.next)
                    .onSubmit { focus = .password }
                SecureField("Password", text: $password)
                    .textContentType(.password)
                    .focused($focus, equals: .password)
                    .submitLabel(.go)
                    .onSubmit(submit)
            }
            .textFieldStyle(.roundedBorder)

            Button(action: submit) {
                Group {
                    if model.signInStep == .working { ProgressView() } else { Text("Sign In") }
                }
                .font(.title3.weight(.semibold))
                .frame(maxWidth: .infinity)
            }
            .buttonStyle(PillButtonStyle())
            .disabled(account.isEmpty || password.isEmpty || model.signInStep == .working)
            .padding(.top, 20)

            // What comes next, so the phone step isn't a surprise.
            Label("Then approve it in the Steam app on your phone.", systemImage: "iphone")
                .font(.callout)
                .foregroundStyle(.secondary)
                .padding(.top, 14)

            Spacer(minLength: 24)

            Group {
                if let err = model.signInError {
                    Label(err, systemImage: "exclamationmark.circle.fill").foregroundStyle(.orange)
                } else {
                    Label("Your password is sent only to Steam.", systemImage: "lock.fill").foregroundStyle(.tertiary)
                }
            }
            .font(.footnote)
        }
    }

    // MARK: Steam Guard

    // A header, a picture of what to look for, and a footer with the way out.
    private func approve(canEnterCode: Bool) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            GuideHeader(label: "Steam Guard") { model.cancelSignIn() }
            GuideStep(title: "Approve on your phone",
                      detail: "Open the sign-in request in the Steam app and tap Approve.") {
                PhoneMock()
            } footer: {
                VStack(alignment: .leading, spacing: 14) {
                    if slowApproval {
                        Hint(symbol: "questionmark.circle", text: "No request? Open the Steam app and check the Steam Guard tab.")
                            .transition(.opacity)
                    }
                    WaitingRow(text: "Waiting for approval") {
                        if canEnterCode { Button("Use a Code") { model.useCodeInstead() }.buttonStyle(.borderless) }
                    }
                }
            }
        }
    }

    private var enterCode: some View {
        VStack(alignment: .leading, spacing: 0) {
            GuideHeader(label: "Steam Guard") { model.cancelSignIn() }
            GuideStep(title: "Enter your code", detail: codeHint) {
                CodeBoxes { model.submitCode($0) }
            } footer: {
                if let err = model.signInError {
                    Label(err, systemImage: "exclamationmark.circle.fill").font(.callout).foregroundStyle(.orange)
                }
            }
        }
    }

    private var codeHint: String {
        if case .enterCode(let email?) = model.signInStep { return "Steam emailed it to \(email)." }
        return "Open the Steam app and tap Steam Guard."
    }

    private func submit() {
        guard !account.isEmpty, !password.isEmpty else { return }
        model.signIn(account: account, password: password)
        password = ""
    }
}

/// Five boxes for a Steam Guard code, filled from one hidden field. Submits at five characters.
struct CodeBoxes: View {
    var onComplete: (String) -> Void
    @State private var code = ""
    @FocusState private var focused: Bool

    var body: some View {
        ZStack {
            TextField("", text: $code)
                .textContentType(.oneTimeCode)
                .textInputAutocapitalization(.characters)
                .autocorrectionDisabled()
                .focused($focused)
                .opacity(0.02)
                .onChange(of: code) { _, new in
                    let clean = String(new.uppercased().filter { $0.isLetter || $0.isNumber }.prefix(5))
                    if clean != new { code = clean; return }
                    if clean.count == 5 { onComplete(clean) }
                }
            HStack(spacing: 10) {
                ForEach(0..<5, id: \.self) { i in
                    let chars = Array(code)
                    Text(i < chars.count ? String(chars[i]) : "")
                        .font(.system(size: 32, weight: .semibold, design: .rounded))
                        .frame(width: 60, height: 72)
                        .background(.black.opacity(0.25), in: .rect(cornerRadius: 14))
                        .overlay(RoundedRectangle(cornerRadius: 14)
                            .stroke(i == min(chars.count, 4) && focused ? Color.accentColor : .white.opacity(0.15),
                                    lineWidth: 2))
                }
            }
            .allowsHitTesting(false)
        }
        .contentShape(.rect)
        .onTapGesture { focused = true }
        .onAppear { focused = true }
    }
}
