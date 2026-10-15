import SwiftUI
import AppKit
import GloveboxKit
import SteamLauncherKit

/// The whole installer. Setup is a list of steps in the sidebar; the app checks them in the
/// background every few seconds, does everything it can by itself (adding visionOS to Xcode,
/// downloading the game files), and moves to the next step the moment one is done. The player
/// acts only where Apple or Steam needs a person.
@MainActor
final class InstallerModel: ObservableObject {
    enum Step: String, CaseIterable, Identifiable, Hashable {
        case welcome, xcode, steam, account, headset, install
        var id: Self { self }
    }

    enum Mode: Equatable { case setup, home }

    /// A fix the app is running by itself, e.g. adding visionOS to Xcode.
    /// Set once "Open Xcode" was pressed for Xcode's own setup, until the check passes.
    @Published var xcodeOpenedForSetup = false

    struct AutoFix: Equatable {
        var code: String
        var fraction: Double?
        var secondsLeft: Double?
    }

    /// The Mac's share of the game, downloading in the background from the moment Steam says the
    /// player owns it.
    struct Download: Equatable {
        var running = false
        var fraction: Double = 0
        var secondsLeft: Double?
        var done = false
        var error: EngineError?
    }

    @Published var mode: Mode = .setup
    @Published var step: Step = .welcome
    @Published var started = false
    @Published var setup = SetupState()
    @Published var checkedOnce = false
    @Published var autoFix: AutoFix?
    /// The player pressed Get Xcode; the App Store has it now.
    @Published var xcodeRequested = false
    @Published var download = Download()
    @Published var progress = InstallProgress()
    @Published var installing = false
    @Published var installError: EngineError?
    @Published var waitingForHeadset = false
    @Published var result: InstallResult?
    @Published var status: HeadsetStatus?
    @Published var engineMissing = false
    /// Made-up states for screenshots and the walkthrough (Demo.swift): no checks run.
    private(set) var isDemo = false
    @Published var toast: String?
    /// The installed Xcode's major version: 27 and later pair in Device Hub.
    @Published var xcodeMajor: Int?
    /// Xcode's own icon, once it's installed.
    @Published var xcodeIcon: NSImage?
    /// Whether the headset may send stutter reports: asked once, during the first install, before
    /// its build (which carries the answer to the headset). Nil until the player answers.
    /// VeRantyx Standalone: always no, never asked (`upstreamServices`).
    @Published var shareStutters: Bool? = InstallerModel.upstreamServices
        ? UserDefaults.standard.object(forKey: "shareStutters") as? Bool : false
    /// The reports question is on screen (ShareAsk.swift).
    @Published var askingToShare = false
    /// The news sign-up is up (NewsAsk.swift): once, in a quiet stretch of an install, or from Home's link.
    @Published var askingForNews = false
    /// Signed up for news: Home's link goes away.
    @Published private(set) var newsSignedUp = UserDefaults.standard.bool(forKey: "newsSignedUp")
    /// No Thanks or a sign-up: installs don't ask again.
    private var newsAnswered = !InstallerModel.upstreamServices || UserDefaults.standard.bool(forKey: "newsAnswered")
    /// Glovebox's own services: stutter reports to its server and its news sign-up (glovebox.vision). They belong to
    /// official Glovebox, so this unofficial build (VeRantyx Standalone) neither asks for them nor sends anything there.
    nonisolated static let upstreamServices = false
    private var newsTask: Task<Void, Never>?
    /// macOS will ask, once, before codesign may use the signing key (readyToSign): the card says why.
    @Published var allowingSigning = false
    /// Continue on that card: macOS is asking now.
    @Published var signingAsked = false
    /// The card's why was read once: Try Again goes straight to macOS's window.
    private var signingExplained = false
    /// The headset opens a free account's apps only once its developer is trusted there: the last page says how.
    @Published var trustFirst = false
    /// Whether the game had run on the headset before a free account's install (nil: not looked, or no answer).
    @Published var ranBefore: Bool?
    /// When the player asked to install (Install, Update, Try Again, or a way on from the install's error card).
    /// The install starts by itself once a check begun since then finds every step done.
    @Published var installAsked: Date?
    /// When the latest finished check began.
    @Published private(set) var checkedAt = Date.distantPast
    /// When the latest check began, and how many are running.
    private var checkStarted = Date.distantPast
    private var checksRunning = 0
    /// Since when, and over how many checks in a row, the headset's checks have failed.
    private var headsetFailing: (since: Date, checks: Int)?

    private(set) var recipe: Recipe
    let steam: SteamAccount
    let guide = XcodeGuide()
    private(set) var engine: Engine?
    private var developerDir: String?
    /// When the last of the Mac's own checks started.
    private(set) var preflightStarted = Date.distantPast

    private var monitorTask: Task<Void, Never>?
    private var installTask: Task<Void, Never>?
    private var downloadTask: Task<Void, Never>?
    private var installRun: EngineRun?
    private var activity: NSObjectProtocol?
    private var lastError: EngineError?
    private var triedAutoFix: Set<String> = []
    /// The check after installAsked took the player to a step that isn't done: only once.
    private var askRouted = false
    /// Apple's website is open for its new developer agreement: back in front, the install starts again.
    private var agreementOpened = false
    private var activeObserver: NSObjectProtocol?
    /// Whether the page on screen was already done when the player got there: only a step that
    /// gets done while the player watches moves on by itself.
    private var arrivedDone = false
    private var advanceTask: Task<Void, Never>?

    init(engine: Engine? = Engine.locate(), demo: Bool = false) {
        self.engine = engine
        var r = Recipe.alyx
        if let package = engine?.source?.appendingPathComponent("packages/\(r.id)/package.json"),
           let data = try? Data(contentsOf: package) {
            r = r.updated(fromPackage: data)
        }
        recipe = r
        steam = SteamAccount(recipe: r)
        setup.chosenTeam = UserDefaults.standard.string(forKey: "team")
        steam.onChange = { [weak self] in self?.steamChanged() }
        steam.demo = demo
        isDemo = demo
        guide.model = self
        guard !demo else { return }
        activeObserver = NotificationCenter.default.addObserver(forName: NSApplication.didBecomeActiveNotification, object: nil,
                                                                queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.becameActive() }
        }
        if engine == nil {
            engineMissing = true
        } else if let s = Self.savedStatus() {
            status = s
            mode = .home
        }
    }

    /// The Glovebox release this app builds; a newer one than what's installed means Update.
    static var release: String { Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "0" }

    // MARK: Moving between steps

    func getStarted() {
        started = true
        startMonitor()
        go(firstOpenStep() ?? .xcode)
    }

    /// The sidebar, or a button, picked a step.
    func select(_ s: Step) {
        if s != .welcome, !started, mode == .setup {
            started = true
            startMonitor()
        }
        // From the install's error to a step: once that's sorted out, the install starts again by itself.
        if installError != nil, s != .install, s != .welcome { return recover(at: s) }
        go(s)
    }

    /// Continue: the next step after this one that isn't done.
    func continueOn() {
        let all = Step.allCases
        let i = all.firstIndex(of: step) ?? 0
        let next = all[(i + 1)...].first { state(of: $0) != .done } ?? .install
        go(next)
    }

    /// The first step that isn't done, in order, or nil when setup is through.
    func firstOpenStep() -> Step? {
        Step.allCases.dropFirst().first { state(of: $0) != .done }
    }

    func go(_ s: Step) {
        advanceTask?.cancel()
        arrivedDone = state(of: s) == .done
        withAnimation(.easeInOut(duration: 0.25)) { step = s }
        if s == .install, let asked = installAsked {
            if checkedAt >= asked { return installIfReady() }
            // A check begun since the ask, unless one already has (the checks every few seconds).
            Task {
                guard checkStarted < asked else { return }
                await checkAll()
                stateChanged()
            }
        }
    }

    /// Called whenever a check, Steam or the install changes something: a step that just got
    /// done while on screen moves on to the next open one after a beat.
    private func stateChanged() {
        guard started, mode == .setup, step != .welcome, step != .install else { return }
        let done = state(of: step) == .done
        if !done { arrivedDone = false }
        guard done, !arrivedDone, advanceTask == nil else { return }
        let from = step
        advanceTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 1_200_000_000)
            guard let self, !Task.isCancelled else { return }
            self.advanceTask = nil
            guard self.step == from, self.state(of: from) == .done else { return }
            self.go(self.firstOpenStep() ?? .install)
        }
    }

    // MARK: Checks

    func startMonitor() {
        // A made-up state stays as it is: clicking around it must not run the real checks.
        guard monitorTask == nil, !isDemo else { return }
        monitorTask = Task { await monitor() }
        // A returning player's saved Steam session: check it still owns the game, quietly.
        if steam.hasSession, steam.step == .form { Task { _ = await steam.verifySaved() } }
    }

    /// Checks everything every few seconds while setup isn't through, then now and then.
    private func monitor() async {
        while !Task.isCancelled {
            // One check at a time: a recheck the player started counts for this round.
            if !installing, checksRunning == 0 { await checkAll() }
            runAutomaticFixes()
            stateChanged()
            let through = [Step.xcode, .account, .headset].allSatisfy { state(of: $0) == .done }
            try? await Task.sleep(nanoseconds: through ? 12_000_000_000 : 2_500_000_000)
        }
    }

    /// Check again right away (after the player did something).
    func recheck() {
        guard !isDemo else { return }
        Task {
            await checkAll()
            stateChanged()
        }
    }

    /// The Mac's own checks, then the engine's `doctor` once Xcode can run it.
    private func checkAll() async {
        let started = Date()
        checkStarted = started
        checksRunning += 1
        defer {
            checksRunning -= 1
            if started > checkedAt { checkedAt = started }
            installIfReady()
        }
        let pre = await MacTools.preflight()
        preflightStarted = started
        let xcodeBefore = setup.results[.xcode]
        developerDir = pre.developerDir
        if xcodeMajor != pre.xcodeMajor || (xcodeIcon == nil && pre.xcodeMajor != nil) {
            xcodeMajor = pre.xcodeMajor
            xcodeIcon = MacTools.xcodeIcon
        }
        if let d = pre.developerDir {
            engine?.environment["DEVELOPER_DIR"] = d
            // Xcode's first launch offers about 30 GB of platforms the game doesn't need.
            MacTools.trimXcodeFirstLaunch()
        }
        withAnimation(.spring(duration: 0.35)) { for r in pre.checks { setup.record(r) } }
        xcodeArrived(before: xcodeBefore, now: setup.results[.xcode])
        guard pre.engineCanRun, let engine, let run = try? engine.run(["doctor"], recipe: recipe.id) else {
            if engine == nil { failEngine() }
            // Without a working Xcode the later checks can't run: they're unknown until it does.
            let seen = Set(pre.checks.map(\.id))
            for id in CheckID.allCases where !seen.contains(id) { setup.results[id] = nil }
            checkedOnce = true
            return
        }
        var fresh: [CheckResult] = []
        for await e in run.events {
            switch e {
            case .check(var r):
                if r.id == .team, r.passes {
                    let teams = await MacTools.teams()
                    r.data = .object(["teams": .array(teams.map {
                        .object(["id": .string($0.id), "name": .string($0.name), "paid": .bool($0.paid)])
                    })])
                }
                fresh.append(r)
            case .error(let err):
                lastError = err
            default:
                break
            }
        }
        // The engine can't tell a nearby headset that isn't paired yet from one with Developer
        // Mode off; devicectl can.
        if let i = fresh.firstIndex(where: { $0.id == .headset && $0.code == nil && $0.status == .ok }),
           fresh.contains(where: { $0.id == .developerMode && $0.code == "developer-mode-off" }),
           let h = await MacTools.nearbyHeadset(developerDir: developerDir), !h.paired {
            fresh[i] = CheckResult(id: .headset, status: .fail, code: "headset-not-paired", value: h.name,
                                   fix: Fix(kind: .guide, label: "Pair your Vision Pro", guide: "pair-headset"))
            fresh.removeAll { $0.id == .developerMode }
        }
        // A headset waking up says Developer Mode is off (or isn't paired) for a while, though nothing changed: a
        // failing headset check counts once it has failed for 10 s, three checks in a row. Until then it's unknown,
        // still being checked (and the checks come every few seconds).
        let headsetIDs: Set<CheckID> = [.headset, .developerMode]
        if fresh.contains(where: { headsetIDs.contains($0.id) && !$0.passes }) {
            headsetFailing = (headsetFailing?.since ?? started, (headsetFailing?.checks ?? 0) + 1)
        } else if fresh.contains(where: { $0.id == .headset }) {
            headsetFailing = nil
        }
        if headsetSettling { fresh.removeAll { headsetIDs.contains($0.id) } }
        #if DEBUG
        if Tour.teamNotLoaded, let i = fresh.firstIndex(where: { $0.id == .team }) {
            fresh[i] = CheckResult(id: .team, status: .fail, code: "apple-team-not-loaded",
                                   fix: Fix(kind: .openApp, label: "Load your team in Xcode's Accounts settings", url: "xcode://"))
        }
        #endif
        withAnimation(.spring(duration: 0.35)) {
            for r in fresh { setup.record(r) }
            // A check the engine didn't get to (it stops after a missing Xcode) is unknown again.
            let seen = Set(fresh.map(\.id) + pre.checks.map(\.id))
            for id in CheckID.allCases where !seen.contains(id) { setup.results[id] = nil }
        }
        checkedOnce = true
    }

    /// Xcode in the App Store, then Xcode's own setup: when either is through, the player is
    /// probably still looking at that app, so Glovebox comes back.
    private func xcodeArrived(before: CheckResult?, now: CheckResult?) {
        let missing: Set<String> = ["xcode-missing", "xcode-too-old"]
        if now?.value == "downloading" { xcodeRequested = true }
        // The guides come back by themselves when they finish.
        if let b = before?.code, missing.contains(b), let now, !missing.contains(now.code ?? ""), !guide.isRunning {
            comeBack(from: ["com.apple.AppStore"])
        }
        if xcodeOpenedForSetup, before?.code == "xcode-first-launch", now?.passes == true {
            xcodeOpenedForSetup = false
            if !guide.isRunning { comeBack(from: ["com.apple.dt.Xcode"]) }
        }
    }

    /// Something done in another app: Glovebox comes to the front if the player is still in that
    /// app (or in Glovebox), else its Dock icon bounces, so it never takes the screen from
    /// something else.
    func comeBack(from apps: Set<String>) {
        if NSApp.isActive || Self.inFront(apps) {
            bringToFront(over: apps)
        } else {
            NSApp.requestUserAttention(.informationalRequest)
        }
    }

    private static func inFront(_ apps: Set<String>) -> Bool {
        let front = NSWorkspace.shared.frontmostApplication
        return apps.contains(front?.bundleIdentifier ?? "") || apps.contains(front?.localizedName ?? "")
    }

    /// Since macOS 14 an app in the back comes forward with `activate()` only when the app in front
    /// lets it, and Xcode and the App Store don't. Then the older call, then Launch Services, the way
    /// a click on the Dock icon does it. Twice, if the player is still in that app a moment later:
    /// one that opens a window just then (Xcode's welcome window after its first pages) takes the
    /// front back.
    private func bringToFront(over apps: Set<String>) {
        Task {
            for _ in 0..<2 {
                NSApp.activate()
                NSApp.windows.first { $0.canBecomeMain }?.makeKeyAndOrderFront(nil)
                try? await Task.sleep(nanoseconds: 300_000_000)
                if !NSApp.isActive {
                    NSRunningApplication.current.activate(options: [.activateAllWindows, .activateIgnoringOtherApps])
                    try? await Task.sleep(nanoseconds: 300_000_000)
                }
                if !NSApp.isActive {
                    let config = NSWorkspace.OpenConfiguration()
                    config.activates = true
                    _ = try? await NSWorkspace.shared.openApplication(at: Bundle.main.bundleURL, configuration: config)
                }
                try? await Task.sleep(nanoseconds: 1_500_000_000)
                guard !NSApp.isActive, Self.inFront(apps) else { return }
            }
        }
    }

    /// What the app can do without asking, it does, once: today that's adding visionOS to Xcode.
    private func runAutomaticFixes() {
        guard autoFix == nil else { return }
        for r in setup.results.values where r.status == .fail && r.fix?.kind == .command {
            let code = r.code ?? r.id.rawValue
            guard !triedAutoFix.contains(code) else { continue }
            triedAutoFix.insert(code)
            Task { await runFix(r) }
            return
        }
    }

    /// Does what a check's fix says: opens a page or an app, or runs a command with its progress.
    func runFix(_ r: CheckResult) async {
        guard let fix = r.fix else { return }
        let code = r.code ?? ""
        switch fix.kind {
        case .openURL:
            if let u = fix.url.flatMap(URL.init(string:)) { NSWorkspace.shared.open(u) }
        case .openApp:
            if r.id == .xcode {
                // Xcode's license, its components and its first pages, with the guide; the checks
                // every few seconds see when they're done.
                xcodeOpenedForSetup = true
                guide.start(.setup)
            } else {
                MacTools.openXcode()
            }
        case .guide:
            break
        case .command:
            guard let command = fix.command else { return }
            autoFix = AutoFix(code: code)
            let started = Date()
            let exit = await MacTools.stream(command, developerDir: developerDir, environment: engine?.environment ?? [:]) { line in
                guard let f = MacTools.percent(in: line) else { return }
                Task { @MainActor [weak self] in
                    guard let self, self.autoFix?.code == code else { return }
                    let elapsed = Date().timeIntervalSince(started)
                    withAnimation { self.autoFix?.fraction = f }
                    self.autoFix?.secondsLeft = f > 0.03 && elapsed > 5 ? elapsed * (1 - f) / f : nil
                }
            }
            if exit != 0 {
                lastError = EngineError(code: code, message: "\(command) exited \(exit)")
                // Let it try again on the next round, unless it keeps failing.
                if !triedAutoFix.contains(code + "#2") { triedAutoFix.remove(code); triedAutoFix.insert(code + "#2") }
            }
        }
        autoFix = nil
        recheck()
    }

    func fix(_ r: CheckResult) { Task { await runFix(r) } }

    /// Get Xcode: the App Store's page with the guide on it, then watch for Xcode to arrive.
    func getXcode() {
        guide.start(.appStore)
        withAnimation { xcodeRequested = true }
    }

    func chooseTeam(_ id: String) {
        setup.chosenTeam = id
        UserDefaults.standard.set(id, forKey: "team")
        stateChanged()
    }

    func openPairing() { guide.start(.pair) }

    // MARK: Steam

    private func steamChanged() {
        objectWillChange.send()
        if steam.step == .owned { startDownload() }
        installIfReady()
        stateChanged()
    }

    func useAnotherSteamAccount() {
        downloadTask?.cancel()
        downloadTask = nil
        download = Download()
        steam.signOut()
    }

    /// The Mac's share of the game, from Steam, in the background. Run again it fetches only what's
    /// missing, so a finished download costs a few seconds at install time.
    func startDownload() {
        guard downloadTask == nil, !download.done, let engine else { return }
        guard let creds = SteamKeychain.load() else { return }
        let dl = GameDownload(recipe: recipe, root: engine.work(for: recipe.id).appendingPathComponent("linux"))
        download = Download(running: true)
        let simulated = engine.source == nil
        downloadTask = Task { [weak self] in
            let report: @Sendable (Double, Double?) -> Void = { f, left in
                Task { @MainActor [weak self] in
                    guard let self else { return }
                    self.download.fraction = f
                    self.download.secondsLeft = left
                    if self.installing, self.progress.stage == .download {
                        withAnimation(.easeOut(duration: 0.3)) { self.progress.download(fraction: f, secondsLeft: left) }
                        self.badge()
                    }
                }
            }
            do {
                if simulated { try await dl.simulate(progress: report) } else { try await dl.run(creds, progress: report) }
                self?.download = Download(fraction: 1, done: true)
            } catch let e as EngineError {
                self?.download = Download(error: e)
            } catch {
                self?.download = Download()
            }
            self?.downloadTask = nil
        }
    }

    // MARK: Install

    /// Install, Update, Renew, Reinstall, Try Again: every step is checked again first, and the install starts by
    /// itself once they're all done; a step that isn't comes up instead. (Checks from before Xcode was signed out
    /// once let an Update through to a build that couldn't sign.)
    func requestInstall() {
        guard !installing else { return }
        started = true
        withAnimation {
            installError = nil
            result = nil
            installAsked = Date()
            mode = .setup
        }
        askRouted = false
        go(.install)
        startMonitor()
    }

    /// A way on from the install's error card into a step (Check Account, or the step bar): the error goes, and
    /// once that step and the rest are done the install starts again by itself.
    func recover(at s: Step) {
        withAnimation { installError = nil }
        installAsked = Date()
        askRouted = true
        go(s)
        recheck()
    }

    /// The install the player asked for starts once a check begun since then finds every step done (Steam still
    /// checking a saved session, or a headset still waking up, gets a moment more). Until then the install page says it's getting ready; a step
    /// that check finds not done comes up, once.
    private func installIfReady() {
        guard let asked = installAsked, mode == .setup, step == .install, !installing, result == nil,
              installError == nil, checkedAt >= asked else { return }
        let steps: [Step] = [.xcode, .steam, .account, .headset]
        if steps.allSatisfy({ state(of: $0) == .done }) { return startInstall() }
        guard !askRouted, let open = steps.first(where: {
            state(of: $0) != .done && !($0 == .steam && steam.step == .checkingOwnership) && !($0 == .headset && headsetSettling)
        }) else { return }
        askRouted = true
        go(open)
    }

    /// The headset's checks failing for under 10 s (or three checks): maybe a headset waking up, so not shown yet.
    var headsetSettling: Bool {
        headsetFailing.map { $0.checks < 3 || Date().timeIntervalSince($0.since) < 10 } ?? false
    }

    /// Asked to install, and waiting for the checks (or Steam) to say whether it can.
    var gettingReady: Bool {
        guard let asked = installAsked, !installing, installError == nil, result == nil else { return false }
        return !askRouted || checkedAt < asked
    }

    /// Starts the install now, as things stand; requestInstall checks them first.
    func startInstall(from start: Stage = .download) {
        guard let engine else { return failEngine() }
        guard SteamKeychain.load() != nil else {
            installAsked = Date()
            askRouted = true
            return go(.steam)
        }
        installAsked = nil
        installError = nil
        waitingForHeadset = false
        result = nil
        if start == .download { progress = InstallProgress() }
        installing = true
        go(.install)
        keepAwake(true)
        askToShareSoon()
        askForNewsSoon()
        installTask = Task { await install(engine, from: start) }
    }

    private enum Outcome { case ok(String?), failed(EngineError), cancelled }

    /// The download (the app's own, from Steam), then the engine's `build` and `install`.
    private func install(_ engine: Engine, from start: Stage) async {
        defer {
            keepAwake(false)
            NSApp.dockTile.badgeLabel = nil
            installing = false
            installRun = nil
        }
        let work = engine.work(for: recipe.id)
        if start == .download {
            progress.begin(.download)
            progress.download(fraction: download.fraction, secondsLeft: download.secondsLeft)
            if download.error != nil { download = Download() }
            startDownload()
            while let t = downloadTask {
                await t.value
                if Task.isCancelled { return go(.install) }
            }
            if let e = download.error { return await failed(e) }
            guard download.done else { return }  // cancelled
            progress.finish(.download)
        }

        var built: String?
        var needsBuild = start != .install
        // The stutter-report answer goes into the build, so the build waits for it: asked now if
        // it isn't up yet (game files already here), while a download still running carries on.
        if needsBuild, shareStutters == nil {
            withAnimation(.spring(duration: 0.45)) { askingToShare = true }
            while shareStutters == nil, askingToShare, !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 200_000_000)
            }
            if Task.isCancelled { return }
        }
        if needsBuild, let team = setup.team?.id, !(await readyToSign(team, logs: engine.logs)) { return }
        // The headset asleep, locked or out of range: wait for it, then go on from the same step.
        while true {
            if needsBuild {
                var env: [String: String] = [:]
                if let t = setup.team?.id { env["GLOVEBOX_TEAM"] = t }
                if let share = shareStutters {
                    env["GLOVEBOX_SHARE_STUTTERS"] = share ? "1" : "0"
                    // When the player answered, not when this build ran: an answer given on the headset
                    // since then must still win.
                    let at = UserDefaults.standard.double(forKey: "shareStuttersAt")
                    if at > 0 { env["GLOVEBOX_SHARE_STUTTERS_AT"] = String(Int(at)) }
                }
                switch await follow(try? engine.run(["build"], recipe: recipe.id, environment: env)) {
                case .ok(let app): built = app; needsBuild = false; progress.finish(.convert); progress.finish(.build)
                case .failed(let e) where e.code == "headset-unreachable":
                    lastError = e
                    if await waitForHeadset() { continue } else { return }
                case .failed(let e): return await failed(e)
                case .cancelled: return
                }
            }
            switch await follow(try? engine.run(["install"], recipe: recipe.id)) {
            case .ok: progress.finish(.install)
            case .failed(let e) where e.code == "headset-unreachable":
                lastError = e
                if await waitForHeadset() { continue } else { return }
            case .failed(let e): return await failed(e)
            case .cancelled: return
            }
            break
        }

        let app = URL(fileURLWithPath: built ?? work.appendingPathComponent("KleptonAlyx.app").path)
        let expires = (try? Data(contentsOf: app.appendingPathComponent("embedded.mobileprovision"))).flatMap(XcodeInfo.profileExpiry)
        let ran = await ranOnHeadset(app)
        if Task.isCancelled { return }
        finished(InstallResult(app: app.path, device: setup.headsetName, expires: expires), ranBefore: ran)
    }

    /// Whether the game has run on the headset. visionOS starts a free account's apps only once their developer is
    /// trusted there, and at every launch the game writes Documents/klepton-memprobe.txt in its data container, which
    /// an update keeps. Nothing is started to find out. Nil: a paid team's game (no trust needed), or no answer.
    private func ranOnHeadset(_ app: URL) async -> Bool? {
        guard setup.team?.paid != true else { return nil }
        #if DEBUG
        if let pretend = Tour.ran { return pretend }
        #endif
        guard let id = NSDictionary(contentsOf: app.appendingPathComponent("Info.plist"))?["CFBundleIdentifier"] as? String
        else { return nil }
        return await MacTools.headsetHasFile(app: id, path: "Documents/klepton-memprobe.txt", developerDir: developerDir)
    }

    /// A signing key the App Store Xcode app made makes macOS ask before codesign may use it: once, for the
    /// Mac's password and Always Allow. The build signs ~94 files at once and each of them would ask, so that
    /// one question comes first, alone, and the build starts only once codesign may use the key. A key
    /// xcodebuild made, or one already allowed, asks nothing and shows nothing here (SigningKeyAccess).
    private func readyToSign(_ team: String, logs: URL) async -> Bool {
        var key = SigningKey.check(team: team)
        // Apple's intermediate certificate isn't on this Mac: codesign can't use the key at all and macOS never asks
        // (glovebox-vr/glovebox#4). Put it there first; only if that fails is there something to tell the player.
        if let identity = key.identity, let generation = SigningKey.missingIssuer(identity: identity) {
            let added = await SigningKey.addIssuer(generation, identity: identity)
            let log = Self.signingLog(in: logs, lines: ["certificate \(identity) has no chain: Apple's \(generation) intermediate is missing",
                                                        added.note, SigningKey.describe(identity: identity)])
            guard added.ok else {
                await failed(EngineError(code: "signing-certificate-missing", stage: .build, message: added.note, log: log?.path))
                return false
            }
            key = SigningKey.check(team: team)
        }
        guard key.access == .willAsk, let identity = key.identity else { return true }
        signingAsked = signingExplained
        appear { allowingSigning = true }
        // The install waits for Continue: a player in another app meanwhile sees the Dock icon bounce until they're back.
        if !signingAsked, !NSApp.isActive { NSApp.requestUserAttention(.criticalRequest) }
        // macOS asks once the player has read why and clicked Continue.
        while !signingAsked, !Task.isCancelled { try? await Task.sleep(nanoseconds: 100_000_000) }
        if !Task.isCancelled { signingExplained = true }
        // The card shows the click first; then the guide points at Always Allow in macOS's window.
        try? await Task.sleep(nanoseconds: 300_000_000)
        if !Task.isCancelled { guide.start(.keychain) }
        let signed = Task.isCancelled ? (status: Int32(1), output: "") : await SigningKey.signOnce(with: identity)
        let allowed = !Task.isCancelled && SigningKey.check(team: team).access != .willAsk
        guide.keychainAnswered(allowed: allowed)
        withAnimation { allowingSigning = false; signingAsked = false }
        if Task.isCancelled { return false }
        if allowed { return true }
        // Allow (not Always Allow) signs the one file and leaves the key as it was. Anything else, the player
        // said no or codesign couldn't use the key at all (then macOS never asked): its own words go in a log
        // that Copy Diagnostics shows.
        let log = Self.signingLog(in: logs, identity: identity, signed: signed)
        await failed(EngineError(code: signed.status == 0 ? "signing-allowed-once" : "signing-not-allowed", stage: .build,
                                 message: signed.status == 0 ? nil : signed.output.split(separator: "\n").last.map(String.init),
                                 log: log?.path))
        return false
    }

    /// What the one signature tried and what codesign said, for Copy Diagnostics.
    private static func signingLog(in folder: URL, identity: String, signed: (status: Int32, output: String)) -> URL? {
        signingLog(in: folder, lines: ["$ /usr/bin/codesign --force --sign \(identity) --timestamp=none <scratch file>",
                                       signed.output, "exit \(signed.status)", SigningKey.describe(identity: identity)])
    }

    private static func signingLog(in folder: URL, lines: [String]) -> URL? {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "yyyyMMdd-HHmmss"
        let url = folder.appendingPathComponent("signing-\(f.string(from: Date())).log")
        do {
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
            try (lines + [""]).joined(separator: "\n").write(to: url, atomically: true, encoding: .utf8)
        } catch { return nil }
        return url
    }

    private func follow(_ run: EngineRun?) async -> Outcome {
        guard let run else { return .failed(EngineError(code: "internal", message: "The engine couldn't start")) }
        installRun = run
        var outcome: Outcome = .failed(EngineError(code: "internal", message: "The engine stopped without a result", log: run.log.path))
        for await e in run.events {
            withAnimation(.easeOut(duration: 0.3)) { progress.apply(e) }
            badge()
            switch e {
            case .error(let err): outcome = .failed(err)
            case .finished(_, true, let app): outcome = .ok(app)
            case .cancelled: outcome = .cancelled
            default: break
            }
        }
        return outcome
    }

    private func failed(_ err: EngineError) async {
        lastError = err
        switch err.code {
        case "steam-session-expired":
            steam.sessionExpired()
            installAsked = Date()
            askRouted = true
            go(.steam)
        default:
            appear { installError = err }
        }
    }

    /// A card that comes up while the player may be in another app (the install takes a while): animated in front,
    /// plain in the back. Behind another app SwiftUI sometimes never drew an animated change, even once Glovebox was
    /// in front again (the signing card stayed on the download in 6 of 15 tries on a cloud Mac, 0 of 5 without the
    /// animation: research/redraw-probe).
    private func appear(_ animation: Animation? = .default, _ change: () -> Void) {
        if NSApp.isActive { withAnimation(animation, change) } else { change() }
    }

    private func badge() { NSApp.dockTile.badgeLabel = "\(Int(progress.overall * 100))%" }

    /// The headset went to sleep, got locked or went out of range: true once the doctor finds it
    /// again, false if the install was cancelled meanwhile. The install goes on in the same task.
    private func waitForHeadset() async -> Bool {
        appear { waitingForHeadset = true }
        while waitingForHeadset, !Task.isCancelled {
            try? await Task.sleep(nanoseconds: 3_000_000_000)
            guard waitingForHeadset, !Task.isCancelled, let engine, let run = try? engine.run(["doctor"], recipe: recipe.id) else { break }
            var ready = false
            for await case .check(let r) in run.events where r.id == .headset { ready = r.status == .ok }
            if ready, waitingForHeadset {
                appear { waitingForHeadset = false }
                return true
            }
        }
        waitingForHeadset = false
        return false
    }

    /// Continue on the signing card.
    func continueSigning() {
        if allowingSigning { signingAsked = true }
    }

    func cancelInstall() {
        installAsked = nil
        waitingForHeadset = false
        if installing, progress.stage == .download { downloadTask?.cancel() }
        installTask?.cancel()
        installRun?.cancel()
    }

    func retryInstall() { requestInstall() }

    /// Xcode's saved sign-in for the account expired (the install's error card): the guide to sign in to Xcode
    /// again. Once Apple's sign-in has come and gone, the install starts again by itself
    /// (signedInAgain). The checks can't tell: Xcode's account and team lists said signed in all along.
    func signInAgain() { guide.start(.signInAgain) }

    func signedInAgain() { requestInstall() }

    /// Apple's new developer agreement (the install's error card): its website, where the account accepts it. Once
    /// Glovebox is back in front, the install starts again by itself.
    func acceptAgreement() {
        agreementOpened = true
        NSWorkspace.shared.open(URL(string: "https://developer.apple.com/account")!)
    }

    /// Glovebox came back to the front: what changed while it was in the back draws now (SwiftUI held an animated
    /// change there, see appear), and from Apple's website the install starts again.
    func becameActive() {
        objectWillChange.send()
        guard agreementOpened else { return }
        agreementOpened = false
        if installError?.code == "apple-agreement-pending" { requestInstall() }
    }

    private func finished(_ r: InstallResult, ranBefore ran: Bool?) {
        let team = setup.team
        ranBefore = ran
        // A free account's last page says how to trust its developer on the headset, unless the game has run there
        // (it's trusted). With no answer from the headset, only the team's first install here says how.
        let shown = team.map { "trustShown.\($0.id)" }
        trustFirst = team?.paid == false
            && (ran == false || (ran == nil && !(shown.map(UserDefaults.standard.bool(forKey:)) ?? false)))
        if trustFirst, let shown { UserDefaults.standard.set(true, forKey: shown) }
        appear(.spring(duration: 0.5)) {
            result = r
            // The news sign-up stays on the finished page until it has an answer, or comes up there.
            if !newsAnswered, !isDemo { askingForNews = true }
        }
        let s = HeadsetStatus(installed: true, version: Self.release, latest: Self.release, expires: r.expires,
                              freeAccount: team.map { !$0.paid })
        status = s
        if let data = try? JSONEncoder().encode(s) { UserDefaults.standard.set(data, forKey: "installed") }
        NSApp.requestUserAttention(.informationalRequest)
    }

    private static func savedStatus() -> HeadsetStatus? {
        guard let data = UserDefaults.standard.data(forKey: "installed"),
              var s = try? JSONDecoder().decode(HeadsetStatus.self, from: data) else { return nil }
        s.latest = release
        return s
    }

    private func keepAwake(_ on: Bool) {
        // Asleep, the Mac can't build or reach the headset; locked, Xcode can lose the Apple account mid-signing.
        if on, activity == nil {
            activity = ProcessInfo.processInfo.beginActivity(options: [.userInitiated, .idleSystemSleepDisabled, .idleDisplaySleepDisabled],
                                                             reason: "Installing on Apple Vision Pro")
        } else if !on, let a = activity {
            ProcessInfo.processInfo.endActivity(a)
            activity = nil
        }
    }

    // MARK: Home

    /// The install finished and the player has seen it: from now on the app opens on the game.
    func doneTapped() {
        withAnimation {
            mode = .home
            step = .welcome
            askingForNews = false
        }
    }

    /// Update or renew: the same path as the first install, which skips whatever is already done.
    func update() { requestInstall() }

    // MARK: Stutter reports

    /// The player's answer, kept for every later build. The build bakes it into the headset app
    /// (`GLOVEBOX_SHARE_STUTTERS`) and waits for it; the headset asks only when a build had none.
    func setShareStutters(_ on: Bool) {
        withAnimation(.spring(duration: 0.4)) {
            shareStutters = on
            askingToShare = false
        }
        UserDefaults.standard.set(on, forKey: "shareStutters")
        UserDefaults.standard.set(Date().timeIntervalSince1970, forKey: "shareStuttersAt")
    }

    /// The stutter-report question, a few seconds into the install: once the player has seen it
    /// start, and while they wait anyway. Only until it has an answer.
    private func askToShareSoon() {
        guard shareStutters == nil else { return }
        Task {
            try? await Task.sleep(nanoseconds: 3_000_000_000)
            guard installing, shareStutters == nil else { return }
            withAnimation(.spring(duration: 0.45)) { askingToShare = true }
        }
    }

    // MARK: News by email

    /// The news sign-up, once: in the install's quiet stretch (the download, the conversion, the build), after 20
    /// seconds in which nothing has asked anything. It stays up through the finished page, until it has an answer.
    private func askForNewsSoon() {
        guard !newsAnswered, !isDemo else { return }
        newsTask?.cancel()
        newsTask = Task {
            var calm = 0
            while installing, !askingForNews, !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 1_000_000_000)
                let quiet = shareStutters != nil && !askingToShare && !allowingSigning && !waitingForHeadset && installError == nil
                    && progress.stage.map { [Stage.download, .convert, .build].contains($0) } == true
                calm = quiet ? calm + 1 : 0
                if calm >= 20, installing { appear(.spring(duration: 0.45)) { askingForNews = true } }
            }
        }
    }

    /// The card steps aside for anything that needs the player, and comes back after.
    var showsNewsAsk: Bool {
        askingForNews && !askingToShare && !allowingSigning && !waitingForHeadset && installError == nil
    }

    /// Home's link.
    func askForNews() {
        guard Self.upstreamServices else { return }
        withAnimation(.spring(duration: 0.45)) { askingForNews = true }
    }

    /// No Thanks, or a sign-up (its card says so for a moment first).
    func answerNews(signedUp: Bool) {
        newsAnswered = true
        UserDefaults.standard.set(true, forKey: "newsAnswered")
        if signedUp {
            newsSignedUp = true
            UserDefaults.standard.set(true, forKey: "newsSignedUp")
        }
        withAnimation(.spring(duration: 0.4)) { askingForNews = false }
    }

    // MARK: Diagnostics

    func copyDiagnostics() {
        let info = Bundle.main.infoDictionary
        var app: [(String, String)] = [
            (AppName.shown, "\(info?["CFBundleShortVersionString"] as? String ?? "?") (\(info?["CFBundleVersion"] as? String ?? "?"))"),
            ("macOS", ProcessInfo.processInfo.operatingSystemVersionString),
            ("Mac", Self.macModel()),
            ("Step", "\(mode == .home ? "home" : step.rawValue)"),
        ]
        if let e = engine { app.append(("Engine", e.source?.path ?? e.executable.path)) }
        if let d = developerDir { app.append(("Xcode", d)) }
        let known = [steam.accountName] + (setup.results[.team]?.teams.map(\.name) ?? []) + [setup.headsetName].compactMap { $0 }
        let error = installError ?? download.error ?? lastError
        let text = DiagnosticsReport.compose(app: app, checks: Array(setup.results.values), error: error,
                                             logTail: engine.map { DiagnosticsReport.logTail(for: error, in: $0.logs) } ?? [],
                                             redactor: Redactor(known: known.filter { !$0.isEmpty && $0 != "Apple Vision Pro" }))
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
        show(toast: "Diagnostics copied")
    }

    func show(toast text: String) {
        withAnimation(.spring(duration: 0.3)) { toast = text }
        Task {
            try? await Task.sleep(nanoseconds: 2_200_000_000)
            withAnimation(.easeOut(duration: 0.3)) { if toast == text { toast = nil } }
        }
    }

    private static func macModel() -> String {
        var size = 0
        sysctlbyname("hw.model", nil, &size, nil, 0)
        var buf = [CChar](repeating: 0, count: max(1, size))
        sysctlbyname("hw.model", &buf, &size, nil, 0)
        return String(cString: buf)
    }

    private func failEngine() {
        engineMissing = engine == nil
        lastError = EngineError(code: "internal", message: "The engine couldn't start")
    }
}
