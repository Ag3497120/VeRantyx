import Foundation

/// The player's answer to "Help make it smoother for everyone". It can be given on the Mac (the build carries it) and on
/// the headset; the newest answer wins.
public enum ReportConsent: String, Codable { case yes, no, ask }

/// One answer and when it was given (unix seconds). On the headset it is kept in Documents/klfz/report-consent.json as
/// {"answer": true|false, "at": <unix time>}, the launcher's format (launcher/App/AlyxLauncher/StutterSharing.swift).
public struct ConsentAnswer: Codable, Equatable {
    public var answer: ReportConsent
    public var at: Double
    public init(answer: ReportConsent, at: Double) { self.answer = answer; self.at = at }

    enum CodingKeys: String, CodingKey { case answer, at }
    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        if let b = try? c.decode(Bool.self, forKey: .answer) { answer = b ? .yes : .no }
        else { answer = try c.decode(ReportConsent.self, forKey: .answer) }
        at = try c.decode(Double.self, forKey: .at)
    }
    public func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode(answer == .yes, forKey: .answer)
        try c.encode(at, forKey: .at)
    }

    public static func load(_ url: URL) -> ConsentAnswer? {
        guard let d = try? Data(contentsOf: url), let a = try? JSONDecoder().decode(ConsentAnswer.self, from: d),
              a.answer != .ask else { return nil }
        return a
    }

    /// The Mac app's answer, from the build's alyx-env.txt: GLOVEBOX_SHARE_STUTTERS=1|0 and GLOVEBOX_SHARE_STUTTERS_AT
    /// (the build time; missing counts as older than any headset answer).
    public static func fromBuild(env: String) -> ConsentAnswer? {
        var kv: [String: String] = [:]
        for line in env.split(whereSeparator: \.isNewline) {
            guard let eq = line.firstIndex(of: "=") else { continue }
            kv[line[..<eq].trimmingCharacters(in: .whitespaces)] =
                line[line.index(after: eq)...].trimmingCharacters(in: .whitespaces)
        }
        let answer: ReportConsent
        switch kv["GLOVEBOX_SHARE_STUTTERS"] {
        case "1": answer = .yes
        case "0": answer = .no
        default: return nil
        }
        let at = kv["GLOVEBOX_SHARE_STUTTERS_AT"].flatMap(Double.init) ?? 0
        return ConsentAnswer(answer: answer, at: at)
    }

    public static func fromBuild(bundle: Bundle = .main) -> ConsentAnswer? {
        guard let url = bundle.url(forResource: "alyx-env", withExtension: "txt", subdirectory: "alyx-demo"),
              let s = try? String(contentsOf: url, encoding: .utf8) else { return nil }
        return fromBuild(env: s)
    }

    /// The newer of the two; the Mac's on a tie, as the launcher decides.
    public static func newest(mac: ConsentAnswer?, headset: ConsentAnswer?) -> ConsentAnswer? {
        guard let m = mac else { return headset }
        guard let h = headset else { return m }
        return h.at > m.at ? h : m
    }
}

/// Versions and endpoint, from the build's alyx-demo/glovebox.json.
public struct GloveboxBuildInfo: Codable {
    public var glovebox: String?
    public var kit: String?
    public var endpoint: String?
    public static func load(bundle: Bundle = .main) -> GloveboxBuildInfo {
        guard let url = bundle.url(forResource: "glovebox", withExtension: "json", subdirectory: "alyx-demo"),
              let d = try? Data(contentsOf: url), let info = try? JSONDecoder().decode(GloveboxBuildInfo.self, from: d)
        else { return GloveboxBuildInfo() }
        return info
    }
}

public final class ReportUploader {
    public let documents: URL
    public let info: GloveboxBuildInfo
    /// The Mac app's answer as the build carries it (nil: not answered there).
    public let macAnswer: ConsentAnswer?

    public init(documents: URL, info: GloveboxBuildInfo = .load(), macAnswer: ConsentAnswer? = .fromBuild()) {
        self.documents = documents; self.info = info; self.macAnswer = macAnswer
    }

    var consentURL: URL { documents.appendingPathComponent("klfz/report-consent.json") }

    /// The newest answer, from the Mac or the headset (nil: never answered, so ask).
    public var consentAnswer: ConsentAnswer? {
        ConsentAnswer.newest(mac: macAnswer, headset: ConsentAnswer.load(consentURL))
    }

    /// yes / no, whichever was answered last, or ask.
    public var consent: ReportConsent { consentAnswer?.answer ?? .ask }

    /// Keeps the player's answer on the headset, timed now. `.ask` forgets it (the Mac app's answer, if any, applies).
    public func setConsent(_ c: ReportConsent, at: Date = Date()) throws {
        if c == .ask {
            try? FileManager.default.removeItem(at: consentURL)
            return
        }
        try FileManager.default.createDirectory(at: consentURL.deletingLastPathComponent(),
                                                withIntermediateDirectories: true)
        try JSONEncoder().encode(ConsentAnswer(answer: c, at: at.timeIntervalSince1970))
            .write(to: consentURL, options: .atomic)
    }

    var stateURL: URL { documents.appendingPathComponent("klfz/report-state.json") }

    /// Sends what's new, if the player said yes, in up to `maxBatches` reports (a long recording needs several; the
    /// rest waits for the next call). Safe to call often: an empty report is not sent, and nothing is marked sent
    /// unless the endpoint accepted it.
    @discardableResult
    public func uploadIfAllowed(maxBatches: Int = 20, session: URLSession = .shared) async -> Bool {
        guard consent == .yes, let ep = URL(string: info.endpoint ?? "https://glovebox-reports.vercel.app/api/report")
        else { return false }
        var state = ReportState.load(stateURL)
        var sentAny = false
        for _ in 0 ..< max(maxBatches, 0) {
            guard let (report, next) = try? ReportBuilder.next(
                recording: documents.appendingPathComponent("klfz/recorded.foz"),
                slowLog: documents.appendingPathComponent("klfz/slow.txt"),
                prewarmLog: documents.appendingPathComponent("klfz/prewarm.log"),
                perfLog: documents.appendingPathComponent("klfz/perf.txt"), state: state,
                app: .init(glovebox: info.glovebox ?? "dev", kit: info.kit ?? "dev"),
                device: .init(model: Self.model, os: Self.osVersion)),
                !report.isEmpty, let body = try? ReportBuilder.body(report)
            else { break }
            var req = URLRequest(url: ep)
            req.httpMethod = "POST"
            req.setValue("application/x-glovebox-report", forHTTPHeaderField: "Content-Type")
            req.httpBody = body
            guard let (_, resp) = try? await session.data(for: req), (resp as? HTTPURLResponse)?.statusCode == 204 else { break }
            state = next
            try? state.save(stateURL)
            sentAny = true
        }
        return sentAny
    }

    public static var model: String {
        var u = utsname(); uname(&u)
        return withUnsafeBytes(of: &u.machine) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
    }
    public static var osVersion: String {
        let v = ProcessInfo.processInfo.operatingSystemVersion
        return "\(v.majorVersion).\(v.minorVersion).\(v.patchVersion)"
    }
}
