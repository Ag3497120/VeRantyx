import SwiftUI

/// The one question about reports, asked a few seconds into the first install while the player
/// waits anyway: a big ask, why it helps, and at a glance what we get and what never leaves the
/// headset. Home has a switch to change the answer. The lists match what a report holds
/// (glovebox-vr/glovebox diagnostics/README.md): hitches and pipeline fingerprints, the headset
/// model and visionOS version, app versions and a random install id.
struct ShareAskCard: View {
    var answer: (Bool) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            VStack(alignment: .leading, spacing: 8) {
                Text("Help make Glovebox better")
                    .font(.system(size: 28, weight: .bold))
                    .tracking(-0.4)
                Text("Glovebox is an open-source fan project. Let us see where it stutters, and we'll fix it for everyone.")
                    .font(.system(size: 15))
                    .foregroundStyle(.white.opacity(0.78))
                    .fixedSize(horizontal: false, vertical: true)
            }
            HStack(alignment: .top, spacing: 14) {
                ShareList(title: "We get", shared: true, items: [
                    "Where the game stutters",
                    "Your Vision Pro model and visionOS version",
                ])
                ShareList(title: "Never shared", shared: false, items: [
                    "Your name, email or accounts",
                    "Eye, hand and face tracking",
                    "Camera and room data",
                ])
            }
            HStack(spacing: 12) {
                Button("No Thanks") { answer(false) }
                    .secondaryButton()
                    .keyboardShortcut(.cancelAction)
                Spacer()
                Button("Count Me In") { answer(true) }
                    .primaryButton()
                    .keyboardShortcut(.defaultAction)
            }
        }
        .foregroundStyle(.white)
        .padding(28)
        .frame(width: 560)
        .glass(RoundedRectangle(cornerRadius: 28, style: .continuous), tint: 0.55)
        .shadow(color: .black.opacity(0.45), radius: 30, y: 12)
    }
}

/// What's in a report, or what never is: a heading and a few lines, each with an orange tick or a
/// red cross.
private struct ShareList: View {
    var title: String
    var shared: Bool
    var items: [String]

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(title)
                .font(.system(size: 11, weight: .semibold))
                .tracking(0.6)
                .textCase(.uppercase)
                .foregroundStyle(.white.opacity(0.6))
            ForEach(items, id: \.self) { item in
                HStack(alignment: .firstTextBaseline, spacing: 8) {
                    Image(systemName: shared ? "checkmark.circle.fill" : "xmark.circle.fill")
                        .font(.system(size: 15))
                        .foregroundStyle(.white, shared ? Color.green : Color(red: 1, green: 0.36, blue: 0.36))
                    Text(item)
                        .font(.system(size: 14))
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .topLeading)
        .background(RoundedRectangle(cornerRadius: 16, style: .continuous).fill(.white.opacity(0.08)))
    }
}
