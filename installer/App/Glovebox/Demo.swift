import SwiftUI
import AppKit
import GloveboxKit

/// `-demo <state>` opens the app in one made-up state, without running the engine, and
/// `-appearance light|dark` picks the look. With `-snapshot <file.png>` it writes the window's
/// number to `<file.png>.wid` once the page has drawn, for CI's `screencapture -l` (which draws
/// glass and materials the way the screen does), then waits to be quit.
enum Demo {
    static let current: String? = value(after: "-demo")
    static let snapshotPath: String? = value(after: "-snapshot")
    static let appearance: String? = value(after: "-appearance")
    /// Recording the walkthrough video (Tour.swift).
    static let isTour = value(after: "-tour") != nil

    static let all = ["welcome", "xcode-get", "xcode-downloading", "xcode-setup", "xcode-visionos",
                      "steam-signin", "steam-scanned", "steam-password", "steam-password-toolong", "steam-password-revealed", "steam-approve", "steam-code", "steam-owned", "steam-notowned",
                      "account-signin", "account-teamload", "account-program", "account-team", "account-free",
                      "headset-pair", "headset-pair26", "headset-devmode",
                      "install-ready", "install-running", "install-consent", "install-news", "install-signing", "install-signing-once", "install-signing-denied", "install-signing-certificate",
                      "install-error", "install-signin-expired", "install-signin-guiding", "install-agreement",
                      "install-headsetwait", "install-done", "install-done-news", "install-done-free",
                      "home", "home-allset", "home-news", "home-renew-free", "guide-signin", "guide-pair", "guide-keychain", "guide-signinagain", "guide-loadteam", "guide-loadteam2", "guide-loadteam3"]

    static func value(after flag: String) -> String? {
        let args = CommandLine.arguments
        guard let i = args.firstIndex(of: flag), i + 1 < args.count else { return nil }
        return args[i + 1]
    }

    @MainActor static func applyAppearance() {
        switch appearance {
        case "dark": NSApp.appearance = NSAppearance(named: .darkAqua)
        case "light": NSApp.appearance = NSAppearance(named: .aqua)
        default: break
        }
    }

    @MainActor static func snapshotIfAsked(_ model: InstallerModel) {
        guard let path = snapshotPath else { return }
        Task { @MainActor in
            #if DEBUG
            if let name = current, name.hasPrefix("guide-") {
                switch name {
                case "guide-pair": model.guide.showForDemo(.pair, stage: .finish, xcodeMajor: 27)
                case "guide-keychain": model.guide.showForDemo(.keychain, stage: .click, xcodeMajor: 27)
                case "guide-signinagain": model.guide.showForDemo(.signInAgain, stage: .click, xcodeMajor: 27)
                case "guide-loadteam": model.guide.showForDemo(.refresh, stage: .click, xcodeMajor: 27, loadingTeam: true)
                case "guide-loadteam2": model.guide.showForDemo(.refresh, stage: .team, xcodeMajor: 27, loadingTeam: true)
                case "guide-loadteam3": model.guide.showForDemo(.refresh, stage: .finish, xcodeMajor: 27, loadingTeam: true)
                default: model.guide.showForDemo(.signIn, stage: .open, xcodeMajor: 27)
                }
            }
            #endif
            NSApp.activate()
            NSApp.windows.first { $0.canBecomeMain }?.makeKeyAndOrderFront(nil)
            // Steam's art comes over the network.
            try? await Task.sleep(nanoseconds: 6_000_000_000)
            var window = NSApp.windows.first { $0.isVisible && $0.canBecomeMain }
            #if DEBUG
            if current?.hasPrefix("guide-") == true { window = model.guide.demoPanel }
            #endif
            guard let window else { print("snapshot: no window"); exit(1) }
            if window.attachedSheet != nil, let screen = NSScreen.screens.first {
                // An alert is a window of its own: CI captures the screen where the window is instead.
                let f = window.frame
                try? "\(Int(f.minX)),\(Int(screen.frame.maxY - f.maxY)),\(Int(f.width)),\(Int(f.height))"
                    .write(toFile: path + ".rect", atomically: true, encoding: .utf8)
            }
            try? "\(window.windowNumber)".write(toFile: path + ".wid", atomically: true, encoding: .utf8)
            print("snapshot: window \(window.windowNumber) -> \(path)")
            fflush(stdout)
        }
    }
}

extension InstallerModel {
    static func demo(_ name: String) -> InstallerModel {
        let m = InstallerModel(engine: nil, demo: true)
        m.applyDemo(name)
        return m
    }

    private static let paidTeams: JSONValue = .object(["teams": .array([
        .object(["id": .string("A1B2C3D4E5"), "name": .string("Jane Appleseed"), "paid": .bool(true)]),
        .object(["id": .string("F6G7H8J9K0"), "name": .string("Appleseed Games LLC"), "paid": .bool(true)]),
    ])])

    private static let freeTeam: JSONValue = .object(["teams": .array([
        .object(["id": .string("Z9Y8X7W6V5"), "name": .string("Jane Appleseed (Personal Team)"), "paid": .bool(false)]),
    ])])

    private static func allOK() -> SetupState {
        var s = SetupState()
        let values: [CheckID: String] = [.mac: "macOS 27.0", .disk: "412 GB free", .xcode: "Xcode 27.0",
                                         .visionOS: "visionOS 27.0", .headset: "Jane’s Vision Pro", .developerMode: "On"]
        for id in CheckID.allCases { s.record(CheckResult(id: id, status: .ok, value: values[id])) }
        s.record(CheckResult(id: .team, status: .ok, value: "Jane Appleseed", data: .object(["teams": .array([
            .object(["id": .string("A1B2C3D4E5"), "name": .string("Jane Appleseed"), "paid": .bool(true)])])])))
        return s
    }

    /// Everything fine but `id`, which fails with `code`. Without a working Xcode, the checks
    /// after it are unknown.
    private func blocked(_ id: CheckID, _ code: String, value: String? = nil, fix: Fix? = nil, data: JSONValue? = nil) {
        var s = Self.allOK()
        if id == .xcode {
            for later in [CheckID.visionOS, .team, .headset, .developerMode] { s.results[later] = nil }
        }
        s.record(CheckResult(id: id, status: .fail, code: code, value: value, fix: fix, data: data))
        setup = s
    }

    func applyDemo(_ name: String) {
        // The walkthrough (Tour.swift) shows one state after another: each starts from scratch.
        mode = .setup
        autoFix = nil
        xcodeRequested = false
        download = Download()
        progress = InstallProgress()
        installing = false
        installError = nil
        waitingForHeadset = false
        allowingSigning = false
        signingAsked = false
        trustFirst = false
        ranBefore = nil
        installAsked = nil
        result = nil
        #if DEBUG
        guide.pretendRunning(name == "account-guiding" ? .signIn : name == "install-signin-guiding" ? .signInAgain : nil)
        #endif
        status = nil
        steam.step = .form
        steam.method = .qr
        steam.qrURL = nil
        steam.error = nil
        started = name != "welcome"
        checkedOnce = true
        steam.accountName = "jane"
        xcodeMajor = name == "headset-pair26" ? 26 : 27
        xcodeIcon = MacTools.xcodeIcon
        setup = Self.allOK()
        shareStutters = name == "install-consent" ? nil : true
        askingToShare = name == "install-consent"
        askingForNews = name.hasSuffix("-news")
        if !name.hasPrefix("steam") && !name.hasPrefix("xcode") && name != "welcome" {
            steam.step = .owned
            download = Download(fraction: 1, done: true)
        }
        switch name {
        case "xcode-get":
            xcodeIcon = nil
            blocked(.xcode, "xcode-missing", fix: Fix(kind: .openURL, label: "Get Xcode", url: MacTools.appStoreXcode))
            step = .xcode
        case "xcode-downloading":
            xcodeIcon = nil
            blocked(.xcode, "xcode-missing", fix: Fix(kind: .openURL, label: "Get Xcode", url: MacTools.appStoreXcode))
            xcodeRequested = true
            step = .xcode
        case "xcode-setup":
            blocked(.xcode, "xcode-first-launch", value: "license", fix: Fix(kind: .openApp, label: "Open Xcode", url: "xcode://"))
            step = .xcode
        case "xcode-visionos":
            blocked(.visionOS, "visionos-missing", fix: Fix(kind: .command, label: "Add visionOS",
                                                            command: "xcodebuild -downloadPlatform visionOS"))
            autoFix = AutoFix(code: "visionos-missing", fraction: 0.38, secondsLeft: 260)
            step = .xcode
        case "steam-signin":
            // Made up: Steam's codes are links like this one.
            steam.qrURL = "https://s.team/q/1/2850163397412468015"
            step = .steam
        case "steam-scanned":
            steam.step = .approve(canEnterCode: false)
            step = .steam
        case "steam-password", "steam-password-toolong", "steam-password-revealed":
            steam.method = .password
            step = .steam
        case "steam-approve":
            steam.step = .approve(canEnterCode: true)
            step = .steam
        case "steam-code":
            steam.step = .code(email: nil)
            step = .steam
        case "steam-owned":
            steam.step = .owned
            download = Download(running: true, fraction: 0.42, secondsLeft: 380)
            step = .steam
        case "steam-notowned":
            steam.step = .notOwned
            step = .steam
        case "account-signin":
            blocked(.team, "apple-account-missing", fix: Fix(kind: .openApp, label: "Sign in", url: "xcode://"))
            step = .account
        case "account-teamload", "guide-loadteam", "guide-loadteam2", "guide-loadteam3":
            blocked(.team, "apple-team-not-loaded", fix: Fix(kind: .openApp, label: "Load your team in Xcode's Accounts settings", url: "xcode://"))
            step = .account
        case "account-program":
            blocked(.team, "team-free-only", fix: Fix(kind: .openURL, label: "Join", url: "https://developer.apple.com/programs/enroll/"),
                    data: Self.freeTeam)
            step = .account
        case "account-team":
            setup.record(CheckResult(id: .team, status: .ok, data: Self.paidTeams))
            step = .account
        case "account-free":
            setup.record(CheckResult(id: .team, status: .ok, value: "Jane Appleseed (Personal Team)", data: Self.freeTeam))
            step = .account
        case "headset-pair", "headset-pair26":
            setup.record(CheckResult(id: .headset, status: .fail, code: "headset-not-paired", value: "Jane’s Vision Pro"))
            setup.results[.developerMode] = nil
            step = .headset
        case "headset-checking":
            // A waking headset's first checks: the step is still checking.
            setup.results[.headset] = nil
            setup.results[.developerMode] = nil
            step = .headset
        case "account-guiding":
            blocked(.team, "apple-account-missing")
            step = .account
        case "headset-devmode":
            setup.record(CheckResult(id: .developerMode, status: .fail, code: "developer-mode-off"))
            step = .headset
        case "install-ready":
            step = .install
        case "install-getting-ready", "install-headset-checking":
            installAsked = Date()
            if name == "install-headset-checking" {
                // A waking headset's first checks: Developer Mode off for a few seconds, so the step is still checking.
                setup.results[.headset] = nil
                setup.results[.developerMode] = nil
            }
            step = .install
        case "install-running", "install-consent", "install-news":
            installing = true
            progress.download(fraction: 1, secondsLeft: nil)
            progress.finish(.download)
            for s in ["kit", "runtime", "game", "pipelines"] { progress.apply(.stepDone(step: s, detail: nil)) }
            progress.apply(.stepProgress(step: "translate", line: "libengine2.so", done: 40, total: 89))
            step = .install
        case "install-error", "install-signin-expired", "install-signin-guiding", "install-agreement":
            progress.finish(.download)
            progress.finish(.convert)
            progress.begin(.build)
            installError = EngineError(code: name == "install-error" ? "signing-failed"
                                       : name == "install-agreement" ? "apple-agreement-pending" : "xcode-signin-expired", stage: .build)
            step = .install
        case "install-signing", "install-signing-asked", "guide-keychain":
            installing = true
            progress.download(fraction: 1, secondsLeft: nil)
            progress.finish(.download)
            allowingSigning = true
            signingAsked = name == "install-signing-asked"
            step = .install
        case "install-signing-once", "install-signing-denied", "install-signing-certificate":
            progress.finish(.download)
            installError = EngineError(code: name == "install-signing-once" ? "signing-allowed-once"
                                       : name == "install-signing-certificate" ? "signing-certificate-missing" : "signing-not-allowed",
                                       stage: .build)
            step = .install
        case "install-headsetwait":
            installing = true
            for s in [Stage.download, .convert, .build] { progress.finish(s) }
            progress.begin(.install)
            waitingForHeadset = true
            step = .install
        case "install-done", "install-done-news":
            result = InstallResult(app: nil, device: "Jane’s Vision Pro", expires: Date().addingTimeInterval(365 * 86_400))
            step = .install
        case "install-done-free":
            setup.record(CheckResult(id: .team, status: .ok, value: "Jane Appleseed (Personal Team)", data: Self.freeTeam))
            result = InstallResult(app: nil, device: "Jane’s Vision Pro", expires: Date().addingTimeInterval(7 * 86_400))
            trustFirst = true
            ranBefore = false
            step = .install
        case "home", "home-allset", "home-news":
            mode = .home
            status = HeadsetStatus(installed: true, version: "0.1.0", latest: name == "home" ? "0.2.0" : "0.1.0",
                                   expires: Date().addingTimeInterval(341 * 86_400))
            step = .welcome
        case "home-renew-free", "home-expired-free":
            mode = .home
            setup.record(CheckResult(id: .team, status: .ok, value: "Jane Appleseed (Personal Team)", data: Self.freeTeam))
            let left: TimeInterval = name == "home-expired-free" ? -2 * 86_400 : 30 * 3600
            status = HeadsetStatus(installed: true, version: "0.1.0", latest: "0.1.0", expires: Date().addingTimeInterval(left),
                                   freeAccount: true)
            step = .welcome
        case "guide-signin", "guide-pair":
            blocked(.team, "apple-account-missing")
            step = .account
        default:
            // A first launch: nothing checked yet.
            setup = SetupState()
            xcodeIcon = nil
            step = .welcome
        }
    }
}
