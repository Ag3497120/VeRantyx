#if DEBUG
import SwiftUI
import AppKit
import GloveboxKit
import SteamLauncherKit

/// `-tour <folder>`: the walkthrough video CI records (installer/App/Tour/tour.sh), and the
/// rehearsal on a real Mac's state (rehearsal.sh). The script writes one command at a time to
/// `<folder>/next` and the app does it: a made-up state from `-demo`, a caption under the window,
/// the real Xcode guide, or the real flow's buttons. `<folder>/status` says where the guide is and
/// where its ring sits on screen, so the script can click it, and what each step and check says.
///
///     state <demo state>     caption <title>|<text>     guide signin|signinagain|signedin|pair|paircode|appstore|setup|stop
///     answer share|dontshare (the stutter-report ask)    front
///     touchid on|off|- (the guide at macOS's prompt for Xcode as on a Mac with Touch ID, without, or as this Mac is)
///     start (Let's Go!)      go <step>                  recheck
///     fix <check> (its card's button)   getxcode (Get Xcode)   install (Install)   agreement (Open Apple's Website)
///     fakesteam (the fake engine's run only:
///     a made-up Steam session in the "(dev)" item, as owned, so download, build and install run)
///     steamout (forget the saved Steam session, as Use Another Account does)
///     devicectl <arguments> (run as the app runs it; exit code and output in <folder>/devicectl.out)
///     run <tool> <arguments> (the same for any tool; <folder>/run.out)
@MainActor
enum Tour {
    static let folder: String? = Demo.value(after: "-tour")
    private static let caption = Caption()
    private static var captionPanel: NSPanel?
    private static var codePanel: NSPanel?
    /// A small screen (CI's default 1024 × 768) leaves one line under the window.
    private static var compact: Bool { (NSScreen.main?.frame.height ?? 1080) < 900 }

    static func startIfAsked(_ model: InstallerModel) {
        guard let folder else { return }
        let next = URL(fileURLWithPath: folder).appendingPathComponent("next")
        let status = URL(fileURLWithPath: folder).appendingPathComponent("status")
        Task { @MainActor in
            try? await Task.sleep(nanoseconds: 500_000_000)
            placeWindow()
            showCaption()
            var last = ""
            while true {
                if let text = try? String(contentsOf: next, encoding: .utf8) {
                    try? FileManager.default.removeItem(at: next)
                    for line in text.split(separator: "\n") { run(String(line), model) }
                }
                let now = statusLine(model)
                if now != last {
                    try? now.write(to: status, atomically: true, encoding: .utf8)
                    last = now
                }
                try? await Task.sleep(nanoseconds: 100_000_000)
            }
        }
    }

    private static func run(_ line: String, _ model: InstallerModel) {
        let parts = line.split(separator: " ", maxSplits: 1).map(String.init)
        let arg = parts.count > 1 ? parts[1] : ""
        print("tour: \(line)")
        switch parts.first ?? "" {
        case "state":
            withAnimation(.easeInOut(duration: 0.35)) { model.applyDemo(arg) }
        case "caption":
            let t = arg.split(separator: "|", maxSplits: 1, omittingEmptySubsequences: false).map(String.init)
            withAnimation(.easeInOut(duration: 0.3)) {
                caption.title = t.count > 1 ? t[0] : ""
                caption.text = t.last ?? ""
            }
        case "guide":
            switch arg {
            case "stop":
                model.guide.stop()
                codePanel?.orderOut(nil)
                codePanel = nil
            case "pair": model.guide.start(.pair)
            case "paircode":
                // Device Hub's code window needs a real headset: a stand-in of its size, mid-screen.
                model.guide.start(.pair)
                let screen = NSScreen.screens.first?.frame ?? .zero
                let code = CGRect(x: screen.midX - 235, y: screen.midY - 156, width: 470, height: 313)
                model.guide.pretendCodeWindow = code  // from the top left, as the window list has it
                showCodeStandIn(at: NSRect(x: code.minX, y: screen.height - code.maxY, width: code.width, height: code.height))
            case "appstore": model.guide.start(.appStore)
            case "signinagain": model.signInAgain()
            case "signedin":
                // Apple's sign-in came and went (CI has no account to sign in to again).
                model.guide.pretendSignedInAgain()
            case "setup": model.guide.start(.setup)
            default: model.guide.start(.signIn)
            }
        case "start":
            model.getStarted()
        case "go":
            if let s = InstallerModel.Step(rawValue: arg) { model.select(s) } else { print("tour: no step \(arg)") }
        case "recheck":
            model.recheck()
        case "fix":
            if let id = CheckID(rawValue: arg), let r = model.setup.results[id] { Task { await model.runFix(r) } }
        case "install":
            model.requestInstall()
        case "agreement":
            model.acceptAgreement()
        case "update":
            model.update()
        case "recover":
            if let s = InstallerModel.Step(rawValue: arg) { model.recover(at: s) } else { print("tour: no step \(arg)") }
        case "ran":
            // Whether the game has run on the headset before, made up (CI has no headset): yes, no, or off for
            // devicectl's answer.
            ran = arg == "yes" ? true : arg == "no" ? false : nil
        case "continue":
            model.continueSigning()
        case "getxcode":
            model.getXcode()
        case "fakesteam":
            guard model.engine?.source == nil else { print("tour: fakesteam is for the fake engine only"); break }
            SteamKeychain.save(SteamCredentials(accountName: "tour", refreshToken: "tour"))
            model.steam.accountName = "tour"
            model.steam.step = .owned
        case "steamout":
            model.useAnotherSteamAccount()
        case "answer":
            model.setShareStutters(arg == "share")
        case "news":
            // The news card's No Thanks, for when accessibility can't press it.
            model.answerNews(signedUp: false)
        case "touchid":
            model.guide.pretendTouchID = arg == "on" ? true : arg == "off" ? false : nil
        case "front":
            NSApp.activate()
            mainWindow?.makeKeyAndOrderFront(nil)
        case "devicectl":
            // devicectl the way Glovebox runs it (its plain child); its exit code and output in
            // <folder>/devicectl.out: whether macOS asks to let Glovebox find devices on the local network.
            let dev = model.engine?.environment["DEVELOPER_DIR"]
            Task {
                let (code, out) = await MacTools.run("/usr/bin/xcrun", ["devicectl"] + arg.split(separator: " ").map(String.init),
                                                     developerDir: dev)
                guard let folder else { return }
                try? "exit \(code)\n\(out.prefix(2000))".write(toFile: folder + "/devicectl.out", atomically: true, encoding: .utf8)
            }
        case "run":
            // Any tool, the way Glovebox starts devicectl and the engine; its exit code and output in
            // <folder>/run.out. For lanprobe: whether what Glovebox starts may use the local network.
            let dev = model.engine?.environment["DEVELOPER_DIR"]
            let line = arg.split(separator: " ").map(String.init)
            guard let tool = line.first else { break }
            Task {
                let (code, out) = await MacTools.run(tool, Array(line.dropFirst()), developerDir: dev)
                guard let folder else { return }
                try? "exit \(code)\n\(out.prefix(2000))".write(toFile: folder + "/run.out", atomically: true, encoding: .utf8)
            }
        default:
            print("tour: unknown command")
        }
    }

    private static var mainWindow: NSWindow? { NSApp.windows.first { $0.canBecomeMain && $0.isVisible } }

    /// The made-up answer to whether the game ran on the headset before (`ran`), nil for devicectl's.
    static var ran: Bool?

    /// Install's card as last drawn, and how many times SwiftUI asked for it (with allowingSigning then).
    static var card = "-"
    private static var cardAsks = 0, cardSigning = false
    static func askedForCard(_ model: InstallerModel) {
        cardAsks += 1
        cardSigning = model.allowingSigning
    }

    /// `stage=<guide stage> ring=<x>,<y> page=<step> rm=<0|1> rt=<0|1>`: the ring (or what the bubble
    /// points up at) in screen points from the top left, where clicks go, and the Reduce Motion and Reduce Transparency settings.
    /// Then each step's state, each check's result (and its code), whether the checks ran once, the
    /// Xcode the app picked and its developer folder, where the install stands and the download's
    /// percent, and the app in front (Glovebox comes back when a guide is done).
    private static func statusLine(_ model: InstallerModel) -> String {
        let g = model.guide
        let stage = g.isRunning ? "\(g.stage)" : "off"
        var ring = "none"
        if let r = g.demoRing?.frame, let h = NSScreen.screens.first?.frame.height {
            ring = "\(Int(r.midX)),\(Int(h - r.midY))"
        } else if let t = g.demoPointedAt {
            ring = "\(Int(t.midX)),\(Int(t.midY))"
        }
        // CI's Macs come with Reduce Motion and Reduce Transparency on; the log says which.
        let ws = NSWorkspace.shared
        let steps = InstallerModel.Step.allCases.map { "\($0.rawValue):\(model.state(of: $0))" }.joined(separator: ",")
        let checks = CheckID.allCases.compactMap { id in
            model.setup.results[id].map { "\(id.rawValue):\($0.status.rawValue)\($0.code.map { "(\($0))" } ?? "")" }
        }.joined(separator: ",")
        let dev = model.engine?.environment["DEVELOPER_DIR"] ?? "-"
        let install = model.result != nil ? "done" : model.waitingForHeadset ? "headset" : model.allowingSigning ? "signing" : model.installing ? "running"
            : model.installError.map { "error(\($0.code))" } ?? (model.gettingReady ? "ready" : model.installAsked != nil ? "asked" : "-")
        return "stage=\(stage) ring=\(ring) page=\(model.step.rawValue) rm=\(ws.accessibilityDisplayShouldReduceMotion ? 1 : 0) rt=\(ws.accessibilityDisplayShouldReduceTransparency ? 1 : 0)"
            + " steps=\(steps) checks=\(checks.isEmpty ? "-" : checks) checked=\(model.checkedOnce ? 1 : 0) xcode=\(model.xcodeMajor.map(String.init) ?? "-") dev=\(dev.replacingOccurrences(of: " ", with: "%20"))"
            + " install=\(install) download=\(Int(model.download.fraction * 100))"
            + " ran=\(model.ranBefore.map { $0 ? "yes" : "no" } ?? "-") trust=\(model.result == nil ? "-" : model.trustFirst ? "steps" : "none")"
            + " hs=\(model.headsetSettling ? "settling" : "-")"
            + " share=\(model.askingToShare ? "ask" : model.shareStutters.map { $0 ? "yes" : "no" } ?? "-")"
            + " news=\(model.showsNewsAsk ? "ask" : model.newsSignedUp ? "yes" : "-")"
            + " front=\(ws.frontmostApplication?.bundleIdentifier ?? "-")"
            + " card=\(card) asks=\(cardAsks)/\(cardSigning ? 1 : 0) active=\(NSApp.isActive ? 1 : 0)"
            + " visible=\(mainWindow.map { $0.occlusionState.contains(.visible) ? "1" : "0" } ?? "-")\n"
    }

    /// The window near the top of the screen, the caption under it.
    private static func placeWindow() {
        guard let w = mainWindow, let screen = NSScreen.main?.visibleFrame else { return }
        w.setFrameTopLeftPoint(NSPoint(x: screen.midX - w.frame.width / 2, y: screen.maxY - (compact ? 2 : 24)))
        NSApp.activate()
        w.makeKeyAndOrderFront(nil)
    }

    private static func showCodeStandIn(at frame: NSRect) {
        let p = NSPanel(contentRect: frame, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        p.isOpaque = false
        p.backgroundColor = .clear
        p.hasShadow = true
        // Over Device Hub (which comes to the front), under the guide's bubble and ring.
        p.level = NSWindow.Level(rawValue: NSWindow.Level.floating.rawValue - 1)
        p.contentView = NSHostingView(rootView: CodeWindowStandIn())
        p.orderFrontRegardless()
        codePanel = p
    }

    private static func showCaption() {
        guard let screen = NSScreen.main?.visibleFrame else { return }
        let size = compact ? NSSize(width: screen.width - 16, height: 56) : NSSize(width: 960, height: 120)
        let p = NSPanel(contentRect: NSRect(x: screen.midX - size.width / 2, y: screen.minY + (compact ? 4 : 24), width: size.width, height: size.height),
                        styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        p.isOpaque = false
        p.backgroundColor = .clear
        p.hasShadow = false
        p.ignoresMouseEvents = true
        p.level = .statusBar
        p.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
        p.contentView = NSHostingView(rootView: CaptionView(caption: caption, compact: compact)
            .frame(width: size.width, height: size.height, alignment: .bottom))
        p.orderFrontRegardless()
        captionPanel = p
    }
}

/// Device Hub's code window as a real pairing shows it (470 × 313, the six boxes 163 pt down),
/// for the video and the rehearsal, which have no headset to bring up the real one.
private struct CodeWindowStandIn: View {
    var body: some View {
        ZStack(alignment: .top) {
            RoundedRectangle(cornerRadius: 16, style: .continuous).fill(Color(nsColor: .windowBackgroundColor))
            Image(systemName: "visionpro").font(.system(size: 36)).foregroundStyle(.secondary).padding(.top, 44)
            Text("Enter the verification code displayed on \u{201C}Apple Vision Pro\u{201D}")
                .font(.system(size: 13, weight: .semibold)).multilineTextAlignment(.center)
                .frame(width: 360).padding(.top, 104)
            HStack(spacing: 10) {
                ForEach(0..<6, id: \.self) { _ in
                    RoundedRectangle(cornerRadius: 7, style: .continuous).stroke(Color.secondary.opacity(0.6), lineWidth: 1)
                        .frame(width: 42, height: 46)
                }
            }
            .padding(.top, 163)
            Text("Stand-in: Device Hub shows this with a real headset")
                .font(.system(size: 11)).foregroundStyle(.tertiary).padding(.top, 262)
        }
        .frame(width: 470, height: 313)
    }
}

private final class Caption: ObservableObject {
    @Published var title = ""
    @Published var text = ""
}

/// Subtitles for the video: what the player does at this point, and why.
private struct CaptionView: View {
    @ObservedObject var caption: Caption
    var compact: Bool

    var body: some View {
        if compact, !caption.text.isEmpty {
            (Text(caption.title.isEmpty ? "" : caption.title + "   ").foregroundColor(.white.opacity(0.6))
                + Text(caption.text).foregroundColor(.white))
                .font(.system(size: 16, weight: .medium))
                .lineLimit(2)
                .minimumScaleFactor(0.8)
                .padding(.horizontal, 18)
                .padding(.vertical, 10)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(RoundedRectangle(cornerRadius: 14, style: .continuous).fill(Color.black.opacity(0.82)))
                .id(caption.text)
                .transition(.opacity)
        } else if !caption.text.isEmpty {
            VStack(alignment: .leading, spacing: 5) {
                if !caption.title.isEmpty {
                    Text(caption.title)
                        .font(.system(size: 14, weight: .semibold))
                        .foregroundStyle(.white.opacity(0.65))
                }
                Text(caption.text)
                    .font(.system(size: 21, weight: .medium))
                    .foregroundStyle(.white)
                    .fixedSize(horizontal: false, vertical: true)
            }
            .padding(.horizontal, 26)
            .padding(.vertical, 16)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(RoundedRectangle(cornerRadius: 18, style: .continuous).fill(Color.black.opacity(0.8)))
            .id(caption.text)
            .transition(.opacity)
        }
    }
}
#endif
