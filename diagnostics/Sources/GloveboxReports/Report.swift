import Foundation
import CryptoKit

/// What one upload carries (version 1; the endpoint's validate() in reports/lib.js is the other half).
public struct Report: Codable {
    public struct App: Codable {
        public var glovebox: String; public var kit: String
        public init(glovebox: String, kit: String) { self.glovebox = glovebox; self.kit = kit }
    }
    public struct Device: Codable {
        public var model: String; public var os: String
        public init(model: String, os: String) { self.model = model; self.os = os }
    }
    /// A slow compile: pipeline hash (16 hex), how long it took, seconds into the session.
    public struct Hitch: Codable { public var p: String; public var ms: Double; public var t: Double }
    /// One "Preparing graphics" before Play (Documents/klfz/prewarm.log): when it began (unix time), how long it took,
    /// pipelines compiled, how many took over 100 ms, and its result (0, or -1/-2: it could not run).
    public struct Prep: Codable { public var at: Double; public var s: Double; public var n: Int; public var slow: Int; public var rc: Int }
    /// How smoothly one minute of play ran (Documents/klfz/perf.txt, Klepton's heartbeat), and nothing else: seconds
    /// since the game's first frame, game frames per second, display rate, thermal state (0 nominal .. 3 critical, -1
    /// not known), MB left before the memory limit, footprint MB.
    public struct Perf: Codable {
        public var s: Double; public var fps: Double; public var hz: Double; public var heat: Int
        public var avail: Int; public var foot: Int
    }
    /// Any entry but a shader module: its 40-character name and its JSON (Vulkan state, no code).
    public struct Entry: Codable { public var n: String; public var j: String }
    /// A shader module: its JSON header and a fingerprint of its SPIR-V instead of the code.
    public struct Module: Codable { public var n: String; public var j: String; public var sha256: String; public var size: Int }
    public var v = 1
    public var install: String
    public var seq: Int
    public var app: App
    public var device: Device
    public var hitches: [Hitch] = []
    public var entries: [Entry] = []
    public var modules: [Module] = []
    public var prep: [Prep] = []
    public var perf: [Perf] = []
    public var isEmpty: Bool { hitches.isEmpty && entries.isEmpty && modules.isEmpty && prep.isEmpty && perf.isEmpty }
}

/// Remembered between uploads: an anonymous install id, the next batch number, which recorded entries were sent,
/// and how far into the hitch log.
public struct ReportState: Codable {
    public var install = UUID().uuidString.lowercased()
    public var seq = 0
    public var sent: Set<String> = []
    public var slowOffset = 0
    public var prepOffset = 0
    public var perfOffset = 0

    enum CodingKeys: String, CodingKey { case install, seq, sent, slowOffset, prepOffset, perfOffset }
    public init() {}
    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        install = try c.decode(String.self, forKey: .install)
        seq = try c.decode(Int.self, forKey: .seq)
        sent = try c.decode(Set<String>.self, forKey: .sent)
        slowOffset = try c.decode(Int.self, forKey: .slowOffset)
        prepOffset = try c.decodeIfPresent(Int.self, forKey: .prepOffset) ?? 0  // older state files have none
        perfOffset = try c.decodeIfPresent(Int.self, forKey: .perfOffset) ?? 0
    }

    public static func load(_ url: URL) -> ReportState {
        (try? JSONDecoder().decode(ReportState.self, from: Data(contentsOf: url))) ?? ReportState()
    }
    public func save(_ url: URL) throws {
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try JSONEncoder().encode(self).write(to: url, options: .atomic)
    }
}

public enum ReportBuilder {
    /// The next report: every recorded entry not sent yet (up to `maxEntries`) and the hitch-log lines after the
    /// saved offset. Returns the report and the state to save once the upload succeeds.
    public static func next(recording: URL, slowLog: URL, prewarmLog: URL? = nil, perfLog: URL? = nil, state: ReportState,
                            app: Report.App, device: Report.Device, maxEntries: Int = 4000) throws -> (Report, ReportState) {
        var st = state
        var r = Report(install: st.install, seq: st.seq, app: app, device: device)
        if let data = try? Data(contentsOf: recording, options: .mappedIfSafe) {
            for e in try readFoz(data) where !st.sent.contains(e.name) {
                if r.entries.count + r.modules.count >= maxEntries { break }
                if e.tag == "04" {
                    let (json, code) = try splitModule(e.payload)
                    let sha = SHA256.hash(data: code).map { String(format: "%02x", $0) }.joined()
                    r.modules.append(.init(n: e.name, j: json, sha256: sha, size: code.count))
                } else {
                    let nul = e.payload.firstIndex(of: 0) ?? e.payload.endIndex
                    guard let json = String(data: e.payload[e.payload.startIndex ..< nul], encoding: .utf8) else { continue }
                    r.entries.append(.init(n: e.name, j: json))
                }
                st.sent.insert(e.name)
            }
        }
        if let log = try? Data(contentsOf: slowLog), log.count > st.slowOffset {
            let tail = log[(log.startIndex + st.slowOffset)...]
            let lastNewline = tail.lastIndex(of: 0x0a)  // only whole lines; a half-written one waits for next time
            if let end = lastNewline, let text = String(data: tail[tail.startIndex ... end], encoding: .utf8) {
                for line in text.split(separator: "\n") {
                    let f = line.split(separator: " ")
                    if f.count >= 3, f[0].count == 16, let ms = Double(f[1]), let t = Double(f[2]) {
                        r.hitches.append(.init(p: String(f[0]), ms: ms, t: t))
                    }
                }
                st.slowOffset += tail.distance(from: tail.startIndex, to: end) + 1
            }
        }
        if let url = prewarmLog, let log = try? Data(contentsOf: url), log.count > st.prepOffset {
            let tail = log[(log.startIndex + st.prepOffset)...]
            if let end = tail.lastIndex(of: 0x0a), let text = String(data: tail[tail.startIndex ... end], encoding: .utf8) {
                for line in text.split(separator: "\n") {
                    let f = line.split(separator: " ")
                    if f.count >= 5, let at = Double(f[0]), let sec = Double(f[1]), let n = Int(f[2]), let slow = Int(f[3]),
                       let rc = Int(f[4]) {
                        r.prep.append(.init(at: at, s: sec, n: n, slow: slow, rc: rc))
                    }
                }
                st.prepOffset += tail.distance(from: tail.startIndex, to: end) + 1
            }
        }
        if let url = perfLog, let log = try? Data(contentsOf: url), log.count > st.perfOffset {
            let tail = log[(log.startIndex + st.perfOffset)...]
            if let end = tail.lastIndex(of: 0x0a), let text = String(data: tail[tail.startIndex ... end], encoding: .utf8) {
                for line in text.split(separator: "\n") {
                    let f = line.split(separator: " ")
                    if f.count == 6, let s = Double(f[0]), let fps = Double(f[1]), let hz = Double(f[2]), let heat = Int(f[3]),
                       let avail = Int(f[4]), let foot = Int(f[5]) {
                        r.perf.append(.init(s: s, fps: fps, hz: hz, heat: heat, avail: avail, foot: foot))
                    }
                }
                st.perfOffset += tail.distance(from: tail.startIndex, to: end) + 1
            }
        }
        st.seq += 1
        return (r, st)
    }

    /// The upload body: raw-deflate JSON.
    public static func body(_ r: Report) throws -> Data {
        let enc = JSONEncoder()
        enc.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
        return deflateRaw(try enc.encode(r))
    }
}
