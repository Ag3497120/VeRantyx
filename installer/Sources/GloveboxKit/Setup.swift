import Foundation

/// The setup screen's rows. Each row stands for one or more checks, and is as good as its worst
/// one, so the player sees five things to get right.
public enum SetupRow: String, CaseIterable, Sendable {
    case mac, xcode, account, headset, developerMode

    public var checks: [CheckID] {
        switch self {
        case .mac: return [.mac, .disk]
        case .xcode: return [.xcode, .visionOS]
        case .account: return [.team]
        case .headset: return [.headset]
        case .developerMode: return [.developerMode]
        }
    }

    public static func of(_ check: CheckID) -> SetupRow { allCases.first { $0.checks.contains(check) }! }
}

/// Where setup stands, from the latest result of every check.
public struct SetupState: Sendable, Equatable {
    public enum RowState: Sendable, Equatable { case waiting, checking, ok, blocked }

    public var results: [CheckID: CheckResult] = [:]
    /// The check being re-run right now, if any.
    public var running: CheckID?
    /// The team the player picked when there are several paid ones.
    public var chosenTeam: String?

    public init() {}

    public mutating func record(_ r: CheckResult) { results[r.id] = r }

    /// The first failing check, in order: the one thing the player is asked to fix.
    public var blocker: CheckResult? {
        CheckID.allCases.lazy.compactMap { results[$0] }.first { !$0.passes }
    }

    /// Nothing fails, but a choice is still open (several teams the build could sign with).
    public var openChoice: CheckResult? {
        guard blocker == nil else { return nil }
        if let t = results[.team], team == nil, usableTeams.count > 1 { return t }
        return nil
    }

    /// The teams a build may sign with: the paid ones, else the free ones (Personal Teams), whose builds
    /// stop opening after 7 days. The engine picks the same way.
    public var usableTeams: [Team] {
        let all = results[.team]?.teams ?? []
        let paid = all.filter(\.paid)
        return paid.isEmpty ? all : paid
    }

    /// The team the build signs with: the chosen one, else the only one it may use.
    public var team: Team? {
        let teams = usableTeams
        if let id = chosenTeam, let t = teams.first(where: { $0.id == id }) { return t }
        return teams.count == 1 ? teams[0] : nil
    }

    /// The headset's name, as the engine reported it.
    public var headsetName: String? { results[.headset]?.value }

    /// Every check has answered, nothing fails and nothing is left to choose.
    public var complete: Bool {
        CheckID.allCases.allSatisfy { results[$0] != nil } && blocker == nil && openChoice == nil
    }

    public func state(of row: SetupRow) -> RowState {
        let rs = row.checks.map { results[$0] }
        if let r = running, row.checks.contains(r) { return .checking }
        if rs.contains(where: { $0.map { !$0.passes } ?? false }) {
            // Only the first problem is shown as blocking; later rows wait their turn.
            return blocker.map { SetupRow.of($0.id) == row } == true ? .blocked : .waiting
        }
        if rs.contains(where: { $0 == nil }) { return .waiting }
        // Rows after the blocker aren't trusted yet: they may change once it's fixed.
        if let b = blocker, SetupRow.allCases.firstIndex(of: row)! > SetupRow.allCases.firstIndex(of: SetupRow.of(b.id))! {
            return .waiting
        }
        return .ok
    }
}

/// One progress bar across the four install stages, weighted by how long each usually takes. The
/// download reports bytes; the engine's steps mostly report lines, so those stages creep forward
/// per line and jump when a step finishes.
public struct InstallProgress: Sendable, Equatable {
    public static let weights: [Stage: Double] = [.download: 0.4, .convert: 0.3, .build: 0.2, .install: 0.1]
    /// Each engine step's share of its stage.
    public static let stepShare: [String: Double] = ["kit": 0.05, "runtime": 0.15, "game": 0.02, "pipelines": 0.18,
                                                     "defaults": 0.02, "translate": 0.58, "app": 1, "install": 1]
    /// Typical seconds for the engine's stages on an M-series Mac, for "about N minutes left".
    public static let typicalSeconds: [Stage: Double] = [.convert: 90, .build: 60, .install: 40]

    public var stage: Stage?
    public var stageFraction: Double = 0
    public var finished: Set<Stage> = []
    public var secondsLeft: Double?
    private var stepsDone: Set<String> = []
    private var step: String?
    private var stepFraction: Double = 0

    public init() {}

    public var overall: Double {
        var sum = 0.0
        for s in Stage.allCases {
            let w = Self.weights[s] ?? 0
            if finished.contains(s) { sum += w } else if s == stage { sum += w * stageFraction }
        }
        return min(1, sum)
    }

    public mutating func begin(_ s: Stage) {
        stage = s
        stageFraction = 0
        step = nil
        stepFraction = 0
        estimate()
    }

    public mutating func finish(_ s: Stage) {
        finished.insert(s)
        if stage == s { stageFraction = 1 }
        estimate()
    }

    /// The app's own Steam download.
    public mutating func download(fraction: Double, secondsLeft: Double?) {
        if stage != .download { begin(.download) }
        stageFraction = min(1, max(0, fraction))
        // The engine's stages follow the download.
        self.secondsLeft = secondsLeft.map { $0 + Self.after(.download) }
    }

    public mutating func apply(_ event: EngineEvent) {
        switch event {
        case .stepProgress(let name, _, let done, let total):
            guard let s = Stage(step: name) else { return }
            if s != stage { begin(s) }
            if name != step { step = name; stepFraction = 0 }
            if let d = done, let t = total, t > 0 {
                stepFraction = min(0.95, Double(d) / Double(t))
            } else {
                stepFraction += (0.9 - stepFraction) * 0.03
            }
            update()
        case .stepDone(let name, _):
            guard let s = Stage(step: name) else { return }
            if s != stage { begin(s) }
            stepsDone.insert(name)
            if step == name { step = nil; stepFraction = 0 }
            update()
        default:
            break
        }
    }

    private mutating func update() {
        guard let s = stage else { return }
        let done = stepsDone.filter { Stage(step: $0) == s }.reduce(0) { $0 + (Self.stepShare[$1] ?? 0) }
        let current = step.map { (Self.stepShare[$0] ?? 0) * stepFraction } ?? 0
        stageFraction = max(stageFraction, min(0.99, done + current))
        estimate()
    }

    private mutating func estimate() {
        guard let s = stage, s != .download else { return }
        secondsLeft = (finished.contains(s) ? 0 : (1 - stageFraction) * (Self.typicalSeconds[s] ?? 0)) + Self.after(s)
    }

    /// Typical seconds for the stages after `s`.
    private static func after(_ s: Stage) -> Double {
        Stage.allCases.drop { $0 != s }.dropFirst().reduce(0) { $0 + (typicalSeconds[$1] ?? 0) }
    }
}

public enum Format {
    /// "About 3 minutes left", in plain words.
    public static func timeLeft(_ seconds: Double?) -> String? {
        guard let s = seconds, s.isFinite, s >= 0 else { return nil }
        switch s {
        case ..<45: return "Almost done"
        case ..<90: return "About a minute left"
        case ..<3_300: return "About \(Int((s / 60).rounded())) minutes left"
        default:
            let h = s / 3600
            return h < 1.5 ? "About an hour left" : "About \(Int(h.rounded())) hours left"
        }
    }

    public static func gigabytes(_ bytes: UInt64) -> String {
        let gb = Double(bytes) / 1_000_000_000
        return gb >= 10 ? "\(Int(gb.rounded())) GB" : String(format: "%.1f GB", gb)
    }
}
