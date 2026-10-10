import SwiftUI

/// Title, one line, a centred picture, and a footer pinned to the bottom of the card.
struct GuideStep<Picture: View, Footer: View>: View {
    let title: String
    let detail: String
    @ViewBuilder var picture: Picture
    @ViewBuilder var footer: Footer

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text(title)
                .font(.title.weight(.bold))
                .fixedSize(horizontal: false, vertical: true)
            Text(detail)
                .font(.title3)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
                .padding(.top, 8)
            Spacer(minLength: 20)
            picture.frame(maxWidth: .infinity)
            Spacer(minLength: 20)
            footer
        }
    }
}

/// What this is and the way out, above every guide step, so titles sit at the same height as the
/// sign-in form's (whose step tracker takes this place).
struct GuideHeader: View {
    let label: String
    var onClose: () -> Void
    var body: some View {
        HStack {
            Text(label)
                .font(.subheadline.weight(.semibold))
                .foregroundStyle(.secondary)
                .contentTransition(.numericText())
            Spacer()
            CloseButton(action: onClose)
        }
        .padding(.bottom, 20)
    }
}

/// A small line of help with an icon, for the moment someone is likely to get stuck.
struct Hint: View {
    let symbol: String
    let text: String
    var body: some View {
        Label {
            Text(text).fixedSize(horizontal: false, vertical: true)
        } icon: {
            Image(systemName: symbol)
        }
        .font(.callout)
        .foregroundStyle(.secondary)
    }
}

struct WaitingRow<Trailing: View>: View {
    let text: String
    @ViewBuilder var trailing: Trailing
    var body: some View {
        HStack(spacing: 10) {
            ProgressView().controlSize(.small)
            Text(text).font(.callout).foregroundStyle(.secondary)
            Spacer()
            trailing
        }
    }
}

/// The round close button used on visionOS sheets: neutral glass, not the accent colour.
struct CloseButton: View {
    var action: () -> Void
    var body: some View {
        Button(action: action) {
            Image(systemName: "xmark")
                .font(.body.weight(.semibold))
                .foregroundStyle(.secondary)
                .frame(width: 44, height: 44)
                .background(.thinMaterial, in: .circle)
                .contentShape(.circle)
        }
        .buttonStyle(.plain)
        .hoverEffect()
        .accessibilityLabel("Cancel")
    }
}

/// A phone showing what to look for: the Steam app's sign-in request, with Approve pulsing.
struct PhoneMock: View {
    @State private var pulse = false
    private let steamBlue = Color(red: 0.10, green: 0.62, blue: 1.0)

    var body: some View {
        RoundedRectangle(cornerRadius: 30, style: .continuous)
            .fill(Color.black)
            .overlay(RoundedRectangle(cornerRadius: 30, style: .continuous).stroke(Color(white: 0.5), lineWidth: 4))
            .frame(width: 150, height: 260)
            .overlay { request.padding(14) }
            .overlay(alignment: .top) {
                Capsule().fill(Color(white: 0.16)).frame(width: 46, height: 13).padding(.top, 11)
            }
            .onAppear {
                withAnimation(.easeInOut(duration: 0.9).repeatForever()) { pulse = true }
            }
    }

    private var request: some View {
        VStack(spacing: 6) {
            SteamAppIcon(size: 36)
            Text("Sign-in request").font(.caption2.weight(.semibold)).foregroundStyle(.white).padding(.top, 4)
            Text("Apple Vision Pro").font(.caption2).foregroundStyle(Color(white: 0.7))
            Text("Approve")
                .font(.caption.weight(.bold))
                .foregroundStyle(.white)
                .frame(width: 104, height: 30)
                .background(steamBlue, in: .capsule)
                .scaleEffect(pulse ? 1.07 : 1)
                .shadow(color: steamBlue.opacity(pulse ? 0.9 : 0), radius: 12)
                .padding(.top, 8)
            Text("Deny").font(.caption2).foregroundStyle(Color(white: 0.55))
        }
    }
}

/// The Steam iOS app's icon (from the App Store), with a drawn stand-in while it loads.
struct SteamAppIcon: View {
    var size: CGFloat
    static let url = URL(string: "https://is1-ssl.mzstatic.com/image/thumb/Purple211/v4/1d/e1/8d/1de18df0-9f13-4bdb-728a-ddc7ecaf9849/AppIcon-0-0-1x_U007emarketing-0-8-0-85-220.png/512x512bb.jpg")!

    var body: some View {
        AsyncImage(url: Self.url) { image in
            image.resizable().scaledToFit()
        } placeholder: {
            LinearGradient(colors: [Color(red: 0.07, green: 0.13, blue: 0.26), Color(red: 0.05, green: 0.35, blue: 0.62)],
                           startPoint: .top, endPoint: .bottom)
                .overlay(Image(systemName: "gamecontroller.fill").font(.system(size: size * 0.45)).foregroundStyle(.white))
        }
        .frame(width: size, height: size)
        .clipShape(RoundedRectangle(cornerRadius: size * 0.225, style: .continuous))
    }
}
