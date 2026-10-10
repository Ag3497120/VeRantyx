import SwiftUI
import AppKit
import GloveboxKit

// The window's look: the game's art fills it, the steps run along the top on glass, and each step
// is one glass card over the art, showing one thing to do. Liquid Glass is made to sit over rich
// images like this, and one card at a time keeps setup from feeling like a settings dialog.

extension View {
    /// Liquid Glass on macOS 26 and later, a dark material before, with a veil of black between it
    /// and the content so white text reads over any art. (Tinting the glass itself black made it
    /// opaque.)
    @ViewBuilder func glass<S: Shape>(_ shape: S, tint: Double = 0.3) -> some View {
        if #available(macOS 26.0, *) {
            background(shape.fill(Color.black.opacity(tint)))
                .glassEffect(.regular, in: shape)
        } else {
            background {
                ZStack {
                    shape.fill(.ultraThinMaterial)
                    shape.fill(Color.black.opacity(tint))
                }
            }
            .overlay(shape.stroke(Color.white.opacity(0.16), lineWidth: 1))
        }
    }
}

/// The game's art behind everything: its library hero, a still (moving footage behind the steps
/// pulls the eye off them). Darker at the top for the steps and at the bottom for the card.
struct Backdrop: View {
    var appID: UInt32
    @StateObject private var art: StoreArt

    init(appID: UInt32) {
        self.appID = appID
        _art = StateObject(wrappedValue: StoreArt.of(appID))
    }

    var body: some View {
        // The art sits in overlays on a plain base, so a wide image can't widen the window.
        Color(red: 0.05, green: 0.07, blue: 0.07)
            .overlay {
                if let hero = art.hero {
                    Image(nsImage: hero).resizable().aspectRatio(contentMode: .fill)
                        .transition(.opacity)
                }
            }
            .overlay {
                LinearGradient(stops: [
                    .init(color: .black.opacity(0.4), location: 0),
                    .init(color: .clear, location: 0.3),
                    .init(color: .clear, location: 0.45),
                    .init(color: .black.opacity(0.65), location: 1),
                ], startPoint: .top, endPoint: .bottom)
            }
            .clipped()
            .onAppear { art.load() }
            .accessibilityHidden(true)
    }
}

/// The game's name set in type with "on Vision Pro." in the logo's orange under it, like
/// glovebox.vision and the teaser.
struct GameTitle: View {
    var title: String

    var body: some View {
        VStack(alignment: .trailing, spacing: -3) {
            Text(title).foregroundStyle(.white)
            Text("on Vision Pro.").foregroundStyle(Theme.accent)
        }
        .font(.system(size: 42, weight: .heavy))
        .tracking(-1.4)
        .lineLimit(1)
        .fixedSize()
        .shadow(color: .black.opacity(0.45), radius: 16, y: 4)
        .accessibilityElement(children: .combine)
    }
}

/// Glovebox's mark and name at the top left, beside the window's buttons: the frame around
/// whichever game it installs.
struct GloveboxChip: View {
    var body: some View {
        HStack(spacing: 7) {
            Image("GloveboxMark").resizable().frame(width: 22, height: 22)
            Text("Glovebox").font(.system(size: 14, weight: .bold))
        }
        .foregroundStyle(.white)
        .padding(.leading, 4)
        .padding(.trailing, 12)
        .padding(.vertical, 4)
        .glass(Capsule(), tint: 0.15)
        .accessibilityElement(children: .combine)
    }
}

/// A page: the game's title at the top right and one card over the art, at the bottom left, or
/// centered for Install.
struct CardScreen<Card: View>: View {
    enum Layout { case side, center }
    var layout: Layout = .side
    @ViewBuilder var card: Card
    @EnvironmentObject var model: InstallerModel

    var body: some View {
        ZStack(alignment: layout == .side ? .bottomLeading : .bottom) {
            Color.clear
            card
        }
        .overlay(alignment: .topTrailing) {
            GameTitle(title: model.recipe.title)
                .padding(.top, 16)
                .padding(.trailing, 12)
                .allowsHitTesting(false)
        }
    }
}

/// One step's card: what to do, a few words more when they help, then the button that does it.
struct StepCard<Content: View, Footer: View>: View {
    var title: String
    var subtitle: String?
    /// Nil fills the window's width.
    var width: CGFloat? = 470
    @ViewBuilder var content: Content
    @ViewBuilder var footer: Footer

    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            VStack(alignment: .leading, spacing: 5) {
                Text(title)
                    .font(.system(size: 26, weight: .bold))
                    .tracking(-0.4)
                    .fixedSize(horizontal: false, vertical: true)
                if let subtitle {
                    Text(subtitle)
                        .font(.system(size: 15))
                        .foregroundStyle(.white.opacity(0.75))
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            content
            HStack(spacing: 12) { footer }
        }
        .foregroundStyle(.white)
        .padding(.horizontal, 24)
        .padding(.vertical, 22)
        .frame(width: width, alignment: .leading)
        .frame(maxWidth: width == nil ? .infinity : nil, alignment: .leading)
        .glass(RoundedRectangle(cornerRadius: 26, style: .continuous))
        .shadow(color: .black.opacity(0.3), radius: 20, y: 8)
    }
}

extension StepCard where Footer == EmptyView {
    init(title: String, subtitle: String? = nil, width: CGFloat? = 470, @ViewBuilder content: () -> Content) {
        self.init(title: title, subtitle: subtitle, width: width, content: content) { EmptyView() }
    }
}

/// A few words on what's happening, with a spinner or a symbol, at the left of a card's footer.
struct CardStatus: View {
    var text: String
    var symbol: String?
    var working = false

    var body: some View {
        HStack(spacing: 8) {
            if working {
                ProgressView().controlSize(.small)
            } else if let symbol {
                Image(systemName: symbol).foregroundStyle(.white.opacity(0.6))
            }
            Text(text).foregroundStyle(.white.opacity(0.72)).lineLimit(2)
        }
        .font(.callout)
    }
}

/// An error line in a card.
struct CardError: View {
    var text: String
    var body: some View {
        Label(text, systemImage: "exclamationmark.circle.fill")
            .font(.callout)
            .foregroundStyle(Color(red: 1, green: 0.55, blue: 0.5))
            .fixedSize(horizontal: false, vertical: true)
    }
}

/// The steps along the top, on glass: done ones ticked, the current one bright. Click one to go
/// there.
struct StepBar: View {
    @EnvironmentObject var model: InstallerModel

    var body: some View {
        HStack(spacing: 2) {
            ForEach(Array(InstallerModel.Step.allCases.dropFirst())) { s in
                Button { model.select(s) } label: { item(s) }
                    .buttonStyle(.plain)
                    .help(model.caption(of: s) ?? "")
            }
        }
        .padding(4)
        .glass(Capsule(), tint: 0.15)
        .animation(.spring(duration: 0.35), value: model.step)
    }

    private func item(_ s: InstallerModel.Step) -> some View {
        let current = model.step == s
        let state = model.state(of: s)
        return HStack(spacing: 5) {
            Group {
                switch state {
                case .done:
                    Image(systemName: "checkmark").font(.system(size: 10, weight: .heavy)).foregroundStyle(Theme.accent)
                case .attention where !current:
                    Circle().fill(Theme.accent).frame(width: 6, height: 6)
                case .working where !current:
                    ProgressView().controlSize(.mini).scaleEffect(0.7).frame(width: 10, height: 10)
                default:
                    if current { Circle().fill(Theme.accent).frame(width: 6, height: 6).shadow(color: Theme.accent, radius: 4) }
                }
            }
            Text(model.title(of: s))
        }
        .font(.system(size: 12, weight: .medium))
        .foregroundStyle(current ? Color.white : Color.white.opacity(state == .done ? 0.8 : 0.6))
        .padding(.horizontal, 11)
        .padding(.vertical, 5)
        .background { if current { Capsule().fill(.white.opacity(0.18)) } }
        .contentShape(Capsule())
    }
}

/// How far the install is: the logo's orange port filling up around the glove.
struct GloveRing: View {
    var fraction: Double
    var size: CGFloat = 132

    var body: some View {
        let line = size * 0.075
        ZStack {
            Circle().stroke(.white.opacity(0.16), lineWidth: line).padding(line / 2)
            Circle()
                .trim(from: 0, to: max(0.001, min(1, fraction)))
                .stroke(Theme.accent, style: StrokeStyle(lineWidth: line, lineCap: .round))
                .rotationEffect(.degrees(-90))
                .padding(line / 2)
                .shadow(color: Theme.accent.opacity(0.6), radius: 8)
            Circle()
                .fill(RadialGradient(colors: [.white.opacity(0.16), .black.opacity(0.35)],
                                     center: UnitPoint(x: 0.5, y: 0.4), startRadius: 0, endRadius: size * 0.45))
                .overlay(Circle().strokeBorder(.white.opacity(0.1)))
                .padding(line * 1.7)
            Image("Glove").resizable().aspectRatio(contentMode: .fit)
                .frame(width: size * 0.64)
                .offset(x: size * 0.02)
                .shadow(color: .black.opacity(0.35), radius: 6, y: 4)
        }
        .frame(width: size, height: size)
        .animation(.easeInOut(duration: 0.4), value: fraction)
        .accessibilityElement()
        .accessibilityLabel("Installing")
        .accessibilityValue("\(Int(fraction * 100)) percent")
    }
}

/// One thing the player needs, on the first page.
struct NeedRow: View {
    var symbol: String
    var text: String

    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: Symbols.available(symbol, fallback: "checkmark.circle"))
                .font(.system(size: 15, weight: .semibold))
                .foregroundStyle(Theme.accent)
                .frame(width: 24)
            Text(text).font(.system(size: 14.5))
        }
    }
}
