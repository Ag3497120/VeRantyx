// Offline test peer. It produces only synthetic tracking; no ALVR, graphics, Steam or game.
import Foundation
import Darwin

@main enum FakePeer {
    static func main() throws {
        let env = ProcessInfo.processInfo.environment
        guard env["NATIVE_TEST_PEER"] == "1", let root = env["NATIVE_TEST_ROOT"] else { exit(2) }
        let role = URL(fileURLWithPath: CommandLine.arguments[0]).lastPathComponent
        func audioSettings(_ enabled: Bool) throws {
            guard let log = env["NATIVE_TEST_ALVR_LOG"] else { return }
            let directory = URL(fileURLWithPath: log).deletingLastPathComponent()
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            let settings: [String: Any] = ["session_settings": ["audio": ["game_audio": ["enabled": enabled]]]]
            try JSONSerialization.data(withJSONObject: settings).write(to: directory.appendingPathComponent("session.json"), options: .atomic)
        }
        try "\(getpid())".write(toFile: root + "/" + role + ".pid", atomically: true, encoding: .utf8)
        if let prompt = env["VERANTYX_GAME_PROMPT_FILE"] {
            try prompt.write(toFile: root + "/" + role + "-prompt-path", atomically: true, encoding: .utf8)
            if role == "host" { try "Synthetic loading prompt".write(toFile: prompt, atomically: true, encoding: .utf8) }
        }
        if let loading = env["VERANTYX_NATIVE_LOADING_FILE"] {
            try loading.write(toFile: root + "/" + role + "-loading-path", atomically: true, encoding: .utf8)
            if role == "host" { try Data("{\"type\":\"begin_loading\",\"mapName\":\"synthetic\"}".utf8).write(to: URL(fileURLWithPath: loading)) }
        }
        if let stage = env["VERANTYX_NATIVE_STAGE_FILE"] {
            try stage.write(toFile: root + "/" + role + "-stage-path", atomically: true, encoding: .utf8)
            if role == "host" {
                try Data("synthetic".utf8).write(to: URL(fileURLWithPath: stage))
                try Data("1".utf8).write(to: URL(fileURLWithPath: stage + ".ready"))
            }
        }
        if role == "host", let archive = env["KL_VK_PIPELINE_ARCHIVE"] {
            try archive.write(toFile: root + "/host-archive-path", atomically: true, encoding: .utf8)
        }
        if role == "host" {
            try "started".write(toFile: root + "/host-started", atomically: true, encoding: .utf8)
            usleep(300_000)
            try audioSettings(false)
            return
        }
        if role == "unrelated" { sleep(30); return }
        if env["NATIVE_TEST_RESTART"] == "1", !FileManager.default.fileExists(atPath: root + "/restarted") {
            try Data().write(to: URL(fileURLWithPath: root + "/restarted")); exit(75)
        }
        try audioSettings(true)
        if let log = env["NATIVE_TEST_ALVR_LOG"] { try "Synthetic ALVR log".write(toFile: log, atomically: true, encoding: .utf8) }
        guard let file = env["VERANTYX_SHARED_FRAME"] else { exit(3) }
        let fd = open(file, O_RDWR); guard fd >= 0 else { exit(4) }; defer { close(fd) }
        func put<T>(_ value: T, _ at: Int) { var v = value; _ = withUnsafeBytes(of: &v) { pwrite(fd, $0.baseAddress, $0.count, off_t(at)) } }
        var sequence: UInt32 = 0
        while true {
            if env["NATIVE_TEST_NO_TRACKING"] != "1" {
                sequence += 2; put(sequence - 1, 20)
                for i in 0..<16 { put(Float(i % 5 == 0 ? 1 : 0), NativeIPC.hands + i * 4) }
                put(Double(sequence), NativeIPC.hands + 244)
                for i in 0..<8 { put(Float(i % 2 == 0 ? -1 : 1), NativeIPC.hands + 252 + 4 * i) }
                put(UInt8(1), NativeIPC.hands + 286); put(UInt32(0x44504956), NativeIPC.hands + 320); put(Float(0.064), NativeIPC.hands + 324)
                put(sequence, 20)
            }
            usleep(50_000)
        }
    }
}
