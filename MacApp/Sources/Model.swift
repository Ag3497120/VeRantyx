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
    var playable: Bool { antiCheat.isEmpty && (alyx || exe != nil) }
    var flat: Bool { title?.flat == true }
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
    var page: Page = .setup

    // This Mac
    var rosetta: Bool? = nil
    var steamInstalled = false
    var steamRunning = false
    var freeBytes: Int64? = nil
    var drive = Paths.drive

    // What is installed
    var games: [Game] = []
    var steamVR = false
    var redistCount = 0

    // Long jobs
    var job: Job? = nil
    var error: String? = nil
    let console = Console()
    var showConsole = false
    var importChoice: (library: URL, apps: [SteamApp])? = nil
    var account: String = UserDefaults.standard.string(forKey: "steamAccount") ?? "" {
        didSet { UserDefaults.standard.set(account, forKey: "steamAccount") }   // (the account name only; never a password)
    }
    var wanted: Set<String> = [Title.steamVR, "546560"]

    // Headset and play
    var headset: Headset = Headset(rawValue: UserDefaults.standard.string(forKey: "headset") ?? "") ?? .quest {
        didSet { UserDefaults.standard.set(headset.rawValue, forKey: "headset"); if !Quality.choices(for: headset).contains(quality) { quality = .standard } }
    }
    var wired: Bool = UserDefaults.standard.bool(forKey: "wired") { didSet { UserDefaults.standard.set(wired, forKey: "wired") } }
    var quality: Quality = Quality(rawValue: UserDefaults.standard.string(forKey: "quality") ?? "") ?? .standard {
        didSet { UserDefaults.standard.set(quality.rawValue, forKey: "quality") }
    }
    var play: Play? = nil
    var elsewhere: [String] = []          // VXRuntime programs running that the app did not start (the scripts')

    init() {
        Task { await refresh() }
        RunLoop.main.add(Timer(timeInterval: 4, repeats: true) { [weak self] _ in Task { @MainActor in await self?.refreshLight() } }, forMode: .common)
    }

    // MARK: checks

    var appleSilicon: Bool { Mac.appleSilicon }
    var macOSOK: Bool { ProcessInfo.processInfo.isOperatingSystemAtLeast(OperatingSystemVersion(majorVersion: 14, minorVersion: 2, patchVersion: 0)) }
    var gamesInstalled: Bool { !games.isEmpty }
    var ready: Bool { appleSilicon && rosetta == true && steamInstalled && steamVR && redistCount > 0 }
    var setupSteps: (done: Int, of: Int) {
        let steps = [appleSilicon && macOSOK, rosetta == true, steamInstalled, steamVR && gamesInstalled, redistCount > 0]
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
        freeBytes = Mac.freeSpace(drive)
        elsewhere = play?.running == true ? [] : await pgrep("[b]uild/vxrt.*\\.exe")
    }

    func reloadLibrary() {
        let installed = Library.installed()
        steamVR = installed[Title.steamVR] != nil
        redistCount = Redist.count
        let found = installed.filter { $0.key != Title.steamVR && $0.key != "228980" }
        Task.detached {
            var list: [Game] = []
            for (id, entry) in found {
                let title = Title.named(id)
                let scan = Library.scan(entry.folder, name: entry.app.name, exe: title?.exe)
                list.append(Game(id: id, name: entry.app.name.isEmpty ? (title?.name ?? id) : entry.app.name, folder: entry.folder,
                                 exe: id == "546560" ? nil : scan.exe, title: title, antiCheat: scan.antiCheat, bytes: entry.app.sizeOnDisk))
            }
            // (the titles known to run first, in the app's order; then the others by name)
            let order = Title.all.map(\.id)
            list.sort { (order.firstIndex(of: $0.id) ?? 99, $0.name) < (order.firstIndex(of: $1.id) ?? 99, $1.name) }
            let sorted = list
            await MainActor.run { self.games = sorted }
        }
    }

    func isInstalled(_ id: String) -> Bool { id == Title.steamVR ? steamVR : games.contains { $0.id == id } }

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

    /// Downloads the Windows builds of the chosen titles with the user's own Steam account (steamcmd), one at a time,
    /// SteamVR first; then gathers the runtime files.
    func download() async {
        let ids = Title.all.map(\.id).filter { wanted.contains($0) && !isInstalled($0) }
        guard !ids.isEmpty, !account.trimmingCharacters(in: .whitespaces).isEmpty else { return }
        error = nil
        let need = ids.compactMap { Title.named($0)?.bytes }.reduce(0, +)
        if let free = Mac.freeSpace(drive), free < need + (5 << 30) {
            error = TF("Not enough space: these need about %@, %@ is free on that disk. Choose another place in step 3, or fewer games.",
                       "空き容量が足りません。約 %@ 必要で、そのディスクの空きは %@ です。手順 3 で別の場所を選ぶか、ゲームを減らしてください。",
                       gigabytes(need), gigabytes(free))
            return
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
        }
        reloadLibrary()
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
        for app in apps where !isInstalled(app.id) {
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

    func start(_ game: Game, flat: Bool = false) {
        guard play?.running != true else { return }
        error = nil
        do {
            play = try Play(title: game.name, appID: game.id, exe: game.alyx ? nil : game.exe, headset: headset, wired: wired, quality: quality,
                            known: game.title, flat: flat && game.title?.flat == true)
        } catch {
            self.error = error.localizedDescription
        }
    }

    // MARK: help

    func systemReport() -> String {
        return """
        VeRantyx app \(AppBuild.description)
        Mac: \(Mac.model), \(Mac.chip), \(Mac.memoryGB) GB, macOS \(Mac.macOS)
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
        let game = play.appID.map { "\(play.title) (\($0))" } ?? play.title
        let place = play.flat ? "The Mac's screen" :
            "\(headset.name)\(headset == .quest && wired ? ", USB cable" : ""), \(quality.name)"
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
                        URLQueryItem(name: "title", value: "[runs] \(game) on \(Mac.chip)\(play.flat ? ", Mac screen" : ", \(headset.rawValue)")"),
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
                        URLQueryItem(name: "command", value: "VeRantyx app: \(headset.rawValue)\(headset == .quest && wired ? ", USB cable" : ""), \(quality.rawValue)")]
        c.percentEncodedQuery = c.percentEncodedQuery?.replacingOccurrences(of: "+", with: "%2B")   // (a "+" is a space to GitHub)
        return c.url!
    }
}
