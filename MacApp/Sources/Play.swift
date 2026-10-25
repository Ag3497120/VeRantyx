// Playing: the repository's launch scripts, run from the app's runtime folder with the settings chosen here, and what
// their logs say while they run (SteamVR up, the headset connected, the frame rate).
import Foundation
import Observation

enum Headset: String, CaseIterable, Identifiable {
    case quest, visionApp, visionALVR
    var id: String { rawValue }
    var usesALVR: Bool { self != .visionApp }
    var name: String {
        switch self {
        case .quest: return T("Meta Quest or Pico (ALVR)", "Meta Quest・Pico（ALVR）")
        case .visionApp: return T("Apple Vision Pro: the VeRantyx app", "Apple Vision Pro：VeRantyx アプリ")
        case .visionALVR: return T("Apple Vision Pro: the ALVR app", "Apple Vision Pro：ALVR アプリ")
        }
    }
    var route: Route { usesALVR ? .alvr : .visionApp }
}

/// The way to the headset a game takes (chosen on its card, kept per game): the ALVR app on the headset (a Vision
/// Pro's from the App Store, or a Quest's or Pico's: the Headset page says which), or the VeRantyx app on an Apple
/// Vision Pro. A game with none chosen follows the Headset page.
enum Route: String, CaseIterable, Identifiable {
    case alvr, visionApp
    var id: String { rawValue }
    var name: String {
        switch self {
        case .alvr: return T("ALVR app", "ALVR アプリ")
        case .visionApp: return T("VeRantyx app", "VeRantyx アプリ")
        }
    }
}

enum Quality: String, CaseIterable, Identifiable {
    case standard, sharp, k3 = "3k", k25 = "2.5k", max
    var id: String { rawValue }
    static func choices(for headset: Headset) -> [Quality] { headset.usesALVR ? [.standard, .sharp] : [.standard, .k3, .k25, .max] }
    var name: String {
        switch self {
        case .standard: return T("Recommended", "おすすめ")
        case .sharp: return T("Sharper", "高精細")
        case .k3: return "3K"
        case .k25: return "2.5K"
        case .max: return T("Maximum", "最高")
        }
    }
    var detail: String {
        switch self {
        case .standard: return T("Smooth on any Apple silicon Mac.", "どの Apple silicon Mac でも滑らか。")
        case .sharp: return T("Renders about 1300×1400 per eye (Recommended: 850×920), at 100 Mbps: for Pro and Max chips, best over a USB cable.",
                              "片目 約 1300×1400 で描画（おすすめは 850×920）・100 Mbps。Pro・Max チップ向け、USB ケーブル推奨。")
        case .k3: return T("1440×1620 per eye, 60 fps: for Pro and Max chips (24+ GPU cores).", "片目 1440×1620・60 fps。Pro・Max チップ向け（GPU 24 コア以上）。")
        case .k25: return T("2560×2880 per eye rendered: Max chips; 30–50 fps.", "片目 2560×2880 で描画。Max チップ向け、30〜50 fps。")
        case .max: return T("A Vision Pro eye's pixels: 16–21 fps on an M1 Max.", "Vision Pro の片目の画素数。M1 Max で 16〜21 fps。")
        }
    }
}

@MainActor @Observable final class Play {
    enum Phase: Equatable { case starting, steamVR, playing, stopping, ended }

    let title: String
    let appID: String?
    /// On the Mac's screen, without SteamVR or a headset.
    let flat: Bool
    var phase: Phase = .starting
    var headsetConnected = false
    var fps: Double? = nil
    /// What an ALVR headset shows, from its own statistics (ALVRBridge's "VeRantyx: headset:" line every 5 s): frames
    /// a second, and the whole latency from its movement to the picture (nil: none reported yet).
    var headsetFPS: Double? = nil
    var headsetLatency: Double? = nil
    /// Advice when the headset shows far fewer frames than the Mac makes, or late ones: the link is the limit.
    var headsetHint: String? = nil
    /// A game with its own launcher (Elite Dangerous): the game itself has started (from the launcher's PLAY).
    var gameStarted = false
    var usesLauncher: Bool { known?.launcher == true }
    var gameLines: [String] = []
    var problem: String? = nil { didSet { if let problem { Play.lastProblem = problem } } }
    /// Something the player is asked to do now (not a problem): a first ALVR headset to connect.
    var notice: String? = nil
    /// The last problem a play ended with (in the title of a report written after it).
    static var lastProblem: String?
    var startedAt = Date()
    /// When it ended, the Windows functions the game called that VXRuntime lacks ("DLL!function", as the runtime logs
    /// them), for a report of names only.
    var missingFunctions: [String] = []
    var endedAt: Date? = nil
    /// The frame rates read while the game ran (every second), for the "it ran" report.
    @ObservationIgnored private(set) var fpsSamples: [Double] = []
    /// The ALVR headset's reports while the game ran (one each 5 s), for the "it ran" report.
    @ObservationIgnored private(set) var headsetSamples: [HeadsetReport] = []
    @ObservationIgnored private var lastHeadsetLine = ""
    @ObservationIgnored private let wired: Bool
    /// The picture setting this play started with (the game's way to the headset may take another than the Play page's).
    @ObservationIgnored let quality: Quality
    @ObservationIgnored private var stopping = false

    @ObservationIgnored private var process = Process()
    @ObservationIgnored private var timer: Timer?
    /// The headset this play streams to (the game's own way to it: its card's choice).
    @ObservationIgnored let headset: Headset
    @ObservationIgnored private let known: Title?
    @ObservationIgnored private let gameLog: URL
    /// Where this play's part of the game log begins: a Windows Steam game's lines are in Steam's log, which runs on
    /// from play to play (0: the log is the game's own).
    @ObservationIgnored private var gameLogStart: UInt64 = 0
    /// A game Windows Steam starts (No Man's Sky): its lines in Steam's log are "[vxrt <gameExe>:<pid>] ...".
    var viaWindowsSteam: Bool { known?.windowsSteam == true }
    @ObservationIgnored private let launcherLog = Paths.logs.appendingPathComponent(Play.dryRun ? "launcher-dry-run.log" : "launcher.log")
    /// VERANTYX_DRY_RUN=1 (development, `--play ... --dry-run`): the script is not run; what would be run (the script,
    /// its arguments and the settings given to it) is written to launcher-dry-run.log instead.
    nonisolated static var dryRun: Bool { ProcessInfo.processInfo.environment["VERANTYX_DRY_RUN"] == "1" }

    init(title: String, appID: String? = nil, exe: URL?, headset: Headset, wired: Bool, quality: Quality, known: Title? = nil,
         flat: Bool = false) throws {
        self.title = title
        self.appID = appID
        self.headset = headset
        self.wired = wired
        self.quality = quality
        self.known = known
        self.flat = flat
        let scripts = Paths.runtime.appendingPathComponent("scripts")
        try FileManager.default.createDirectory(at: Paths.logs, withIntermediateDirectories: true)
        var env = ProcessInfo.processInfo.environment
        env.removeValue(forKey: "VERANTYX_REPO")
        env["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin:/usr/local/bin"
        env["VERANTYX_VR_DRIVE"] = Paths.drive.path
        env["VXRT_NO_WINE"] = "1"                          // (the app's setup only: never a Wine version's prefix)
        env["VXRT_VR_LOG_DIR"] = Paths.logs.path
        if known?.launcher == true { env["VXRT_MESSAGEBOX"] = "auto" }   // (the launcher's message boxes answer themselves)
        // (always said, never inherited: launch_vxrt_nms_vr.sh takes ALVR when it is not set)
        env["VERANTYX_ALVR"] = !flat && headset.usesALVR ? "1" : "0"
        if flat {
            env["VXRT_FLAT"] = "1"                          // (launch_vxrt_game.sh: no SteamVR, the game takes the Mac's focus)
            env["VXRT_STATS"] = "1"                         // (the game's frame rate, "present: N/s", in its log)
        } else if headset.usesALVR {
            if wired && headset == .quest { env["VERANTYX_ALVR_WIRED"] = "1" }
            if quality == .sharp {
                // (the game renders more pixels, not only the stream: with ALVR's defaults SteamVR's 0.55 supersampling
                //  leaves some 850x920 per eye whatever the stream's size, so a 3680x1840 stream only stretched them,
                //  issue #8. VXRT_VR_RENDER_SCALE sets supersampling to 1.0, as the Vision Pro app's 3k preset does;
                //  1.6: the driver's 800x900 x 1.6 a side; the stream about the same size, both eyes)
                env["VXRT_VR_RENDER_SCALE"] = "1.6"
                env["VERANTYX_STREAM_WIDTH"] = "2560"; env["VERANTYX_STREAM_HEIGHT"] = "1440"; env["VERANTYX_BITRATE_MBPS"] = "100"
            }
        } else if [.k3, .k25, .max].contains(quality) {
            env["VXRT_VR_QUALITY"] = quality.rawValue
        }
        if !flat, let proxy = known?.captureProxy { env["VXRT_CAPTURE_PROXY"] = proxy }
        process.executableURL = URL(fileURLWithPath: "/bin/bash")
        if known?.windowsSteam == true {
            // The player's Windows Steam starts the game, as its own Play button would (steam.exe -applaunch <id>,
            // with -HmdEnable 1 for VR; with SteamVR and the encoder in Steam's root first). VXRT_WSTEAM_NO_START:
            // Steam must be running already, opened and signed in to by the player (the script starts none itself,
            // so it answers none of its windows).
            env["VXRT_WSTEAM_NO_START"] = "1"
            env["VXRT_WSTEAM_LOG"] = Paths.logs.appendingPathComponent("vxrt-windows-steam.log").path
            process.arguments = [scripts.appendingPathComponent("launch_vxrt_nms_vr.sh").path]
            gameLog = Paths.logs.appendingPathComponent("vxrt-windows-steam.log")
            gameLogStart = (try? FileManager.default.attributesOfItem(atPath: gameLog.path)[.size] as? UInt64) ?? 0
        } else if let exe {
            process.arguments = [scripts.appendingPathComponent("launch_vxrt_game.sh").path, exe.path] + (known?.args ?? []) + (flat ? [] : known?.vrArgs ?? [])
            process.currentDirectoryURL = exe.deletingLastPathComponent()
            gameLog = Paths.logs.appendingPathComponent("vxrt-\(exe.deletingPathExtension().lastPathComponent).log")
        } else {
            process.arguments = [scripts.appendingPathComponent("launch_vxrt_alyx.sh").path]
            gameLog = Paths.logs.appendingPathComponent("vxrt-alyx.log")
        }
        process.environment = env
        if Play.dryRun {
            let keys = env.keys.filter { k in ["VERANTYX_", "VXRT_"].contains { k.hasPrefix($0) } && ProcessInfo.processInfo.environment[k] != env[k] }.sorted()
            let said = (process.arguments ?? []).map { "arg: \($0)" } + keys.map { "env: \($0)=\(env[$0]!)" }
            process.arguments = ["-c", "printf '%s\\n' \"$@\"", "dry-run"] + said
        }
        FileManager.default.createFile(atPath: launcherLog.path, contents: nil)
        let out = try FileHandle(forWritingTo: launcherLog)
        process.standardOutput = out
        process.standardError = out
        process.standardInput = FileHandle.nullDevice
        process.terminationHandler = { [weak self] p in
            let status = p.terminationStatus
            Task { @MainActor in self?.ended(status) }
        }
        try process.run()
        let t = Timer(timeInterval: 1, repeats: true) { [weak self] _ in Task { @MainActor in self?.poll() } }
        RunLoop.main.add(t, forMode: .common)             // (.common: also while a menu is open or the window is resized)
        timer = t
    }

    var running: Bool { phase != .ended }

    func stop() {
        guard process.isRunning else { return }
        phase = .stopping
        stopping = true
        process.terminate()                               // (the scripts' trap stops SteamVR, the encoder and the game)
        if viaWindowsSteam, let exe = known?.gameExe {
            // (launch_vxrt_nms_vr.sh leaves the game to Steam: the game is ended here; Windows Steam keeps running.
            //  Steam starts it by an upper-case path, ".../BINARIES/NMS.EXE": the match ignores case)
            let pattern = "build/vxrt.*/" + NSRegularExpression.escapedPattern(for: exe) + "( |$)"
            Task.detached { _ = await run("/usr/bin/pkill", ["-TERM", "-i", "-f", pattern]) }
        }
        let p = process
        DispatchQueue.global().asyncAfter(deadline: .now() + 15) {
            guard p.isRunning else { return }
            kill(p.processIdentifier, SIGKILL)
            Task { await Play.stopEverything() }
        }
    }

    /// Stops whatever VeRantyx started and left running (SteamVR on VXRuntime, a game, the encoder).
    nonisolated static func stopEverything() async {
        for signal in ["-TERM", "-KILL"] {
            _ = await run("/usr/bin/pkill", [signal, "-f", "VXRuntime/build/vxrt"])
            _ = await run("/usr/bin/pkill", [signal, "-f", "build/HardwareEncoder"])
            _ = await run("/usr/bin/pkill", [signal, "-f", "build/vxrt-helper webui-standin"])
            try? await Task.sleep(nanoseconds: 1_000_000_000)
        }
    }

    /// What the dry run said it would run (empty: not a dry run).
    var dryRunText: String { Play.dryRun ? Play.tail(launcherLog, 20000) : "" }

    private func ended(_ status: Int32) {
        timer?.invalidate()
        poll()
        phase = .ended
        endedAt = Date()
        headsetConnected = false
        headsetHint = nil
        missingFunctions = !fresh(gameLog) ? [] : viaWindowsSteam ? Play.missingFunctions(inText: gameText(1 << 22)) : Play.missingFunctions(in: gameLog)
        guard !stopping, problem == nil else { return }
        // (the game ended by itself: what its own last lines say; a child's, such as SteamVR's vrwebhelper, which
        //  cannot run here, are "[vxrt <name>:<pid>]" lines and say nothing about the game; in Windows Steam's log the
        //  game is such a child, and its lines are those)
        let own = (fresh(gameLog) ? viaWindowsSteam ? gameText(12000) : Play.tail(gameLog, 12000) : "").split(separator: "\n")
            .filter { viaWindowsSteam || $0.hasPrefix("[vxrt] ") }.suffix(40).joined(separator: "\n")
        problem = Play.hint(Play.tail(launcherLog, 6000) + "\n" + own)
            ?? T("The game stopped by itself. If it keeps doing so, please report it (Help > Report a problem) with the logs.",
                 "ゲームが自分で終了しました。繰り返す時は、ログを添えて報告してください（ヘルプ > 問題を報告）。")
    }

    /// A log written since this start (the last start's may still be there until the scripts rotate it).
    private func fresh(_ url: URL) -> Bool {
        ((try? url.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast) > startedAt
    }

    private func poll() {
        pollStatus()
        // (a sample a second while the game itself runs: its reported rate changes every 5 s, so this weighs by time)
        if let fps, running, usesLauncher ? gameStarted : phase == .playing { fpsSamples.append(fps) }
    }

    /// The game's own lines of this play: in Windows Steam's log, those of the game's process since the play began.
    private func gameText(_ bytes: Int) -> String {
        guard viaWindowsSteam, let exe = known?.gameExe else { return Play.tail(gameLog, bytes) }
        return Play.tail(gameLog, bytes * 4, from: gameLogStart).split(separator: "\n").filter { $0.contains("[vxrt \(exe):") }
            .joined(separator: "\n").suffix(bytes).description
    }

    private func pollStatus() {
        if phase == .starting && !flat && listening(27062) { phase = .steamVR }
        let game = fresh(gameLog) ? gameText(8000) : ""
        gameLines = Array(game.split(separator: "\n", omittingEmptySubsequences: true).suffix(14).map(String.init))
        if let exe = known?.gameExe, game.contains("[vxrt \(exe):") {
            gameStarted = true
            if flat, let line = game.split(separator: "\n").last(where: { $0.contains("[vxrt \(exe):") && $0.contains("present: ") }) {
                fps = Play.number(in: line, after: "present: ")
            }
        }
        if (phase == .steamVR || flat && phase == .starting) && (viaWindowsSteam ? gameStarted : !game.isEmpty) { phase = .playing }
        if flat {
            if let hint = Play.hint(Play.tail(launcherLog, 6000)) { problem = hint }
            notice = launcherNotice
            return
        }
        // The encoder's report every 5 s: frames reaching it (the game's, through SteamVR) and the headset's poses.
        let encoderLog = Paths.logs.appendingPathComponent("hardware-encoder.log")
        let encoder = fresh(encoderLog) ? Play.tail(encoderLog, 6000) : ""
        if let line = encoder.split(separator: "\n").last(where: { $0.hasPrefix("Pipeline ") }) {
            fps = Play.number(in: line, after: "capture=")
            if !headset.usesALVR {
                let pose = Play.number(in: line, after: "pose=") ?? 0
                headsetConnected = pose > 1 && !line.hasSuffix("peer=none")
            }
        }
        if headset.usesALVR {
            // (ALVR's own log: the bridge writes "VeRantyx: N frames in ..." every 5 s while a headset streams)
            let log = Play.tail(Paths.alvrLog, 6000)
            let fresh = ((try? Paths.alvrLog.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast) > Date().addingTimeInterval(-12)
            let lastStream = log.range(of: "VeRantyx: ", options: .backwards)?.lowerBound
            let lastGone = log.range(of: "Client disconnected", options: .backwards)?.lowerBound
            headsetConnected = fresh && lastStream != nil && (lastGone == nil || lastStream! > lastGone!)
            readHeadset(log)
        }
        let launcher = Play.tail(launcherLog, 6000)
        if let hint = Play.hint(launcher) { problem = hint }   // (the scripts' own messages)
        // a first ALVR headset: SteamVR waits for it (scripts/launch_vxrt_steamvr.sh), so the player connects it now
        let firstWait = launcher.range(of: "First start with this ALVR headset", options: .backwards)?.lowerBound
        let waitOver = launcher.range(of: "starting SteamVR", options: .backwards)?.lowerBound ?? launcher.range(of: "No ALVR headset connected yet", options: .backwards)?.lowerBound
        notice = firstWait != nil && (waitOver == nil || waitOver! < firstWait!)
            ? T("First start with this headset: open ALVR on the headset and connect now. SteamVR starts once it has, so that the picture lines up in both eyes.",
                "このヘッドセットで初めての起動です。ヘッドセットで ALVR を開いて接続してください。接続すると SteamVR が始まり、両目の映像がずれずに表示されます。")
            : launcherNotice
    }

    /// The newest "VeRantyx: headset:" line: what the headset shows, against the frames the Mac makes.
    private func readHeadset(_ log: String) {
        guard headsetConnected, let line = log.split(separator: "\n").last(where: { $0.contains("VeRantyx: headset: ") }),
              let report = HeadsetReport(line) else {
            headsetFPS = nil; headsetLatency = nil; headsetHint = nil
            return
        }
        headsetFPS = report.fps
        headsetLatency = report.latency
        guard String(line) != lastHeadsetLine else { return }
        lastHeadsetLine = String(line)
        guard usesLauncher ? gameStarted : phase == .playing else { return }
        headsetSamples.append(report)
        headsetHint = Play.headsetHint(headsetSamples.suffix(3), mac: fps, headset: headset, wired: wired, quality: quality)
    }

    /// Advice from the last three headset reports (15 s: one slow moment, a level loading, is not the link), when the
    /// headset shows under three quarters of the Mac's frames, or they spend over 40 ms on the network (ALVR's estimate:
    /// what is left of the whole latency after the Mac's and the headset's parts; some 20 ms over a home Wi-Fi), or it
    /// loses video (nil: it is fine). The whole latency is no measure of the link here: the Mac's part (the game, the
    /// encoder) is much of it.
    nonisolated static func headsetHint(_ recent: ArraySlice<HeadsetReport>, mac: Double?, headset: Headset, wired: Bool, quality: Quality) -> String? {
        guard recent.count == 3, let mac, mac >= 20 else { return nil }
        let shown = recent.compactMap(\.fps).sorted()
        let network = recent.compactMap(\.network).sorted()
        let fewer = shown.count == 3 && shown[1] < mac * 0.75
        let late = network.count == 3 && network[1] > 40
        let lossy = recent.map(\.losses).reduce(0, +) >= 6
        guard fewer || late || lossy else { return nil }
        var steps: [String] = []
        if quality == .sharp { steps.append(T("choose the quality Recommended (Headset page)", "画質を「おすすめ」にする（ヘッドセットのページ）")) }
        if headset == .quest && !wired { steps.append(T("connect the headset with a USB cable", "ヘッドセットを USB ケーブルでつなぐ")) }
        if !wired {
            steps.append(T("put the Mac on Ethernet (a cable to the router)", "Mac を有線 LAN（ルーターへのケーブル）にする"))
            steps.append(T("have the headset on the router's 5 or 6 GHz band with 80 MHz channels or wider, near the router",
                           "ヘッドセットをルーターの 5 GHz か 6 GHz（チャンネル幅 80 MHz 以上）につなぎ、ルーターの近くで使う"))
        }
        let what: String
        if fewer {
            what = TF("The headset shows %@ of the %@ frames a second the Mac makes: the stream is held up on the way.",
                      "Mac が毎秒 %2$@ フレーム作っても、ヘッドセットに表示されるのは %1$@ フレームです。途中の通信で詰まっています。",
                      String(format: "%.0f", shown[1]), String(format: "%.0f", mac))
        } else if late {
            what = TF("Each frame spends %@ ms on the network to the headset: the stream is held up on the way.",
                      "1 フレームがヘッドセットまでの通信に %@ ms かかっています。途中の通信で遅れています。", String(format: "%.0f", network[1]))
        } else {
            what = T("Video is lost on the way to the headset.", "ヘッドセットへの映像が途中で失われています。")
        }
        return steps.isEmpty ? what : TF("%@ Try: %@.", "%@試すこと：%@。", what, steps.joined(separator: T("; ", "、")))
    }

    /// What the player does in a launcher game's launcher, or in Windows Steam (nil once the game has started).
    private var launcherNotice: String? {
        if viaWindowsSteam && !gameStarted {
            return T("Windows Steam starts the game. If its window asks you something (an update, the game's agreement), answer it there yourself: VeRantyx presses nothing in Steam for you.",
                     "Windows 版 Steam がゲームを起動します。Steam のウィンドウで何か聞かれたら（更新、ゲームの利用規約など）、そこでご自身で答えてください。VeRantyx は Steam の中のボタンを代わりに押しません。")
        }
        guard usesLauncher && !gameStarted else { return nil }
        return T("The game's own launcher opens: use it as on Windows. Sign in, let it update if it asks, then press PLAY once.",
                 "ゲーム専用のランチャーが開きます。Windows と同じように操作してください。サインインし、更新を求められたら済ませてから、PLAY を 1 回押します。")
    }

    /// The Windows functions a game called that VXRuntime lacks, from its log: "DLL!function", each once, in the order
    /// first called. SteamVR's own helper programs (vrwebhelper and the like) are left out: they are not the game.
    nonisolated static func missingFunctions(in log: URL) -> [String] {
        guard let data = FileManager.default.contents(atPath: log.path) else { return [] }
        return missingFunctions(inText: String(decoding: data, as: UTF8.self))
    }

    nonisolated static func missingFunctions(inText text: String) -> [String] {
        let marker = "called an unimplemented function: "
        let steamVR = ["vrwebhelper", "vrcompositor", "vrserver", "vrmonitor", "vrdashboard", "vrstartup", "vrcmd"]
        var found: [String] = []
        for line in text.split(separator: "\n") where line.contains(marker) {
            if steamVR.contains(where: { line.contains("[vxrt \($0)") }) { continue }
            guard let r = line.range(of: marker) else { continue }
            let name = line[r.upperBound...].prefix { !$0.isWhitespace }
            if !name.isEmpty && !found.contains(String(name)) { found.append(String(name)) }
            if found.count == 60 { break }
        }
        return found
    }

    nonisolated static func number<S: StringProtocol>(in line: S, after key: String) -> Double? {
        guard let r = line.range(of: key) else { return nil }
        return Double(line[r.upperBound...].prefix { $0.isNumber || $0 == "." })
    }

    /// The last bytes of a log file, as text.
    /// (`from`: not before that offset; a log that has since become shorter, rotated, is read from its start)
    nonisolated static func tail(_ url: URL, _ bytes: Int, from start: UInt64 = 0) -> String {
        guard let h = try? FileHandle(forReadingFrom: url) else { return "" }
        defer { try? h.close() }
        let end = (try? h.seekToEnd()) ?? 0
        let first = start <= end ? start : 0
        try? h.seek(toOffset: max(first, end > UInt64(bytes) ? end - UInt64(bytes) : 0))
        return String(decoding: (try? h.readToEnd()) ?? Data(), as: UTF8.self)
    }

    /// What a log says went wrong, in words a player can act on (nil: nothing known).
    nonisolated static func hint(_ log: String) -> String? {
        if log.contains("Start Steam on the Mac and sign in first") || log.contains("Mac Steam is not running") {
            return T("Steam for Mac is not running: open Steam, sign in, and try again.", "Steam for Mac が起動していません。Steam を開いてサインインしてから、もう一度。")
        }
        if log.contains("Steam bridge: cannot load") {
            return T("The game could not reach Steam for Mac. Open Steam, let it finish updating, sign in, and try again.", "ゲームが Steam for Mac に接続できませんでした。Steam を開いて更新を終え、サインインしてから、もう一度。")
        }
        if log.contains("Windows Steam is not running") {
            return T("Windows Steam is not running: open it (Play page), sign in in its window yourself, and try again.",
                     "Windows 版 Steam が起動していません。開いて（プレイのページ）、そのウィンドウでご自身でサインインしてから、もう一度。")
        }
        if log.contains("No Windows Steam in") {
            return T("Windows Steam is not installed on VXRuntime (see the repository: scripts/launch_vxrt_windows_steam.sh).",
                     "VXRuntime に Windows 版 Steam が入っていません（リポジトリの scripts/launch_vxrt_windows_steam.sh を参照）。")
        }
        if log.contains("is not installed in that Steam") {
            return T("The game is not installed in your Windows Steam: install it there.", "ゲームが Windows 版 Steam に入っていません。そちらでインストールしてください。")
        }
        if log.contains("No Man's Sky did not start") {
            return T("The game did not start. Look at Windows Steam's window: it may be waiting for you (an update, the game's agreement).",
                     "ゲームが起動しませんでした。Windows 版 Steam のウィンドウを見てください。あなたの操作を待っているかもしれません（更新、ゲームの利用規約など）。")
        }
        if log.contains("SteamVR not found") { return T("SteamVR is not installed yet (Get started).", "SteamVR がまだ入っていません（はじめに）。") }
        if log.contains("No Microsoft runtime DLLs") { return T("The Windows runtime files are missing (Get started).", "Windows のランタイムファイルがありません（はじめに）。") }
        if log.contains("is already running") { return T("SteamVR or a game is already running. Stop it first (Help > Stop everything).", "SteamVR かゲームがすでに動いています。先に止めてください（ヘルプ > すべて停止）。") }
        if log.contains("called an unimplemented function") {
            return T("The game called a Windows function VXRuntime does not have yet. Please report it (Help > Report a problem).", "ゲームが VXRuntime にまだない Windows の機能を呼びました。報告してください（ヘルプ > 問題を報告）。")
        }
        return nil
    }
}

/// One of ALVRBridge's reports of what the headset shows (ALVRBridge/src/lib.rs, HeadsetLog): "VeRantyx: headset: 58.9
/// fps shown, latency 70 ms (Mac 20 ms, network 9 ms, decode 5 ms, headset 30 ms), 0 video losses (IDRs asked for),
/// 38.0 Mbps sent", or "0.0 fps shown, ..." or "no statistics from the client yet, ..." (a client that sends none).
struct HeadsetReport: Equatable {
    var fps: Double?
    var latency: Double?
    var network: Double?
    var losses: Int

    init?(_ line: Substring) {
        guard let r = line.range(of: "VeRantyx: headset: ") else { return nil }
        let text = line[r.upperBound...]
        fps = text.contains(" fps shown") ? Double(text.prefix { $0.isNumber || $0 == "." }) : nil
        latency = Play.number(in: text, after: "latency ")
        network = Play.number(in: text, after: "network ")
        losses = text.range(of: " video losses").map { Int(text[..<$0.lowerBound].split(separator: " ").last ?? "") ?? 0 } ?? 0
        if fps == nil && !text.hasPrefix("no statistics") { return nil }
    }
}
