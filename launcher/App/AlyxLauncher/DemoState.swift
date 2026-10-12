import Foundation
import SteamLauncherKit

/// Fixed screens for screenshots and design review, without a Steam account:
/// launch with `-demo signin|toolong|revealed|approve|code|checking|notowned|install|downloading|share|ready|controllers|denied|preparing|appupdate|appupdatesteps|savesync|saves|savesplay`.
enum DemoState: String {
    case signin, approve, code, checking, notowned, install, downloading, ready
    /// The sign-in form with far more in the password field than a password, hidden and shown.
    case toolong, revealed
    /// The download, a few seconds in: the one question about stutter reports.
    case share
    /// The account menu's Share Diagnostics, open (`-demo diagnostics`).
    case diagnostics
    /// Play waiting: the controllers are off, or tracking them was turned off.
    case controllers, denied
    /// After Play, while the game gets its graphics ready.
    case preparing
    /// A newer Glovebox is out: the notice, and with what to do open.
    case appupdate, appupdatesteps
    /// Saves: syncing as the launcher opens; after Play, downloading; and after Play from the quick look at the cloud
    /// to the last download, over and over (for a recording).
    case savesync, saves, savesplay

    /// Screens of someone who has played before, whom the first-time steps don't show for (a demo has no game installed).
    var returningPlayer: Bool { [.appupdate, .appupdatesteps, .savesync, .saves, .savesplay].contains(self) }

    static var current: DemoState? {
        let args = ProcessInfo.processInfo.arguments
        if let i = args.firstIndex(of: "-demo"), i + 1 < args.count { return DemoState(rawValue: args[i + 1]) }
        return ProcessInfo.processInfo.environment["LAUNCHER_DEMO"].flatMap(DemoState.init)
    }

    @MainActor
    func apply(to m: LauncherModel) {
        m.accountName = "gordon_freeman"
        m.buildID = 25_487_405
        m.downloadBytes = 67_386_000_000
        m.freeSpace = 181_200_000_000
        m.controllers = ControllerState(connected: 2, access: .allowed)
        // Only `share` asks about stutter reports; every other screen already has an answer.
        m.shareStutters = self == .share ? nil : true
        m.gloveboxUpdate = self == .appupdate || self == .appupdatesteps ? "0.1.2" : nil
        m.saves = .synced
        switch self {
        case .signin, .toolong, .revealed:
            m.phase = .signIn
        case .approve:
            m.phase = .signIn
            m.signInStep = .approveInApp(canEnterCode: true)
        case .code:
            m.phase = .signIn
            m.signInStep = .enterCode(email: nil)
        case .checking:
            m.phase = .checkingOwnership
        case .notowned:
            m.phase = .notOwned
        case .install:
            m.phase = .library
            m.install = .notInstalled
        case .downloading, .share:
            m.phase = .library
            var p = DownloadProgress()
            p.totalBytes = 67_386_000_000
            p.doneBytes = 23_918_000_000
            p.bytesPerSecond = 61_400_000
            p.totalFiles = 1_842
            p.doneFiles = 611
            m.install = .downloading(p)
        case .ready, .diagnostics:
            m.phase = .library
            m.install = .installed(updateAvailable: false)
        case .controllers:
            m.phase = .library
            m.install = .installed(updateAvailable: false)
            m.controllers.connected = 0
        case .denied:
            m.phase = .library
            m.install = .installed(updateAvailable: false)
            m.controllers.access = .denied
        case .appupdate, .appupdatesteps:
            m.phase = .library
            m.install = .installed(updateAvailable: false)
        case .preparing:
            m.phase = .library
            m.install = .installed(updateAvailable: false)
            m.preparing = LauncherModel.Preparing(fraction: 0.42, secondsLeft: 95)
        case .savesync:
            m.phase = .library
            m.install = .installed(updateAvailable: false)
            m.saves = .syncing(CloudSync.Progress(stage: .downloading, done: 3, total: 7))
        case .saves:
            m.phase = .library
            m.install = .installed(updateAvailable: false)
            m.launching = true
            m.saves = .syncing(CloudSync.Progress(stage: .downloading, done: 2, total: 5))
        case .savesplay:
            m.phase = .library
            m.install = .installed(updateAvailable: false)
            Task { @MainActor in
                // Play pressed after a PC session: the quick look finds new saves, three come down, the game starts.
                func step(_ s: Double) async { try? await Task.sleep(nanoseconds: UInt64(s * 1e9)) }
                while true {
                    m.launching = false
                    m.saves = .synced
                    await step(2)
                    m.launching = true
                    m.saves = .syncing(CloudSync.Progress(stage: .checking))
                    await step(1.6)
                    for done in 0...3 {
                        m.saves = .syncing(CloudSync.Progress(stage: .downloading, done: done, total: 3))
                        await step(0.7)
                    }
                    m.saves = .synced
                    m.launching = false
                    m.preparing = LauncherModel.Preparing(fraction: 0, secondsLeft: nil, firstTimeOnly: false, title: "Starting")
                    await step(2)
                    m.preparing = nil
                }
            }
        }
    }
}
