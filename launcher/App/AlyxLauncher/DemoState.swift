import Foundation
import SteamLauncherKit

/// Fixed screens for screenshots and design review, without a Steam account:
/// launch with `-demo signin|approve|code|checking|notowned|install|downloading|share|ready|controllers|denied|preparing`.
enum DemoState: String {
    case signin, approve, code, checking, notowned, install, downloading, ready
    /// The download, a few seconds in: the one question about stutter reports.
    case share
    /// Play waiting: the controllers are off, or tracking them was turned off.
    case controllers, denied
    /// After Play, while the game gets its graphics ready.
    case preparing

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
        switch self {
        case .signin:
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
        case .ready:
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
        case .preparing:
            m.phase = .library
            m.install = .installed(updateAvailable: false)
            m.preparing = LauncherModel.Preparing(fraction: 0.42, secondsLeft: 95)
        }
    }
}
