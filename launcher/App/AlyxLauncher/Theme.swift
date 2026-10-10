import SwiftUI

/// Glovebox's look, as on glovebox.vision and the Mac installer: the orange of the logo's port for
/// progress and done, white pill buttons, and the game's name set in type.
enum Brand {
    static let orange = Color(red: 1, green: 0.624, blue: 0.18)        // #ff9f2e
    static let link = Color(red: 1, green: 0.765, blue: 0.478)
}

/// Glovebox's mark and name, on glass: the frame around the game.
struct GloveboxChip: View {
    var body: some View {
        HStack(spacing: 10) {
            Image(uiImage: GloveboxArt.mark).resizable().frame(width: 32, height: 32)
            Text("Glovebox").font(.title3.weight(.bold))
        }
        .padding(.leading, 6).padding(.trailing, 18).padding(.vertical, 6)
        .glassBackgroundEffect()
        .accessibilityElement(children: .combine)
    }
}

/// The game's name with "on Vision Pro." in orange under it, like glovebox.vision.
struct GameTitle: View {
    let title: String
    var size: CGFloat = 56

    var body: some View {
        VStack(alignment: .leading, spacing: -size * 0.06) {
            Text(title).foregroundStyle(.white)
            Text("on Vision Pro.").foregroundStyle(Brand.orange)
        }
        .font(.system(size: size, weight: .heavy))
        .tracking(-size * 0.03)
        .lineLimit(1)
        .minimumScaleFactor(0.6)
        .shadow(color: .black.opacity(0.5), radius: 20)
        .accessibilityElement(children: .combine)
    }
}

/// Glovebox's buttons: a white pill with black words for the one thing to do, see-through glass
/// for the other. Both light up under the player's gaze.
struct PillButtonStyle: ButtonStyle {
    var prominent = true
    @Environment(\.isEnabled) private var enabled

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            // A spinner in the label draws dark on the white pill.
            .environment(\.colorScheme, prominent ? .light : .dark)
            .foregroundStyle(prominent ? Color.black : .white)
            .padding(.horizontal, 24)
            .padding(.vertical, 14)
            .background(prominent ? AnyShapeStyle(Color.white) : AnyShapeStyle(Color.white.opacity(0.16)), in: .capsule)
            .contentShape(.hoverEffect, .capsule)
            .hoverEffect(.highlight)
            .opacity(enabled ? 1 : 0.45)
            .scaleEffect(configuration.isPressed ? 0.97 : 1)
            .animation(.easeOut(duration: 0.12), value: configuration.isPressed)
    }
}

/// Steam's library palette, adapted for glass on visionOS.
enum Steam {
    static let navy = Color(red: 0.09, green: 0.10, blue: 0.13)        // #171a21
    static let panel = Color(red: 0.11, green: 0.16, blue: 0.22)       // #1b2838
    static let blue = Color(red: 0.40, green: 0.75, blue: 0.96)        // #66c0f4
    static let brightBlue = Color(red: 0.10, green: 0.62, blue: 1.00)  // #1a9fff
    static let text = Color(red: 0.78, green: 0.84, blue: 0.87)        // #c7d5e0
    static let dim = Color(red: 0.55, green: 0.59, blue: 0.63)
    /// Play button green, a shade deeper than Steam's so white text stays crisp on glass.
    static let play = Color(red: 0.30, green: 0.64, blue: 0.16)
}

/// The game's library art from Steam's CDN, as the Steam client shows it.
struct HeroArt: View {
    let url: URL
    var body: some View {
        AsyncImage(url: url, transaction: Transaction(animation: .easeOut(duration: 0.6))) { phase in
            switch phase {
            case .success(let image): image.resizable().scaledToFill()
            default:
                LinearGradient(colors: [Color(red: 0.25, green: 0.18, blue: 0.10), Steam.navy], startPoint: .topTrailing, endPoint: .bottomLeading)
            }
        }
    }
}

extension UInt64 {
    var bytes: String { ByteCountFormatter.string(fromByteCount: Int64(self), countStyle: .file) }
}

/// "12 min left", "1 hr 5 min left", "Less than a minute left".
func timeLeftText(_ seconds: Double) -> String {
    if seconds < 60 { return "Less than a minute left" }
    let minutes = Int((seconds / 60).rounded(.up))
    if minutes < 60 { return "\(minutes) min left" }
    let h = minutes / 60, m = minutes % 60
    return m == 0 ? "\(h) hr left" : "\(h) hr \(m) min left"
}

/// The whole way to playing, for someone new: Sign in, Download, Play. It opens the sign-in card
/// and stays on the game's page until the first Play, so each hard part reads as one of three
/// short steps, and the done ones turn orange.
struct JourneySteps: View {
    /// 0 Sign in, 1 Download, 2 Play.
    let current: Int
    private static let steps = ["Sign in", "Download", "Play"]

    var body: some View {
        HStack(spacing: 10) {
            ForEach(Self.steps.indices, id: \.self) { i in
                if i > 0 {
                    Capsule()
                        .fill(i <= current ? Brand.orange.opacity(0.85) : Color.white.opacity(0.18))
                        .frame(minWidth: 12, maxWidth: .infinity)
                        .frame(height: 2)
                }
                step(i)
            }
        }
        .frame(height: 44)
        .animation(.spring(duration: 0.5), value: current)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("Step \(current + 1) of 3: \(Self.steps[current])")
    }

    private func step(_ i: Int) -> some View {
        HStack(spacing: 8) {
            ZStack {
                Circle().fill(i <= current ? Brand.orange : Color.white.opacity(0.12))
                if i < current {
                    Image(systemName: "checkmark").font(.footnote.weight(.bold)).foregroundStyle(.black)
                        .transition(.scale.combined(with: .opacity))
                } else {
                    Text("\(i + 1)").font(.subheadline.weight(.bold))
                        .foregroundStyle(i == current ? AnyShapeStyle(.black) : AnyShapeStyle(.secondary))
                }
            }
            .frame(width: 28, height: 28)
            Text(Self.steps[i])
                .font(.subheadline.weight(i == current ? .semibold : .medium))
                .foregroundStyle(i == current ? .primary : .secondary)
        }
        .fixedSize()
    }
}
