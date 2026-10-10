import Combine
import SwiftUI

/// The first screen someone sees after installing from TestFlight, knowing nothing: the game,
/// full-bleed and moving, what's special about it, and one clear next step (sign in to Steam,
/// and why). The two columns share one height, so the logo lines up with the card's top and
/// the screenshot strip with its bottom; the card keeps that frame in every state.
struct LandingView: View {
    @EnvironmentObject var model: LauncherModel
    @State private var shot = 0
    private let timer = Timer.publish(every: 6, on: .main, in: .common).autoconnect()

    static let contentHeight: CGFloat = 680
    static let cardWidth: CGFloat = 480

    var body: some View {
        ZStack {
            ScreenshotBackdrop(urls: model.profile.screenshotURLs, index: shot)
            LinearGradient(stops: [.init(color: .black.opacity(0.8), location: 0),
                                   .init(color: .black.opacity(0.35), location: 0.55),
                                   .init(color: .black.opacity(0.5), location: 1)],
                           startPoint: .leading, endPoint: .trailing)
            LinearGradient(colors: [.clear, .black.opacity(0.55)], startPoint: .center, endPoint: .bottom)

            HStack(alignment: .top, spacing: 48) {
                pitch
                Spacer(minLength: 0)
                card
            }
            .frame(height: Self.contentHeight)
            .padding(.horizontal, 64)
        }
        .onReceive(timer) { _ in
            guard !model.profile.screenshotURLs.isEmpty else { return }
            withAnimation(.easeInOut(duration: 1.2)) { shot = (shot + 1) % model.profile.screenshotURLs.count }
        }
    }

    // MARK: Left: what this is

    private var pitch: some View {
        VStack(alignment: .leading, spacing: 0) {
            GloveboxChip()

            GameTitle(title: model.profile.title, size: 60)
                .padding(.top, 36)

            VStack(alignment: .leading, spacing: 16) {
                Highlight(icon: Image(systemName: "desktopcomputer"), slashed: true, text: "No gaming PC")
                Highlight(icon: Image(systemName: "wifi.slash"), text: "No streaming")
                Highlight(icon: Image(systemName: LauncherModel.cloudUploads ? "arrow.triangle.2.circlepath.icloud" : "icloud.and.arrow.down"),
                          text: LauncherModel.cloudUploads ? "Your Steam saves, in sync" : "Your Steam saves")
            }
            .padding(.top, 28)

            Spacer(minLength: 24)
            Thumbnails(urls: model.profile.screenshotURLs, selection: $shot)
        }
        .frame(maxWidth: 560, maxHeight: .infinity, alignment: .topLeading)
    }

    // MARK: Right: the one next step

    private var card: some View {
        ZStack {
            switch model.phase {
            case .signIn: SignInCard()
            case .notOwned: NotOwnedCard()
            case .checkingOwnership: StatusCard(signedInAs: model.accountName, text: "Checking your Steam library")
            default: StatusCard(signedInAs: nil, text: "Connecting to Steam")
            }
        }
        .padding(40)
        .frame(width: Self.cardWidth, height: Self.contentHeight)
        .glassBackgroundEffect()
        .animation(.spring(duration: 0.45), value: model.phase)
    }
}

private struct Highlight: View {
    let icon: Image
    var slashed = false
    let text: String

    var body: some View {
        HStack(spacing: 16) {
            icon
                .font(.system(size: 20, weight: .semibold))
                .foregroundStyle(Brand.orange)
                .overlay {
                    if slashed {
                        // SF Symbols' slash, drawn: a line with a gap cut out of the icon beneath it.
                        Capsule().fill(.background).frame(width: 5, height: 34).rotationEffect(.degrees(-45)).blendMode(.destinationOut)
                        Capsule().fill(Brand.orange).frame(width: 2.2, height: 32).rotationEffect(.degrees(-45))
                    }
                }
                .compositingGroup()
                .frame(width: 48, height: 48)
                .background(.thinMaterial, in: .circle)
            Text(text).font(.title3.weight(.medium))
        }
    }
}

/// Crossfading in-game screenshots with a slow push-in, like a TV app's hero.
private struct ScreenshotBackdrop: View {
    let urls: [URL]
    let index: Int

    var body: some View {
        GeometryReader { geo in
            ZStack {
                Color.black
                ForEach(Array(urls.enumerated()), id: \.offset) { i, url in
                    AsyncImage(url: url) { image in
                        image.resizable().scaledToFill()
                    } placeholder: { Color.clear }
                    .frame(width: geo.size.width, height: geo.size.height)
                    .scaleEffect(i == index ? 1.08 : 1.0)
                    .animation(.linear(duration: 7), value: index)
                    .opacity(i == index ? 1 : 0)
                    .clipped()
                }
            }
        }
        .ignoresSafeArea()
    }
}

private struct Thumbnails: View {
    let urls: [URL]
    @Binding var selection: Int

    var body: some View {
        HStack(spacing: 10) {
            ForEach(Array(urls.enumerated()), id: \.offset) { i, url in
                Button {
                    withAnimation(.easeInOut(duration: 0.8)) { selection = i }
                } label: {
                    AsyncImage(url: url) { image in
                        image.resizable().scaledToFill()
                    } placeholder: { Color.white.opacity(0.08) }
                    .frame(width: 80, height: 45)
                    .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
                    .overlay(RoundedRectangle(cornerRadius: 8, style: .continuous)
                        .stroke(.white.opacity(i == selection ? 0.95 : 0), lineWidth: 2))
                    .opacity(i == selection ? 1 : 0.55)
                }
                .buttonStyle(.plain)
                .hoverEffect(.lift)
                .accessibilityLabel("Screenshot \(i + 1)")
            }
        }
    }
}

/// Connecting, or signed in and checking the library.
private struct StatusCard: View {
    let signedInAs: String?
    let text: String

    var body: some View {
        if let name = signedInAs, !name.isEmpty {
            SignedInBadge(title: "Signed in as \(name)", status: text, bounces: true)
        } else {
            HStack(spacing: 10) {
                ProgressView().controlSize(.small)
                Text(text).font(.title3).foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }
}

/// The signed-in moment: the check mark lands once, and the line under it moves on to the
/// library check.
struct SignedInBadge: View {
    let title: String
    var status: String?
    var bounces = false
    @State private var appeared = false

    var body: some View {
        VStack(spacing: 18) {
            Image(systemName: "checkmark.circle.fill")
                .font(.system(size: 88))
                .foregroundStyle(.white, Brand.orange)
                .symbolEffect(.bounce, value: appeared)
            Text(title)
                .font(.title2.weight(.bold))
                .lineLimit(1)
                .minimumScaleFactor(0.7)
            HStack(spacing: 10) {
                ProgressView().controlSize(.small)
                Text(status ?? " ").font(.title3).foregroundStyle(.secondary)
            }
            .opacity(status == nil ? 0 : 1)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .onAppear { if bounces { appeared = true } }
    }
}

private struct NotOwnedCard: View {
    @EnvironmentObject var model: LauncherModel
    @Environment(\.openURL) private var openURL

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text("\(model.profile.title) isn't in your Steam library")
                .font(.largeTitle.weight(.bold))
                .fixedSize(horizontal: false, vertical: true)
            Text("You're signed in as \(model.accountName).")
                .font(.title3)
                .foregroundStyle(.secondary)
                .padding(.top, 8)
            Spacer(minLength: 24)
            // The art fills a fixed box as a background, so its own size can't widen the card.
            Color.clear
                .frame(maxWidth: .infinity)
                .frame(height: 200)
                .background {
                    AsyncImage(url: model.profile.heroImageURL) { image in
                        image.resizable().scaledToFill()
                    } placeholder: { Color.white.opacity(0.06) }
                }
                .clipShape(RoundedRectangle(cornerRadius: 20, style: .continuous))
            Spacer(minLength: 24)
            Button { openURL(model.profile.storeURL) } label: {
                Text("Get It on Steam").font(.title3.weight(.semibold)).frame(maxWidth: .infinity)
            }
            .buttonStyle(PillButtonStyle())
            Button("Use Another Account") { model.signOut() }
                .buttonStyle(.borderless)
                .frame(maxWidth: .infinity)
                .padding(.top, 12)
        }
    }
}
