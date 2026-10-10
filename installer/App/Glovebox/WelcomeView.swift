import SwiftUI
import GloveboxKit

/// The first page: the game playing behind, what the player will need, and one big button. The
/// small print is glovebox.vision's.
struct WelcomePage: View {
    @EnvironmentObject var model: InstallerModel

    var body: some View {
        CardScreen {
            StepCard(title: "Let's get you set up!") {
                VStack(alignment: .leading, spacing: 9) {
                    Text("You'll need")
                        .font(.system(size: 11, weight: .semibold))
                        .tracking(0.6)
                        .textCase(.uppercase)
                        .foregroundStyle(.white.opacity(0.55))
                    NeedRow(symbol: "visionpro.fill", text: "Apple Vision Pro (M2 or M5)")
                    if model.recipe.needsSenseControllers {
                        NeedRow(symbol: "gamecontroller.fill", text: "PSVR2 controllers")
                    }
                    NeedRow(symbol: "checkmark.seal.fill", text: "\(model.recipe.title) in your Steam library")
                }
            } footer: {
                Spacer()
                Button("Let's Go!") { model.getStarted() }
                    .primaryButton()
                    .keyboardShortcut(.defaultAction)
            }
        }
        .overlay(alignment: .bottomTrailing) {
            Text("Open source. Not affiliated with Valve or Apple.")
                .font(.system(size: 11.5, weight: .medium))
                .foregroundStyle(.white.opacity(0.5))
                .padding(.bottom, 4)
        }
    }
}
