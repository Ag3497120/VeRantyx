import Foundation

/// One game Glovebox can put on a Vision Pro: the app's side of the engine's
/// `packages/<id>/package.json`. The engine builds it by `id`.
public struct Recipe: Sendable, Identifiable, Equatable {
    public var id: String
    public var title: String
    public var steamAppID: UInt32
    /// The Steam depots the Mac download comes from.
    public var depots: [UInt32]
    /// The depot paths the Mac downloads (prefixes): program files, plus shader files for the
    /// pipeline rebuild. Everything else downloads on the headset.
    public var macDownload: [String]
    /// The app's name on the headset.
    public var headsetAppName: String
    public var macDownloadGB: Double
    /// What the headset downloads after install, and the room it needs for it.
    public var gameDownloadGB: Int
    public var headsetSpaceGB: Int
    public var needsSenseControllers: Bool
    public var storeURL: URL
    /// The game build testers get: each depot's manifest (package.json "steam.manifests"), the build we tested,
    /// instead of whatever Steam's current one is. Empty: the current build.
    public var pinnedManifests: [UInt32: UInt64] = [:]

    public init(id: String, title: String, steamAppID: UInt32, depots: [UInt32], macDownload: [String], headsetAppName: String,
                macDownloadGB: Double, gameDownloadGB: Int, headsetSpaceGB: Int, needsSenseControllers: Bool, storeURL: URL) {
        self.id = id; self.title = title; self.steamAppID = steamAppID; self.depots = depots; self.macDownload = macDownload
        self.headsetAppName = headsetAppName; self.macDownloadGB = macDownloadGB
        self.gameDownloadGB = gameDownloadGB; self.headsetSpaceGB = headsetSpaceGB
        self.needsSenseControllers = needsSenseControllers; self.storeURL = storeURL
    }

    /// This recipe with the Steam parts of the engine's `package.json`, which wins where they differ.
    public func updated(fromPackage data: Data) -> Recipe {
        guard let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return self }
        var r = self
        if let game = j["game"] as? String { r.title = game; r.headsetAppName = game }
        if let name = j["app_name"] as? String, !name.isEmpty { r.headsetAppName = name }
        if let steam = j["steam"] as? [String: Any] {
            if let app = steam["app"] as? Int { r.steamAppID = UInt32(app) }
            if let depots = steam["depots"] as? [String: Int], !depots.isEmpty { r.depots = depots.values.map(UInt32.init).sorted() }
            if let paths = steam["mac_download"] as? [String], !paths.isEmpty { r.macDownload = paths }
            if let pins = steam["manifests"] as? [String: Any] {
                r.pinnedManifests = Dictionary(uniqueKeysWithValues: pins.compactMap { k, v in
                    UInt32(k).flatMap { d in UInt64("\(v)").map { (d, $0) } }
                })
            }
        }
        return r
    }
}

public extension Recipe {
    static let alyx = Recipe(
        id: "alyx", title: "Half-Life: Alyx", steamAppID: 546_560, depots: [546_561, 546_564],
        macDownload: ["game/bin/linuxsteamrtarm64/", "game/hlvr/bin/linuxsteamrtarm64/",
                      "game/core/shaders_vulkan_", "game/hlvr/shaders_vulkan_"],
        headsetAppName: "Glovebox: HLA", macDownloadGB: 1.7, gameDownloadGB: 67, headsetSpaceGB: 75,
        needsSenseControllers: true, storeURL: URL(string: "https://store.steampowered.com/app/546560/")!)

    static let all: [Recipe] = [.alyx]
}
