import Foundation

/// Takes personal details out of a diagnostics report before it leaves the Mac: Steam sessions,
/// account names, email addresses, team and device ids, the user's name in device names, and the
/// home folder.
public struct Redactor: Sendable {
    /// Exact strings to hide wherever they appear (the Steam account, Apple account, team names…).
    public var known: [String]
    public var home: String

    public init(known: [String] = [], home: String = NSHomeDirectory()) {
        self.known = known; self.home = home
    }

    private static let rules: [(String, String)] = [
        // Steam sessions: JWTs, and anything labelled as a token.
        (#"eyJ[A-Za-z0-9_-]{6,}\.[A-Za-z0-9_-]{6,}\.[A-Za-z0-9_-]{6,}"#, "‹token›"),
        (#"("(?:refreshToken|accessToken|password|token)"\s*:\s*)"[^"]*""#, "$1\"‹hidden›\""),
        (#"[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}"#, "‹email›"),
        // Device ids: Apple silicon UDIDs and UUIDs.
        (#"\b[0-9A-Fa-f]{8}-[0-9A-Fa-f]{16}\b"#, "‹device id›"),
        (#"\b[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\b"#, "‹id›"),
        // "Jane's Apple Vision Pro" → "‹name›'s Apple Vision Pro"; host names too.
        (#"\b[\p{L}\p{N}][\p{L}\p{N} .-]{0,40}?['’]s (Apple Vision Pro|Vision Pro|Mac Studio|MacBook Pro|MacBook Air|Mac mini|Mac Pro|iMac|iPhone)"#,
         "‹name›'s $1"),
        (#"\b[\w-]+\.local\b"#, "‹host›.local"),
    ]

    /// Apple team ids: ten capitals and digits, with at least one of each. The last two stay, so two
    /// teams can still be told apart.
    private static let team = try! NSRegularExpression(pattern: #"\b(?=[A-Z0-9]*[0-9])(?=[A-Z0-9]*[A-Z])[A-Z0-9]{8}([A-Z0-9]{2})\b"#)

    public func redact(_ text: String) -> String {
        var s = text
        for k in known.filter({ $0.count >= 3 }).sorted(by: { $0.count > $1.count }) {
            s = s.replacingOccurrences(of: k, with: "‹redacted›", options: .caseInsensitive)
        }
        for (pattern, template) in Self.rules {
            let re = try! NSRegularExpression(pattern: pattern)
            s = re.stringByReplacingMatches(in: s, range: NSRange(s.startIndex..., in: s), withTemplate: template)
        }
        s = Self.team.stringByReplacingMatches(in: s, range: NSRange(s.startIndex..., in: s), withTemplate: "‹team …$1›")
        if home.count > 1 { s = s.replacingOccurrences(of: home, with: "~") }
        return s
    }
}

/// The text Copy Diagnostics puts on the clipboard: enough to fix an install without a back and
/// forth, and nothing personal.
public enum DiagnosticsReport {
    public static func compose(app: [(String, String)], checks: [CheckResult], error: EngineError?,
                               engineReport: String? = nil, logTail: [String], redactor: Redactor) -> String {
        var out = ["Glovebox diagnostics", ""]
        out += app.map { "\($0.0): \($0.1)" }
        out += ["", "Checks:"]
        for id in CheckID.allCases {
            guard let r = checks.first(where: { $0.id == id }) else { out.append("  \(id.rawValue): not run"); continue }
            out.append("  \(id.rawValue): \(r.status.rawValue)\(r.code.map { " (\($0))" } ?? "")\((r.detail ?? r.value).map { " \($0)" } ?? "")")
        }
        if let e = error {
            out += ["", "Last error: \(e.code)\(e.stage.map { " during \($0.rawValue)" } ?? "")"]
            if let m = e.message, !m.isEmpty { out.append(m) }
        }
        if let r = engineReport, !r.isEmpty { out += ["", "Engine:", r] }
        if !logTail.isEmpty { out += ["", "Last log lines:"] + logTail }
        return redactor.redact(out.joined(separator: "\n"))
    }

    /// The last lines of the run that ended in this error (a build's or an install's, not the checks' that ran
    /// since), or else of the newest engine log.
    public static func logTail(for error: EngineError?, in folder: URL, lines: Int = 80) -> [String] {
        if let path = error?.log, FileManager.default.fileExists(atPath: path) {
            return logTail(of: URL(fileURLWithPath: path), lines: lines)
        }
        return latestLogTail(in: folder, lines: lines)
    }

    public static func logTail(of url: URL, lines: Int = 80) -> [String] {
        guard let text = try? String(contentsOf: url, encoding: .utf8) else { return [] }
        return ["(\(url.lastPathComponent))"] + text.split(separator: "\n", omittingEmptySubsequences: false).suffix(lines).map(String.init)
    }

    /// The newest engine log's last lines.
    public static func latestLogTail(in folder: URL, lines: Int = 80) -> [String] {
        let fm = FileManager.default
        guard let files = try? fm.contentsOfDirectory(at: folder, includingPropertiesForKeys: [.contentModificationDateKey]) else { return [] }
        let newest = files.filter { $0.pathExtension == "log" }.max {
            let a = (try? $0.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
            let b = (try? $1.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
            return a < b
        }
        guard let url = newest else { return [] }
        return logTail(of: url, lines: lines)
    }
}
