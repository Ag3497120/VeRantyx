import Foundation

/// Everything the launcher needs to know about one game: which app and depots to fetch,
/// where files land in the app container, and which files the port handles itself.
public struct GameProfile: Sendable {
    public let appID: UInt32
    public let title: String
    public let depots: [UInt32]
    /// Maps a depot path to a path under the install folder, or nil to leave the file out.
    public let localPath: @Sendable (String) -> String?
    public let policy: @Sendable (DepotManifest.File) -> FilePolicy
    /// Where Steam Cloud root tokens (lowercased, e.g. `gameinstall`) point, relative to the install
    /// folder. Paths under them still go through `localPath`.
    public let cloudRoots: [String: String]
    /// Save-folder file names (globs) the headset never sends to Steam Cloud.
    public let cloudNeverUpload: [String]
    public let storeURL: URL
    public let heroImageURL: URL
    public let logoURL: URL
    /// In-game screenshots from the store page, for the launcher's first screen.
    public let screenshotURLs: [URL]
    /// Depots fetched at an exact, tested build instead of Steam's current one, so a game update
    /// can't break the port. Depots not listed follow the current public build.
    public var pinnedManifests: [UInt32: UInt64]

    public init(appID: UInt32, title: String, depots: [UInt32], localPath: @escaping @Sendable (String) -> String?,
                policy: @escaping @Sendable (DepotManifest.File) -> FilePolicy, cloudRoots: [String: String],
                cloudNeverUpload: [String] = [], storeURL: URL, heroImageURL: URL, logoURL: URL,
                screenshotURLs: [URL] = [], pinnedManifests: [UInt32: UInt64] = [:]) {
        self.appID = appID; self.title = title; self.depots = depots; self.localPath = localPath; self.policy = policy
        self.cloudRoots = cloudRoots; self.cloudNeverUpload = cloudNeverUpload
        self.storeURL = storeURL; self.heroImageURL = heroImageURL; self.logoURL = logoURL
        self.screenshotURLs = screenshotURLs; self.pinnedManifests = pinnedManifests
    }

}

public extension GameProfile {
    /// Half-Life: Alyx on the Klepton port. The Linux ARM64 build's ELF libraries are
    /// translated and signed at build time and ship inside the app, so on the device they are
    /// only empty placeholders (the engine still looks them up by name). The x86-64 and
    /// Windows builds are never downloaded.
    static let halfLifeAlyx = GameProfile(
        appID: 546_560,
        title: "Half-Life: Alyx",
        depots: [546_561, 546_564],  // shared content, Linux binaries
        localPath: { path in
            // The app reads the depot's game/ folder from Documents/alyx/assets.
            guard path.hasPrefix("game/") else { return nil }
            return String(path.dropFirst("game/".count))
        },
        policy: { file in
            let p = file.name
            if p.contains("/linuxsteamrt64/") || p.contains("/win64/") || p.contains("/win32/") { return .skip }
            if p.contains("/linuxsteamrtarm64/") {
                let name = (p as NSString).lastPathComponent
                let isELF = name.hasSuffix(".so") || name.contains(".so.") || !name.contains(".")
                return isELF ? .placeholder : .download
            }
            return .download
        },
        cloudRoots: ["gameinstall": ""],
        // personal.cfg is this device's settings (a PC would get the headset's). The .hl1/.hl2/.hl3 files
        // hold the level the game was in, left behind because it exits the app (a PC's cloud saves have none).
        cloudNeverUpload: ["personal.cfg", "*.hl1", "*.hl2", "*.hl3"],
        storeURL: URL(string: "https://store.steampowered.com/app/546560/HalfLife_Alyx/")!,
        heroImageURL: URL(string: "https://cdn.cloudflare.steamstatic.com/steam/apps/546560/library_hero.jpg")!,
        logoURL: URL(string: "https://cdn.cloudflare.steamstatic.com/steam/apps/546560/logo.png")!,
        screenshotURLs: [
            "d61365e93f20ceb5a94a1e5b2811cf504cbfa303",  // City 17 and the Citadel
            "fe7066404a704aa20f7c6f251facb7aef2606bda",  // Combine soldiers in a hallway
            "5d228b092e93ff148e6a998c33e751fb968cc956",  // the gravity gloves
            "ddc667aa2687543c0baa1a63c6bdb5fa59e0617e",  // pistol, street fight
            "0360004603a7861cf6781d5449e641f916f1ee07",  // city square
            "4912f4c3d259a472e9898f0a7b1f819a533d2c1e",  // canal
        ].map { URL(string: "https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/546560/ss_\($0).1920x1080.jpg")! },
        // The build we tested (Steam build 25487405), the same as packages/alyx/package.json "steam.manifests"
        // (GameProfileTests checks they agree)
        pinnedManifests: [546_561: 1_436_897_171_613_240_821, 546_564: 9_198_205_754_520_568_813]
    )
}
