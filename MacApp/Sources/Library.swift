// The Steam library VXRuntime runs SteamVR and the games from (~/Verantyx_VR_Drive/vxrt_steam), laid out as a Steam
// library is: steamapps/common/<folder> and steamapps/appmanifest_<id>.acf. The same layout tools/vxrt_steam.py
// reads and writes, so a setup made with either works with the other.
import Foundation

struct SteamApp: Identifiable, Hashable {
    var id: String          // the Steam app id
    var name: String
    var installdir: String
    var manifest: URL
    var sizeOnDisk: Int64
}

/// The titles the app offers to fetch and knows how to start. SteamVR is needed for all of them.
struct Title: Identifiable, Hashable {
    var id: String
    var name: String
    var bytes: Int64        // about, as Steam installs it (for the space check)
    var free: Bool
    var exe: String?        // the game's executable (nil: Half-Life: Alyx, which has its own launcher)
    var summary: String     // in the app's language
    var works: Bool         // played through in a headset (games/<appid>.json: "works"), not only measured
    var flat = false        // also plays on the Mac's screen, without a headset
    var args: [String] = [] // the game's arguments (its launcher's)
    var vrArgs: [String] = []   // and those that ask it for VR
    /// The game's own openvr_api.dll, relative to its executable's folder: its frames are taken from its Submit
    /// (launch_vxrt_game.sh's VXRT_CAPTURE_PROXY) instead of through SteamVR's compositor.
    var captureProxy: String? = nil
    /// The game opens its own launcher first, which the player uses as on Windows (sign in, PLAY).
    var launcher = false
    /// The game the launcher starts (its lines in the launcher's log are "[vxrt <name>:<pid>] ...").
    var gameExe: String? = nil
    /// Started by the player's own Windows Steam on VXRuntime (Paths.windowsSteam; scripts/launch_vxrt_nms_vr.sh),
    /// which they sign in to themselves: the game's executable is wrapped in Steam's DRM (SteamStub), which only that
    /// Steam answers. It comes from that Steam's own library, not from steamcmd.
    var windowsSteam = false

    /// Where the game's Windows files come from, and what it starts with.
    enum Source {
        /// Valve's steamcmd on the Mac, with the player's account; started with Mac Steam through the bridge.
        case steamcmd
        /// The player's Windows Steam on VXRuntime, which downloads and starts it itself (SteamStub games).
        case windowsSteam
    }
    var source: Source { windowsSteam ? .windowsSteam : .steamcmd }

    static func == (a: Title, b: Title) -> Bool { a.id == b.id }
    func hash(into h: inout Hasher) { h.combine(id) }

    static let steamVR = "250820"
    static let all: [Title] = [
        Title(id: steamVR, name: "SteamVR", bytes: 5_820_607_244, free: true, exe: nil,
              summary: T("Needed for every game. Free.", "どのゲームにも必要です。無料。"), works: true),
        Title(id: "546560", name: "Half-Life: Alyx", bytes: 73_476_413_180, free: false, exe: nil,
              summary: T("Plays in the headset, levels and saves included. 60 fps with the 3K preset on an M1 Max.",
                         "ヘッドセットでプレイ済み（ステージの切り替えとセーブも）。M1 Max で 3K 設定 60 fps。"), works: true),
        Title(id: "450390", name: "The Lab", bytes: 16_625_763_869, free: true, exe: "TheLab.exe",
              summary: T("Runs (title scene about 55 fps); not played through yet. Free.",
                         "動作（タイトル場面 約 55 fps）。通しのプレイはまだ。無料。"), works: false),
        Title(id: "348250", name: "Google Earth VR", bytes: 2_996_347_031, free: true, exe: "Earth.exe",
              summary: T("Runs (terrain and labels, about 57 fps); not played through yet. Free.",
                         "動作（地形とラベル、約 57 fps）。通しのプレイはまだ。無料。"), works: false),
        Title(id: "359320", name: "Elite Dangerous", bytes: 58_055_995_280, free: false, exe: "EDLaunch.exe",
              summary: T("Flies on the Mac's screen (about 60 fps in game on an M1 Max) or in the headset (30-35 fps); keyboard, mouse and a DualSense. It opens its own launcher: sign in and press PLAY there.",
                         "Mac の画面（M1 Max のゲーム中で約 60 fps）でもヘッドセット（30〜35 fps）でも飛べます。キーボード・マウス・DualSense。専用のランチャーが開くので、そこでサインインして PLAY を押します。"),
              works: false, flat: true, args: ["/steam"], vrArgs: ["/vr"],
              captureProxy: "Products/elite-dangerous-odyssey-64/Openvr/win64/openvr_api.dll", launcher: true, gameExe: "EliteDangerous64.exe"),
        Title(id: "275850", name: "No Man's Sky", bytes: 33_471_534_235, free: false, exe: "NMS.exe",
              summary: T("Played on the Mac's screen (the planet's terrain drawn). In the headset, picture and sound checked with both the Vision Pro's VeRantyx app and the ALVR app (hand input through ALVR too); 25-40 fps so far. Started by your own Windows Steam, which you sign in to yourself.",
                         "Mac の画面で遊べました（惑星の地形まで描画）。ヘッドセットは Vision Pro の VeRantyx アプリと ALVR アプリの両方で映像と音を確認（ALVR では手の入力も）。今のところ 25〜40 fps。あなた自身がサインインした Windows 版 Steam から起動します。"),
              works: false, flat: true, gameExe: "NMS.exe", windowsSteam: true),
    ]
    static func named(_ id: String) -> Title? { all.first { $0.id == id } }
}

enum Library {
    /// A Steam app manifest's fields.
    static func manifest(_ url: URL) -> SteamApp? {
        guard let data = FileManager.default.contents(atPath: url.path) else { return nil }
        let text = String(decoding: data, as: UTF8.self)
        func get(_ key: String) -> String {
            guard let re = try? NSRegularExpression(pattern: "\"\(key)\"\\s+\"([^\"]*)\"", options: .caseInsensitive),
                  let m = re.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)),
                  let r = Range(m.range(at: 1), in: text) else { return "" }
            return String(text[r])
        }
        let id = get("appid")
        guard !id.isEmpty else { return nil }
        return SteamApp(id: id, name: get("name"), installdir: get("installdir"), manifest: url, sizeOnDisk: Int64(get("SizeOnDisk")) ?? 0)
    }

    /// The apps of a Steam library, by app id: its steamapps/appmanifest_*.acf, and those of steamcmd installs inside
    /// its steamapps/common (force_install_dir puts the manifest beside the app).
    static func apps(in library: URL) -> [String: SteamApp] {
        var found: [String: SteamApp] = [:]
        let fm = FileManager.default
        let steamapps = library.appendingPathComponent("steamapps")
        var manifests = ((try? fm.contentsOfDirectory(at: steamapps, includingPropertiesForKeys: nil)) ?? [])
        let common = steamapps.appendingPathComponent("common")
        for dir in (try? fm.contentsOfDirectory(at: common, includingPropertiesForKeys: nil)) ?? [] {
            manifests += (try? fm.contentsOfDirectory(at: dir.appendingPathComponent("steamapps"), includingPropertiesForKeys: nil)) ?? []
        }
        for url in manifests where url.lastPathComponent.hasPrefix("appmanifest_") && url.pathExtension == "acf" {
            if let m = manifest(url), found[m.id] == nil { found[m.id] = m }
        }
        return found
    }

    static func folder(_ app: SteamApp, in library: URL) -> URL? {
        let d = library.appendingPathComponent("steamapps/common").appendingPathComponent(app.installdir)
        var isDir: ObjCBool = false
        return !app.installdir.isEmpty && FileManager.default.fileExists(atPath: d.path, isDirectory: &isDir) && isDir.boolValue ? d : nil
    }

    /// The installed apps of the app's library (vxrt_steam), with their folders; and those in vxrt_library, where
    /// scripts/fetch_windows_vr_app.sh puts one app per folder (steamcmd's force_install_dir: the folder is the app).
    static func installed() -> [String: (app: SteamApp, folder: URL)] {
        var out: [String: (SteamApp, URL)] = [:]
        for (id, app) in apps(in: Paths.steamLibrary) {
            if let f = folder(app, in: Paths.steamLibrary) { out[id] = (app, f) }
        }
        let more = Paths.drive.appendingPathComponent("vxrt_library")
        for dir in (try? FileManager.default.contentsOfDirectory(at: more, includingPropertiesForKeys: nil)) ?? [] {
            for (id, app) in apps(in: dir) where out[id] == nil { out[id] = (app, folder(app, in: dir) ?? dir) }
        }
        return out
    }

    /// The games of the player's Windows Steam (Paths.windowsSteam) that start from it (Title.windowsSteam), with
    /// their folders. Only read: that Steam keeps its library itself.
    static func windowsSteamGames() -> [String: (app: SteamApp, folder: URL)] {
        var out: [String: (SteamApp, URL)] = [:]
        for (id, app) in apps(in: Paths.windowsSteam) where Title.named(id)?.windowsSteam == true {
            // (only a finished install: Steam writes the manifest when it begins one)
            if let f = folder(app, in: Paths.windowsSteam), windowsSteamProgress(id)?.done != false { out[id] = (app, f) }
        }
        return out
    }

    /// How far Windows Steam is with installing a game, from its app manifest (nil: none yet): StateFlags 4 is "fully
    /// installed" (with 2 too when an update waits: still installed); BytesDownloaded of BytesToDownload while it downloads.
    static func windowsSteamProgress(_ id: String) -> (done: Bool, fraction: Double?)? {
        let url = Paths.windowsSteam.appendingPathComponent("steamapps/appmanifest_\(id).acf")
        guard let data = FileManager.default.contents(atPath: url.path) else { return nil }
        let text = String(decoding: data, as: UTF8.self)
        func number(_ key: String) -> Double? {
            guard let re = try? NSRegularExpression(pattern: "\"\(key)\"\\s+\"([0-9]+)\"", options: .caseInsensitive),
                  let m = re.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)),
                  let r = Range(m.range(at: 1), in: text) else { return nil }
            return Double(text[r])
        }
        let flags = Int(number("StateFlags") ?? 0)
        let total = number("BytesToDownload") ?? 0
        return (flags & 4 != 0, total > 0 ? min(1, (number("BytesDownloaded") ?? 0) / total) : nil)
    }

    // MARK: executables

    static let helpers = ["crashhandler", "crashpad", "crash_handler", "unitycrashhandler", "setup", "redist", "vcredist", "dxsetup",
                          "uninst", "launcher_helper", "webhelper", "easyanticheat", "beservice", "prereq",
                          "tb_streamer", "vconsole", "resourcecompiler", "injector"]
    static let antiCheat = ["easyanticheat": "Easy Anti-Cheat", "battleye": "BattlEye", "beservice": "BattlEye", "vgk": "Vanguard",
                            "xigncode": "XIGNCODE3", "nprotect": "nProtect GameGuard", "gameguard": "nProtect GameGuard",
                            "equ8": "EQU8", "anticheatexpert": "ACE", "denuvo": "Denuvo Anti-Cheat"]

    /// A game's executable (the one a known title names, else the one named like the game, else the largest that is
    /// not a helper: tools/vxrt_doctor.py's pick_exe) and the anti-cheat systems its files show (such games are not
    /// started: VeRantyx never works around anti-cheat).
    static func scan(_ folder: URL, name: String, exe: String?) -> (exe: URL?, antiCheat: [String]) {
        var exes: [URL] = []
        var cheats = Set<String>()
        guard let walk = FileManager.default.enumerator(at: folder, includingPropertiesForKeys: [.fileSizeKey], options: [.skipsHiddenFiles]) else { return (nil, []) }
        for case let url as URL in walk {
            let low = url.lastPathComponent.lowercased()
            for (key, system) in antiCheat where low.contains(key) { cheats.insert(system) }
            if low.hasSuffix(".exe") { exes.append(url) }
        }
        if let exe, let hit = exes.first(where: { $0.lastPathComponent.caseInsensitiveCompare(exe) == .orderedSame }) {
            return (hit, cheats.sorted())
        }
        let games = exes.filter { e in !helpers.contains { e.lastPathComponent.lowercased().contains($0) } }
        let pool0 = games.isEmpty ? exes : games
        let key = name.lowercased().filter { $0.isLetter || $0.isNumber }
        let named = pool0.filter { [key, key + "64", key + "win64"].contains($0.deletingPathExtension().lastPathComponent.lowercased().filter { $0.isLetter || $0.isNumber }) }
        let pool = named.isEmpty ? pool0 : named
        let size = { (u: URL) in (try? u.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0 }
        return (pool.max { size($0) < size($1) }, cheats.sorted())
    }

    // MARK: import

    /// Apps a folder holds as a Steam library (or the library above it, or a steamcmd install of one app).
    static func importable(from folder: URL) -> (library: URL, apps: [SteamApp])? {
        var u = folder
        for _ in 0..<4 {
            let found = apps(in: u).values.filter { self.folder($0, in: u) != nil }
            if !found.isEmpty { return (u, found.sorted { $0.name < $1.name }) }
            if u.lastPathComponent == "steamapps" || u.lastPathComponent == "common" { u.deleteLastPathComponent(); continue }
            u.deleteLastPathComponent()
        }
        return nil
    }

    /// Copies an app from another Steam library into vxrt_steam: as an APFS clone on the same disk (no extra space,
    /// immediate; the source is only read), else as a copy. `progress` gets 0...1 while copying.
    static func importApp(_ app: SteamApp, from library: URL, progress: @escaping @Sendable (Double) -> Void) async throws {
        guard let source = folder(app, in: library) else { throw Failure(TF("%@ is not in that folder", "%@ はそのフォルダにありません", app.name)) }
        let common = Paths.steamLibrary.appendingPathComponent("steamapps/common")
        try FileManager.default.createDirectory(at: common, withIntermediateDirectories: true)
        let dest = common.appendingPathComponent(app.installdir)
        if FileManager.default.fileExists(atPath: dest.path) { throw Failure(TF("%@ is already there", "%@ はもう入っています", app.name)) }
        let partial = common.appendingPathComponent(".\(app.installdir).partial")
        try? FileManager.default.removeItem(at: partial)
        let sameDisk = Mac.volumeID(source) == Mac.volumeID(common) && Mac.volumeID(source) != nil
        if sameDisk {
            let r = await run("/bin/cp", ["-c", "-R", source.path, partial.path])
            if r.status != 0 { try? FileManager.default.removeItem(at: partial); throw Failure(r.text) }
        } else {
            // (the copy's progress: how much the destination disk has filled since it started, against the app's size)
            let before = Mac.freeSpace(common) ?? 0
            let total = max(app.sizeOnDisk, 1)
            let ticker = Task.detached {
                while !Task.isCancelled {
                    try? await Task.sleep(nanoseconds: 1_000_000_000)
                    let used = before - (Mac.freeSpace(common) ?? before)
                    progress(min(0.99, Double(max(used, 0)) / Double(total)))
                }
            }
            let r = await run("/bin/cp", ["-R", source.path, partial.path])
            ticker.cancel()
            if r.status != 0 { try? FileManager.default.removeItem(at: partial); throw Failure(r.text) }
        }
        try FileManager.default.moveItem(at: partial, to: dest)
        let manifest = Paths.steamLibrary.appendingPathComponent("steamapps/appmanifest_\(app.id).acf")
        try? FileManager.default.removeItem(at: manifest)
        try FileManager.default.copyItem(at: app.manifest, to: manifest)
        progress(1)
    }
}
