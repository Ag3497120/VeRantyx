import Foundation
#if canImport(Darwin)
import Darwin
#else
import Glibc
#endif

/// The engine: `xcrun python3 engine/glovebox.py <command> --json`, run from a copy of the Glovebox
/// source that ships inside the app (the engine writes into its own tree, so it can't run from the
/// signed bundle).
public struct Engine: Sendable {
    public var executable: URL
    /// Arguments before the command: `python3 <source>/engine/glovebox.py` for the real engine.
    public var leadingArguments: [String]
    /// The Glovebox source the engine runs from, if it's the real one.
    public var source: URL?
    /// Everything Glovebox keeps: one work folder per game (`GLOVEBOX_WORK`), the source copies.
    public var home: URL
    /// One log per run: every line the engine printed. Copy Diagnostics reads them.
    public var logs: URL
    /// Set for every run: `DEVELOPER_DIR`, test switches.
    public var environment: [String: String] = [:]

    public init(executable: URL, leadingArguments: [String] = [], source: URL? = nil, home: URL, logs: URL,
                environment: [String: String] = [:]) {
        self.executable = executable; self.leadingArguments = leadingArguments; self.source = source
        self.home = home; self.logs = logs; self.environment = environment
    }

    /// The real engine from the source in the app (or `GLOVEBOX_SRC`, a checkout), else a stand-in
    /// (`GLOVEBOX_ENGINE`, or the fake one a Debug build bundles). `developerDir` is the Xcode to
    /// use, so nothing depends on `xcode-select`.
    public static func locate(in bundle: Bundle = .main, environment: [String: String] = ProcessInfo.processInfo.environment,
                              developerDir: String? = nil) -> Engine? {
        let fm = FileManager.default
        let library = fm.urls(for: .libraryDirectory, in: .userDomainMask).first ?? fm.temporaryDirectory
        let home = environment["GLOVEBOX_HOME"].map { URL(fileURLWithPath: $0) }
            ?? library.appendingPathComponent("Application Support/Glovebox", isDirectory: true)
        let logs = library.appendingPathComponent("Logs/Glovebox", isDirectory: true)
        var env: [String: String] = [:]
        if let d = developerDir { env["DEVELOPER_DIR"] = d }
        // The engine finds the kit the app bundles (glovebox-kit.zip at the source root) by itself.
        if let kit = environment["GLOVEBOX_KIT"], fm.fileExists(atPath: kit) { env["GLOVEBOX_KIT"] = kit }

        if let stand = environment["GLOVEBOX_ENGINE"], fm.isExecutableFile(atPath: stand) {
            return Engine(executable: URL(fileURLWithPath: stand), home: home, logs: logs, environment: env)
        }
        let source = environment["GLOVEBOX_SRC"].map { URL(fileURLWithPath: $0) }
            ?? bundle.resourceURL.flatMap { r in
                let bundled = r.appendingPathComponent("glovebox", isDirectory: true)
                guard fm.fileExists(atPath: bundled.appendingPathComponent("engine/glovebox.py").path) else { return nil }
                let version = bundle.infoDictionary?["CFBundleShortVersionString"] as? String ?? "0"
                let build = bundle.infoDictionary?["CFBundleVersion"] as? String ?? "0"
                return try? Self.unpack(bundled, into: home.appendingPathComponent("source", isDirectory: true), version: version, build: build)
            }
        if let source, fm.fileExists(atPath: source.appendingPathComponent("engine/glovebox.py").path) {
            return Engine(executable: URL(fileURLWithPath: "/usr/bin/xcrun"),
                          leadingArguments: ["python3", source.appendingPathComponent("engine/glovebox.py").path],
                          source: source, home: home, logs: logs, environment: env)
        }
        if let fake = bundle.resourceURL?.appendingPathComponent("engine/glovebox"), fm.isExecutableFile(atPath: fake.path) {
            return Engine(executable: fake, home: home, logs: logs, environment: env)
        }
        return nil
    }

    /// Copies the bundled source (with the kit at its root) to `<folder>/<version>`, the engine's
    /// `source/<version>` (docs/INSTALLER.md, "Bundling"), once per app build, and drops older copies.
    static func unpack(_ bundled: URL, into folder: URL, version: String, build: String) throws -> URL {
        let fm = FileManager.default
        let dest = folder.appendingPathComponent(version, isDirectory: true)
        let stamp = dest.appendingPathComponent(".unpacked")
        if (try? String(contentsOf: stamp, encoding: .utf8)) != build {
            try fm.createDirectory(at: folder, withIntermediateDirectories: true)
            try? fm.removeItem(at: dest)
            try fm.copyItem(at: bundled, to: dest)
            fm.createFile(atPath: stamp.path, contents: Data(build.utf8))
        }
        for old in (try? fm.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil)) ?? [] where old.lastPathComponent != version {
            try? fm.removeItem(at: old)
        }
        return dest
    }

    /// The work folder for one game (`GLOVEBOX_WORK`).
    public func work(for recipe: String) -> URL { home.appendingPathComponent(recipe, isDirectory: true) }

    /// Starts `glovebox <arguments> --json` with `GLOVEBOX_WORK` set for `recipe`, plus `environment`.
    ///
    /// Call it from the main thread. A child started from a Dispatch worker thread can inherit that
    /// thread's blocked signals, and then the SIGINT that cancels never arrives.
    public func run(_ arguments: [String], recipe: String, environment extra: [String: String] = [:]) throws -> EngineRun {
        let fm = FileManager.default
        try fm.createDirectory(at: work(for: recipe), withIntermediateDirectories: true)
        try fm.createDirectory(at: logs, withIntermediateDirectories: true)
        let stamp = Self.stamp.string(from: Date())
        let name = (arguments.first ?? "run").filter { $0.isLetter || $0 == "-" }
        let log = logs.appendingPathComponent("\(name)-\(stamp).log")
        Self.pruneLogs(in: logs)

        let p = Process()
        p.executableURL = executable
        p.arguments = leadingArguments + arguments + ["--json"]
        var env = ProcessInfo.processInfo.environment
        env["GLOVEBOX_WORK"] = work(for: recipe).path
        env["PYTHONUNBUFFERED"] = "1"
        // When the app is gone (a crash included), the engine stops itself and what it started.
        env["GLOVEBOX_APP_PID"] = String(getpid())
        env.merge(environment) { $1 }
        env.merge(extra) { $1 }
        p.environment = env
        if let source { p.currentDirectoryURL = source }
        let run = EngineRun(process: p, log: log)
        try run.start()
        return run
    }

    private static let stamp: DateFormatter = {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "yyyyMMdd-HHmmss"
        return f
    }()

    /// The newest `checks` doctor logs, and the newest `others` of each other command's (build, install, collect):
    /// the checks every few seconds must not push out the last install's logs.
    static func pruneLogs(in folder: URL, checks: Int = 30, others: Int = 10) {
        let fm = FileManager.default
        guard let files = try? fm.contentsOfDirectory(at: folder, includingPropertiesForKeys: [.contentModificationDateKey]) else { return }
        let logs = files.filter { $0.pathExtension == "log" }.sorted {
            let a = (try? $0.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
            let b = (try? $1.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
            return a > b
        }
        let kinds = Dictionary(grouping: logs) { $0.lastPathComponent.split(separator: "-").first.map(String.init) ?? "" }
        for (kind, newestFirst) in kinds {
            for old in newestFirst.dropFirst(kind == "doctor" ? checks : others) { try? fm.removeItem(at: old) }
        }
    }
}

/// One running engine command. Read `events` to the end; `status` is the exit code after that.
public final class EngineRun: @unchecked Sendable {
    public let events: AsyncStream<EngineEvent>
    public let log: URL
    private let continuation: AsyncStream<EngineEvent>.Continuation
    private let process: Process
    private let lock = NSLock()
    private var logHandle: FileHandle?
    private var stderrTail: [String] = []
    private var exit: Int32?
    /// An `error` or `done` came: the engine said how it ended (doctor exits 1 when something is missing).
    private var sawEnding = false
    private var cancelled = false

    init(process: Process, log: URL) {
        self.process = process
        self.log = log
        (events, continuation) = AsyncStream.makeStream(of: EngineEvent.self, bufferingPolicy: .unbounded)
    }

    /// The exit code, once `events` has finished.
    public var status: Int32? { lock.withLock { exit } }

    /// The engine's last stderr lines, for an error the engine didn't describe itself.
    public var stderrLines: [String] { lock.withLock { stderrTail } }

    func start() throws {
        FileManager.default.createFile(atPath: log.path, contents: nil)
        logHandle = try? FileHandle(forWritingTo: log)
        writeLog("$ \(process.executableURL?.lastPathComponent ?? "glovebox") \((process.arguments ?? []).joined(separator: " "))")

        let out = Pipe(), err = Pipe()
        process.standardOutput = out
        process.standardError = err
        process.standardInput = FileHandle.nullDevice
        try process.run()
        Children.add(process)

        let group = DispatchGroup()
        group.enter()
        DispatchQueue.global(qos: .userInitiated).async { [self] in
            readLines(out.fileHandleForReading) { line in
                self.writeLog(line)
                for var event in EngineEvent.decode(line) {
                    switch event {
                    case .error(var err):
                        // This run's log, for Copy Diagnostics (checks run every few seconds write newer ones).
                        if err.log == nil { err.log = self.log.path; event = .error(err) }
                        self.lock.withLock { self.sawEnding = true }
                    case .finished: self.lock.withLock { self.sawEnding = true }
                    default: break
                    }
                    self.continuation.yield(event)
                }
            }
            group.leave()
        }
        group.enter()
        DispatchQueue.global(qos: .utility).async { [self] in
            readLines(err.fileHandleForReading) { line in
                self.writeLog("  " + line)
                self.lock.withLock {
                    self.stderrTail.append(line)
                    if self.stderrTail.count > 200 { self.stderrTail.removeFirst(self.stderrTail.count - 200) }
                }
            }
            group.leave()
        }
        group.notify(queue: .global()) { [self] in
            process.waitUntilExit()
            Children.remove(process)
            let code = process.terminationStatus
            writeLog("exit \(code)")
            lock.withLock {
                exit = code
                try? logHandle?.close()
                logHandle = nil
            }
            if lock.withLock({ cancelled }) {
                continuation.yield(.cancelled)
            } else if code != 0, !lock.withLock({ sawEnding }) {
                // An engine that died without saying why still reports an error, with what it printed.
                continuation.yield(.error(EngineError(code: "internal", message: stderrLines.suffix(5).joined(separator: "\n"),
                                                      log: log.path)))
            }
            continuation.finish()
        }
    }

    /// SIGINT to the engine and everything it started (xcodebuild, devicectl); SIGTERM if it hasn't
    /// stopped ten seconds later. The engine keeps finished steps, so the next run picks up from here.
    public func cancel() {
        guard process.isRunning else { return }
        lock.withLock { cancelled = true }
        signal(SIGINT)
        DispatchQueue.global().asyncAfter(deadline: .now() + 10) { [self] in
            if process.isRunning { signal(SIGTERM) }
        }
    }

    /// To the engine's process group when it has its own (Foundation starts children that way), so
    /// the tools it runs stop too; else to the engine alone.
    private func signal(_ sig: Int32) {
        let pid = process.processIdentifier
        let group = getpgid(pid)
        if group > 0, group != getpgid(0) {
            killpg(group, sig)
        } else {
            kill(pid, sig)
        }
    }

    private func writeLog(_ line: String) {
        lock.withLock { try? logHandle?.write(contentsOf: Data((line + "\n").utf8)) }
    }
}

/// Every tool Glovebox has running (the engine, devicectl, a fix's xcodebuild), so quitting stops
/// them all: nothing of Glovebox's keeps running once the app is gone (macOS 27 tells the player
/// when an app leaves processes behind, and lists it as running in the background).
public enum Children {
    private static let lock = NSLock()
    /// Each with its process group when it leads one of its own (Foundation starts children that
    /// way), so what it started stops too; 0: the process alone.
    private static var live: [ObjectIdentifier: (process: Process, group: pid_t)] = [:]

    /// Right after `process.run()`.
    public static func add(_ process: Process) {
        let g = getpgid(process.processIdentifier)
        lock.withLock { live[ObjectIdentifier(process)] = (process, g > 0 && g != getpgid(0) ? g : 0) }
    }

    public static func remove(_ process: Process) {
        lock.withLock { _ = live.removeValue(forKey: ObjectIdentifier(process)) }
    }

    /// SIGINT to each, then SIGKILL to whatever of theirs is still there after `seconds`. Returns once
    /// it's all gone (or a second after the SIGKILL): macOS 27 checks an app for background tasks when
    /// it quits with anything of its own still running.
    public static func stopAll(within seconds: TimeInterval) {
        let all = lock.withLock { Array(live.values) }
        for c in all { send(c, SIGINT) }
        wait(for: all, until: Date().addingTimeInterval(seconds))
        // The groups too: a tool the engine started can outlive the engine.
        for c in all { send(c, SIGKILL) }
        wait(for: all, until: Date().addingTimeInterval(1))
    }

    private static func wait(for all: [(process: Process, group: pid_t)], until end: Date) {
        while all.contains(where: alive), Date() < end { usleep(20_000) }
    }

    /// The process, or anything still in its group.
    private static func alive(_ c: (process: Process, group: pid_t)) -> Bool {
        c.process.isRunning || (c.group > 0 && killpg(c.group, 0) == 0)
    }

    private static func send(_ c: (process: Process, group: pid_t), _ sig: Int32) {
        if c.group > 0 { killpg(c.group, sig) } else if c.process.isRunning { kill(c.process.processIdentifier, sig) }
    }
}

/// Splits a pipe into lines until it closes.
private func readLines(_ handle: FileHandle, _ body: (String) -> Void) {
    var buffer = Data()
    while true {
        let chunk = handle.availableData
        if chunk.isEmpty { break }
        buffer.append(chunk)
        while let nl = buffer.firstIndex(of: 0x0A) {
            let line = String(decoding: buffer[buffer.startIndex..<nl], as: UTF8.self)
            buffer.removeSubrange(buffer.startIndex...nl)
            let trimmed = line.trimmingCharacters(in: .whitespacesAndNewlines)
            if !trimmed.isEmpty { body(trimmed) }
        }
    }
    let rest = String(decoding: buffer, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
    if !rest.isEmpty { body(rest) }
}
