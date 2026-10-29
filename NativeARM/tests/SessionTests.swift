import Foundation
import Darwin

@main enum SessionTests {
    static func main() throws {
        let fm = FileManager.default, build = URL(fileURLWithPath: CommandLine.arguments[1])
        let root = build.appendingPathComponent("session-test-" + UUID().uuidString)
        try fm.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: root) }
        let helper = build.deletingLastPathComponent().appendingPathComponent("native-session")
        let fake = build.appendingPathComponent("fake_peer")
        for role in ["host", "encoder", "unrelated", "parent"] { try fm.createSymbolicLink(at: root.appendingPathComponent(role), withDestinationURL: fake) }
        func process(_ path: URL, _ args: [String], env: [String: String]) throws -> Process {
            let p = Process(); p.executableURL = path; p.arguments = args; p.environment = env
            p.standardOutput = FileHandle.nullDevice; p.standardError = FileHandle.nullDevice; p.standardInput = FileHandle.nullDevice
            try p.run(); return p
        }
        var environment = ["NATIVE_TEST_PEER": "1", "NATIVE_TEST_ROOT": root.path, "NATIVE_TEST_RESTART": "1",
                           "VERANTYX_GAME_PROMPT_FILE": root.appendingPathComponent("inherited-prompt.txt").path]
        let unrelated = try process(root.appendingPathComponent("unrelated"), [], env: environment)
        defer { if unrelated.isRunning { unrelated.terminate(); unrelated.waitUntilExit() } }
        for name in ["game", "lib"] { try fm.createDirectory(at: root.appendingPathComponent(name), withIntermediateDirectories: true) }
        for mode in ["normal", "timeout", "cancel"] {
            let session = root.appendingPathComponent(mode)
            try fm.createDirectory(at: session, withIntermediateDirectories: true)
            environment["NATIVE_TEST_NO_TRACKING"] = mode == "normal" ? "0" : "1"
            let alvrLog = session.appendingPathComponent("fake-alvr/session_log.txt")
            environment["NATIVE_TEST_ALVR_LOG"] = alvrLog.path
            let config = NativeSessionConfig(host: root.appendingPathComponent("host"), encoder: root.appendingPathComponent("encoder"),
                game: root.appendingPathComponent("game"), lib: root.appendingPathComponent("lib"), state: session.appendingPathComponent("state"),
                logs: session.appendingPathComponent("logs"), alvrLog: alvrLog, environment: environment, parentPID: getpid(), waitSeconds: 1)
            let file = session.appendingPathComponent("config.json")
            try JSONEncoder().encode(config).write(to: file)
            try? fm.removeItem(at: root.appendingPathComponent("host-started"))
            let p = try process(helper, ["--session", file.path], env: ProcessInfo.processInfo.environment)
            if mode == "cancel" { usleep(200_000); p.terminate() }
            let deadline = Date().addingTimeInterval(8)
            while p.isRunning, Date() < deadline { usleep(20_000) }
            if p.isRunning { p.terminate(); throw NativeFailure("helper did not stop") }
            p.waitUntilExit()
            let started = fm.fileExists(atPath: root.appendingPathComponent("host-started").path)
            guard started == (mode == "normal"), unrelated.isRunning else { throw NativeFailure("Wrong host launch or unrelated process terminated") }
            let expected: Int32 = mode == "timeout" ? 1 : 0
            guard p.terminationStatus == expected else { throw NativeFailure("Wrong helper exit status in \(mode): \(p.terminationStatus)") }
            guard !fm.fileExists(atPath: config.logs.appendingPathComponent("frame.dat").path) else { throw NativeFailure("frame map not cleaned") }
            let record = try JSONSerialization.jsonObject(with: Data(contentsOf: config.logs.appendingPathComponent("session.json"))) as! [String: Any]
            if mode == "normal" {
                let command = record["command"] as? [String] ?? []
                guard command.contains("-noautofidelity"), let fidelity = command.firstIndex(of: "+vr_fidelity_level"),
                      fidelity + 1 < command.count, command[fidelity + 1] == "3" else { throw NativeFailure("Native eye budget can be overridden by automatic fidelity") }
                guard try String(contentsOf: root.appendingPathComponent("host-archive-path")) ==
                    config.state.appendingPathComponent("native-pipelines.bin").path else {
                    throw NativeFailure("Pipeline archive must survive log cleanup and stay outside game files")
                }
                let prompt = config.logs.appendingPathComponent("game-prompt.txt").path
                guard try String(contentsOf: root.appendingPathComponent("host-prompt-path")) == prompt else { throw NativeFailure("Host loading diagnostics are not private to the session") }
                guard !fm.fileExists(atPath: root.appendingPathComponent("encoder-prompt-path").path) else { throw NativeFailure("Native encoder still draws loading prompts") }
                let loading = config.logs.appendingPathComponent("loading-map.json").path
                for role in ["host", "encoder"] {
                    guard try String(contentsOf: root.appendingPathComponent(role + "-loading-path")) == loading else { throw NativeFailure("Loading map is not shared with both native processes") }
                }
                let stage = config.logs.appendingPathComponent("loading-stage.json").path
                for role in ["host", "encoder"] {
                    guard try String(contentsOf: root.appendingPathComponent(role + "-stage-path")) == stage else { throw NativeFailure("Stage is not shared with both native processes") }
                }
                guard !fm.fileExists(atPath: stage), !fm.fileExists(atPath: stage + ".ready") else { throw NativeFailure("Stage survived helper exit") }
                guard !fm.fileExists(atPath: loading) else { throw NativeFailure("Loading map survived helper exit") }
                guard !fm.fileExists(atPath: prompt) else { throw NativeFailure("Loading prompt survived helper exit") }
                for (name, enabled) in [("alvr-start-audio.json", true), ("alvr-session-audio.json", false)] {
                    let data = try Data(contentsOf: config.logs.appendingPathComponent(name))
                    let audio = try JSONSerialization.jsonObject(with: data) as? [String: Any]
                    guard (audio?["game_audio"] as? [String: Any])?["enabled"] as? Bool == enabled else {
                        throw NativeFailure("ALVR audio route snapshot changed after it was archived")
                    }
                }
            }
            if mode == "normal", record["encoder_restarts"] as? Int != 1 { throw NativeFailure("ALVR exit 75 was not restarted") }
            if let pidText = try? String(contentsOf: root.appendingPathComponent("encoder.pid")), let pid = Int32(pidText), kill(pid, 0) == 0 { throw NativeFailure("owned encoder survived helper exit") }
        }
        func checkQuit(_ mode: String) throws {
            let session = root.appendingPathComponent("quit-" + mode)
            try fm.createDirectory(at: session, withIntermediateDirectories: true)
            let slow = ["slow", "interrupt", "parent", "encoder-error"].contains(mode)
            let env = ["NATIVE_TEST_PEER": "1", "NATIVE_TEST_ROOT": session.path,
                       "NATIVE_TEST_HOST_MODE": slow ? "slow" : "ignore",
                       "NATIVE_TEST_ENCODER_FAIL": mode == "encoder-error" ? "1" : "0"]
            let config = NativeSessionConfig(host: root.appendingPathComponent("host"), encoder: root.appendingPathComponent("encoder"),
                game: root.appendingPathComponent("game"), lib: root.appendingPathComponent("lib"), state: session.appendingPathComponent("state"),
                logs: session.appendingPathComponent("logs"), alvrLog: session.appendingPathComponent("alvr.log"), environment: env,
                parentPID: getpid(), waitSeconds: 2, quitWaitSeconds: 0.1, allowForceKill: mode == "configured-force")
            let file = session.appendingPathComponent("config.json")
            try JSONEncoder().encode(config).write(to: file)
            // Older configuration files must continue to default to a 60 s wait
            // without permitting a forced game shutdown.
            var legacy = try JSONSerialization.jsonObject(with: Data(contentsOf: file)) as! [String: Any]
            legacy.removeValue(forKey: "quitWaitSeconds"); legacy.removeValue(forKey: "allowForceKill")
            let decoded = try JSONDecoder().decode(NativeSessionConfig.self, from: JSONSerialization.data(withJSONObject: legacy))
            guard decoded.quitWaitSeconds == nil, decoded.allowForceKill != true else { throw NativeFailure("Unsafe legacy quit policy") }
            let p = try process(mode == "parent" ? root.appendingPathComponent("parent") : helper,
                                mode == "parent" ? [helper.path, file.path] : ["--session", file.path], env: env)
            defer {
                try? Data().write(to: session.appendingPathComponent("release-host"))
                try? Data().write(to: session.appendingPathComponent("release-parent"))
                try? NativeQuit.request(in: config.logs, force: true)
                if p.isRunning { p.terminate() }
                p.waitUntilExit()
            }
            func wait(_ condition: () -> Bool) throws {
                let deadline = ProcessInfo.processInfo.systemUptime + 5
                while !condition(), ProcessInfo.processInfo.systemUptime < deadline { usleep(10_000) }
                guard condition() else { throw NativeFailure("Quit test timed out: \(mode)") }
            }
            func record() -> [String: Any] {
                guard let data = try? Data(contentsOf: config.logs.appendingPathComponent("session.json")),
                      let value = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return [:] }
                return value
            }
            try wait { fm.fileExists(atPath: session.appendingPathComponent("host-started").path) }
            if mode == "parent" {
                try Data().write(to: session.appendingPathComponent("release-parent")); p.waitUntilExit()
            } else if mode == "interrupt" {
                guard getpgid(p.processIdentifier) == p.processIdentifier else { throw NativeFailure("Helper has no private process group") }
                kill(-p.processIdentifier, SIGINT)
            } else if mode != "encoder-error" { p.terminate() }
            if !["configured-force", "encoder-error"].contains(mode) {
                try wait { record()["state"] as? String == "quit_timeout" }
                guard fm.fileExists(atPath: config.logs.appendingPathComponent("frame.dat").path), unrelated.isRunning else {
                    throw NativeFailure("Quit timeout released session resources")
                }
                let lock = open(config.state.appendingPathComponent("session.lock").path, O_RDWR)
                guard lock >= 0 else { throw NativeFailure("Session lock disappeared during quit") }
                let acquired = flock(lock, LOCK_EX | LOCK_NB) == 0; close(lock)
                guard !acquired else { throw NativeFailure("Session lock released before the game exited") }
                if mode == "force" { try NativeQuit.request(in: config.logs, force: true) }
                if mode == "ignore" {
                    p.terminate(); p.terminate(); usleep(150_000)
                    guard p.isRunning, !fm.fileExists(atPath: session.appendingPathComponent("host-signal").path) else {
                        throw NativeFailure("Repeated stop escalated to a game signal")
                    }
                    try Data().write(to: session.appendingPathComponent("release-host"))
                }
            }
            try wait { ["stopped", "error"].contains(record()["state"] as? String ?? "") }
            p.waitUntilExit()
            let result = record(), forced = ["force", "configured-force"].contains(mode)
            guard result["host_shutdown"] as? String == (forced ? "forced" : "requested"),
                  result["host_exit_reason"] as? String == (forced ? "signal" : "exit"),
                  result["host_exit_code"] as? Int == (forced ? Int(SIGKILL) : 0),
                  result["quit_timed_out"] as? Bool == true, unrelated.isRunning,
                  !fm.fileExists(atPath: config.logs.appendingPathComponent("frame.dat").path),
                  !fm.fileExists(atPath: session.appendingPathComponent("host-signal").path),
                  forced || fm.fileExists(atPath: session.appendingPathComponent("host-clean-exit").path) else {
                throw NativeFailure("Incorrect shutdown or lost game cleanup: \(mode), \(result)")
            }
            if slow, result["quit_acknowledged"] as? Bool != true { throw NativeFailure("Quit acknowledgement not recorded") }
            if mode == "encoder-error", result["state"] as? String != "error" { throw NativeFailure("Encoder failure hidden by shutdown state") }
        }
        for mode in ["slow", "interrupt", "ignore", "force", "configured-force", "parent", "encoder-error"] { try checkQuit(mode) }
        print("Native session: tracking, restart, timeout, cancel, slow quit, no escalation, explicit force, parent loss and encoder failure passed")
    }
}
