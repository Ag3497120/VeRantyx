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
        for role in ["host", "encoder", "unrelated"] { try fm.createSymbolicLink(at: root.appendingPathComponent(role), withDestinationURL: fake) }
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
        print("Native session: synthetic tracking, exit 75, timeout, cancel and owned-process cleanup passed")
    }
}
