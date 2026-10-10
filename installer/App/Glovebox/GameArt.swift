import SwiftUI
import AppKit

/// The game's store art, fetched from Steam's public store page when the app opens. Nothing of
/// the game ships inside Glovebox.
@MainActor
final class StoreArt: ObservableObject {
    @Published private(set) var hero: NSImage?
    @Published private(set) var capsule: NSImage?

    private static var cache: [UInt32: StoreArt] = [:]
    private var loading = false
    let appID: UInt32

    static func of(_ appID: UInt32) -> StoreArt {
        if let a = cache[appID] { return a }
        let a = StoreArt(appID: appID)
        cache[appID] = a
        return a
    }

    private init(appID: UInt32) { self.appID = appID }

    func load() {
        guard !loading else { return }
        loading = true
        let base = "https://cdn.cloudflare.steamstatic.com/steam/apps/\(appID)/"
        Task {
            async let hero = Self.image(base + "library_hero.jpg")
            async let capsule = Self.image(base + "library_600x900_2x.jpg")
            let (h, c) = await (hero, capsule)
            withAnimation(.easeOut(duration: 0.4)) {
                self.hero = h
                self.capsule = c
            }
        }
    }

    private static func image(_ url: String) async -> NSImage? {
        guard let u = URL(string: url), let got = try? await URLSession.shared.data(from: u),
              (got.1 as? HTTPURLResponse)?.statusCode == 200 else { return nil }
        return NSImage(data: got.0)
    }
}

/// The game's box art as a small icon.
struct GameIcon: View {
    var appID: UInt32
    var size: CGFloat = 22
    @StateObject private var art: StoreArt

    init(appID: UInt32, size: CGFloat = 22) {
        self.appID = appID
        self.size = size
        _art = StateObject(wrappedValue: StoreArt.of(appID))
    }

    var body: some View {
        Group {
            if let c = art.capsule {
                Image(nsImage: c).resizable().aspectRatio(contentMode: .fill)
            } else {
                Rectangle().fill(Color(white: 0.15))
            }
        }
        .frame(width: size * 0.75, height: size)
        .clipShape(RoundedRectangle(cornerRadius: size * 0.12, style: .continuous))
        .frame(width: size, height: size)
        .onAppear { art.load() }
    }
}

/// An app's icon from the App Store, for an app that isn't installed yet.
@MainActor
final class AppStoreIcon: ObservableObject {
    @Published private(set) var image: NSImage?
    private static var cache: [Int: AppStoreIcon] = [:]
    private var loading = false
    let id: Int

    static func of(_ id: Int) -> AppStoreIcon {
        if let a = cache[id] { return a }
        let a = AppStoreIcon(id: id)
        cache[id] = a
        return a
    }

    private init(id: Int) { self.id = id }

    func load() {
        guard !loading, image == nil else { return }
        loading = true
        Task {
            guard let u = URL(string: "https://itunes.apple.com/lookup?id=\(id)"),
                  let got = try? await URLSession.shared.data(from: u),
                  let j = try? JSONSerialization.jsonObject(with: got.0) as? [String: Any],
                  let art = ((j["results"] as? [[String: Any]])?.first?["artworkUrl512"] as? String).flatMap(URL.init(string:)),
                  let img = try? await URLSession.shared.data(from: art) else { return }
            withAnimation { image = NSImage(data: img.0) }
        }
    }
}

/// Xcode's icon: the installed app's, or the App Store's before it's installed.
struct XcodeIcon: View {
    var size: CGFloat
    var installed: NSImage?
    @StateObject private var store = AppStoreIcon.of(497_799_835)

    var body: some View {
        Group {
            if let i = installed ?? store.image {
                Image(nsImage: i).resizable().aspectRatio(contentMode: .fit)
                    .clipShape(RoundedRectangle(cornerRadius: installed == nil ? size * 0.225 : 0, style: .continuous))
            } else {
                IconTile(symbol: "hammer.fill", color: .blue, size: size * 0.82)
            }
        }
        .frame(width: size, height: size)
        .onAppear { if installed == nil { store.load() } }
    }
}
