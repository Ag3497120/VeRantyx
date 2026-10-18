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
        case .sharp: return T("3680×1840 at 100 Mbps: for Pro and Max chips, best over a USB cable.", "3680×1840・100 Mbps。Pro・Max チップ向け、USB ケーブル推奨。")
        case .k3: return T("1440×1620 per eye, 60 fps: for Pro and Max chips (24+ GPU cores).", "片目 1440×1620・60 fps。Pro・Max チップ向け（GPU 24 コア以上）。")
        case .k25: return T("2560×2880 per eye rendered: Max chips; 30–50 fps.", "片目 2560×2880 で描画。Max チップ向け、30〜50 fps。")
        case .max: return T("A Vision Pro eye's pixels: 16–21 fps on an M1 Max.", "Vision Pro の片目の画素数。M1 Max で 16〜21 fps。")
        }
    }
}

@MainActor @Observable final class Play {
    enum Phase: Equatable { case starting, steamVR, playing, stopping, ended }

    let title: String
    var phase: Phase = .starting
    var headsetConnected = false
    var fps: Double? = nil
    var gameLines: [String] = []
    var problem: String? = nil
    var startedAt = Date()
    @ObservationIgnored private var stopping = false

    @ObservationIgnored private var process = Process()
    @ObservationIgnored private var timer: Timer?
    @ObservationIgnored private let headset: Headset
    @ObservationIgnored private let gameLog: URL
    @ObservationIgnored private let launcherLog = Paths.logs.appendingPathComponent("launcher.log")

    init(title: String, exe: URL?, headset: Headset, wired: Bool, quality: Quality) throws {
        self.title = title
        self.headset = headset
        let scripts = Paths.runtime.appendingPathComponent("scripts")
        try FileManager.default.createDirectory(at: Paths.logs, withIntermediateDirectories: true)
        var env = ProcessInfo.processInfo.environment
        env.removeValue(forKey: "VERANTYX_REPO")
        env["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin:/usr/local/bin"
        env["VERANTYX_VR_DRIVE"] = Paths.drive.path
        env["VXRT_NO_WINE"] = "1"                          // (the app's setup only: never a Wine version's prefix)
        env["VXRT_VR_LOG_DIR"] = Paths.logs.path
        if headset.usesALVR {
            env["VERANTYX_ALVR"] = "1"
            if wired && headset == .quest { env["VERANTYX_ALVR_WIRED"] = "1" }
            if quality == .sharp {
                env["VERANTYX_STREAM_WIDTH"] = "3680"; env["VERANTYX_STREAM_HEIGHT"] = "1840"; env["VERANTYX_BITRATE_MBPS"] = "100"
            }
        } else if [.k3, .k25, .max].contains(quality) {
            env["VXRT_VR_QUALITY"] = quality.rawValue
        }
        process.executableURL = URL(fileURLWithPath: "/bin/bash")
        if let exe {
            process.arguments = [scripts.appendingPathComponent("launch_vxrt_game.sh").path, exe.path]
            process.currentDirectoryURL = exe.deletingLastPathComponent()
            gameLog = Paths.logs.appendingPathComponent("vxrt-\(exe.deletingPathExtension().lastPathComponent).log")
        } else {
            process.arguments = [scripts.appendingPathComponent("launch_vxrt_alyx.sh").path]
            gameLog = Paths.logs.appendingPathComponent("vxrt-alyx.log")
        }
        process.environment = env
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

    private func ended(_ status: Int32) {
        timer?.invalidate()
        poll()
        phase = .ended
        headsetConnected = false
        guard !stopping, problem == nil else { return }
        // (the game ended by itself: what its own last lines say; a child's, such as SteamVR's vrwebhelper, which
        //  cannot run here, are "[vxrt <name>:<pid>]" lines and say nothing about the game)
        let own = (fresh(gameLog) ? Play.tail(gameLog, 12000) : "").split(separator: "\n").filter { $0.hasPrefix("[vxrt] ") }.suffix(40).joined(separator: "\n")
        problem = Play.hint(Play.tail(launcherLog, 6000) + "\n" + own)
            ?? T("The game stopped by itself. If it keeps doing so, please report it (Help > Report a problem) with the logs.",
                 "ゲームが自分で終了しました。繰り返す時は、ログを添えて報告してください（ヘルプ > 問題を報告）。")
    }

    /// A log written since this start (the last start's may still be there until the scripts rotate it).
    private func fresh(_ url: URL) -> Bool {
        ((try? url.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast) > startedAt
    }

    private func poll() {
        if phase == .starting && listening(27062) { phase = .steamVR }
        let game = fresh(gameLog) ? Play.tail(gameLog, 8000) : ""
        if phase == .steamVR && !game.isEmpty { phase = .playing }
        gameLines = Array(game.split(separator: "\n", omittingEmptySubsequences: true).suffix(14).map(String.init))
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
        }
        if let hint = Play.hint(Play.tail(launcherLog, 6000)) { problem = hint }   // (the scripts' own messages)
    }

    nonisolated static func number(in line: Substring, after key: String) -> Double? {
        guard let r = line.range(of: key) else { return nil }
        return Double(line[r.upperBound...].prefix { $0.isNumber || $0 == "." })
    }

    /// The last bytes of a log file, as text.
    nonisolated static func tail(_ url: URL, _ bytes: Int) -> String {
        guard let h = try? FileHandle(forReadingFrom: url) else { return "" }
        defer { try? h.close() }
        let end = (try? h.seekToEnd()) ?? 0
        try? h.seek(toOffset: end > UInt64(bytes) ? end - UInt64(bytes) : 0)
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
        if log.contains("SteamVR not found") { return T("SteamVR is not installed yet (Get started).", "SteamVR がまだ入っていません（はじめに）。") }
        if log.contains("No Microsoft runtime DLLs") { return T("The Windows runtime files are missing (Get started).", "Windows のランタイムファイルがありません（はじめに）。") }
        if log.contains("is already running") { return T("SteamVR or a game is already running. Stop it first (Help > Stop everything).", "SteamVR かゲームがすでに動いています。先に止めてください（ヘルプ > すべて停止）。") }
        if log.contains("called an unimplemented function") {
            return T("The game called a Windows function VXRuntime does not have yet. Please report it (Help > Report a problem).", "ゲームが VXRuntime にまだない Windows の機能を呼びました。報告してください（ヘルプ > 問題を報告）。")
        }
        return nil
    }
}
