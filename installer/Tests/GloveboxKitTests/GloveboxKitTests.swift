import XCTest
@testable import GloveboxKit

final class ContractTests: XCTestCase {
    // Lines exactly as engine/glovebox.py prints them (json.dumps: spaces after separators).
    func testReadsTheEnginesChecks() {
        XCTAssertEqual(EngineEvent.decode(#"{"event": "check", "check": "mac", "title": "This Mac", "status": "ok", "detail": "Apple silicon, macOS 26.0", "fix": null}"#),
                       [.check(CheckResult(id: .mac, status: .ok, value: "macOS 26.0", detail: "Apple silicon, macOS 26.0"))])
        let xcode = EngineEvent.decode(#"{"event": "check", "check": "xcode", "title": "Xcode", "status": "missing", "detail": "Xcode is not installed", "fix": {"kind": "open-url", "label": "Get Xcode", "url": "macappstore://apps.apple.com/app/xcode/id497799835"}}"#)
        guard case .check(let r) = xcode.first else { return XCTFail("\(xcode)") }
        XCTAssertEqual(r.status, .fail)
        XCTAssertEqual(r.code, "xcode-missing")
        XCTAssertEqual(r.fix, Fix(kind: .openURL, label: "Get Xcode", url: "macappstore://apps.apple.com/app/xcode/id497799835"))

        func code(_ line: String) -> String? {
            guard case .check(let r)? = EngineEvent.decode(line).last else { return nil }
            return r.code
        }
        XCTAssertEqual(code(#"{"event": "check", "check": "xcode", "status": "missing", "detail": "Xcode 15.4 is too old (needs 26 or later)", "fix": {"kind": "open-url", "label": "Update Xcode", "url": "x"}}"#), "xcode-too-old")
        XCTAssertEqual(code(#"{"event": "check", "check": "xcode", "status": "missing", "fix": {"kind": "open-app", "label": "Open Xcode", "url": "xcode://"}}"#), "xcode-first-launch")
        XCTAssertEqual(code(#"{"event": "check", "check": "visionos", "status": "missing", "fix": {"kind": "command", "label": "Install visionOS support", "command": "xcodebuild -downloadPlatform visionOS"}}"#), "visionos-missing")
        XCTAssertEqual(code(#"{"event": "check", "check": "team", "status": "missing", "fix": {"kind": "open-url", "label": "Join the Apple Developer Program", "url": "https://developer.apple.com/programs/enroll/"}}"#), "team-free-only")
        XCTAssertEqual(code(#"{"event": "check", "check": "team", "status": "missing", "fix": {"kind": "open-app", "label": "Open Xcode's Accounts settings", "url": "xcode://"}}"#), "apple-account-missing")
        // Signed in, but Xcode hasn't saved the account's team yet (a tester, 2026-10-10): not a sign-in.
        XCTAssertEqual(code(#"{"event": "check", "check": "team", "status": "missing", "fix": {"kind": "open-app", "label": "Load your team in Xcode's Accounts settings", "url": "xcode://"}}"#), "apple-team-not-loaded")
        XCTAssertEqual(code(#"{"event": "check", "check": "disk", "status": "missing", "detail": "9 GB free, 15 GB needed", "fix": {"kind": "open-url", "label": "Free up space", "url": "x-apple.systempreferences:com.apple.settings.Storage"}}"#), "disk-low")
    }

    func testOneHeadsetCheckIsTwoSteps() {
        let unpaired = EngineEvent.decode(#"{"event": "check", "check": "headset", "title": "Vision Pro", "status": "missing", "detail": "No Vision Pro paired with this Mac", "fix": {"kind": "guide", "label": "Pair your Vision Pro", "guide": "pair-headset"}}"#)
        XCTAssertEqual(unpaired.count, 1)
        guard case .check(let h)? = unpaired.first else { return XCTFail() }
        XCTAssertEqual(h.code, "headset-not-paired")

        let devmode = EngineEvent.decode(#"{"event": "check", "check": "headset", "status": "missing", "detail": "Jane’s Vision Pro: Developer Mode is off", "fix": {"kind": "guide", "label": "Turn on Developer Mode", "guide": "developer-mode"}}"#)
        var s = SetupState()
        for case .check(let r) in devmode { s.record(r) }
        XCTAssertEqual(s.results[.headset]?.status, .ok)
        XCTAssertEqual(s.headsetName, "Jane’s Vision Pro")
        XCTAssertEqual(s.results[.developerMode]?.code, "developer-mode-off")

        let ok = EngineEvent.decode(#"{"event": "check", "check": "headset", "status": "ok", "detail": "Jane’s Vision Pro (visionOS 26.0)", "fix": null}"#)
        for case .check(let r) in ok { s.record(r) }
        XCTAssertEqual(s.headsetName, "Jane’s Vision Pro")
        XCTAssertTrue(s.results[.developerMode]?.passes == true)

        // Asleep: set up, so setup goes on; the install waits for it.
        let asleep = EngineEvent.decode(#"{"event": "check", "check": "headset", "status": "warn", "detail": "Jane’s Vision Pro is paired but not reachable right now (wake it, same Wi-Fi)", "fix": null}"#)
        for case .check(let r) in asleep { s.record(r) }
        XCTAssertTrue(s.results[.headset]?.passes == true)
        XCTAssertEqual(s.results[.headset]?.code, "headset-unreachable")
    }

    func testReadsBuildEvents() {
        XCTAssertEqual(EngineEvent.decode(#"{"event": "progress", "step": "translate", "line": "libtier0.so", "done": 3, "total": 17}"#),
                       [.stepProgress(step: "translate", line: "libtier0.so", done: 3, total: 17)])
        XCTAssertEqual(EngineEvent.decode(#"{"event": "step", "step": "pipelines", "status": "ok", "detail": "56 MB pipeline list"}"#),
                       [.stepDone(step: "pipelines", detail: "56 MB pipeline list")])
        XCTAssertEqual(EngineEvent.decode(#"{"event": "done", "step": "build", "ok": true, "app": "/w/KleptonAlyx.app"}"#),
                       [.finished(step: "build", ok: true, app: "/w/KleptonAlyx.app")])
        func code(_ message: String) -> String? {
            guard case .error(let e)? = EngineEvent.decode(#"{"event": "error", "message": "\#(message)"}"#).first else { return nil }
            return e.code
        }
        XCTAssertEqual(code("no paid Apple Developer Program team in Xcode"), "signing-no-team")
        XCTAssertEqual(code("the app did not build: No profiles for 'com.glovebox.alyx.tx' were found"), "signing-failed")
        // A tester's beta 3 build, Xcode's saved sign-in expired (its signing errors come with it).
        XCTAssertEqual(code("the app did not build: No 'iOS Development' signing certificate matching team ID 'A1B2C3D4E5' with a private key was found. | Unable to log in with account 'jane@example.com'. The login details for account 'jane@example.com' were rejected."),
                       "xcode-signin-expired")
        // The same tester once signed in again: Apple's updated developer agreement not accepted yet.
        XCTAssertEqual(code("the app did not build: No 'iOS Development' signing certificate matching team ID 'A1B2C3D4E5' with a private key was found. | Unable to process request - PLA Update available: You currently don't have access to this membership resource. To resolve this issue, agree to the latest Program License Agreement in your developer account."),
                       "apple-agreement-pending")
        XCTAssertEqual(code("devicectl failed (1): ERROR: The device is locked."), "headset-unreachable")
        XCTAssertEqual(code("no paired Vision Pro with Developer Mode on"), "headset-unreachable")
        XCTAssertEqual(code("xcrun failed (1): Connection with the remote side was unexpectedly closed"), "headset-unreachable")
        XCTAssertEqual(code("game files missing in /w/linux/game: bin/linuxsteamrtarm64"), "game-files-missing")
        XCTAssertEqual(code("bash failed (1): klepton-ld: error"), "internal")
        XCTAssertEqual(Stage(step: "translate"), .convert)
        XCTAssertEqual(Stage(step: "app"), .build)
    }

    func testUnknownInputIsIgnoredNotFatal() {
        XCTAssertEqual(EngineEvent.decode("Building…"), [.unknown("Building…")])
        XCTAssertEqual(EngineEvent.decode(#"{"event": "check", "check": "toaster", "status": "ok"}"#),
                       [.unknown(#"{"event": "check", "check": "toaster", "status": "ok"}"#)])
        XCTAssertEqual(EngineEvent.decode(#"{"event": "step", "step": "app", "status": "ok", "extra": [1, 2]}"#),
                       [.stepDone(step: "app", detail: nil)])
    }

    func testXcodeTeamsAndProfileExpiry() throws {
        let prefs: [String: Any] = ["IDEProvisioningTeamByIdentifier": [
            "account-1": [["teamID": "Z9Y8X7W6V5", "teamName": "Jane Appleseed (Personal Team)", "isFreeProvisioningTeam": true],
                          ["teamID": "A1B2C3D4E5", "teamName": "Jane Appleseed", "isFreeProvisioningTeam": false]]]]
        let data = try PropertyListSerialization.data(fromPropertyList: prefs, format: .xml, options: 0)
        XCTAssertEqual(XcodeInfo.teams(fromPreferences: data),
                       [Team(id: "A1B2C3D4E5", name: "Jane Appleseed", paid: true),
                        Team(id: "Z9Y8X7W6V5", name: "Jane Appleseed (Personal Team)", paid: false)])
        let profile = Data("\u{30}\u{82}junk<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>ExpirationDate</key><date>2027-10-05T00:00:00Z</date></dict></plist>\u{a0}sig".utf8)
        XCTAssertEqual(XcodeInfo.profileExpiry(profile), ISO8601DateFormatter().date(from: "2027-10-05T00:00:00Z"))
        XCTAssertEqual(XcodeInfo.newest([("/Applications/Xcode.app", "26.0.1"), ("/Applications/Xcode-beta.app", "26.1")])?.path,
                       "/Applications/Xcode-beta.app")
    }

    func testRecipeTakesTheEnginesPackage() {
        let package = Data(#"{"id": "alyx", "game": "Half-Life: Alyx", "steam": {"app": 546560, "depots": {"content": 546561, "linux-arm64": 546564}, "mac_download": ["game/bin/linuxsteamrtarm64/", "game/core/shaders_vulkan_"]}}"#.utf8)
        let r = Recipe.alyx.updated(fromPackage: package)
        XCTAssertEqual(r.depots, [546_561, 546_564])
        XCTAssertEqual(r.macDownload, ["game/bin/linuxsteamrtarm64/", "game/core/shaders_vulkan_"])
        XCTAssertEqual(r.headsetAppName, "Half-Life: Alyx")
        let named = Data(#"{"id": "alyx", "game": "Half-Life: Alyx", "app_name": "Glovebox: HLA"}"#.utf8)
        XCTAssertEqual(Recipe.alyx.updated(fromPackage: named).headsetAppName, "Glovebox: HLA")
        XCTAssertEqual(Recipe.alyx.updated(fromPackage: named).title, "Half-Life: Alyx")
    }
}

final class SetupTests: XCTestCase {
    func allOK() -> SetupState {
        var s = SetupState()
        for id in CheckID.allCases { s.record(CheckResult(id: id, status: .ok)) }
        s.record(CheckResult(id: .team, status: .ok, data: .object(["teams": .array([
            .object(["id": .string("A1B2C3D4E5"), "name": .string("Jane"), "paid": .bool(true)])])])))
        return s
    }

    func testFirstFailureInOrderIsTheBlocker() {
        var s = allOK()
        XCTAssertTrue(s.complete)
        s.record(CheckResult(id: .developerMode, status: .fail, code: "developer-mode-off"))
        s.record(CheckResult(id: .xcode, status: .fail, code: "xcode-missing"))
        XCTAssertEqual(s.blocker?.code, "xcode-missing")
        XCTAssertFalse(s.complete)
        XCTAssertEqual(s.state(of: .mac), .ok)
        XCTAssertEqual(s.state(of: .xcode), .blocked)
        XCTAssertEqual(s.state(of: .account), .waiting)
        XCTAssertEqual(s.state(of: .developerMode), .waiting)
        s.record(CheckResult(id: .xcode, status: .ok))
        XCTAssertEqual(s.state(of: .developerMode), .blocked)
    }

    func testSeveralPaidTeamsNeedAChoice() {
        var s = allOK()
        s.record(CheckResult(id: .team, status: .ok, data: .object(["teams": .array([
            .object(["id": .string("A1B2C3D4E5"), "name": .string("Jane"), "paid": .bool(true)]),
            .object(["id": .string("F6G7H8J9K0"), "name": .string("Studio"), "paid": .bool(true)]),
        ])])))
        XCTAssertEqual(s.openChoice?.id, .team)
        XCTAssertNil(s.team)
        XCTAssertFalse(s.complete)
        s.chosenTeam = "F6G7H8J9K0"
        XCTAssertEqual(s.team?.name, "Studio")
        XCTAssertTrue(s.complete)
    }

    func testAFreeTeamSignsOnlyWhenThereIsNoPaidOne() {
        func teams(_ list: [(String, Bool)]) -> SetupState {
            var s = allOK()
            s.record(CheckResult(id: .team, status: .ok, data: .object(["teams": .array(list.map {
                .object(["id": .string($0.0), "name": .string($0.0), "paid": .bool($0.1)])
            })])))
            return s
        }
        // A free account alone: its Personal Team.
        var s = teams([("Z9Y8X7W6V5", false)])
        XCTAssertEqual(s.team?.id, "Z9Y8X7W6V5")
        XCTAssertEqual(s.team?.paid, false)
        XCTAssertTrue(s.complete)
        // A paid team as well: the paid one, as the engine picks, even if the free one was chosen before.
        s = teams([("Z9Y8X7W6V5", false), ("A1B2C3D4E5", true)])
        s.chosenTeam = "Z9Y8X7W6V5"
        XCTAssertEqual(s.team?.id, "A1B2C3D4E5")
        XCTAssertNil(s.openChoice)
        // Two free accounts in Xcode: a choice.
        s = teams([("Z9Y8X7W6V5", false), ("K4M2P8Q6R3", false)])
        XCTAssertNil(s.team)
        XCTAssertEqual(s.openChoice?.id, .team)
        s.chosenTeam = "K4M2P8Q6R3"
        XCTAssertEqual(s.team?.id, "K4M2P8Q6R3")
    }

    func testAFreeAccountsBuildIsRenewedTwoDaysAhead() {
        let now = Date()
        func status(days: Double, free: Bool?) -> HeadsetStatus {
            HeadsetStatus(installed: true, expires: now.addingTimeInterval(days * 86_400), freeAccount: free)
        }
        XCTAssertFalse(status(days: 6.5, free: true).renewSoon(now: now))
        XCTAssertFalse(status(days: 2.5, free: true).renewSoon(now: now))
        XCTAssertTrue(status(days: 1.5, free: true).renewSoon(now: now))
        XCTAssertTrue(status(days: -1, free: true).renewSoon(now: now))
        // A paid team's year: a month ahead, as before (an older saved status says nothing about the account).
        XCTAssertFalse(status(days: 200, free: false).renewSoon(now: now))
        XCTAssertTrue(status(days: 20, free: nil).renewSoon(now: now))
        XCTAssertFalse(HeadsetStatus(installed: true).renewSoon(now: now))
    }

    func testABetasHeadsetAppIsOlderThanTheRelease() {
        func update(_ installed: String, _ latest: String) -> Bool {
            HeadsetStatus(installed: true, version: installed, latest: latest).updateAvailable
        }
        // A beta tester's Mac app updated to 0.1.0: Home offers to update the headset app.
        XCTAssertTrue(update("0.1.0-beta.4", "0.1.0"))
        XCTAssertTrue(update("0.1.0-beta.3", "0.1.0-beta.4"))
        XCTAssertTrue(update("0.1.0-beta.9", "0.1.0-beta.10"))
        XCTAssertTrue(update("0.1.0", "0.1.1"))
        XCTAssertTrue(update("0.1.0", "0.2.0-beta.1"))
        XCTAssertFalse(update("0.1.0", "0.1.0"))
        XCTAssertFalse(update("0.1.0", "0.1.0-beta.4"))
        XCTAssertFalse(update("0.1.0-beta.4", "0.1.0-beta.4"))
        XCTAssertFalse(update("0.1.1", "0.1.0"))
        XCTAssertFalse(HeadsetStatus(installed: true, version: nil, latest: "0.1.0").updateAvailable)
    }

    func testOverallProgressAcrossStages() {
        var p = InstallProgress()
        p.download(fraction: 0.5, secondsLeft: 60)
        XCTAssertEqual(p.overall, 0.2, accuracy: 1e-9)
        XCTAssertEqual(p.secondsLeft ?? 0, 60 + 90 + 60 + 40, accuracy: 1e-9)
        p.finish(.download)
        p.apply(.stepProgress(step: "kit", line: "x", done: nil, total: nil))
        XCTAssertEqual(p.stage, .convert)
        p.apply(.stepDone(step: "kit", detail: nil))
        p.apply(.stepProgress(step: "translate", line: "lib", done: 29, total: 58))
        // kit (0.05) + half of translate (0.58 / 2), in a stage worth 0.3.
        XCTAssertEqual(p.overall, 0.4 + 0.3 * (0.05 + 0.29), accuracy: 1e-9)
        let before = p.overall
        p.apply(.stepProgress(step: "translate", line: "lib", done: 10, total: 58))
        XCTAssertGreaterThanOrEqual(p.overall, before, "the bar never goes back")
        p.apply(.stepProgress(step: "app", line: "Signing", done: nil, total: nil))
        XCTAssertEqual(p.stage, .build)
        for s in Stage.allCases { p.finish(s) }
        XCTAssertEqual(p.overall, 1, accuracy: 1e-9)
    }

    func testTimeLeftInPlainWords() {
        XCTAssertEqual(Format.timeLeft(20), "Almost done")
        XCTAssertEqual(Format.timeLeft(70), "About a minute left")
        XCTAssertEqual(Format.timeLeft(190), "About 3 minutes left")
        XCTAssertEqual(Format.timeLeft(4000), "About an hour left")
        XCTAssertNil(Format.timeLeft(nil))
        XCTAssertEqual(Format.gigabytes(75_000_000_000), "75 GB")
        XCTAssertEqual(Format.gigabytes(1_060_000_000), "1.1 GB")
    }
}

final class DiagnosticsTests: XCTestCase {
    func testRedactsPersonalDetails() {
        let r = Redactor(known: ["gordon1998"], home: "/Users/username")
        let device = "00008112" + "-0001234567890ABC"  // split so the repo's scrub check doesn't take it for a real one
        let text = """
        steam account gordon1998 signed in, token eyJhbGciOiJFZERTQSJ9.eyJpc3MiOiJyOnN0ZWFtIn0.c2lnbmF0dXJlX2hlcmU
        {"refreshToken":"abc.def","account":"x"}
        Apple account jane@example.com, team A1B2C3D4E5 (Jane Appleseed)
        device \(device) Jane’s Apple Vision Pro on Janes-Mac-Studio.local
        log at /Users/username/Library/Logs/Glovebox/install.log
        Xcode 26.0.1 (17A400) build OK
        """
        let out = r.redact(text)
        for leak in ["gordon1998", "eyJhbGci", "abc.def", "jane@example.com", "A1B2C3D4", device, "Jane’s", "Janes-Mac-Studio", "/Users/username"] {
            XCTAssertFalse(out.contains(leak), "\(leak) leaked:\n\(out)")
        }
        XCTAssertTrue(out.contains("‹team …E5›"))
        XCTAssertTrue(out.contains("‹name›'s Apple Vision Pro"))
        XCTAssertTrue(out.contains("~/Library/Logs/Glovebox/install.log"))
        XCTAssertTrue(out.contains("Xcode 26.0.1"), "versions must survive:\n\(out)")
    }

    func testReportListsEveryCheck() {
        let text = DiagnosticsReport.compose(app: [("Glovebox", "0.1.0")],
                                             checks: [CheckResult(id: .xcode, status: .fail, code: "xcode-missing")],
                                             error: EngineError(code: "signing-failed", stage: .build, message: "exit 65"),
                                             logTail: ["line"], redactor: Redactor())
        XCTAssertTrue(text.contains("xcode: fail (xcode-missing)"))
        XCTAssertTrue(text.contains("mac: not run"))
        XCTAssertTrue(text.contains("Last error: signing-failed during build"))
    }
}

final class EngineTests: XCTestCase {
    var home: URL!

    override func setUp() {
        home = FileManager.default.temporaryDirectory.appendingPathComponent("glovebox-tests-\(UUID().uuidString)")
    }
    override func tearDown() { try? FileManager.default.removeItem(at: home) }

    func engine(_ env: [String: String] = [:]) -> Engine {
        let fake = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().appendingPathComponent("FakeEngine/glovebox")
        var e = Engine(executable: fake, home: home, logs: home.appendingPathComponent("Logs"))
        e.environment = ["FAKE_STEP": "0"].merging(env) { $1 }
        return e
    }

    func collect(_ run: EngineRun) async -> [EngineEvent] {
        var all: [EngineEvent] = []
        for await e in run.events { all.append(e) }
        return all
    }

    func testDoctorReportsEveryCheckInOrder() async throws {
        let run = try engine(["FAKE_FAIL": "team:1,headset:5"]).run(["doctor"], recipe: "alyx")
        let events = await collect(run)
        var state = SetupState()
        for case .check(let r) in events { state.record(r) }
        XCTAssertEqual(state.blocker?.code, "team-free-only")
        XCTAssertEqual(state.state(of: .account), .blocked)
        XCTAssertNil(state.results[.developerMode], "Developer Mode can't be read before the headset is paired")
        XCTAssertEqual(events.last, .finished(step: "doctor", ok: false, app: nil))
        XCTAssertEqual(run.status, 1)
        // The next run passes: the fake's "player" joined the program.
        var again = SetupState()
        for case .check(let r) in await collect(try engine(["FAKE_FAIL": "team:1,headset:5"]).run(["doctor"], recipe: "alyx")) { again.record(r) }
        XCTAssertEqual(again.blocker?.code, "headset-not-paired")
    }

    func testRunsTheFixCommandTheEngineNames() async throws {
        let e = engine(["FAKE_FAIL": "visionos:5"])
        var first = SetupState()
        for case .check(let r) in await collect(try e.run(["doctor"], recipe: "alyx")) { first.record(r) }
        guard let command = first.results[.visionOS]?.fix?.command else { return XCTFail("no fix") }
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/sh")
        p.arguments = ["-c", command]
        p.environment = ["GLOVEBOX_WORK": e.work(for: "alyx").path, "FAKE_STEP": "0"]
        try p.run()
        p.waitUntilExit()
        var after = SetupState()
        for case .check(let r) in await collect(try e.run(["doctor"], recipe: "alyx")) { after.record(r) }
        XCTAssertTrue(after.results[.visionOS]?.passes == true)
    }

    func testBuildAndInstallWalkTheStages() async throws {
        let e = engine()
        let game = e.work(for: "alyx").appendingPathComponent("linux/game")
        for d in ["bin/linuxsteamrtarm64", "hlvr/bin/linuxsteamrtarm64"] {
            try FileManager.default.createDirectory(at: game.appendingPathComponent(d), withIntermediateDirectories: true)
        }
        var progress = InstallProgress()
        progress.finish(.download)
        let build = await collect(try e.run(["build"], recipe: "alyx", environment: ["GLOVEBOX_TEAM": "A1B2C3D4E5"]))
        build.forEach { progress.apply($0) }
        XCTAssertEqual(progress.stage, .build)
        guard case .finished("build", true, let app?) = build.last else { return XCTFail("\(build.last as Any)") }
        let profile = try Data(contentsOf: URL(fileURLWithPath: app).appendingPathComponent("embedded.mobileprovision"))
        XCTAssertNotNil(XcodeInfo.profileExpiry(profile))
        let install = await collect(try e.run(["install"], recipe: "alyx"))
        install.forEach { progress.apply($0) }
        XCTAssertEqual(progress.stage, .install)
        XCTAssertEqual(install.last, .finished(step: "install", ok: true, app: nil))
    }

    func testAnInstallOnALockedHeadsetSaysSo() async throws {
        let e = engine(["FAKE_BUILD_FAIL": "install"])
        try FileManager.default.createDirectory(at: e.work(for: "alyx").appendingPathComponent("KleptonAlyx.app"),
                                                withIntermediateDirectories: true)
        let events = await collect(try e.run(["install"], recipe: "alyx"))
        guard case .error(let err) = events.last else { return XCTFail("\(events)") }
        XCTAssertEqual(err.code, "headset-unreachable")
        // The error names its own run's log (Copy Diagnostics shows that one's last lines).
        XCTAssertEqual(err.log.map { URL(fileURLWithPath: $0).lastPathComponent.hasPrefix("install-") }, true)
        // Woken up: the next install goes through.
        let again = await collect(try e.run(["install"], recipe: "alyx"))
        XCTAssertEqual(again.last, .finished(step: "install", ok: true, app: nil))
    }

    func testCancelStopsAndReportsIt() async throws {
        // Started from the main thread, as the app does: on Linux a child spawned from a Dispatch
        // worker thread inherits its blocked signals, SIGINT included.
        let e = engine(["FAKE_STEP": "0.2"])
        for d in ["bin/linuxsteamrtarm64", "hlvr/bin/linuxsteamrtarm64"] {
            try FileManager.default.createDirectory(at: e.work(for: "alyx").appendingPathComponent("linux/game/\(d)"),
                                                    withIntermediateDirectories: true)
        }
        let run = try await MainActor.run { try e.run(["build"], recipe: "alyx") }
        var events: [EngineEvent] = []
        for await ev in run.events {
            events.append(ev)
            if case .stepProgress = ev, events.count == 1 { run.cancel() }
        }
        XCTAssertEqual(events.last, .cancelled)
        XCTAssertFalse(events.contains { if case .error = $0 { return true }; return false })
    }

    func testQuittingStopsEveryRun() async throws {
        // The app quitting mid-build (Children.stopAll): the engine is gone within the time given.
        let e = engine(["FAKE_STEP": "0.2"])
        for d in ["bin/linuxsteamrtarm64", "hlvr/bin/linuxsteamrtarm64"] {
            try FileManager.default.createDirectory(at: e.work(for: "alyx").appendingPathComponent("linux/game/\(d)"),
                                                    withIntermediateDirectories: true)
        }
        let run = try await MainActor.run { try e.run(["build"], recipe: "alyx") }
        for await ev in run.events { if case .stepProgress = ev { break } }
        Children.stopAll(within: 3)
        let start = Date()
        while run.status == nil, Date().timeIntervalSince(start) < 5 { try await Task.sleep(nanoseconds: 50_000_000) }
        XCTAssertNotNil(run.status, "the engine is still running")
    }

    func testQuittingWaitsForWhatTheRunStartedToo() throws {
        // A tool the run started that ignores SIGINT: gone (SIGKILL) by the time stopAll returns.
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/sh")
        p.arguments = ["-c", "trap '' INT; sleep 30 & wait"]
        try p.run()
        Children.add(p)
        defer { Children.remove(p) }
        let group = getpgid(p.processIdentifier)
        usleep(200_000)
        let start = Date()
        Children.stopAll(within: 0.5)
        XCTAssertLessThan(Date().timeIntervalSince(start), 3)
        if group > 0, group != getpgid(0) {
            XCTAssertNotEqual(killpg(group, 0), 0, "something the run started is still running")
        }
    }

    func testAnEngineThatDiesSilentlyStillReportsAnError() async throws {
        let run = try engine().run(["no-such-command"], recipe: "alyx")
        let events = await collect(run)
        guard case .error(let e) = events.last else { return XCTFail("\(events)") }
        XCTAssertEqual(e.code, "internal")
        XCTAssertTrue(e.message?.contains("usage") == true)
        XCTAssertEqual(run.status, 2)
    }

    func testUnpacksTheBundledSourceOnce() throws {
        let fm = FileManager.default
        let bundled = home.appendingPathComponent("bundle/glovebox")
        try fm.createDirectory(at: bundled.appendingPathComponent("engine"), withIntermediateDirectories: true)
        fm.createFile(atPath: bundled.appendingPathComponent("engine/glovebox.py").path, contents: Data("v1".utf8))
        fm.createFile(atPath: bundled.appendingPathComponent("glovebox-kit.zip").path, contents: Data("kit".utf8))
        let src = home.appendingPathComponent("source")
        let a = try Engine.unpack(bundled, into: src, version: "0.1.0", build: "7")
        XCTAssertEqual(a.lastPathComponent, "0.1.0")
        XCTAssertEqual(try String(contentsOf: a.appendingPathComponent("engine/glovebox.py"), encoding: .utf8), "v1")
        XCTAssertTrue(fm.fileExists(atPath: a.appendingPathComponent("glovebox-kit.zip").path), "the kit sits at the source root")
        fm.createFile(atPath: a.appendingPathComponent("klepton-build-output").path, contents: Data())
        _ = try Engine.unpack(bundled, into: src, version: "0.1.0", build: "7")
        XCTAssertTrue(fm.fileExists(atPath: a.appendingPathComponent("klepton-build-output").path), "kept between runs")
        _ = try Engine.unpack(bundled, into: src, version: "0.1.0", build: "8")
        XCTAssertFalse(fm.fileExists(atPath: a.appendingPathComponent("klepton-build-output").path), "a new app build copies afresh")
        _ = try Engine.unpack(bundled, into: src, version: "0.2.0", build: "9")
        XCTAssertEqual(try fm.contentsOfDirectory(atPath: src.path), ["0.2.0"], "older copies go")
    }
}
