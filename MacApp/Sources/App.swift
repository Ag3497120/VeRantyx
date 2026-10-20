// VeRantyx for Mac: the setup and the launcher of the VeRantyx runtime, for players who would rather not use Terminal.
//
// Also, for testing without the window (development):
//   VeRantyx --collect-redist              gathers the runtime DLLs as the app does (VXRT_REDIST: elsewhere)
//   VeRantyx --play <app id> [seconds]     starts a game as the Play button does and reports what the app sees
//   VeRantyx --snapshot <folder>           saves pictures of each page of the window (scrolled: <page>-2.png, ...)
//   VeRantyx --report                      saves the logs as a report does and prints the form's link (opens nothing)
import AppKit
import SwiftUI

@main
enum Entry {
    static func main() {
        let a = CommandLine.arguments
        if let i = a.firstIndex(of: "--collect-redist") {
            _ = i
            Task {
                do {
                    let n = try await Redist.collect { print("  \($0)") }
                    print("\(n) DLLs in \(Paths.redist.path)")
                    exit(0)
                } catch { print(error.localizedDescription); exit(1) }
            }
            dispatchMain()
        }
        if a.contains("--report") {
            Task { @MainActor in
                let zip = await Report.saveLogs()
                print(zip?.path ?? "no logs")
                print(AppModel().reportURL(logs: zip?.lastPathComponent).absoluteString)
                exit(0)
            }
            dispatchMain()
        }
        if let i = a.firstIndex(of: "--play"), i + 1 < a.count {
            let id = a[i + 1]
            let seconds = i + 2 < a.count ? Double(a[i + 2]) ?? 90 : 90
            Task { @MainActor in await Headless.play(id, seconds: seconds) }
            RunLoop.main.run()                            // (the run loop, not dispatchMain: the app's timers need it)
        }
        VeRantyxApp.main()
    }
}

struct VeRantyxApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @State private var model = AppModel()

    var body: some Scene {
        Window("VeRantyx", id: "main") {
            RootView()
                .environment(model)
                .frame(minWidth: 900, minHeight: 640)
                .task { delegate.model = model; await Snapshot.runIfAsked(model) }
        }
        .defaultSize(width: 1060, height: 760)
        .commands {
            CommandGroup(replacing: .newItem) {}
            CommandGroup(replacing: .help) {
                Link(T("VeRantyx on GitHub", "GitHub の VeRantyx"), destination: Paths.repoURL)
            }
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    var model: AppModel?

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    /// Quitting while a game runs stops it (SteamVR, the encoder and the game with it) first.
    @MainActor func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard let model, let play = model.play, play.running else {
            if model?.console.running == true { model?.console.cancel() }
            return .terminateNow
        }
        play.stop()
        Task { @MainActor in
            for _ in 0..<40 where play.running { try? await Task.sleep(nanoseconds: 500_000_000) }
            sender.reply(toApplicationShouldTerminate: true)
        }
        return .terminateLater
    }
}

// MARK: - Testing without the window

@MainActor enum Headless {
    static func play(_ id: String, seconds: Double) async {
        let model = AppModel()
        await model.refresh()
        for _ in 0..<50 where model.games.isEmpty { try? await Task.sleep(nanoseconds: 200_000_000) }
        print("ready=\(model.ready) rosetta=\(model.rosetta ?? false) steam=\(model.steamRunning) steamVR=\(model.steamVR) dlls=\(model.redistCount) games=\(model.games.map(\.id))")
        guard let game = model.games.first(where: { $0.id == id }) else { print("\(id): not installed"); exit(2) }
        print("start \(game.name): exe=\(game.exe?.path ?? "(own launcher)") headset=\(model.headset.rawValue) quality=\(model.quality.rawValue)")
        model.start(game)
        guard let play = model.play else { print("did not start: \(model.error ?? "")"); exit(1) }
        let began = Date()
        while Date().timeIntervalSince(began) < seconds && play.running {
            try? await Task.sleep(nanoseconds: 5_000_000_000)
            print(String(format: "%5.0fs phase=%@ steamvr=%d headset=%d fps=%@ problem=%@", Date().timeIntervalSince(began), "\(play.phase)",
                         listening(27062) ? 1 : 0, play.headsetConnected ? 1 : 0, play.fps.map { String(format: "%.1f", $0) } ?? "-", play.problem ?? "-"))
        }
        if play.running { play.stop() }
        for _ in 0..<40 where play.running { try? await Task.sleep(nanoseconds: 500_000_000) }
        print("ended: phase=\(play.phase) problem=\(play.problem ?? "-") last game log line: \(play.gameLines.last ?? "-")")
        exit(0)
    }
}

/// Pictures of each page: the window server's image of the app's own window (no screen recording permission is
/// needed for one's own windows; the function is looked up at run time, as the SDK no longer declares it).
@MainActor enum Snapshot {
    typealias CreateImage = @convention(c) (CGRect, UInt32, UInt32, UInt32) -> Unmanaged<CGImage>?
    static let createImage: CreateImage? = {
        guard let h = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_NOW), let f = dlsym(h, "CGWindowListCreateImage") else { return nil }
        return unsafeBitCast(f, to: CreateImage.self)
    }()

    static func runIfAsked(_ model: AppModel) async {
        let a = CommandLine.arguments
        guard let i = a.firstIndex(of: "--snapshot"), i + 1 < a.count else { return }
        let dir = URL(fileURLWithPath: a[i + 1])
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        try? await Task.sleep(nanoseconds: 2_500_000_000)
        for page in Page.allCases {
            model.page = page
            try? await Task.sleep(nanoseconds: 1_200_000_000)
            guard let window = NSApp.windows.first(where: { $0.isVisible }) else { continue }
            // (the page, then the rest of it a screen at a time: <page>-2.png, ...)
            let scroll = window.contentView.flatMap { scrollViews(in: $0) }?.max { $0.frame.width < $1.frame.width }
            var shot = 1
            while true {
                guard let image = createImage?(.null, 1 << 3, UInt32(window.windowNumber), 1 << 0 | 1 << 3)?.takeRetainedValue() else { break }
                try? NSBitmapImageRep(cgImage: image).representation(using: .png, properties: [:])?
                    .write(to: dir.appendingPathComponent(shot == 1 ? "\(page.rawValue).png" : "\(page.rawValue)-\(shot).png"))
                guard let scroll, let document = scroll.documentView, shot < 6 else { break }
                let visible = scroll.contentView.bounds
                guard visible.maxY < document.frame.height - 1 else { break }
                let y = min(visible.minY + visible.height * 0.9, document.frame.height - visible.height)
                scroll.contentView.scroll(to: NSPoint(x: visible.minX, y: y))
                scroll.reflectScrolledClipView(scroll.contentView)
                shot += 1
                try? await Task.sleep(nanoseconds: 500_000_000)
            }
        }
        NSApp.terminate(nil)
    }

    static func scrollViews(in view: NSView) -> [NSScrollView] {
        (view as? NSScrollView).map { [$0] } ?? view.subviews.flatMap { scrollViews(in: $0) }
    }
}
