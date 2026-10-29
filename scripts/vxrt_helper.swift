// vxrt-helper: the small jobs the launch scripts need besides the shell, so that they run on a Mac with nothing
// installed but macOS (the VeRantyx app ships this built; scripts/launch_vxrt_steamvr.sh builds it here the first
// time, swiftc -O scripts/vxrt_helper.swift -o build/vxrt-helper).
//
//   vxrt-helper vrsettings <SteamVR's steamvr.vrsettings> <the overlay's copy> <no standby 1|0> <render scale> <extra JSON>
//   vxrt-helper json-get <file> <key.key...>              # a value of a JSON file, printed (nothing, exit 1, if absent)
//   vxrt-helper webui-standin [page mailbox] [game mailbox]
//   vxrt-helper webui-replay <prompt file>                # the game's messages to the page, one JSON a line on stdin:
//                                                         # what the prompt file says after each (a test and a tool)
//
// vrsettings: the overlay's copy of SteamVR's settings with the VeRantyx headset as the only driver (the original is
// never written). webui-standin: stands in for a game's SteamVR web UI, which SteamVR shows in vrwebhelper (Chromium,
// which does not run on VXRuntime): Half-Life: Alyx's loading-screen page talks to the game through SteamVR's
// "vrmailbox", a WebSocket to vrserver's web server (ws://localhost:27062; pages vrserver serves itself need no
// secret, so this one comes from that origin), where it opens its own mailbox and tells the game's mailbox it is
// "ready". Alyx gives up after a minute without that message ("Timed out waiting for response from Mongoose"). This
// does what the page does at start, once per game (a second "ready" during a level load can leave the game paused);
// the loading screens themselves are not shown. What the page would write over the loading screen (show_message:
// after a level has loaded, Alyx waits, paused, for a trigger pull and says "Press Trigger To Start") goes into the
// prompt file (VERANTYX_GAME_PROMPT_FILE, set by scripts/launch_vxrt_steamvr.sh), which the encoder draws into both
// eyes' picture (MacEncoder/HardwareEncoder.swift, GamePromptOverlay); end_loading (the game goes on), the next
// begin_loading or the game quitting takes it away.
import Darwin
import Foundation

setvbuf(stdout, nil, _IOLBF, 0)

func fail(_ text: String) -> Never {
    FileHandle.standardError.write((text + "\n").data(using: .utf8)!)
    exit(2)
}

// MARK: - vrsettings

func vrsettings(_ a: [String]) {
    guard a.count == 5 else { fail("usage: vxrt-helper vrsettings <in> <out> <no standby 1|0> <render scale> <extra JSON>") }
    let (source, out, noStandby, renderScale, extra) = (a[0], a[1], a[2], a[3], a[4])
    var settings: [String: Any] = [:]                    // (a new library has none yet)
    if let data = FileManager.default.contents(atPath: source) {
        guard let parsed = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] else { fail("\(source): not a JSON object") }
        settings = parsed
    }
    func update(_ section: String, _ values: [String: Any]) {
        var s = settings[section] as? [String: Any] ?? [:]
        for (k, v) in values { s[k] = v }
        settings[section] = s
    }
    // The VeRantyx headset only: no other driver is started (SteamVR's own, vrlink, calls Windows networking
    // functions VXRuntime lacks and stops vrserver), no SteamVR Home or dashboard, no safe mode. The Wine version's
    // setup wrote these into its Steam config; here they are set every start.
    update("steamvr", ["forcedDriver": "verantyx", "activateMultipleDrivers": false, "requireHmd": false,
                       "enableHomeApp": false, "enableSafeMode": false, "showMirrorView": false,
                       "startDashboardFromAppLaunch": false, "motionSmoothing": false,
                       // SteamVR covers the game with its empty environment while it judges the tracking bad; with
                       // the Vision Pro's head (or the bench pose) it does so for Elite Dangerous all the time.
                       "forceFadeOnBadTracking": false])
    update("dashboard", ["enableDashboard": false])
    update("verantyx", ["enable": true])
    update("driver_verantyx", ["blocked_by_safe_mode": false])
    update("driver_vrlink", ["enable": false])
    update("lighthouse", ["enable": false])
    update("oculus", ["enable": false])
    // The default picture size the measurements without a preset were made with (the Wine version's setting).
    var steamvr = settings["steamvr"] as? [String: Any] ?? [:]
    if steamvr["supersampleManualOverride"] == nil { steamvr["supersampleManualOverride"] = true }
    if steamvr["supersampleScale"] == nil { steamvr["supersampleScale"] = 0.55 }
    steamvr["allowDisplayLockedMode"] = true
    settings["steamvr"] = steamvr
    if !renderScale.isEmpty {   // the driver's render size as given (SteamVR's automatic resolution would scale it back down)
        update("steamvr", ["supersampleManualOverride": true, "supersampleScale": 1.0])
    }
    if noStandby == "1" {
        update("power", ["pauseCompositorOnStandby": false, "turnOffScreensTimeout": 3600.0])
    }
    if !extra.isEmpty {
        guard let sections = (try? JSONSerialization.jsonObject(with: Data(extra.utf8))) as? [String: Any] else { fail("extra settings: not a JSON object") }
        for (section, values) in sections {
            guard let values = values as? [String: Any] else { fail("extra settings: \(section) is not an object") }
            update(section, values)
        }
    }
    guard let data = try? JSONSerialization.data(withJSONObject: settings, options: [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]) else { fail("cannot write the settings") }
    do { try data.write(to: URL(fileURLWithPath: out), options: .atomic) } catch { fail("\(out): \(error.localizedDescription)") }
}

// MARK: - json-get

func jsonGet(_ a: [String]) {
    guard a.count == 2 else { fail("usage: vxrt-helper json-get <file> <key.key...>") }
    guard let data = FileManager.default.contents(atPath: a[0]),
          var value = try? JSONSerialization.jsonObject(with: data, options: [.fragmentsAllowed]) else { exit(1) }
    for key in a[1].split(separator: ".") {
        guard let object = value as? [String: Any], let next = object[String(key)] else { exit(1) }
        value = next
    }
    switch value {
    case let s as String: print(s)
    case let n as NSNumber: print(CFGetTypeID(n) == CFBooleanGetTypeID() ? (n.boolValue ? "true" : "false") : n.stringValue)
    case is NSNull: exit(1)
    default:
        guard let d = try? JSONSerialization.data(withJSONObject: value, options: [.sortedKeys]) else { exit(1) }
        print(String(decoding: d, as: UTF8.self))
    }
}

// MARK: - webui-standin

enum Closed: Error { case closed(String) }

func log(_ text: String) {
    let stamp = DateFormatter()
    stamp.dateFormat = "HH:mm:ss.SSS"
    print("[webui stand-in] \(stamp.string(from: Date())) \(text)")
}

/// What the game asks its loading page to show, from the game's messages to the page.
struct GamePrompt {
    private(set) var text: String?
    private(set) var phase = "gone"
    /// The words for a message the page shows: Alyx's start prompt in plain words (the pull has to last a moment:
    /// right after a level loads the game sees its input only a few times a second), in Japanese as well on a Mac
    /// set to Japanese; any other message as it is.
    static func words(_ message: String, japanese: Bool) -> String {
        guard message.trimmingCharacters(in: .whitespaces).lowercased() == "press trigger to start" else { return message }
        return japanese ? "Hold the trigger to start\nトリガーを長押しして開始" : "Hold the trigger to start"
    }
    /// Takes one message; true when the text changed.
    mutating func take(_ message: [String: Any], japanese: Bool) -> Bool {
        let before = (text, phase)
        switch message["type"] as? String {
        case "show_message":
            let t = (message["text"] as? String ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
            text = t.isEmpty ? nil : GamePrompt.words(t, japanese: japanese)
            // This is the game's explicit wait for input, not a pause inferred from missing frames.
            if t.lowercased() == "press trigger to start" { phase = "waiting" }
            else if phase == "waiting" { phase = "loading" }
        case "begin_loading": text = nil; phase = "loading"
        case "end_loading": text = nil; phase = "running"
        default: break
        }
        return text != before.0 || phase != before.1
    }
    mutating func clear() -> Bool {
        let changed = text != nil || phase != "gone"
        text = nil; phase = "gone"
        return changed
    }
}

let japaneseMac = Locale.preferredLanguages.first?.hasPrefix("ja") ?? false

/// The prompt file: the text, written whole (renamed into place), or no file.
func writePrompt(_ text: String?, to path: String?) {
    guard let path, !path.isEmpty else { return }
    if let text {
        try? Data(text.utf8).write(to: URL(fileURLWithPath: path), options: .atomic)
    } else {
        try? FileManager.default.removeItem(atPath: path)
    }
}

var gamePrompt = GamePrompt()
let promptFile = ProcessInfo.processInfo.environment["VERANTYX_GAME_PROMPT_FILE"]
let stateFile = ProcessInfo.processInfo.environment["VERANTYX_GAME_STATE_FILE"]

func writeGameState(to path: String?) {
    guard let path, !path.isEmpty else { return }
    let state: [String: Any] = ["version": 1, "phase": gamePrompt.phase, "message": gamePrompt.text as Any? ?? NSNull()]
    if let data = try? JSONSerialization.data(withJSONObject: state, options: [.sortedKeys]) {
        try? data.write(to: URL(fileURLWithPath: path), options: .atomic)
    }
}

func publishGamePrompt() {
    writePrompt(gamePrompt.text, to: promptFile)
    writeGameState(to: stateFile)
}

func noteForThePage(_ text: String) {
    guard let message = (try? JSONSerialization.jsonObject(with: Data(text.utf8))) as? [String: Any] else { return }
    if gamePrompt.take(message, japanese: japaneseMac) {
        publishGamePrompt()
        log(gamePrompt.text.map { "prompt shown: \($0.replacingOccurrences(of: "\n", with: " / "))" } ?? "prompt taken away")
    }
}

func webuiReplay(_ a: [String]) -> Never {
    guard a.count == 1 || a.count == 2 else { fail("usage: vxrt-helper webui-replay <prompt file> [state file]") }
    while let line = readLine() {
        if line == "gone" {
            if gamePrompt.clear() { writePrompt(nil, to: a[0]) }
        } else if let message = (try? JSONSerialization.jsonObject(with: Data(line.utf8))) as? [String: Any],
                  gamePrompt.take(message, japanese: false) {
            writePrompt(gamePrompt.text, to: a[0])
        }
        if a.count == 2 { writeGameState(to: a[1]) }
        print(FileManager.default.contents(atPath: a[0]).map { String(decoding: $0, as: UTF8.self) } ?? "(none)")
    }
    exit(0)
}

final class WebSocket {
    let fd: Int32
    init(host: String, port: UInt16) throws {
        fd = socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else { throw Closed.closed("socket: \(errno)") }
        var addr = sockaddr_in()
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = port.bigEndian
        inet_pton(AF_INET, host, &addr.sin_addr)
        var timeout = timeval(tv_sec: 5, tv_usec: 0)
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
        let r = withUnsafePointer(to: &addr) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }
        guard r == 0 else { close(fd); throw Closed.closed("connect: \(errno)") }
        var key = [UInt8](repeating: 0, count: 16)
        arc4random_buf(&key, key.count)
        try send(Array(("GET / HTTP/1.1\r\nHost: localhost:\(port)\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n" +
                        "Sec-WebSocket-Key: \(Data(key).base64EncodedString())\r\nSec-WebSocket-Version: 13\r\n" +
                        "Origin: http://localhost:\(port)\r\n\r\n").utf8))
        var reply = [UInt8]()
        while !reply.ends(with: [13, 10, 13, 10]) {
            reply += try recv(1)
        }
        let status = String(decoding: reply.prefix { $0 != 13 }, as: UTF8.self)
        guard status.contains(" 101 ") else { close(fd); throw Closed.closed(status) }
        timeout = timeval(tv_sec: 0, tv_usec: 0)                       // (no timeout from here: replies come when they come)
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
    }
    deinit { close(fd) }

    func send(_ bytes: [UInt8]) throws {
        var sent = 0
        while sent < bytes.count {
            let n = bytes[sent...].withUnsafeBufferPointer { Darwin.send(fd, $0.baseAddress, $0.count, 0) }
            guard n > 0 else { throw Closed.closed("send: \(errno)") }
            sent += n
        }
    }
    func recv(_ count: Int) throws -> [UInt8] {
        var out = [UInt8](repeating: 0, count: count)
        var got = 0
        while got < count {
            let n = out[got...].withUnsafeMutableBufferPointer { Darwin.recv(fd, $0.baseAddress, $0.count, 0) }
            guard n > 0 else { throw Closed.closed("closed") }
            got += n
        }
        return out
    }
    func sendText(_ text: String) throws {
        let data = Array(text.utf8)
        var head: [UInt8] = [0x81]                                       // FIN, text
        var mask = [UInt8](repeating: 0, count: 4)                       // clients mask their frames
        arc4random_buf(&mask, 4)
        if data.count < 126 {
            head.append(0x80 | UInt8(data.count))
        } else if data.count < 65536 {
            head += [UInt8(0x80 | 126), UInt8(data.count >> 8), UInt8(data.count & 0xFF)]
        } else {
            head.append(UInt8(0x80 | 127))
            head += (0..<8).reversed().map { UInt8((UInt64(data.count) >> (8 * UInt64($0))) & 0xFF) }
        }
        try send(head + mask + data.enumerated().map { $0.element ^ mask[$0.offset % 4] })
    }
    func recvText() throws -> String {
        while true {
            let h = try recv(2)
            var n = Int(h[1] & 0x7F)
            if n == 126 { let b = try recv(2); n = Int(b[0]) << 8 | Int(b[1]) }
            else if n == 127 { n = try recv(8).reduce(0) { $0 << 8 | Int($1) } }
            let mask = h[1] & 0x80 != 0 ? try recv(4) : nil
            var data = try recv(n)
            if let mask { data = data.enumerated().map { $0.element ^ mask[$0.offset % 4] } }
            switch h[0] & 0x0F {
            case 8: throw Closed.closed("closed by the server")
            case 9: try send([0x8A, 0x80 | UInt8(data.count)] + [0, 0, 0, 0] + data)   // ping: pong
            case 1: return String(decoding: data, as: UTF8.self)
            default: break
            }
        }
    }
}

extension Array where Element == UInt8 {
    func ends(with suffix: [UInt8]) -> Bool { count >= suffix.count && Array(self[(count - suffix.count)...]) == suffix }
}

func json(_ object: [String: Any]) -> String {
    String(decoding: try! JSONSerialization.data(withJSONObject: object, options: [.sortedKeys, .withoutEscapingSlashes]), as: UTF8.self)
}

/// Asks vrserver to tell when `mailbox` is registered (or gone) and waits for that answer; other messages (the game's
/// to the page: paths, loading screens) are logged and otherwise ignored, as the page shows nothing.
func waitFor(_ s: WebSocket, _ request: String, _ mailbox: String, page: String, id: Int) throws {
    try s.sendText("mailbox_send web_server_mailbox " + json(["type": request, "mailbox_name": mailbox, "returnAddress": page, "message_id": id]))
    while true {
        let text = try s.recvText()
        guard let reply = (try? JSONSerialization.jsonObject(with: Data(text.utf8))) as? [String: Any] else { continue }
        if (reply["message_id"] as? NSNumber)?.intValue == id { return }
        log("message for the page: \(text.prefix(300))")
        noteForThePage(text)
    }
}

func webuiStandin(_ a: [String]) -> Never {
    publishGamePrompt()
    let page = a.count > 0 ? a[0] : "hlvr/interstitials"
    let game = a.count > 1 ? a[1] : "hlvr/mailbox"
    while true {
        do {
            let s = try WebSocket(host: "127.0.0.1", port: 27062)
            log("connected to vrserver's web server, mailbox \(page)")
            try s.sendText("mailbox_open \(page)")
            var id = 1
            while true {
                try waitFor(s, "request_mailbox_registration_notification", game, page: page, id: id)
                try s.sendText("mailbox_send \(game) " + json(["type": "ready"]))
                log("\(game) is registered: told it \(page) is ready")
                // Once per game: the next "ready" waits until this game's mailbox is gone (the game quit).
                try waitFor(s, "request_mailbox_unavailable_notification", game, page: page, id: id + 1)
                log("\(game) is gone")
                if gamePrompt.clear() { publishGamePrompt(); log("prompt taken away (the game quit)") }
                id += 2
            }
        } catch {
            if gamePrompt.clear() { publishGamePrompt(); log("prompt taken away (vrserver's web server went away)") }
            sleep(2)
        }
    }
}

// MARK: -

signal(SIGPIPE, SIG_IGN)
let args = Array(CommandLine.arguments.dropFirst())
switch args.first {
case "vrsettings": vrsettings(Array(args.dropFirst()))
case "json-get": jsonGet(Array(args.dropFirst()))
case "webui-standin": webuiStandin(Array(args.dropFirst()))
case "webui-replay": webuiReplay(Array(args.dropFirst()))
default: fail("usage: vxrt-helper vrsettings|json-get|webui-standin|webui-replay ...")
}
