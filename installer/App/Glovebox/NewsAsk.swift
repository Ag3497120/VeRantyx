import SwiftUI

/// The one ask to join the news list (glovebox.vision's sign-up list), laid out the way Apple's own
/// apps ask: the app's mark, one line on what you get, one email field that sends with its arrow or
/// Return, a quiet No Thanks, and a line that says leaving is easy. It sits beside the install's
/// progress in a quiet stretch, stays on the finished page until answered, and opens from Home's link.
/// It never covers anything the player needs.
struct NewsAskCard: View {
    @EnvironmentObject var model: InstallerModel
    var width: CGFloat
    /// Takes the height its row gives it (next to the install's progress card).
    var fill = false
    @State private var email = ""
    @State private var sending = false
    @State private var problem: String?
    @State private var done = false
    @FocusState private var focused: Bool

    var body: some View {
        VStack(spacing: 0) {
            if done {
                VStack(spacing: 12) {
                    Image(systemName: "checkmark.circle.fill")
                        .font(.system(size: 44))
                        .foregroundStyle(.white, Theme.accent)
                    Text("You're on the list.")
                        .font(.system(size: 19, weight: .bold))
                        .tracking(-0.3)
                }
                .transition(.opacity.combined(with: .scale(scale: 0.96)))
            } else {
                ask
            }
        }
        .foregroundStyle(.white)
        .multilineTextAlignment(.center)
        .padding(.horizontal, 26)
        .padding(.vertical, 24)
        .frame(width: width)
        .frame(maxHeight: fill ? .infinity : nil)
        .glass(RoundedRectangle(cornerRadius: 28, style: .continuous))
        .shadow(color: .black.opacity(0.3), radius: 20, y: 8)
    }

    private var ask: some View {
        VStack(spacing: 0) {
            Image("GloveboxCircle").resizable()
                .frame(width: 56, height: 56)
                .overlay(alignment: .bottomTrailing) {
                    Image(systemName: "envelope.fill")
                        .font(.system(size: 11, weight: .bold))
                        .foregroundStyle(.white)
                        .frame(width: 24, height: 24)
                        .background(Circle().fill(Theme.accent.gradient))
                        .overlay(Circle().strokeBorder(.black.opacity(0.35), lineWidth: 2))
                        .offset(x: 4, y: 4)
                }
                .shadow(color: .black.opacity(0.3), radius: 8, y: 4)
                .accessibilityHidden(true)
            Text("Hear from us when more games come to Vision Pro")
                .font(.system(size: 19, weight: .bold))
                .tracking(-0.3)
                .fixedSize(horizontal: false, vertical: true)
                .padding(.top, 14)
            field.padding(.top, 18)
            Text(problem ?? "Unsubscribe anytime.")
                .font(.system(size: 12))
                .foregroundStyle(problem == nil ? Color.white.opacity(0.5) : Color(red: 1, green: 0.55, blue: 0.5))
                .padding(.top, 8)
            Button("No Thanks") { model.answerNews(signedUp: false) }
                .buttonStyle(.plain)
                .font(.system(size: 14, weight: .medium))
                .foregroundStyle(.white.opacity(0.62))
                .padding(.top, 12)
        }
    }

    /// One capsule: the address, and the arrow that sends it.
    private var field: some View {
        HStack(spacing: 8) {
            TextField("Email address", text: $email)
                .textFieldStyle(.plain)
                .textContentType(.emailAddress)
                .font(.system(size: 15))
                .multilineTextAlignment(.leading)
                .focused($focused)
                .focusEffectDisabled()
                .onSubmit(subscribe)
                .onChange(of: email) { problem = nil }
                .disabled(sending)
                .accessibilityLabel("Email address")
            Button(action: subscribe) {
                ZStack {
                    Circle().fill(canSend ? Theme.accent : Color.white.opacity(0.14))
                    if sending {
                        ProgressView().controlSize(.small).environment(\.colorScheme, .light)
                    } else {
                        Image(systemName: "arrow.right")
                            .font(.system(size: 14, weight: .bold))
                            .foregroundStyle(canSend ? Color.black.opacity(0.85) : Color.white.opacity(0.45))
                    }
                }
                .frame(width: 30, height: 30)
            }
            .buttonStyle(.plain)
            .disabled(!canSend)
            .accessibilityLabel("Subscribe")
        }
        .padding(.leading, 16)
        .padding(.trailing, 5)
        .frame(height: 40)
        .background(Capsule().fill(.white.opacity(focused ? 0.14 : 0.1)))
        .overlay(Capsule().strokeBorder(focused ? Theme.accent.opacity(0.9) : Color.white.opacity(0.18), lineWidth: 1))
        .animation(.easeOut(duration: 0.15), value: focused)
    }

    private var canSend: Bool { !sending && !email.trimmingCharacters(in: .whitespaces).isEmpty }

    private func subscribe() {
        let address = email.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !sending, !address.isEmpty else { return }
        guard NewsList.looksLikeEmail(address) else { problem = "Enter your email address."; return }
        sending = true
        Task {
            let outcome = await NewsList.signUp(address)
            sending = false
            switch outcome {
            case .added:
                withAnimation(.easeOut(duration: 0.25)) { done = true }
                try? await Task.sleep(nanoseconds: 2_000_000_000)
                model.answerNews(signedUp: true)
            case .badEmail: problem = "That email doesn't look right."
            case .failed: problem = "Couldn't sign you up. Try again."
            }
        }
    }
}

/// glovebox.vision's sign-up, the one the website's form uses: it checks the address and adds it to the list.
enum NewsList {
    static let signupURL = URL(string: "https://glovebox.vision/api/signup")!

    enum Outcome { case added, badEmail, failed }

    /// The website's own check, so both turn away the same addresses before asking.
    static func looksLikeEmail(_ s: String) -> Bool {
        s.range(of: #"^[^\s@]{1,64}@[^\s@]{1,190}\.[^\s@]{2,63}$"#, options: .regularExpression) != nil
    }

    static func signUp(_ email: String) async -> Outcome {
        var request = URLRequest(url: signupURL, timeoutInterval: 20)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try? JSONSerialization.data(withJSONObject: ["email": email, "src": "mac-app"])
        guard let got = try? await URLSession.shared.data(for: request), let http = got.1 as? HTTPURLResponse else { return .failed }
        switch http.statusCode {
        case 200..<300: return .added
        case 400: return .badEmail
        default: return .failed
        }
    }
}
