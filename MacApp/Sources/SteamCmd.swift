// steamcmd, Valve's command-line Steam client, downloads the Windows builds of SteamVR and the games with the user's own
// account (Steam for Mac lists only Mac games). It runs here in a terminal of its own (a pseudo-terminal), shown in a
// console in the app: when steamcmd asks for the password or a Steam Guard code, the user types it into the console's
// field and it goes to steamcmd as typed. The app never keeps it: not in a file, not in its settings, not in the
// console's text (steamcmd does not echo it). steamcmd remembers the sign-in itself, so later downloads ask nothing.
import Darwin
import Foundation
import Observation

enum SteamCmd {
    static let download = URL(string: "https://steamcdn-a.akamaihd.net/client/installer/steamcmd_osx.tar.gz")!
    /// Homebrew's steamcmd if there is one (it may hold a sign-in already), else the app's own copy.
    static var script: URL {
        let brew = URL(fileURLWithPath: "/opt/homebrew/bin/steamcmd")
        if FileManager.default.isExecutableFile(atPath: brew.path) { return brew }
        return Paths.steamcmd.appendingPathComponent("steamcmd.sh")
    }
    static var installed: Bool { FileManager.default.isExecutableFile(atPath: script.path) }

    /// Downloads steamcmd from Valve into the app's folder and unpacks it (it updates itself on its first run).
    static func install() async throws {
        try FileManager.default.createDirectory(at: Paths.steamcmd, withIntermediateDirectories: true)
        let (file, response) = try await URLSession.shared.download(from: download)
        guard (response as? HTTPURLResponse)?.statusCode == 200 else { throw Failure(T("steamcmd could not be downloaded from Valve", "Valve から steamcmd をダウンロードできませんでした")) }
        let r = await run("/usr/bin/tar", ["-xzf", file.path, "-C", Paths.steamcmd.path])
        try? FileManager.default.removeItem(at: file)
        guard r.status == 0, installed else { throw Failure(r.text) }
    }
}

/// A program running in a pseudo-terminal, its output as lines, and what it asks for.
@MainActor @Observable final class Console {
    enum Ask: Equatable { case secret(String), code(String) }

    var lines: [String] = []
    var partial = ""                      // the line being written: prompts end without a newline
    var asking: Ask? = nil
    var progress: Double? = nil           // steamcmd's "progress: 42.10 (...)", 0...1
    var stage = ""                        // downloading, verifying, ...
    var running = false
    var exitStatus: Int32? = nil
    var phoneConfirmation = false         // steamcmd waits for the sign-in to be approved in the Steam Mobile app

    @ObservationIgnored private var master: Int32 = -1
    @ObservationIgnored private var pid: pid_t = 0
    @ObservationIgnored private var reader: DispatchSourceRead?
    @ObservationIgnored private var exitWatch: DispatchSourceProcess?
    @ObservationIgnored private var waiters: [CheckedContinuation<Int32, Never>] = []
    @ObservationIgnored private var pending = Data()

    func clear() { lines = []; partial = ""; asking = nil; progress = nil; stage = ""; exitStatus = nil; phoneConfirmation = false }

    /// Starts the program (argv[0] a path) in a new session whose controlling terminal is the pseudo-terminal.
    func start(_ argv: [String], environment: [String: String]) throws {
        clear()
        var m: Int32 = -1, s: Int32 = -1
        var size = winsize(ws_row: 50, ws_col: 200, ws_xpixel: 0, ws_ypixel: 0)
        guard openpty(&m, &s, nil, nil, &size) == 0 else { throw Failure("openpty: \(String(cString: strerror(errno)))") }
        let slaveName = String(cString: ptsname(m))
        var fa: posix_spawn_file_actions_t? = nil
        posix_spawn_file_actions_init(&fa)
        posix_spawn_file_actions_addopen(&fa, 0, slaveName, O_RDWR, 0)     // (in the new session: its terminal)
        posix_spawn_file_actions_adddup2(&fa, 0, 1)
        posix_spawn_file_actions_adddup2(&fa, 0, 2)
        var attr: posix_spawnattr_t? = nil
        posix_spawnattr_init(&attr)
        posix_spawnattr_setflags(&attr, Int16(POSIX_SPAWN_SETSID | POSIX_SPAWN_CLOEXEC_DEFAULT))
        let cargs = argv.map { strdup($0) } + [nil]
        let cenv = environment.map { strdup("\($0.key)=\($0.value)") } + [nil]
        var child: pid_t = 0
        let r = posix_spawn(&child, argv[0], &fa, &attr, cargs, cenv)
        cargs.forEach { free($0) }
        cenv.forEach { free($0) }
        posix_spawn_file_actions_destroy(&fa)
        posix_spawnattr_destroy(&attr)
        close(s)
        guard r == 0 else { close(m); throw Failure("\(argv[0]): \(String(cString: strerror(r)))") }
        master = m
        pid = child
        running = true
        let fd = m
        let src = DispatchSource.makeReadSource(fileDescriptor: fd, queue: .global())
        src.setEventHandler { [weak self] in
            var buf = [UInt8](repeating: 0, count: 16384)
            let n = read(fd, &buf, buf.count)
            if n <= 0 { src.cancel(); return }
            let data = Data(buf[0..<n])
            Task { @MainActor in self?.take(data) }
        }
        src.setCancelHandler { close(fd) }
        src.resume()
        reader = src
        let watch = DispatchSource.makeProcessSource(identifier: child, eventMask: .exit, queue: .global())
        watch.setEventHandler { [weak self] in
            var status: Int32 = 0
            waitpid(child, &status, 0)
            let code = (status & 0x7F) == 0 ? (status >> 8) & 0xFF : 128 + (status & 0x7F)
            Task { @MainActor in self?.ended(code) }
            watch.cancel()
        }
        watch.resume()
        exitWatch = watch
    }

    /// Waits until the program has exited; its exit status.
    func finished() async -> Int32 {
        if let exitStatus, !running { return exitStatus }
        return await withCheckedContinuation { waiters.append($0) }
    }

    /// Types a line into the program (what it asked for). Kept nowhere: written to the terminal and dropped.
    func send(_ text: String) {
        guard master >= 0, running else { return }
        var bytes = Array(text.utf8) + [13]
        _ = bytes.withUnsafeBufferPointer { write(master, $0.baseAddress, $0.count) }
        for i in bytes.indices { bytes[i] = 0 }
        asking = nil
    }

    func cancel() {
        guard running, pid > 0 else { return }
        kill(pid, SIGINT)
        let p = pid
        DispatchQueue.global().asyncAfter(deadline: .now() + 5) { kill(p, SIGKILL) }
    }

    private func ended(_ code: Int32) {
        running = false
        exitStatus = code
        asking = nil
        reader?.cancel()
        master = -1
        if !partial.isEmpty { lines.append(partial); partial = "" }
        waiters.forEach { $0.resume(returning: code) }
        waiters = []
    }

    private static let escape = try! NSRegularExpression(pattern: "\u{1B}\\[[0-9;?]*[ -/]*[@-~]")
    private static let progressLine = try! NSRegularExpression(pattern: "Update state \\(0x[0-9a-fA-F]+\\) ([a-z ]+), progress: ([0-9.]+)")
    private static let secretPrompt = try! NSRegularExpression(pattern: "(?i)password:\\s*$")
    private static let codePrompt = try! NSRegularExpression(pattern: "(?i)(steam ?guard|two-factor|authenticator|auth)[^:\\n]*code[^:\\n]*:\\s*$|(?i)enter the current code[^:\\n]*:?\\s*$")

    private func take(_ data: Data) {
        pending.append(data)
        // (whole UTF-8 characters only: a read can end inside one, whose rest comes with the next)
        var cut = pending.count
        while cut > max(0, pending.count - 3) && String(data: pending.prefix(cut), encoding: .utf8) == nil { cut -= 1 }
        if String(data: pending.prefix(cut), encoding: .utf8) == nil { cut = pending.count }
        let text = String(decoding: pending.prefix(cut), as: UTF8.self)
        pending = Data(pending.dropFirst(cut))
        let clean = Console.escape.stringByReplacingMatches(in: text, range: NSRange(text.startIndex..., in: text), withTemplate: "")
        for ch in clean {
            switch ch {
            case "\r\n", "\n": finishLine()
            case "\r": partial = ""                                  // (a line written over itself)
            default: partial.append(ch)
            }
        }
        if !clean.isEmpty, asking != nil, !partial.isEmpty || clean.contains("\n") { asking = nil }
        let tail = partial as NSString
        let all = NSRange(location: 0, length: tail.length)
        if Console.secretPrompt.firstMatch(in: partial, range: all) != nil {
            asking = .secret(T("Steam password", "Steam のパスワード"))
        } else if Console.codePrompt.firstMatch(in: partial, range: all) != nil {
            asking = .code(T("Steam Guard code (from the e-mail or the Steam Mobile app)", "Steam Guard コード（メールか Steam モバイルアプリ）"))
        }
    }

    private func finishLine() {
        let line = partial
        partial = ""
        if let m = Console.progressLine.firstMatch(in: line, range: NSRange(line.startIndex..., in: line)),
           let s = Range(m.range(at: 1), in: line), let p = Range(m.range(at: 2), in: line), let v = Double(line[p]) {
            stage = String(line[s])
            progress = v / 100
        }
        if line.localizedCaseInsensitiveContains("confirm the login in the Steam Mobile app") { phoneConfirmation = true }
        if line.contains("Logged in OK") || line.contains("Waiting for user info...OK") { phoneConfirmation = false }
        lines.append(line)
        if lines.count > 600 { lines.removeFirst(lines.count - 600) }
    }
}
