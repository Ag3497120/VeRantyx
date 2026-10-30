// The app's state: what this Mac has (the checks of Get started), what is installed, the long jobs (downloads,
// imports, gathering the runtime files) and the game being played.
import AppKit
import Foundation
import Observation

struct Game: Identifiable, Hashable {
    var id: String
    var name: String
    var folder: URL
    var exe: URL?               // nil: Half-Life: Alyx (its own launcher) or no executable found
    var title: Title?           // a title the app knows
    var antiCheat: [String]
    var bytes: Int64
    var alyx: Bool { id == "546560" }
    var playable: Bool { antiCheat.isEmpty && (alyx || title?.nativeSupport != nil || exe != nil) }
    var flat: Bool { title?.flat == true }
    /// Started by the player's own Windows Steam (No Man's Sky), not with Mac Steam's bridge.
    var viaWindowsSteam: Bool { title?.windowsSteam == true }
}

enum Page: String, CaseIterable, Identifiable {
    case setup, play, headset, help
    var id: String { rawValue }
    var name: String {
        switch self {
        case .setup: return T("Get started", "はじめに")
        case .play: return T("Play", "プレイ")
        case .headset: return T("Headset", "ヘッドセット")
        case .help: return T("Help", "ヘルプ")
        }
    }
    var icon: String {
        switch self {
        case .setup: return "checklist"
        case .play: return "play.circle"
        case .headset: return "visionpro"
        case .help: return "questionmark.circle"
        }
    }
}

struct Job {
    var title: String
    var detail = ""
    var progress: Double? = nil
}

@MainActor @Observable final class AppModel {
    @ObservationIgnored private let savingPreferences: Bool
    var page: Page = .setup

    // This Mac
    var rosetta: Bool? = nil
    var steamInstalled = false
    var steamRunning = false
    /// The player's Windows Steam on VXRuntime (WindowsSteam), for the games it starts.
    var windowsSteamInstalled = false
    var windowsSteamRunning = false
    @ObservationIgnored private var windowsSteamProcess: Process? = nil
    var freeBytes: Int64? = nil
    var drive = Paths.drive

    // What is installed
    var games: [Game] = []
    var steamVR = false
    var redistCount = 0
    var installedBackends: [String: Set<GameBackend>] = [:]
    var nativeInstallProblems: [String: String] = [:]
    var preferredBackend: GameBackend = .vxrt {
        didSet { if savingPreferences { UserDefaults.standard.set(preferredBackend.rawValue, forKey: "preferredBackend") } }
    }
    var setupFocusID: String?
    var setupDownloading = false
    @ObservationIgnored private var setupDownloadCancelled = false
    @ObservationIgnored var nativeDownloadCancelled = false
    @ObservationIgnored var nativePreparation: Task<Void, Error>?
    @ObservationIgnored var nativeDownloadTask: Task<Bool, Never>?

    // Long jobs
    var job: Job? = nil
    var error: String? = nil
    let console = Console()
    var showConsole = false
    var importChoice: (library: URL, apps: [SteamApp])? = nil
    var account: String = UserDefaults.standard.string(forKey: "steamAccount") ?? "" {
        didSet { if savingPreferences { UserDefaults.standard.set(account, forKey: "steamAccount") } }   // (the account name only; never a password)
    }
    var wanted: Set<String> = Set(Title.all.filter { $0.id != Title.steamVR }.prefix(1).map(\.id))

    // Headset and play
    var headset: Headset = Headset(rawValue: UserDefaults.standard.string(forKey: "headset") ?? "") ?? .quest {
        didSet { if savingPreferences { UserDefaults.standard.set(headset.rawValue, forKey: "headset") }; if !sessionQualityChoices.contains(quality) { quality = .standard } }
    }
    var wired: Bool = UserDefaults.standard.bool(forKey: "wired") { didSet { if savingPreferences { UserDefaults.standard.set(wired, forKey: "wired") } } }
    var quality: Quality = Quality(rawValue: UserDefaults.standard.string(forKey: "quality") ?? "") ?? .standard {
        didSet { if savingPreferences { UserDefaults.standard.set(quality.rawValue, forKey: "quality") } }
    }
    /// Each game's way to the headset, chosen on its card (Route's raw value by app id); a game not in it follows the
    /// Headset page. Kept in the setting "headsetRoutes".
    var routes: [String: String] = (UserDefaults.standard.dictionary(forKey: "headsetRoutes") as? [String: String]) ?? [:] {
        didSet { if savingPreferences { UserDefaults.standard.set(routes, forKey: "headsetRoutes") } }
    }
    var play: Play? = nil
    var elsewhere: [String] = []          // VXRuntime programs running that the app did not start (the scripts')
    /// The same without the player's Windows Steam itself (steam.exe and its web helper), which runs on between plays.
    var elsewhereBesidesWindowsSteam: [String] { elsewhere.filter { !WindowsSteam.isOwn($0) } }

    init(savingPreferences: Bool = true, refreshAutomatically: Bool = true) {
        let preferences = LaunchPreferences.load(UserDefaults.standard.dictionaryRepresentation())
        self.savingPreferences = savingPreferences && !Play.dryRun
        preferredBackend = preferences.backend
        quality = preferences.quality
        if self.savingPreferences {
            UserDefaults.standard.set(preferences.backend.rawValue, forKey: "preferredBackend")
            UserDefaults.standard.set(preferences.quality.rawValue, forKey: "quality")
        }
        guard refreshAutomatically else { return }
        Task { await refresh() }
        RunLoop.main.add(Timer(timeInterval: 4, repeats: true) { [weak self] _ in Task { @MainActor in await self?.refreshLight() } }, forMode: .common)
    }

    // MARK: checks

    var appleSilicon: Bool { Mac.appleSilicon }
    var macOSOK: Bool { ProcessInfo.processInfo.isOperatingSystemAtLeast(OperatingSystemVersion(majorVersion: 14, minorVersion: 2, patchVersion: 0)) }
    var gamesInstalled: Bool { !games.isEmpty }
    var windowsRuntimeReady: Bool { appleSilicon && macOSOK && rosetta == true && steamInstalled && steamVR && redistCount > 0 }
    var ready: Bool { setupChecks.allSatisfy { $0 } }
    var setupSteps: (done: Int, of: Int) {
        let steps = setupChecks
        return (steps.filter { $0 }.count, steps.count)
    }

    func refresh() async {
        rosetta = await Mac.rosetta()
        await refreshLight()
        reloadLibrary()
    }

    /// The quick checks, every few seconds: Steam running, other VXRuntime programs, free space.
    func refreshLight() async {
        steamInstalled = Steam.installed
        steamRunning = await Steam.running()
        windowsSteamInstalled = WindowsSteam.installed
        let wsteam = windowsSteamInstalled ? await WindowsSteam.running() : false
        windowsSteamRunning = wsteam
        freeBytes = Mac.freeSpace(drive)
        elsewhere = play?.running == true ? [] : await pgrep("[b]uild/vxrt.*\\.[Ee][Xx][Ee]|[a]lyx_host|[n]ative-session --session|[H]ardwareEncoder")
    }

    func reloadLibrary() { Task { await loadLibrary() } }

    /// The same, finished when the games list is (the get-and-play flow starts the game it has just installed).
    func loadLibrary() async {
        let installed = Library.installed()
        var variants = Dictionary(uniqueKeysWithValues: installed.keys.map { ($0, Set([GameBackend.vxrt])) })
        var nativeGames: [Game] = []
        var nativeProblems: [String: String] = [:]
        for title in Title.all {
            guard let support = title.nativeSupport else { continue }
            let layout = support.layout(drive: drive, runtime: Paths.runtime)
            do {
                let record = try layout.installation()
                variants[title.id, default: []].insert(.nativeARM64)
                nativeGames.append(Game(id: title.id, name: title.name, folder: layout.versionRoot(record).appendingPathComponent("game-root"), exe: nil, title: title, antiCheat: [], bytes: title.bytes))
            } catch {
                if FileManager.default.fileExists(atPath: layout.manifest.path) { nativeProblems[title.id] = error.localizedDescription }
            }
        }
        steamVR = installed[Title.steamVR] != nil
        redistCount = Redist.count
        var found = installed.filter { $0.key != Title.steamVR && $0.key != "228980" && Title.named($0.key)?.windowsSteam != true }
        // (the games Windows Steam starts: from its own library, where it keeps them)
        for (id, entry) in Library.windowsSteamGames() { found[id] = entry; variants[id, default: []].insert(.vxrt) }
        installedBackends = variants
        nativeInstallProblems = nativeProblems
        let entries = found
        let sorted = await Task.detached { () -> [Game] in
            var list: [Game] = []
            for (id, entry) in entries {
                let title = Title.named(id)
                let scan = Library.scan(entry.folder, name: entry.app.name, exe: title?.exe)
                list.append(Game(id: id, name: entry.app.name.isEmpty ? (title?.name ?? id) : entry.app.name, folder: entry.folder,
                                 exe: id == "546560" ? nil : scan.exe, title: title, antiCheat: scan.antiCheat, bytes: entry.app.sizeOnDisk))
            }
            // (the titles known to run first, in the app's order; then the others by name)
            let order = Title.all.map(\.id)
            list.sort { (order.firstIndex(of: $0.id) ?? 99, $0.name) < (order.firstIndex(of: $1.id) ?? 99, $1.name) }
            return list
        }.value
        games = sorted
        for native in nativeGames.reversed() where !games.contains(where: { $0.id == native.id }) { games.insert(native, at: 0) }
    }

    func isInstalled(_ id: String, backend: GameBackend? = nil) -> Bool {
        let backend = backend ?? self.backend(forID: id)
        if id == Title.steamVR { return backend == .vxrt && steamVR }
        return installedBackends[id]?.contains(backend) == true
    }

    // MARK: actions

    func installRosetta() async {
        job = Job(title: T("Installing Rosetta 2…", "Rosetta 2 をインストール中…"))
        let r = await Mac.installRosetta()
        job = nil
        if r.status != 0 && !r.text.contains("-128") { error = r.text }      // (-128: the password dialog was cancelled)
        await refresh()
    }

    func chooseDrive() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.prompt = T("Use this folder", "このフォルダを使う")
        panel.message = T("Where SteamVR and the games go (they take tens of gigabytes; an external SSD is fine)",
                          "SteamVR とゲームを置く場所（数十 GB 使います。外付け SSD でも構いません）")
        guard panel.runModal() == .OK, let url = panel.url else { return }
        Paths.drive = url
        drive = url
        Task { await refresh() }
    }

    /// The setup button installs selected variants without starting a game.
    func download() async {
        guard !configurationLocked else { return }
        let plan = installationPlan(for: wanted)
        guard !plan.isEmpty else { return }
        setupDownloading = true
        setupDownloadCancelled = false
        defer { setupDownloading = false }
        let windows = plan.filter { $0.backend == .vxrt }.map(\.id)
        if !windows.isEmpty { guard await downloadWindows(windows) else { return } }
        for request in plan where request.backend == .nativeARM64 {
            guard !setupDownloadCancelled else { return }
            guard let support = Title.named(request.id)?.nativeSupport else { continue }
            let task = Task { @MainActor in await support.download(in: self) }
            nativeDownloadTask = task
            let completed = await task.value
            nativeDownloadTask = nil
            if !completed { return }
        }
    }

    func cancelSetupDownload() { setupDownloadCancelled = true; cancelNativeDownload() }

    /// The same for these titles (those not installed yet, and not Windows Steam's); false: not all of them came.
    @discardableResult func downloadWindows(_ wanted: [String]) async -> Bool {
        let ids = Title.all.map(\.id).filter { wanted.contains($0) && !isInstalled($0, backend: .vxrt) && Title.named($0)?.windowsSteam != true }
        guard !ids.isEmpty else { return true }
        guard !account.trimmingCharacters(in: .whitespaces).isEmpty else { return false }
        error = nil
        let need = ids.compactMap { Title.named($0)?.bytes }.reduce(0, +)
        if !Play.dryRun, let free = Mac.freeSpace(drive), free < need + (5 << 30) {
            error = TF("Not enough space: these need about %@, %@ is free on that disk. Choose another game folder in Get started, or fewer games.",
                       "空き容量が足りません。約 %@ 必要で、そのディスクの空きは %@ です。「はじめに」で別の保存先を選ぶか、ゲームを減らしてください。",
                       gigabytes(need), gigabytes(free))
            return false
        }
        if Play.dryRun {
            // (development: what would be downloaded, nothing fetched)
            for id in ids { flowNote("dry run: would download \(id) with steamcmd (+login \(account.trimmingCharacters(in: .whitespaces)) +app_update \(id) validate) into \(Paths.steamLibrary.path)") }
            return false
        }
        do {
            if !SteamCmd.installed {
                job = Job(title: T("Getting steamcmd from Valve…", "Valve から steamcmd を取得中…"))
                try await SteamCmd.install()
            }
            let common = Paths.steamLibrary.appendingPathComponent("steamapps/common")
            try FileManager.default.createDirectory(at: common, withIntermediateDirectories: true)
            showConsole = true
            for id in ids {
                let name = Title.named(id)?.name ?? id
                job = Job(title: TF("Downloading %@…", "%@ をダウンロード中…", name))
                let have = Library.apps(in: Paths.steamLibrary)[id].flatMap { Library.folder($0, in: Paths.steamLibrary) }
                let dest = have ?? common.appendingPathComponent("app_\(id)")
                try FileManager.default.createDirectory(at: dest, withIntermediateDirectories: true)
                var env = ProcessInfo.processInfo.environment
                env["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"
                env["TERM"] = "dumb"
                try console.start([SteamCmd.script.path, "+@sSteamCmdForcePlatformType", "windows", "+force_install_dir", dest.path,
                                   "+login", account.trimmingCharacters(in: .whitespaces), "+app_license_request", id, "+app_update", id, "validate", "+quit"],
                                  environment: env)
                let watch = Task { @MainActor in
                    while self.console.running {
                        self.job?.progress = self.console.progress
                        self.job?.detail = self.console.stage
                        try? await Task.sleep(nanoseconds: 300_000_000)
                    }
                }
                let status = await console.finished()
                watch.cancel()
                let acf = dest.appendingPathComponent("steamapps/appmanifest_\(id).acf")
                guard status == 0, let m = Library.manifest(acf) else {
                    let said = console.lines.last { $0.contains("ERROR") || $0.contains("FAILED") } ?? ""
                    throw Failure(TF("steamcmd did not finish %@ (exit %@). %@", "steamcmd が %@ を完了しませんでした（終了コード %@）。%@", name, "\(status)", said))
                }
                // (the first download goes to app_<id>: then named after the app's own folder, as Steam names it)
                let final = common.appendingPathComponent(m.installdir)
                if dest.standardizedFileURL != final.standardizedFileURL { try FileManager.default.moveItem(at: dest, to: final) }
                let into = Paths.steamLibrary.appendingPathComponent("steamapps/appmanifest_\(id).acf")
                try? FileManager.default.removeItem(at: into)
                try FileManager.default.copyItem(at: final.appendingPathComponent("steamapps/appmanifest_\(id).acf"), to: into)
                reloadLibrary()
            }
            job = nil
            await collect()
        } catch {
            job = nil
            self.error = error.localizedDescription
            await loadLibrary()
            return false
        }
        await loadLibrary()
        return true
    }

    /// Asks for a folder with a Steam library (a Windows PC's drive, a CrossOver or Whisky bottle, ...).
    func chooseImport() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.prompt = T("Look here", "ここを見る")
        panel.message = T("A Steam library with the Windows versions: the folder that holds “steamapps” (from a Windows PC's drive, a CrossOver or Whisky bottle, …)",
                          "Windows 版の入った Steam ライブラリ：「steamapps」のあるフォルダ（Windows PC のドライブ、CrossOver や Whisky のボトルなど）")
        guard panel.runModal() == .OK, let url = panel.url else { return }
        guard let found = Library.importable(from: url) else {
            error = T("No Steam apps there (no steamapps/appmanifest_*.acf).", "そこには Steam のアプリがありません（steamapps/appmanifest_*.acf がない）。")
            return
        }
        importChoice = found
    }

    func importApps(_ apps: [SteamApp], from library: URL) async {
        importChoice = nil
        error = nil
        for app in apps where !isInstalled(app.id, backend: .vxrt) {
            job = Job(title: TF("Copying %@…", "%@ をコピー中…", app.name))
            do {
                try await Library.importApp(app, from: library) { p in Task { @MainActor in self.job?.progress = p } }
            } catch {
                self.error = "\(app.name): \(error.localizedDescription)"
            }
            reloadLibrary()
        }
        job = nil
        await collect()
    }

    /// Gathers the Microsoft runtime DLLs from the installs' own redistributables.
    func collect() async {
        guard steamVR || !games.isEmpty else { return }
        job = Job(title: T("Gathering the Windows runtime files…", "Windows のランタイムファイルを集めています…"))
        do {
            let n = try await Redist.collect { name in Task { @MainActor in self.job?.detail = name } }
            if n == 0 { error = T("No runtime files found in the installs (is SteamVR there?).", "インストールの中にランタイムファイルが見つかりません（SteamVR は入っていますか）。") }
        } catch {
            self.error = error.localizedDescription
        }
        job = nil
        reloadLibrary()
    }

    // MARK: play

    /// A game's way to the headset: its card's choice, else the Headset page's.
    func route(for game: Game) -> Route { route(forID: game.id) }
    func route(forID id: String) -> Route { routes[id].flatMap(Route.init(rawValue:)) ?? headset.route }

    /// Chooses a game's way to the headset; the Headset page's own is kept as "none chosen" (it then follows that page).
    func setRoute(_ route: Route, forID id: String) {
        if route == headset.route { routes.removeValue(forKey: id) } else { routes[id] = route.rawValue }
    }

    /// The headset a game streams to: the VeRantyx app on a Vision Pro, or ALVR on the headset the Headset page names
    /// (a Vision Pro's ALVR app when that page has the VeRantyx app).
    func headset(for game: Game) -> Headset { headset(forID: game.id) }
    func headset(forID id: String) -> Headset {
        switch route(forID: id) {
        case .visionApp: return .visionApp
        case .alvr: return headset.usesALVR ? headset : .visionALVR
        }
    }

    /// The picture setting for a game: the Play page's, when its way to the headset offers it; else Recommended.
    func quality(for game: Game) -> Quality { quality(forID: game.id) }
    func quality(forID id: String) -> Quality {
        if backend(forID: id) == .nativeARM64 { return quality == .sharp ? .sharp : .standard }
        return Quality.choices(for: headset(forID: id)).contains(quality) ? quality : .standard
    }

    /// Whether its Play button can be pressed: a game that waits for Steam for Mac (start) can be, before Steam is up.
    func canPress(_ game: Game, flat: Bool) -> Bool { canStart(game, flat: flat, assumingSteam: waitsForSteam(game)) }

    /// Whether a game can start now, on the Mac's screen or in the headset (assumingSteam: as if Steam for Mac were
    /// running, for a start that will wait for it).
    func canStart(_ game: Game, flat: Bool, assumingSteam: Bool = false) -> Bool {
        guard game.playable, isInstalled(game.id), play?.running != true, steamWait == nil, job == nil, !setupDownloading, !(flat && !game.flat) else { return false }
        let steamRunning = self.steamRunning || assumingSteam
        if backend(forID: game.id) == .nativeARM64 {
            return appleSilicon && macOSOK && steamRunning && game.title.map { nativeReason(for: $0) == nil } == true && elsewhere.isEmpty && !flat
        }
        if game.viaWindowsSteam {
            // (Windows Steam, opened and signed in to by the player, instead of Mac Steam; SteamVR only for the headset)
            return appleSilicon && rosetta == true && redistCount > 0 && windowsSteamRunning && elsewhereBesidesWindowsSteam.isEmpty && (flat || steamVR)
        }
        return windowsRuntimeReady && steamRunning && elsewhere.isEmpty
    }

    /// Starts the player's Windows Steam (scripts/launch_vxrt_windows_steam.sh, with the settings
    /// scripts/launch_vxrt_nms_vr.sh starts it with). Its window opens; the player signs in there and answers its
    /// windows (such as "Steam Service Error": Cancel) themselves. It runs on after the app quits; it is quit from its
    /// own window.
    func openWindowsSteam() {
        guard windowsSteamInstalled, !windowsSteamRunning, windowsSteamProcess?.isRunning != true else { return }
        do {
            try FileManager.default.createDirectory(at: Paths.logs, withIntermediateDirectories: true)
            let p = Process()
            p.executableURL = URL(fileURLWithPath: "/bin/bash")
            p.arguments = [Paths.runtime.appendingPathComponent("scripts/launch_vxrt_windows_steam.sh").path]
            var env = windowsSteamEnv()
            env["VXRT_WSTEAM_GPU"] = "1"
            env["VXRT_WSTEAM_WEBHELPER_ARGS"] = "--single-process --disable-gpu-compositing"
            p.environment = env
            let log = Paths.logs.appendingPathComponent("vxrt-windows-steam-launch.log")
            FileManager.default.createFile(atPath: log.path, contents: nil)
            let out = try FileHandle(forWritingTo: log)
            p.standardOutput = out
            p.standardError = out
            p.standardInput = FileHandle.nullDevice
            try p.run()
            windowsSteamProcess = p
            windowsSteamRunning = true
        } catch {
            self.error = error.localizedDescription
        }
    }

    // MARK: get and play

    /// "Get and play": a known title not installed yet (or SteamVR missing for the headset) is fetched the title's own
    /// way, after the player has seen the size and the free space and said yes; then the first-time setup (the runtime
    /// files); then it starts, on the Mac's screen or the way to the headset chosen on its card.
    struct Flow {
        enum Stage: Equatable {
            /// The size and the free space, waiting for the player's yes.
            case confirm
            /// Windows Steam has to be opened and signed in to by the player (then Continue).
            case openWindowsSteam
            /// Windows Steam installs it (a window it opens of its own is answered there by the player); its progress.
            case windowsSteamInstalling(Double?)
            /// steamcmd, the runtime files, the start.
            case working
            /// Stopped: why, and what the player does.
            case stopped(String)
        }
        var title: Title
        var flat: Bool
        var backend: GameBackend = .vxrt
        var startAfterInstall = true
        var stage: Stage = .confirm
        /// What steamcmd fetches (SteamVR, the game), and whether Windows Steam installs the game.
        var steamcmd: [String] = []
        var windowsSteam = false
        var bytes: Int64 = 0
        var free: Int64? = nil
        /// What it did, step by step (`--get` prints it).
        var log: [String] = []
    }
    var flow: Flow? = nil
    /// The last flow's steps, kept after it has ended (`--get` prints them).
    @ObservationIgnored var flowLog: [String] = []

    func flowNote(_ line: String) { flow?.log.append(line); flowLog.append(line) }

    /// The flow's first step: what is missing, and the question (or, with nothing missing, the start).
    func getAndPlay(_ title: Title, flat: Bool, reinstall: Bool = false, startAfterInstall: Bool = true) {
        guard flow == nil || flow?.stage != .working, play?.running != true, job == nil, !setupDownloading else { return }
        var f = Flow(title: title, flat: flat && title.flat)
        f.startAfterInstall = startAfterInstall
        f.backend = backend(forID: title.id)
        if f.backend == .nativeARM64, let support = title.nativeSupport {
            f.steamcmd = isInstalled(title.id) && !reinstall ? [] : [title.id]
            f.bytes = f.steamcmd.isEmpty ? 0 : estimatedBytes(for: title)
            f.free = Mac.freeSpace(drive)
            flow = f
            flowNote("\(title.name): native ARM64 via ALVR; no Windows SteamVR or redistributables")
            if !support.layout(drive: drive, runtime: Paths.runtime).bundled { stopFlow(nativeReason(for: title) ?? "Native components are missing"); return }
            if isInstalled(title.id) && !reinstall { flow?.stage = .working; Task { await finishFlow() } }
            return
        }
        if !f.flat && !steamVR { f.steamcmd.append(Title.steamVR) }
        switch title.source {
        case .steamcmd: if !isInstalled(title.id) { f.steamcmd.append(title.id) }
        case .windowsSteam: f.windowsSteam = !isInstalled(title.id)
        }
        f.bytes = f.steamcmd.compactMap { Title.named($0)?.bytes }.reduce(0, +) + (f.windowsSteam ? title.bytes : 0)
        f.free = Mac.freeSpace(title.source == .windowsSteam && f.steamcmd.isEmpty ? Paths.windowsSteamRoot : drive) ?? Mac.freeSpace(drive)
        flow = f
        flowNote("\(title.name): steamcmd \(f.steamcmd), Windows Steam install \(f.windowsSteam), \(gigabytes(f.bytes)), free \(f.free.map(gigabytes) ?? "?")")
        if title.source == .windowsSteam && !windowsSteamInstalled {
            // (installing Windows Steam itself is not done by the app: Valve's installer on VXRuntime is not there yet)
            stopFlow(T("This game needs Windows Steam on VXRuntime, which is not installed yet. Installing it is a manual step for now (the repository: VXRuntime/README.md, “The Windows Steam client”).",
                       "このゲームには VXRuntime 上の Windows 版 Steam が必要ですが、まだ入っていません。今のところ導入は手作業です（リポジトリの VXRuntime/README.md「The Windows Steam client」）。"))
            return
        }
        if f.steamcmd.isEmpty && !f.windowsSteam { flow?.stage = .working; Task { await finishFlow() } }
    }

    private func stopFlow(_ why: String) {
        flowNote("stopped: \(why)")
        flow?.stage = .stopped(why)
    }

    /// The player's yes: fetch, then set up and start.
    func confirmFlow() async {
        guard var f = flow, f.stage == .confirm else { return }
        if !Play.dryRun, let free = f.free, free < f.bytes + (5 << 30) {
            stopFlow(TF("Not enough space: about %@ is needed, %@ is free.", "空き容量が足りません。約 %@ 必要で、空きは %@ です。", gigabytes(f.bytes), gigabytes(free)))
            return
        }
        if !Play.dryRun && !f.steamcmd.isEmpty && account.trimmingCharacters(in: .whitespaces).isEmpty {
            stopFlow(T("Enter your Steam account name in Get started → Games first.", "先に「はじめに」→「ゲーム」で Steam アカウント名を入力してください。"))
            return
        }
        f.stage = .working
        flow = f
        if f.backend == .nativeARM64 {
            guard let support = f.title.nativeSupport else { return }
            let task = Task { @MainActor in await support.download(in: self) }
            nativeDownloadTask = task
            let downloaded = await task.value
            nativeDownloadTask = nil
            guard downloaded else { stopFlow(Play.dryRun ? "dry run: nothing downloaded" : error ?? "Native download did not finish"); return }
            await finishFlow()
            return
        }
        if !f.steamcmd.isEmpty {
            flowNote("steamcmd: \(f.steamcmd)")
            guard await downloadWindows(f.steamcmd) else {
                stopFlow(Play.dryRun ? "dry run: nothing downloaded" : error ?? T("The download did not finish.", "ダウンロードが完了しませんでした。"))
                return
            }
        }
        if f.windowsSteam {
            await askWindowsSteam()
        } else {
            await finishFlow()
        }
    }

    /// Windows Steam installs the game: it has to be running (opened and signed in to by the player), then it is asked
    /// to (steam://install/<id>) after the player's yes in the app, and starts the download; a window it opens of its
    /// own (where to install, an agreement) the player answers there. Only games the account owns are installed.
    func askWindowsSteam() async {
        guard let f = flow else { return }
        let running = windowsSteamInstalled ? await WindowsSteam.running() : false
        windowsSteamRunning = running
        guard windowsSteamRunning || Play.dryRun else {
            flowNote("waiting: Windows Steam is not running")
            flow?.stage = .openWindowsSteam
            return
        }
        flow?.stage = .windowsSteamInstalling(nil)
        let r = await run("/bin/bash", [Paths.runtime.appendingPathComponent("scripts/vxrt_windows_steam_ask.sh").path, "--", "steam://install/\(f.title.id)"],
                          env: windowsSteamEnv(dryRun: Play.dryRun))
        flowNote("asked Windows Steam: steam://install/\(f.title.id) (exit \(r.status)) \(r.text.trimmingCharacters(in: .whitespacesAndNewlines))")
        guard r.status == 0 else { stopFlow(Play.hint(r.text) ?? r.text); return }
        if Play.dryRun { stopFlow("dry run: would wait for Windows Steam's install"); return }
        // (until Steam has installed it; the player may take their time in Steam's window, or say no there)
        for _ in 0..<(6 * 60 * 60 / 2) {
            try? await Task.sleep(nanoseconds: 2_000_000_000)
            guard flow?.title.id == f.title.id else { return }        // (cancelled)
            if let p = Library.windowsSteamProgress(f.title.id) {
                flow?.stage = .windowsSteamInstalling(p.fraction)
                if p.done { flowNote("installed by Windows Steam"); await finishFlow(); return }
            }
        }
        stopFlow(T("Windows Steam has not finished installing it. Look at its window; press Play when it is done.",
                   "Windows 版 Steam のインストールが終わっていません。Steam のウィンドウを見て、終わったらプレイを押してください。"))
    }

    /// The first-time setup, then the start.
    private func finishFlow() async {
        guard let f = flow else { return }
        flow?.stage = .working
        if f.backend == .vxrt && redistCount == 0 {
            flowNote("gathering the runtime files")
            if !Play.dryRun { await collect() }
        }
        await loadLibrary()
        if !f.startAfterInstall { flowNote("installation prepared; launch stays in Play"); flow = nil; return }
        guard let game = games.first(where: { $0.id == f.title.id }) else {
            stopFlow(TF("%@ is not installed yet.", "%@ はまだ入っていません。", f.title.name)); return
        }
        guard canPress(game, flat: f.flat) else {
            let why = game.viaWindowsSteam && !windowsSteamRunning
                ? T("It is installed. Open Windows Steam and sign in, then press Play.", "入っています。Windows 版 Steam を開いてサインインしてから、プレイを押してください。")
                : T("It is installed but cannot start yet: finish what the Play page asks (Steam open, Get started, nothing else running), then press Play.",
                    "入っていますが、まだ起動できません。プレイのページの案内（Steam を開く、「はじめに」、ほかに動いているものを止める）を済ませてから、プレイを押してください。")
            stopFlow(why); return
        }
        flowNote("start \(game.name) flat=\(f.flat) headset=\(headset(for: game).rawValue)")
        flow = nil
        start(game, flat: f.flat)
    }

    func cancelFlow() { if flow?.backend == .nativeARM64 { cancelNativeDownload() }; flow = nil }

    /// The settings the Windows Steam scripts are run with (the same places as the app's).
    private func windowsSteamEnv(dryRun: Bool = false) -> [String: String] {
        var env = ProcessInfo.processInfo.environment
        env.removeValue(forKey: "VERANTYX_REPO")
        env["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin:/usr/local/bin"
        env["VERANTYX_VR_DRIVE"] = Paths.drive.path
        env["VXRT_REDIST"] = Paths.redist.path
        env["VXRT_WSTEAM_LOG"] = Paths.logs.appendingPathComponent("vxrt-windows-steam.log").path
        if dryRun { env["VXRT_WSTEAM_DRY_RUN"] = "1" }
        return env
    }

    /// A start waiting for Steam for Mac to be ready (Steam.readiness()): the game, and what Steam is doing meanwhile.
    struct SteamWait: Equatable {
        var gameID: String
        var name: String
        var flat: Bool
        /// nil while the first answer is awaited.
        var state: SteamReadiness?
    }
    var steamWait: SteamWait? = nil
    @ObservationIgnored private var steamWaitTask: Task<Void, Never>?
    /// How Steam for Mac is asked (tests answer for it).
    @ObservationIgnored var steamReadiness: () async -> SteamReadiness = { await Steam.readiness() }
    @ObservationIgnored var steamPollNanoseconds: UInt64 = 2_000_000_000

    /// Whether a game's start waits for Steam for Mac to be signed in: the games VXRuntime runs with Mac Steam (not
    /// those Windows Steam starts, not the native ARM64 ones, which have their own bridge).
    func waitsForSteam(_ game: Game) -> Bool { !Play.dryRun && backend(forID: game.id) == .vxrt && !game.viaWindowsSteam }

    /// Play: once Steam for Mac is signed in (online, for a game whose launcher signs in through Steam), the game
    /// starts; meanwhile the Play page says what it waits for (steamWait), and the player can cancel.
    func start(_ game: Game, flat: Bool = false) {
        guard play?.running != true, steamWait == nil else { return }
        guard waitsForSteam(game) else { launch(game, flat: flat); return }
        if !canStart(game, flat: flat, assumingSteam: true) { launch(game, flat: flat); return }   // (says why it cannot)
        steamWait = SteamWait(gameID: game.id, name: game.name, flat: flat, state: nil)
        steamWaitTask = Task { @MainActor in await waitForSteam(game, flat: flat) }
    }

    /// What the Play page says while a start waits for Steam for Mac.
    var steamWaitText: String? {
        guard let wait = steamWait else { return nil }
        switch wait.state {
        case nil, .ready?: return TF("Checking Steam for Mac before starting %@…", "%@ を起動する前に Steam for Mac を確認しています…", wait.name)
        case .notRunning?: return TF("Steam for Mac is not running. Open it and sign in: %@ starts when it is ready.", "Steam for Mac が起動していません。開いてサインインしてください。準備ができると %@ が起動します。", wait.name)
        case .notSignedIn?: return TF("Waiting for Steam to sign in… %@ starts when it has.", "Steam のサインインを待っています… 済むと %@ が起動します。", wait.name)
        case .offline?: return TF("Waiting for Steam to go online… %@ signs in through Steam, which needs Steam's servers.", "Steam がオンラインになるのを待っています… %@ は Steam 経由でサインインするので、Steam のサーバーへの接続が必要です。", wait.name)
        case .unavailable?: return SteamWaitText.unavailable
        }
    }

    func cancelSteamWait() {
        steamWaitTask?.cancel()
        steamWaitTask = nil
        steamWait = nil
    }

    private func waitForSteam(_ game: Game, flat: Bool) async {
        let needsOnline = game.title?.launcher == true
        while !Task.isCancelled, steamWait?.gameID == game.id {
            let state = await steamReadiness()
            guard !Task.isCancelled, steamWait?.gameID == game.id else { return }
            if state.allowsStart(needsOnline: needsOnline) {
                steamRunning = true                       // (seen just now: the 4-second refresh may not have yet)
                steamWait = nil
                steamWaitTask = nil
                launch(game, flat: flat)
                return
            }
            if state == .unavailable {
                steamWait = nil
                steamWaitTask = nil
                error = SteamWaitText.unavailable
                return
            }
            steamWait?.state = state
            try? await Task.sleep(nanoseconds: steamPollNanoseconds)
        }
    }

    private func launch(_ game: Game, flat: Bool) {
        guard play?.running != true else { return }
        if !Play.dryRun, !canStart(game, flat: flat) { error = backend(forID: game.id) == .nativeARM64 ? game.title.flatMap { nativeReason(for: $0) } ?? T("Open Steam and sign in: the games need it.", "Steam を開いてサインインしてください。ゲームに必要です。") : T("Finish Get started first.", "先に「はじめに」を済ませてください。"); return }
        error = nil
        do {
            play = try Play(title: game.name, appID: game.id, exe: game.alyx ? nil : game.exe, headset: headset(for: game), wired: wired,
                            quality: quality(for: game), known: game.title, flat: flat && game.title?.flat == true,
                            backend: backend(forID: game.id))
        } catch {
            self.error = error.localizedDescription
        }
    }

    // MARK: help

    func systemReport() -> String {
        return """
        VeRantyx app \(AppBuild.description)
        Mac: \(Mac.model), \(Mac.chip), \(Mac.memoryGB) GB, macOS \(Mac.macOS)
        Preferred game runtime: \(effectivePreference.rawValue)
        Headset: \(headset.rawValue)\(headset == .quest && wired ? " (USB cable)" : ""), quality \(quality.rawValue)
        Installed: \(steamVR ? "SteamVR" : "no SteamVR"), \(games.map(\.name).joined(separator: ", ")); runtime DLLs: \(redistCount)
        """
    }

    /// GitHub's form, filled in: the title says the headset, the Mac and what went wrong (the last play's problem),
    /// so that reports differ at a glance; `logs`, the zip file saved for it (Report.saveLogs).
    /// "It ran": the GitHub form filled in from a play that has ended (the game, where it was played, this Mac and the
    /// frame rate the app read); the player only checks it and presses Submit. Nothing personal is in it.
    func runsReportURL(_ play: Play) -> URL {
        var c = URLComponents(string: "https://github.com/Ag3497120/VeRantyx/issues/new")!
        let game = (play.appID.map { "\(play.title) (\($0))" } ?? play.title) + " [\(play.backend.rawValue)]"
        let place = play.flat ? "The Mac's screen" :
            "\(play.headset.name)\(play.headset == .quest && wired ? ", USB cable" : ""), \(play.quality.name)"
        // (loading screens read 0 and start-up benchmarks run far above play, so the range is the middle 80 %)
        let samples = play.fpsSamples.filter { $0 >= 1 }.sorted()
        let minutes = Int(((play.endedAt ?? Date()).timeIntervalSince(play.startedAt) / 60).rounded())
        var fps = samples.isEmpty ? "" : String(format: "median %.0f fps, mostly %.0f-%.0f, over %d min",
            samples[samples.count / 2], samples[samples.count / 10], samples[samples.count * 9 / 10], max(minutes, 1))
        // (with ALVR: what the headset showed, from its own statistics, which the Mac's number above does not say)
        let shown = play.headsetSamples.compactMap(\.fps).sorted(), latency = play.headsetSamples.compactMap(\.latency).sorted()
        if !shown.isEmpty {
            fps += (fps.isEmpty ? "" : " on the Mac; ") + String(format: "headset (ALVR): median %.0f fps shown, mostly %.0f-%.0f", shown[shown.count / 2],
                shown[shown.count / 10], shown[shown.count * 9 / 10])
            if !latency.isEmpty { fps += String(format: ", latency median %.0f ms", latency[latency.count / 2]) }
            let losses = play.headsetSamples.map(\.losses).reduce(0, +)
            if losses > 0 { fps += ", \(losses) video losses" }
        }
        c.queryItems = [URLQueryItem(name: "template", value: "works.yml"),
                        URLQueryItem(name: "title", value: "[runs] \(game) on \(Mac.chip)\(play.flat ? ", Mac screen" : ", \(play.headset.rawValue)")"),
                        URLQueryItem(name: "game", value: game),
                        URLQueryItem(name: "mode", value: place),
                        URLQueryItem(name: "mac", value: "\(Mac.model), \(Mac.chip), \(Mac.memoryGB) GB, macOS \(Mac.macOS), VeRantyx \(AppBuild.description)"),
                        URLQueryItem(name: "fps", value: fps)]
        c.percentEncodedQuery = c.percentEncodedQuery?.replacingOccurrences(of: "+", with: "%2B")   // (a "+" is a space to GitHub)
        return c.url!
    }

    /// The Windows functions a play's game called that VXRuntime lacks, as a GitHub form filled in (names only).
    func missingFunctionsURL(_ play: Play) -> URL {
        var c = URLComponents(string: "https://github.com/Ag3497120/VeRantyx/issues/new")!
        let game = play.appID.map { "\(play.title) (\($0))" } ?? play.title
        let first = play.missingFunctions.prefix(2).joined(separator: ", ")
        c.queryItems = [URLQueryItem(name: "template", value: "missing-functions.yml"),
                        URLQueryItem(name: "title", value: "[missing] \(game): \(first)\(play.missingFunctions.count > 2 ? ", ..." : "")"),
                        URLQueryItem(name: "game", value: game),
                        URLQueryItem(name: "functions", value: play.missingFunctions.joined(separator: "\n")),
                        URLQueryItem(name: "mac", value: "\(Mac.model), \(Mac.chip), \(Mac.memoryGB) GB, macOS \(Mac.macOS), VeRantyx \(AppBuild.description)")]
        c.percentEncodedQuery = c.percentEncodedQuery?.replacingOccurrences(of: "+", with: "%2B")
        return c.url!
    }

    func reportURL(logs: String? = nil) -> URL {
        var c = URLComponents(string: "https://github.com/Ag3497120/VeRantyx/issues/new")!
        let kind: String
        let headset = play?.headset ?? headset             // (the last play's: its game may take another way)
        let quality = play?.quality ?? quality
        switch headset {
        case .quest: kind = "[ALVR] Quest/Pico"
        case .visionALVR: kind = "[ALVR] Vision Pro"
        case .visionApp: kind = "[Vision Pro app]"
        }
        var what = (Play.lastProblem ?? "test report").split(whereSeparator: { ".。\n".contains($0) }).first.map(String.init) ?? "test report"
        if what.count > 70 { what = String(what.prefix(67)) + "..." }
        let title = "\(kind) + \(Mac.chip), macOS \(Mac.macOS): \(what)"
        let logsText = logs.map { "Attached: \($0) (saved on the Desktop by the app; drag it here)" }
            ?? "Logs: ~/Library/Logs/VeRantyx (Help > Open the logs folder)"
        c.queryItems = [URLQueryItem(name: "template", value: "alvr-headset.yml"),
                        URLQueryItem(name: "title", value: title),
                        URLQueryItem(name: "logs", value: logsText),
                        URLQueryItem(name: "mac", value: "\(Mac.model), \(Mac.chip), \(Mac.memoryGB) GB, macOS \(Mac.macOS), VeRantyx \(AppBuild.description)"),
                        URLQueryItem(name: "command", value: "VeRantyx app: \(play?.backend.rawValue ?? effectivePreference.rawValue), \(headset.rawValue)\(headset == .quest && wired ? ", USB cable" : ""), \(quality.rawValue)")]
        c.percentEncodedQuery = c.percentEncodedQuery?.replacingOccurrences(of: "+", with: "%2B")   // (a "+" is a space to GitHub)
        return c.url!
    }
}

enum SteamWaitText {
    static var unavailable: String {
        T("Couldn't reach Steam for Mac: its client could not be loaded. Open Steam, let it finish updating, sign in, and try again.",
          "Steam for Mac に接続できませんでした（Steam のクライアントを読み込めません）。Steam を開いて更新を終え、サインインしてから、もう一度。")
    }
}
