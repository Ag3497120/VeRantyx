// An already compiled part of the app. Owns exactly its encoder and host; never kills by name.
import Foundation
import Darwin

// Only a signal-sized store in the handler. Caught dispositions reset on exec,
// so children keep normal SIGTERM handling (SIG_IGN would be inherited).
private var nativeStopSignal: Int32 = 0

enum NativeSupervisor {
    // Service cleanup is separate from the game's cooperative quit protocol.
    static func stopService(_ process: Process?, grace: Double = 5) {
        guard let process, process.isRunning else { return }
        process.terminate()
        let deadline = Date().addingTimeInterval(grace)
        while process.isRunning, Date() < deadline { usleep(50_000) }
        if process.isRunning { kill(process.processIdentifier, SIGKILL) }
        process.waitUntilExit()
    }
    static func start(_ path: URL, _ arguments: [String], _ config: NativeSessionConfig, log: FileHandle) throws -> Process {
        let process = Process()
        process.executableURL = path
        process.arguments = arguments
        process.environment = config.environment
        process.currentDirectoryURL = config.game
        process.standardInput = FileHandle.nullDevice
        process.standardOutput = log
        process.standardError = log
        try process.run()
        return process
    }
    static func run(_ source: NativeSessionConfig) throws -> Int32 {
        var config = source
        guard config.waitSeconds.isFinite, (1...3600).contains(config.waitSeconds), config.parentPID > 1 else { throw NativeFailure("Invalid session configuration") }
        let quitWait = config.quitWaitSeconds ?? 60
        guard quitWait.isFinite, (0.1...3600).contains(quitWait) else { throw NativeFailure("Invalid quit wait time") }
        let fm = FileManager.default
        try fm.createDirectory(at: config.state, withIntermediateDirectories: true)
        let lock = open(config.state.appendingPathComponent("session.lock").path, O_CREAT | O_RDWR | O_NOFOLLOW, 0o600)
        guard lock >= 0 else { throw NativeFailure("Cannot open native session lock") }; defer { close(lock) }
        guard flock(lock, LOCK_EX | LOCK_NB) == 0 else { throw NativeFailure("Another native session is already running") }
        try NativeFiles.fresh(config.logs, sources: [config.game, config.lib])
        let frame = config.logs.appendingPathComponent("frame.dat")
        let fd = open(frame.path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0o600)
        guard fd >= 0 else { throw NativeFailure("Cannot create a fresh frame map") }
        let truncated = ftruncate(fd, off_t(NativeIPC.size)); close(fd)
        guard truncated == 0 else { throw NativeFailure("Cannot allocate native frame map") }
        // No HOME override: only our service files are redirected, not Steam's identity or licence checks.
        config.environment["VERANTYX_SHARED_FRAME"] = frame.path
        config.environment["VERANTYX_TANGENTS_FILE"] = config.logs.appendingPathComponent("vr_verantyx_tangents.txt").path
        let prompt = config.logs.appendingPathComponent("game-prompt.txt")
        config.environment["VERANTYX_GAME_PROMPT_FILE"] = prompt.path
        let loading = config.logs.appendingPathComponent("loading-map.json")
        config.environment["VERANTYX_NATIVE_LOADING_FILE"] = loading.path
        let stage = config.logs.appendingPathComponent("loading-stage.json")
        config.environment["VERANTYX_NATIVE_STAGE_FILE"] = stage.path
        let quit = config.logs.appendingPathComponent(NativeQuit.requestName)
        let force = config.logs.appendingPathComponent(NativeQuit.forceName)
        config.environment["VERANTYX_NATIVE_QUIT_FILE"] = quit.path
        config.environment["VERANTYX_NATIVE_GAME_DIR"] = config.game.appendingPathComponent("game/hlvr").path
        config.environment["VERANTYX_OPENVR_ROOT"] = config.state.appendingPathComponent("openvr").path
        config.environment["KL_VK_PIPELINE_ARCHIVE"] = config.state.appendingPathComponent("native-pipelines.bin").path
        config.environment["VERANTYX_STEAM_STATE"] = config.state.appendingPathComponent("steam").path
        let vr = config.state.appendingPathComponent("openvr")
        try fm.createDirectory(at: vr.appendingPathComponent("bin/linuxarm64"), withIntermediateDirectories: true)
        fm.createFile(atPath: vr.appendingPathComponent("bin/linuxarm64/vrclient.so").path, contents: Data())
        let registry: [String: Any] = ["config": [vr.appendingPathComponent("config").path], "log": [config.logs.path], "runtime": [vr.path], "jsonid": "vrpathreg", "version": 1]
        let registryFile = vr.appendingPathComponent("openvrpaths.vrpath")
        try JSONSerialization.data(withJSONObject: registry).write(to: registryFile, options: .atomic)
        config.environment["VR_PATHREG_OVERRIDE"] = registryFile.path
        config.environment["VERANTYX_ALVR"] = "1"
        config.environment.removeValue(forKey: "VERANTYX_NO_STEAM")
        config.environment.removeValue(forKey: "VERANTYX_BENCH")
        for number in [SIGINT, SIGTERM] { signal(number) { nativeStopSignal = $0 } }
        var record: [String: Any] = ["backend": "nativeARM64", "started_at": ISO8601DateFormatter().string(from: Date()),
                                   "parent_pid": config.parentPID, "quality": config.environment["VERANTYX_NATIVE_QUALITY"] ?? "standard",
                                   "quit_wait_seconds": quitWait, "allow_force_kill": config.allowForceKill == true,
                                   "unknown": ["headset picture, audio and physical controls require a user playtest", "FPS comparison requires matching scenes and settings"]]
        var failure: String?
        func event(_ state: String, _ message: String = "") {
            if state == "error" { failure = message }
            record["state"] = state; record["message"] = message
            if let data = try? JSONSerialization.data(withJSONObject: record, options: [.prettyPrinted, .sortedKeys]) {
                try? data.write(to: config.logs.appendingPathComponent("session.json"), options: .atomic)
            }
            print("native: \(state) \(message)"); fflush(stdout)
        }
        func log(_ name: String) throws -> FileHandle {
            let url = config.logs.appendingPathComponent(name)
            fm.createFile(atPath: url.path, contents: nil)
            return try FileHandle(forWritingTo: url)
        }
        let encoderLog = try log("hardware-encoder.log"), hostLog = try log("alyx_host.log")
        defer { try? encoderLog.close(); try? hostLog.close(); try? fm.removeItem(at: frame) }
        var encoder: Process?, host: Process?, restarts = 0
        func archiveALVR(_ name: String) {
            if let data = try? Data(contentsOf: config.alvrLog) { try? data.write(to: config.logs.appendingPathComponent(name), options: .atomic) }
            // The shared ALVR settings can change on a later reconnect. Keep
            // the audio route that was actually selected for each encoder.
            if let data = try? Data(contentsOf: config.alvrLog.deletingLastPathComponent().appendingPathComponent("session.json")),
               let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
               let settings = object["session_settings"] as? [String: Any], let audio = settings["audio"] as? [String: Any],
               let snapshot = try? JSONSerialization.data(withJSONObject: audio, options: [.prettyPrinted, .sortedKeys]) {
                try? snapshot.write(to: config.logs.appendingPathComponent(name.replacingOccurrences(of: ".log", with: "-audio.json")), options: .atomic)
            }
        }
        func cancelled() -> Bool {
            nativeStopSignal != 0 || getppid() != config.parentPID ||
                fm.fileExists(atPath: quit.path) || fm.fileExists(atPath: force.path)
        }
        func startEncoder() throws -> Process {
            var encoderConfig = config
            // The loading scene draws mailbox text at its authored world position.
            encoderConfig.environment.removeValue(forKey: "VERANTYX_GAME_PROMPT_FILE")
            encoderConfig.environment.removeValue(forKey: "VERANTYX_NATIVE_QUIT_FILE")
            return try start(config.encoder, [], encoderConfig, log: encoderLog)
        }
        func pollEncoder() throws {
            guard let current = encoder, !current.isRunning else { return }
            if current.terminationStatus == 75 {
                archiveALVR("alvr-restart-\(restarts).log")
                restarts += 1
                guard restarts <= 30 else { throw NativeFailure("ALVR keeps restarting. Check the headset connection and start again.") }
                event(host == nil ? "waiting" : "reconnecting", "ALVR settings changed; reconnecting")
                encoder = try startEncoder()
            } else { throw NativeFailure("Encoder exited (\(current.terminationStatus)); see hardware-encoder.log") }
        }
        defer {
            record["host_shutdown"] = host == nil ? "not_started" : "natural"
            record["stop_reason"] = failure != nil ? "error" : nativeStopSignal != 0 ? "signal" :
                getppid() != config.parentPID ? "parent_lost" : "game_exit"
            if let host {
                if host.isRunning {
                    let began = ProcessInfo.processInfo.systemUptime
                    record["quit_requested_at"] = ISO8601DateFormatter().string(from: Date())
                    record["host_shutdown"] = "requested"
                    record["quit_timed_out"] = false
                    do { try NativeQuit.request(in: config.logs) }
                    catch { record["quit_request_error"] = error.localizedDescription }
                    event("stopping", "Waiting for the game to close normally")
                    var timedOut = false, forced = false
                    while host.isRunning {
                        let elapsed = ProcessInfo.processInfo.systemUptime - began
                        var changed = false
                        for (key, suffix) in [("quit_delivered", ".delivered"), ("quit_acknowledged", ".ack")] {
                            let value = fm.fileExists(atPath: quit.path + suffix)
                            if record[key] as? Bool != value { record[key] = value; changed = true }
                        }
                        if !timedOut, elapsed >= quitWait {
                            timedOut = true; changed = true; record["quit_timed_out"] = true
                        }
                        if !forced, fm.fileExists(atPath: force.path) || (timedOut && config.allowForceKill == true) {
                            // Only the process we started, and only with explicit authorization.
                            if host.isRunning, kill(host.processIdentifier, SIGKILL) == 0 {
                                forced = true; changed = true; record["host_shutdown"] = "forced"
                                record["force_source"] = fm.fileExists(atPath: force.path) ? "user_request" : "configuration"
                            }
                        }
                        if changed {
                            event(timedOut ? "quit_timeout" : "stopping", forced ? "Force stop requested" : timedOut ?
                                "The game is still closing. Quit in the headset, keep waiting, or choose Force stop." :
                                "Waiting for the game to close normally")
                        }
                        // Keep tracking and the stream available while the game exits.
                        if let current = encoder, !current.isRunning, current.terminationStatus == 75 {
                            archiveALVR("alvr-restart-\(restarts).log")
                            restarts += 1
                            encoder = restarts <= 30 ? try? startEncoder() : nil
                        }
                        usleep(50_000)
                    }
                    host.waitUntilExit()
                    record["quit_wait_seconds_actual"] = ProcessInfo.processInfo.systemUptime - began
                    record["quit_delivered"] = fm.fileExists(atPath: quit.path + ".delivered")
                    record["quit_acknowledged"] = fm.fileExists(atPath: quit.path + ".ack")
                }
                record["host_exit_code"] = host.terminationStatus
                record["host_exit_reason"] = host.terminationReason == .uncaughtSignal ? "signal" : "exit"
            }
            stopService(encoder)
            try? fm.removeItem(at: prompt)
            try? fm.removeItem(at: loading)
            try? fm.removeItem(at: stage)
            try? fm.removeItem(atPath: stage.path + ".ready")
            record["encoder_restarts"] = restarts
            record["ended_at"] = ISO8601DateFormatter().string(from: Date())
            archiveALVR("alvr-session.log")
            event(failure == nil ? "stopped" : "error", failure ?? "")
        }
        do {
            event("waiting", "Open ALVR on the Pico / Quest and connect")
            encoder = try startEncoder()
            let deadline = Date().addingTimeInterval(config.waitSeconds)
            var previous: NativeViewSnapshot?
            var live: NativeViewSnapshot?
            while !cancelled() {
                try pollEncoder()
                let current = NativeIPC.snapshot(frame)
                if let current, let previous, current.timestamp > previous.timestamp, current.sequence != previous.sequence { live = current; break }
                previous = current
                guard Date() < deadline else { throw NativeFailure("No live ALVR tracking and eye geometry received. Alyx was not started. Open ALVR on the headset, connect and try again.") }
                usleep(100_000)
            }
            if cancelled() { return 0 }
            if let live { record["headset"] = try JSONSerialization.jsonObject(with: JSONEncoder().encode(live)) }
            archiveALVR("alvr-start.log")
            if let data = try? Data(contentsOf: config.alvrLog.deletingLastPathComponent().appendingPathComponent("session.json")),
               let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
               let settings = object["openvr_config"] as? [String: Any], let hz = settings["refresh_rate"] as? Double,
               hz.isFinite, (30...144).contains(hz) { config.environment["VERANTYX_DISPLAY_HZ"] = String(hz) }
            // The app sets the eye pixel budget. As on the Windows Alyx path,
            // keep Source 2 from independently changing viewport scale when a
            // graphics preset is applied while the streaming session is live.
            let args = ["--libdir", config.lib.path, "--game", config.game.path, "--", "-vr", "-steam", "-noasserts", "-nopassiveasserts", "-vulkan", "-nowindow", "-noautofidelity", "+vr_fidelity_level", "3", "+map", "startup"]
            record["command"] = [config.host.path] + args
            record["settings"] = config.environment.filter { $0.key.hasPrefix("VERANTYX_") || $0.key.hasPrefix("KL_") }
            host = try start(config.host, args, config, log: hostLog)
            event("running")
            while !cancelled(), host?.isRunning == true { try pollEncoder(); usleep(100_000) }
            if cancelled() { return 0 }
            let code = host?.terminationStatus ?? 1
            if code != 0 { event("error", "Alyx exited (\(code)); see alyx_host.log") }
            return code
        } catch {
            event("error", error.localizedDescription)
            return 1
        }
    }
}

if CommandLine.arguments.count == 3, CommandLine.arguments[1] == "--session" {
    do {
        let config = try JSONDecoder().decode(NativeSessionConfig.self, from: Data(contentsOf: URL(fileURLWithPath: CommandLine.arguments[2])))
        exit(try NativeSupervisor.run(config))
    } catch { fputs("Native session: \(error.localizedDescription)\n", stderr); exit(1) }
} else {
    fputs("This helper is started by VeRantyx. Use the app to install and play native Alyx.\n", stderr)
    exit(2)
}
