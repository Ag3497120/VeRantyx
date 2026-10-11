import Foundation

// What the engine (`engine/glovebox.py` in glovebox-vr/glovebox, contract in its docs/INSTALLER.md)
// prints with `--json`, read into the app's own terms. The engine says what's wrong and which fix
// applies; the app picks the picture and the words. Anything it doesn't recognise is ignored, so a
// newer engine never breaks an older app.

/// Any JSON value, for the open-ended parts of an event.
public enum JSONValue: Sendable, Equatable, Decodable {
    case string(String), number(Double), bool(Bool), array([JSONValue]), object([String: JSONValue]), null

    public init(from decoder: Decoder) throws {
        let c = try decoder.singleValueContainer()
        if c.decodeNil() { self = .null }
        else if let b = try? c.decode(Bool.self) { self = .bool(b) }
        else if let n = try? c.decode(Double.self) { self = .number(n) }
        else if let s = try? c.decode(String.self) { self = .string(s) }
        else if let a = try? c.decode([JSONValue].self) { self = .array(a) }
        else { self = .object(try c.decode([String: JSONValue].self)) }
    }

    public subscript(key: String) -> JSONValue? {
        if case .object(let o) = self { return o[key] }
        return nil
    }
    public var string: String? { if case .string(let s) = self { return s }; return nil }
    public var double: Double? { if case .number(let n) = self { return n }; return nil }
    public var bool: Bool? { if case .bool(let b) = self { return b }; return nil }
    public var array: [JSONValue] { if case .array(let a) = self { return a }; return [] }
    public var uint64: UInt64? { double.map { UInt64(max(0, $0)) } }
}

/// The setup checks, in the order they're shown. All but `developerMode` are the engine's `doctor`
/// checks; the engine reports Developer Mode inside `headset`, and the app shows it as its own step.
public enum CheckID: String, CaseIterable, Sendable, Codable {
    case mac, disk, xcode
    case visionOS = "visionos"
    case team, headset
    case developerMode = "developer-mode"
}

/// What the engine says fixes a failed check.
public struct Fix: Sendable, Equatable {
    public enum Kind: String, Sendable {
        /// Opens a URL (App Store, System Settings, a web page).
        case openURL = "open-url"
        /// Opens an app (`xcode://` for Xcode).
        case openApp = "open-app"
        /// The app runs `command` itself, showing its progress.
        case command
        /// The app shows its own picture for `guide` (`pair-headset`, `developer-mode`).
        case guide
    }

    public var kind: Kind
    public var label: String?
    public var url: String?
    public var command: String?
    public var guide: String?

    public init(kind: Kind, label: String? = nil, url: String? = nil, command: String? = nil, guide: String? = nil) {
        self.kind = kind; self.label = label; self.url = url; self.command = command; self.guide = guide
    }
}

public struct CheckResult: Sendable, Equatable {
    public enum Status: String, Sendable { case ok, fail, warn, unknown }
    public var id: CheckID
    public var status: Status
    /// The app's name for the problem, which picks the picture (see `EngineEvent.code(for:)`).
    public var code: String?
    /// A few words for under the row's name when it passes.
    public var value: String?
    /// The engine's own sentence, kept for the generic card and for diagnostics.
    public var detail: String?
    public var fix: Fix?
    public var data: JSONValue?

    public init(id: CheckID, status: Status, code: String? = nil, value: String? = nil, detail: String? = nil,
                fix: Fix? = nil, data: JSONValue? = nil) {
        self.id = id; self.status = status; self.code = code; self.value = value; self.detail = detail
        self.fix = fix; self.data = data
    }

    /// Fine to move on: `warn` and `unknown` don't stop setup.
    public var passes: Bool { status != .fail }
}

/// A developer team, as Xcode's account settings list it.
public struct Team: Sendable, Equatable, Identifiable {
    public var id: String
    public var name: String
    public var paid: Bool
    public init(id: String, name: String, paid: Bool) { self.id = id; self.name = name; self.paid = paid }
}

public extension CheckResult {
    /// The app adds Xcode's teams to the `team` result (`data.teams`) so the player can pick one.
    var teams: [Team] {
        (data?["teams"]?.array ?? []).compactMap { t in
            guard let id = t["id"]?.string else { return nil }
            return Team(id: id, name: t["name"]?.string ?? id, paid: t["paid"]?.bool ?? false)
        }
    }
}

/// Install, as the player sees it. Download is the app's own (Steam); the rest are engine steps.
public enum Stage: String, CaseIterable, Sendable {
    case download, convert, build, install

    /// The stage an engine step belongs to.
    public init?(step: String) {
        switch step {
        case "kit", "runtime", "game", "pipelines", "defaults", "translate": self = .convert
        case "app": self = .build
        case "install": self = .install
        default: return nil
        }
    }
}

public struct EngineError: Error, Sendable, Equatable {
    public var code: String
    public var stage: Stage?
    public var message: String?
    public var log: String?
    public var data: JSONValue?

    public init(code: String, stage: Stage? = nil, message: String? = nil, log: String? = nil, data: JSONValue? = nil) {
        self.code = code; self.stage = stage; self.message = message; self.log = log; self.data = data
    }
}

/// What's on the headset, as the app recorded it after the last install.
public struct HeadsetStatus: Sendable, Equatable, Codable {
    public var installed: Bool
    /// The Glovebox release it was built from.
    public var version: String?
    /// The release this app would build now.
    public var latest: String?
    /// When the signing profile runs out and the app stops opening.
    public var expires: Date?
    /// Signed with a free Apple Account (a Personal Team): its profile lasts 7 days, not a year.
    public var freeAccount: Bool?

    public init(installed: Bool, version: String? = nil, latest: String? = nil, expires: Date? = nil, freeAccount: Bool? = nil) {
        self.installed = installed; self.version = version; self.latest = latest; self.expires = expires
        self.freeAccount = freeAccount
    }

    public var updateAvailable: Bool {
        guard installed, let v = version, let l = latest else { return false }
        return Self.isOlder(v, than: l)
    }
    /// Whether release `a` came before `b`: numbers compared as numbers, and a pre-release before its release
    /// (0.1.0-beta.4 before 0.1.0, which a plain comparison puts the other way round).
    public static func isOlder(_ a: String, than b: String) -> Bool {
        func split(_ v: String) -> (String, String?) {
            guard let i = v.firstIndex(of: "-") else { return (v, nil) }
            return (String(v[..<i]), String(v[v.index(after: i)...]))
        }
        let (ar, ap) = split(a), (br, bp) = split(b)
        switch ar.compare(br, options: .numeric) {
        case .orderedAscending: return true
        case .orderedDescending: return false
        case .orderedSame:
            guard let ap else { return false }
            guard let bp else { return true }
            return ap.compare(bp, options: .numeric) == .orderedAscending
        }
    }
    /// Days until the app stops opening (a paid team's profile lasts a year).
    public func daysLeft(now: Date = Date()) -> Int? {
        expires.map { Int(($0.timeIntervalSince(now) / 86_400).rounded(.down)) }
    }
    /// Time to build it again before it stops opening: a month ahead for a year's profile, two days
    /// ahead for a week's.
    public func renewSoon(now: Date = Date()) -> Bool {
        guard installed, let d = daysLeft(now: now) else { return false }
        return d < (freeAccount == true ? 2 : 30)
    }
}

public struct InstallResult: Sendable, Equatable {
    /// The built app on this Mac.
    public var app: String?
    public var device: String?
    public var expires: Date?

    public init(app: String? = nil, device: String? = nil, expires: Date? = nil) {
        self.app = app; self.device = device; self.expires = expires
    }
}

public enum EngineEvent: Sendable, Equatable {
    case check(CheckResult)
    /// A line of work inside a step, with a count when the engine knows one.
    case stepProgress(step: String, line: String?, done: Int?, total: Int?)
    case stepDone(step: String, detail: String?)
    case error(EngineError)
    /// The command finished (`doctor`, `build`, `install`); `app` is the built app for `build`.
    case finished(step: String, ok: Bool, app: String?)
    /// The app stopped it (the engine has no event for this; the runner adds it).
    case cancelled
    /// A line the app doesn't understand.
    case unknown(String)
}

extension EngineEvent {
    private struct Raw: Decodable {
        var event: String?
        var check: String?
        var title: String?
        var status: String?
        var detail: String?
        var fix: RawFix?
        var step: String?
        var line: String?
        var done: Int?
        var total: Int?
        var ok: Bool?
        var app: String?
        var message: String?
    }

    private struct RawFix: Decodable {
        var kind: String?
        var label: String?
        var url: String?
        var command: String?
        var guide: String?
    }

    /// Decodes one stdout line into what the app works with. Never throws; one `headset` check
    /// becomes two results (paired, and Developer Mode).
    public static func decode(_ line: String) -> [EngineEvent] {
        guard let raw = try? JSONDecoder().decode(Raw.self, from: Data(line.utf8)), let event = raw.event else {
            return [.unknown(line)]
        }
        switch event {
        case "check":
            guard let results = checks(raw) else { return [.unknown(line)] }
            return results.map(EngineEvent.check)
        case "progress":
            guard let step = raw.step else { return [.unknown(line)] }
            return [.stepProgress(step: step, line: raw.line, done: raw.done, total: raw.total)]
        case "step":
            guard let step = raw.step else { return [.unknown(line)] }
            return [.stepDone(step: step, detail: raw.detail)]
        case "error":
            return [.error(error(message: raw.message ?? "", step: raw.step))]
        case "done":
            return [.finished(step: raw.step ?? "", ok: raw.ok ?? false, app: raw.app)]
        default:
            return [.unknown(line)]
        }
    }

    private static func checks(_ raw: Raw) -> [CheckResult]? {
        guard let name = raw.check else { return nil }
        let status: CheckResult.Status
        switch raw.status {
        case "ok": status = .ok
        case "missing": status = .fail
        case "warn": status = .warn
        default: status = .unknown
        }
        let fix = raw.fix.flatMap { f in f.kind.flatMap(Fix.Kind.init(rawValue:)).map {
            Fix(kind: $0, label: f.label, url: f.url, command: f.command, guide: f.guide)
        } }
        let detail = raw.detail
        if name == "headset" { return headset(status, detail: detail, fix: fix) }
        guard let id = CheckID(rawValue: name) else { return nil }
        let failed = status == .fail
        return [CheckResult(id: id, status: status, code: failed ? code(for: id, fix: fix) : nil,
                            value: failed ? nil : caption(id, detail), detail: detail, fix: fix)]
    }

    /// The engine's one headset check, as the app's two steps.
    private static func headset(_ status: CheckResult.Status, detail: String?, fix: Fix?) -> [CheckResult] {
        // "Jane’s Vision Pro: Developer Mode is off" / "Jane’s Vision Pro (visionOS 26.0)"
        let name = detail.map { d in String(d.split(separator: ":").first.map(String.init)?
            .split(separator: "(").first.map { $0.trimmingCharacters(in: .whitespaces) } ?? d) }
        switch status {
        case .fail where fix?.guide == "developer-mode":
            return [CheckResult(id: .headset, status: .ok, value: name, detail: detail),
                    CheckResult(id: .developerMode, status: .fail, code: "developer-mode-off", detail: detail, fix: fix)]
        case .fail:
            return [CheckResult(id: .headset, status: .fail, code: "headset-not-paired", detail: detail, fix: fix)]
        case .warn:
            // Paired and set up, just asleep or away: setup goes on; install waits for it.
            return [CheckResult(id: .headset, status: .warn, code: "headset-unreachable", value: name, detail: detail),
                    CheckResult(id: .developerMode, status: .ok, value: "On")]
        default:
            return [CheckResult(id: .headset, status: status, value: name, detail: detail),
                    CheckResult(id: .developerMode, status: status, value: "On")]
        }
    }

    /// The app's name for a failed check, from the fix the engine offers. Unknown ones get
    /// `<check>-missing` and the generic card, which still shows the engine's words and its fix.
    public static func code(for id: CheckID, fix: Fix?) -> String {
        let label = fix?.label?.lowercased() ?? ""
        let command = fix?.command ?? ""
        switch (id, fix?.kind) {
        case (.mac, _): return "mac-unsupported"
        case (.disk, _): return "disk-low"
        case (.xcode, .openURL?): return label.contains("update") ? "xcode-too-old" : "xcode-missing"
        case (.xcode, .openApp?): return "xcode-first-launch"
        case (.visionOS, _): return "visionos-missing"
        case (.team, .openURL?): return "team-free-only"
        case (.team, .openApp?): return label.contains("team") ? "apple-team-not-loaded" : "apple-account-missing"
        default: return "\(id.rawValue)-missing"
        }
    }

    /// Short row captions: "macOS 26.0", "Xcode 26.0.1", "120 GB free".
    static func caption(_ id: CheckID, _ detail: String?) -> String? {
        guard let d = detail else { return nil }
        switch id {
        case .mac: return d.replacingOccurrences(of: "Apple silicon, ", with: "")
        case .team: return nil  // the app shows the team's name instead
        default: return d
        }
    }

    /// An engine error, given the app's name for it. The engine only sends a sentence.
    static func error(message: String, step: String?) -> EngineError {
        let m = message.lowercased()
        let code: String
        if m.contains("unable to log in with account") || m.contains("login details for account") {
            // Xcode's saved sign-in for the account expired. Its account and team lists still look signed in,
            // so the checks pass: only signing in to Xcode again helps.
            code = "xcode-signin-expired"
        } else if m.contains("pla update available") || m.contains("program license agreement") {
            // Apple updated its developer agreement: until the account accepts it on Apple's website, Xcode can't
            // make certificates or profiles.
            code = "apple-agreement-pending"
        } else if step == "team" || m.contains("no paid apple developer") {
            code = "signing-no-team"
        } else if m.contains("no paired vision pro") || m.contains("locked") || m.contains("unable to connect")
                    || m.contains("not connected") || m.contains("tunnel") || m.contains("unavailable")
                    || m.contains("unexpectedly closed") {
            code = "headset-unreachable"
        } else if m.contains("game files missing") {
            code = "game-files-missing"
        } else if m.contains("no runtime kit") {
            code = "kit-missing"
        } else if m.contains("did not build") || m.contains("provision") || m.contains("signing") {
            code = "signing-failed"
        } else if m.contains("devicectl") {
            code = "install-failed"
        } else {
            code = "internal"
        }
        return EngineError(code: code, stage: step.flatMap(Stage.init(step:)), message: message)
    }
}
