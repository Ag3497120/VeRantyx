import SwiftUI

// Small drawings of what the player sees somewhere Glovebox can't reach: the headset's Settings,
// the Steam app on a phone, Device Hub. They follow the real screens' layout and words.

/// Steps the player does somewhere else, side by side, each with a drawing and a line of text.
struct Storyboard: View {
    struct Card: Identifiable {
        var id: Int
        var art: AnyView
        var text: String
        /// Where, in smaller words under the text.
        var detail: String? = nil
    }

    var cards: [Card]

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            ForEach(cards) { c in
                VStack(spacing: 10) {
                    Shrunk { c.art }
                        .frame(height: 128)
                        .frame(maxWidth: .infinity)
                        .background(RoundedRectangle(cornerRadius: 12, style: .continuous).fill(.quaternary.opacity(0.5)))
                        .overlay(alignment: .topLeading) {
                            Text("\(c.id)")
                                .font(.caption.weight(.bold))
                                .foregroundStyle(.white)
                                .frame(width: 20, height: 20)
                                .background(Circle().fill(Color.accentColor))
                                .padding(8)
                        }
                    VStack(spacing: 3) {
                        Text(c.text).font(.callout.weight(.semibold))
                        if let d = c.detail { Text(d).font(.caption).opacity(0.7) }
                    }
                    .multilineTextAlignment(.center)
                    .fixedSize(horizontal: false, vertical: true)
                }
                .frame(maxWidth: .infinity)
            }
        }
        .padding(.vertical, 6)
    }
}

/// A drawing at its natural size, shrunk to fit the space it's given.
private struct Shrunk<Content: View>: View {
    @ViewBuilder var content: Content
    @State private var natural = CGSize.zero

    var body: some View {
        GeometryReader { g in
            let scale = natural.width > 0 && natural.height > 0
                ? min(1, (g.size.width - 48) / natural.width, (g.size.height - 36) / natural.height) : 1
            content
                .fixedSize()
                .onGeometryChange(for: CGSize.self) { $0.size } action: { natural = $0 }
                .scaleEffect(max(0.1, scale))
                .frame(width: g.size.width, height: g.size.height)
        }
    }
}

/// Pairing. Device Hub (Xcode 27): + › Pair Nearby Device › the Vision Pro tile (a device type); then the headset opens
/// Settings › General › Remote Devices, which only makes it discoverable (the Mac is not listed there); the Mac picks
/// it in Device Hub's list and clicks Next, the headset shows a code, and it's typed on the Mac. Xcode 26 the same way.
struct PairingGuide: View {
    var macName: String
    var deviceHub: Bool

    var body: some View {
        if deviceHub {
            Storyboard(cards: [
                .init(id: 1, art: AnyView(DeviceHubMock()),
                      text: "In Device Hub on this Mac, click +, then Pair Nearby Device, then Vision Pro."),
                .init(id: 2, art: AnyView(RemoteDevicesMock(macName: macName)),
                      text: "On your Vision Pro, open Settings › General › Remote Devices. Then pick it in Device Hub and click Next."),
                .init(id: 3, art: AnyView(DeviceHubCodeMock()),
                      text: "Your Vision Pro shows a code. Type it here, on this Mac."),
            ])
        } else {
            Storyboard(cards: [
                .init(id: 1, art: AnyView(VisionSettingsMock(path: ["General", "Remote Devices"])),
                      text: "On your Vision Pro, open Settings › General › Remote Devices."),
                .init(id: 2, art: AnyView(XcodeDevicesMock()), text: "In Xcode, press ⇧⌘2, click your Vision Pro, then Pair."),
                .init(id: 3, art: AnyView(PairCodeMock()), text: "Type the code your Vision Pro shows."),
            ])
        }
    }
}

/// Signing in to Xcode: what the floating guide walks through.
struct XcodeSignInGuide: View {
    var xcodeMajor: Int

    var body: some View {
        Storyboard(cards: [
            .init(id: 1, art: AnyView(HStack(spacing: 6) { BigKey("⌘"); BigKey(",") }),
                  text: "Glovebox opens Xcode. Press ⌘, for its Settings."),
            .init(id: 2, art: AnyView(XcodeAccountsMock(button: xcodeMajor >= 27 ? "Sign In…" : "Add Apple Account…")),
                  text: xcodeMajor >= 27 ? "Click Sign In…" : "Click Add Apple Account…"),
            .init(id: 3, art: AnyView(AppleSignInMock()), text: "Sign in with your Apple Account."),
        ])
    }
}

/// Developer Mode: the switch, the restart, then Turn On.
struct DeveloperModeGuide: View {
    var body: some View {
        Storyboard(cards: [
            .init(id: 1, art: AnyView(VisionSettingsMock(path: ["Privacy & Security", "Developer Mode"], toggle: true)),
                  text: "Turn on Developer Mode", detail: "Settings › Privacy & Security"),
            .init(id: 2, art: AnyView(Image(systemName: "arrow.clockwise.circle.fill").font(.system(size: 44)).foregroundStyle(.secondary)),
                  text: "Tap Restart"),
            .init(id: 3, art: AnyView(TurnOnAlertMock()),
                  text: "Tap Turn On", detail: "When it's back on"),
        ])
    }
}

/// The headset's own windows are glass over the room, so the drawings always use their dark glass.
struct VisionGlass: ViewModifier {
    var radius: CGFloat
    func body(content: Content) -> some View {
        let shape = RoundedRectangle(cornerRadius: radius, style: .continuous)
        return content
            .foregroundStyle(.white)
            .background(shape.fill(Color(white: 0.28).opacity(0.92)))
            .overlay(shape.strokeBorder(Color.white.opacity(0.16)))
            .environment(\.colorScheme, .dark)
    }
}

/// visionOS Settings, opened at the place the player has to go: the sidebar row lights up, then
/// the row on the right, and a switch turns on if there is one.
struct VisionSettingsMock: View {
    /// Sidebar row, then the row on the right.
    var path: [String]
    var toggle = false
    @State private var step = 0

    private struct Item {
        var title: String
        var symbol: String
        var color: Color
    }

    private let sidebar = [
        Item(title: "General", symbol: "gearshape.fill", color: .gray),
        Item(title: "Apps", symbol: "square.grid.2x2.fill", color: .indigo),
        Item(title: "Privacy & Security", symbol: "hand.raised.fill", color: .blue),
        Item(title: "Bluetooth", symbol: "wave.3.right", color: .blue),
    ]

    var body: some View {
        HStack(spacing: 0) {
            VStack(alignment: .leading, spacing: 4) {
                Text("Settings").font(.system(size: 15, weight: .bold)).padding(.bottom, 6)
                ForEach(sidebar.indices, id: \.self) { i in sidebarRow(sidebar[i]) }
                Spacer(minLength: 0)
            }
            .frame(width: 150)
            .padding(14)
            VStack(alignment: .leading, spacing: 8) {
                Text(path.first ?? "").font(.system(size: 15, weight: .bold)).opacity(step >= 1 ? 1 : 0.3)
                ForEach(rows, id: \.self) { detailRow($0).opacity(step >= 1 ? 1 : 0.3) }
                Spacer(minLength: 0)
            }
            .padding(14)
            .frame(width: 210)
        }
        .frame(height: 200)
        .modifier(VisionGlass(radius: 26))
        .task { await loop() }
    }

    private func sidebarRow(_ item: Item) -> some View {
        let lit = item.title == path.first && step >= 1
        return HStack(spacing: 8) {
            Image(systemName: item.symbol)
                .font(.system(size: 9, weight: .semibold))
                .frame(width: 18, height: 18)
                .background(Circle().fill(item.color))
            Text(item.title).font(.system(size: 11, weight: .medium)).lineLimit(1)
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 6)
        .background(Capsule().fill(Color.white.opacity(lit ? 0.22 : 0)))
    }

    private func detailRow(_ name: String) -> some View {
        let target = name == path.last
        let lit = target && step >= 2
        let shape = RoundedRectangle(cornerRadius: 10, style: .continuous)
        return HStack {
            Text(name).font(.system(size: 11, weight: .medium))
            Spacer()
            if target && toggle {
                ZStack(alignment: step >= 3 ? .trailing : .leading) {
                    Capsule().fill(step >= 3 ? Color.green : Color.white.opacity(0.2))
                    Circle().fill(.white).padding(2)
                }
                .frame(width: 30, height: 18)
            } else {
                Image(systemName: "chevron.right").font(.system(size: 9, weight: .semibold)).opacity(0.4)
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 9)
        .background(shape.fill(Color.white.opacity(lit ? 0.2 : 0.07)))
        .overlay(shape.strokeBorder(Color.accentColor.opacity(lit ? 0.9 : 0), lineWidth: 1.5))
    }

    /// Rows on the right: a couple of neighbours, so the target reads as "in a list".
    private var rows: [String] {
        guard let last = path.last else { return [] }
        switch last {
        case "Developer Mode": return ["Location Services", "Developer Mode", "Analytics & Improvements"]
        case "Remote Devices": return ["About", "Software Update", "Remote Devices"]
        default: return ["About", last, "Storage"]
        }
    }

    private func loop() async {
        while !Task.isCancelled {
            for s in 0...3 {
                withAnimation(.easeInOut(duration: 0.35)) { step = s }
                try? await Task.sleep(nanoseconds: s == 3 ? 2_200_000_000 : 900_000_000)
            }
        }
    }
}

/// Remote Devices on the headset, listing this Mac.
struct RemoteDevicesMock: View {
    var macName: String
    @State private var lit = false

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Remote Devices").font(.system(size: 15, weight: .bold))
            // What the headset shows: it is discoverable now; the Mac is picked on the Mac.
            HStack(spacing: 8) {
                Image(systemName: "dot.radiowaves.left.and.right").font(.system(size: 12))
                Text("This Apple Vision Pro is now discoverable").font(.system(size: 11, weight: .medium)).lineLimit(2)
                Spacer()
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 10)
            .background(RoundedRectangle(cornerRadius: 10, style: .continuous).fill(Color.white.opacity(lit ? 0.22 : 0.08)))
            .overlay(RoundedRectangle(cornerRadius: 10, style: .continuous).strokeBorder(Color.accentColor.opacity(lit ? 0.9 : 0), lineWidth: 1.5))
            Spacer(minLength: 0)
        }
        .padding(16)
        .frame(width: 280, height: 180)
        .modifier(VisionGlass(radius: 26))
        .onAppear { withAnimation(.easeInOut(duration: 0.9).repeatForever()) { lit = true } }
    }
}

/// The code the headset shows, typed on the Mac (Xcode 26).
private struct PairCodeMock: View {
    var body: some View {
        MacWindowMock {
            VStack(spacing: 8) {
                Text("Enter the code shown on your Vision Pro").font(.system(size: 7.5, weight: .medium)).foregroundStyle(.secondary)
                CodeDigits(filled: ["4", "8", "1"], count: 6)
                Text("Pair").font(.system(size: 9, weight: .semibold))
                    .padding(.horizontal, 12).padding(.vertical, 3)
                    .background(Capsule().fill(Color.accentColor))
                    .foregroundStyle(.white)
            }
            .padding(10)
            .frame(maxWidth: .infinity)
        }
        .frame(width: 170)
    }
}

/// Device Hub asking for the code the headset shows (Xcode 27): typed on this Mac.
private struct DeviceHubCodeMock: View {
    var body: some View {
        MacWindowMock {
            VStack(spacing: 7) {
                Image(systemName: "visionpro").font(.system(size: 18)).foregroundStyle(Color.accentColor)
                Text("Enter the verification code displayed on your Apple Vision Pro")
                    .font(.system(size: 7.5, weight: .medium)).foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                CodeDigits(filled: ["4", "8", "1"], count: 6)
            }
            .padding(10)
            .frame(maxWidth: .infinity)
        }
        .frame(width: 170)
    }
}

struct CodeDigits: View {
    var filled: [String]
    var count: Int
    var glass = false

    var body: some View {
        HStack(spacing: 3) {
            ForEach(0..<count, id: \.self) { i in
                Text(i < filled.count ? filled[i] : "")
                    .font(.system(size: 13, weight: .semibold, design: .rounded))
                    .frame(width: 17, height: 22)
                    .background(RoundedRectangle(cornerRadius: 4, style: .continuous)
                        .fill(glass ? Color.white.opacity(0.12) : Color(nsColor: .textBackgroundColor)))
                    .overlay(RoundedRectangle(cornerRadius: 4, style: .continuous)
                        .strokeBorder(i == filled.count ? Color.accentColor : Color.secondary.opacity(0.4),
                                      lineWidth: i == filled.count ? 1.5 : 0.5))
            }
        }
    }
}

/// A small Mac window: traffic lights and whatever's inside, in the Mac's own colors.
private struct MacWindowMock<Content: View>: View {
    @ViewBuilder var content: Content

    var body: some View {
        let shape = RoundedRectangle(cornerRadius: 9, style: .continuous)
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 3) {
                ForEach([Color.red, Color.yellow, Color.green], id: \.self) { Circle().fill($0.opacity(0.85)).frame(width: 5, height: 5) }
            }
            .padding(.horizontal, 7)
            .padding(.top, 6)
            content
        }
        .background(shape.fill(Color(nsColor: .windowBackgroundColor)))
        .overlay(shape.strokeBorder(.separator, lineWidth: 0.5))
        .clipShape(shape)
        .shadow(color: .black.opacity(0.12), radius: 4, y: 2)
    }
}

/// A ring that breathes around the thing to click, like the guide's ring beside Xcode.
private struct Ring: ViewModifier {
    var radius: CGFloat = 6
    @State private var on = false

    func body(content: Content) -> some View {
        content
            .overlay(RoundedRectangle(cornerRadius: radius, style: .continuous)
                .strokeBorder(Color.accentColor, lineWidth: 1.5)
                .padding(-3)
                .opacity(on ? 1 : 0.35))
            .onAppear { withAnimation(.easeInOut(duration: 0.8).repeatForever()) { on = true } }
    }
}

/// Xcode's Settings on Apple Accounts, with its sign-in button ringed.
private struct XcodeAccountsMock: View {
    var button: String

    var body: some View {
        MacWindowMock {
            HStack(alignment: .top, spacing: 0) {
                VStack(alignment: .leading, spacing: 2) {
                    ForEach(["General", "Apple Accounts", "Behaviors", "Editing"], id: \.self) { item in
                        Text(item)
                            .font(.system(size: 7, weight: .medium))
                            .lineLimit(1)
                            .padding(.horizontal, 4).padding(.vertical, 2)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .background(RoundedRectangle(cornerRadius: 3).fill(item == "Apple Accounts" ? Color.accentColor : Color.clear))
                            .foregroundStyle(item == "Apple Accounts" ? Color.white : Color.primary)
                    }
                }
                .frame(width: 62)
                .padding(5)
                VStack(alignment: .trailing, spacing: 7) {
                    HStack(spacing: 4) {
                        Image(systemName: "apple.logo").font(.system(size: 9))
                        Text("Sign in to your Apple Account").font(.system(size: 6.5, weight: .medium)).lineLimit(1)
                        Spacer(minLength: 0)
                    }
                    .padding(5)
                    .background(RoundedRectangle(cornerRadius: 4).fill(.quaternary))
                    Text(button)
                        .font(.system(size: 7, weight: .medium))
                        .padding(.horizontal, 6).padding(.vertical, 2)
                        .background(RoundedRectangle(cornerRadius: 3).fill(.quaternary))
                        .modifier(Ring(radius: 5))
                }
                .padding(6)
                .frame(maxWidth: .infinity)
            }
            .padding(.bottom, 14)
        }
        .frame(width: 176)
    }
}

/// Apple's own sign-in sheet.
private struct AppleSignInMock: View {
    var body: some View {
        MacWindowMock {
            VStack(spacing: 6) {
                Image(systemName: "apple.logo").font(.system(size: 16))
                Text("Sign in with your Apple Account").font(.system(size: 7.5, weight: .semibold))
                RoundedRectangle(cornerRadius: 4)
                    .strokeBorder(.separator)
                    .frame(height: 15)
                    .overlay(alignment: .leading) {
                        Text("Email or Phone Number").font(.system(size: 6)).foregroundStyle(.tertiary).padding(.leading, 5)
                    }
                Text("Continue").font(.system(size: 7, weight: .semibold))
                    .padding(.horizontal, 10).padding(.vertical, 3)
                    .background(Capsule().fill(Color.accentColor)).foregroundStyle(.white)
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 8)
            .frame(maxWidth: .infinity)
        }
        .frame(width: 150)
    }
}

/// Device Hub: + at the top of its sidebar, and Pair Nearby Device in its menu.
private struct DeviceHubMock: View {
    var body: some View {
        MacWindowMock {
            HStack(alignment: .top, spacing: 0) {
                VStack(alignment: .leading, spacing: 4) {
                    Image(systemName: "plus").font(.system(size: 8, weight: .semibold))
                        .frame(width: 14, height: 14)
                        .modifier(Ring(radius: 7))
                    VStack(alignment: .leading, spacing: 1) {
                        Text("Pair Nearby Device").font(.system(size: 7, weight: .medium))
                            .padding(.horizontal, 5).padding(.vertical, 2)
                            .background(RoundedRectangle(cornerRadius: 3).fill(Color.accentColor))
                            .foregroundStyle(.white)
                        Text("Add Simulator…").font(.system(size: 7)).padding(.horizontal, 5).padding(.vertical, 2)
                    }
                    .padding(3)
                    .background(RoundedRectangle(cornerRadius: 5).fill(Color(nsColor: .controlBackgroundColor)).shadow(radius: 2, y: 1))
                }
                .padding(6)
                .frame(width: 96, alignment: .leading)
                Divider()
                Text("No Selection").font(.system(size: 7)).foregroundStyle(.tertiary)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
            .frame(height: 74)
        }
        .frame(width: 176)
    }
}

/// Xcode 26's Devices window: the headset under Discovered, and Pair.
private struct XcodeDevicesMock: View {
    var body: some View {
        MacWindowMock {
            HStack(alignment: .top, spacing: 0) {
                VStack(alignment: .leading, spacing: 3) {
                    Text("DISCOVERED").font(.system(size: 5.5, weight: .semibold)).foregroundStyle(.secondary)
                    HStack(spacing: 3) {
                        Image(systemName: Symbols.available("visionpro", fallback: "eyeglasses")).font(.system(size: 7))
                        Text("Apple Vision Pro").font(.system(size: 6.5, weight: .medium)).lineLimit(1)
                    }
                    .padding(.horizontal, 4).padding(.vertical, 2)
                    .background(RoundedRectangle(cornerRadius: 3).fill(Color.accentColor))
                    .foregroundStyle(.white)
                }
                .padding(6)
                .frame(width: 78, alignment: .leading)
                Divider()
                VStack(spacing: 6) {
                    Image(systemName: Symbols.available("visionpro", fallback: "eyeglasses")).font(.system(size: 20, weight: .light))
                        .foregroundStyle(.secondary)
                    Text("Pair").font(.system(size: 7, weight: .semibold))
                        .padding(.horizontal, 9).padding(.vertical, 2)
                        .background(RoundedRectangle(cornerRadius: 3).fill(.quaternary))
                        .modifier(Ring(radius: 5))
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
            .frame(height: 74)
        }
        .frame(width: 176)
    }
}

/// A keycap at drawing size.
private struct BigKey: View {
    var key: String
    init(_ key: String) { self.key = key }

    var body: some View {
        Text(key)
            .font(.system(size: 22, weight: .medium, design: .rounded))
            .frame(width: 44, height: 44)
            .background(RoundedRectangle(cornerRadius: 9, style: .continuous).fill(Color(nsColor: .controlBackgroundColor)))
            .overlay(RoundedRectangle(cornerRadius: 9, style: .continuous).strokeBorder(.separator))
            .shadow(color: .black.opacity(0.18), radius: 0, y: 2)
    }
}

/// The alert after Developer Mode's restart.
private struct TurnOnAlertMock: View {
    @State private var pulse = false

    var body: some View {
        VStack(spacing: 6) {
            Text("Turn on Developer Mode?").font(.system(size: 12, weight: .bold))
            Text("Turning on Developer Mode lowers the security of your device.")
                .font(.system(size: 8)).opacity(0.7).multilineTextAlignment(.center)
            HStack(spacing: 6) {
                Text("Cancel").font(.system(size: 10, weight: .medium))
                    .frame(width: 70, height: 22).background(Capsule().fill(Color.white.opacity(0.15)))
                Text("Turn On").font(.system(size: 10, weight: .semibold))
                    .frame(width: 70, height: 22).background(Capsule().fill(Color.white.opacity(pulse ? 0.45 : 0.28)))
            }
            .padding(.top, 4)
        }
        .padding(14)
        .frame(width: 190)
        .modifier(VisionGlass(radius: 22))
        .onAppear { withAnimation(.easeInOut(duration: 0.9).repeatForever()) { pulse = true } }
    }
}

/// The Steam app's sign-in request on a phone, with Approve pulsing.
struct PhoneApproveArt: View {
    @State private var pulse = false

    var body: some View {
        GeometryReader { g in
            let w = g.size.width
            RoundedRectangle(cornerRadius: w * 0.2, style: .continuous)
                .fill(Color.black)
                .overlay(RoundedRectangle(cornerRadius: w * 0.2, style: .continuous).strokeBorder(Color(white: 0.5), lineWidth: 2))
                .overlay {
                    VStack(spacing: w * 0.04) {
                        Image(systemName: "person.badge.shield.checkmark.fill").font(.system(size: w * 0.2))
                            .foregroundStyle(.white.opacity(0.85))
                        Text("Sign-in request").font(.system(size: w * 0.08, weight: .semibold)).foregroundStyle(.white)
                        Text("Approve")
                            .font(.system(size: w * 0.09, weight: .bold))
                            .foregroundStyle(.white)
                            .frame(width: w * 0.7, height: w * 0.2)
                            .background(Capsule().fill(Color(red: 0.10, green: 0.62, blue: 1.0)))
                            .scaleEffect(pulse ? 1.07 : 1)
                            .padding(.top, w * 0.04)
                    }
                }
                .overlay(alignment: .top) {
                    Capsule().fill(Color(white: 0.18)).frame(width: w * 0.3, height: w * 0.08).padding(.top, w * 0.07)
                }
        }
        .environment(\.colorScheme, .dark)
        .onAppear { withAnimation(.easeInOut(duration: 0.9).repeatForever()) { pulse = true } }
    }
}

/// macOS's own window asking to sign: the Mac's password typed in, and Always Allow circled. In the Mac's
/// light or dark look, which the real one follows (not Glovebox's).
struct KeychainPromptArt: View {
    @State private var pulse = false

    var body: some View {
        let dark = NSApp.effectiveAppearance.bestMatch(from: [.aqua, .darkAqua]) == .darkAqua
        let shape = RoundedRectangle(cornerRadius: 16, style: .continuous)
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .top, spacing: 12) {
                Image(systemName: "lock.fill")
                    .font(.system(size: 30))
                    .foregroundStyle(LinearGradient(colors: [Color(red: 0.99, green: 0.82, blue: 0.38), Color(red: 0.85, green: 0.57, blue: 0.12)],
                                                    startPoint: .top, endPoint: .bottom))
                    .frame(width: 34)
                VStack(alignment: .leading, spacing: 4) {
                    bar(200, 6, 0.55)
                    bar(118, 6, 0.55)
                    bar(160, 4, 0.28).padding(.top, 2)
                }
                .padding(.top, 3)
            }
            HStack(spacing: 6) {
                Text("Password:").font(.system(size: 9))
                Text("••••••••")
                    .font(.system(size: 9, weight: .bold))
                    .padding(.horizontal, 5)
                    .frame(maxWidth: .infinity, minHeight: 17, alignment: .leading)
                    .background(RoundedRectangle(cornerRadius: 5, style: .continuous).fill(Color(nsColor: .textBackgroundColor)))
                    .overlay(RoundedRectangle(cornerRadius: 5, style: .continuous).strokeBorder(Color(nsColor: .controlAccentColor).opacity(0.75), lineWidth: 1.5))
            }
            .padding(.leading, 46)
            HStack(spacing: 6) {
                Text("?")
                    .font(.system(size: 8, weight: .semibold))
                    .frame(width: 14, height: 14)
                    .background(Circle().fill(.quaternary))
                button("Always Allow")
                    .overlay(Capsule().strokeBorder(Theme.accent, lineWidth: 2).padding(-4).opacity(pulse ? 1 : 0.55))
                Spacer(minLength: 12)
                button("Deny")
                button("Allow")
            }
        }
        .padding(14)
        .frame(width: 310)
        .foregroundStyle(.primary)
        .background(shape.fill(Color(nsColor: .windowBackgroundColor)))
        .overlay(shape.strokeBorder(.separator, lineWidth: 0.5))
        .shadow(color: .black.opacity(0.25), radius: 8, y: 3)
        .environment(\.colorScheme, dark ? .dark : .light)
        .onAppear { withAnimation(.easeInOut(duration: 0.9).repeatForever()) { pulse = true } }
        .accessibilityElement()
        .accessibilityLabel("The macOS window: your Mac password, then Always Allow")
    }

    private func bar(_ width: CGFloat, _ height: CGFloat, _ opacity: Double) -> some View {
        Capsule().fill(Color.primary.opacity(opacity)).frame(width: width, height: height)
    }

    private func button(_ title: String) -> some View {
        Text(title)
            .font(.system(size: 9, weight: .medium))
            .padding(.horizontal, 9)
            .frame(minWidth: 46, minHeight: 17)
            .background(Capsule().fill(.quaternary))
    }
}
