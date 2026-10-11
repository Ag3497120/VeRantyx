import SwiftUI
import GloveboxKit

/// Where each step and each task inside it stands, from the latest checks. The pages and the
/// step bar read only these.
extension InstallerModel {
    // MARK: Steps

    func state(of step: Step) -> StepState {
        switch step {
        case .welcome:
            return started || mode == .home ? .done : .todo
        case .xcode:
            let tasks = [macState, xcodeInstallState, xcodeSetupState, visionOSState]
            return combine(tasks)
        case .steam:
            switch steam.step {
            case .owned: return .done
            case .working, .approve, .code, .checkingOwnership: return .working
            case .notOwned: return .attention
            case .form: return steam.error == nil ? .todo : .attention
            }
        case .account:
            return combine([signInState, programState])
        case .headset:
            return combine([pairState, developerModeState])
        case .install:
            if result != nil || (mode == .home && status != nil && !installing) { return .done }
            if installing || waitingForHeadset { return .working }
            if installError != nil { return .attention }
            return [Step.xcode, .steam, .account, .headset].allSatisfy({ state(of: $0) == .done }) ? .todo : .waiting
        }
    }

    /// A step is as far along as its least finished task.
    private func combine(_ tasks: [StepState]) -> StepState {
        if tasks.allSatisfy({ $0 == .done }) { return .done }
        if tasks.contains(.attention) { return .attention }
        if tasks.contains(.working) { return .working }
        if tasks.allSatisfy({ $0 == .waiting }) { return .waiting }
        return .todo
    }

    private func result(_ id: CheckID) -> CheckResult? { setup.results[id] }

    var xcodeReady: Bool { result(.xcode)?.passes == true }

    // MARK: Xcode

    /// This Mac: only shown when something's wrong (an Intel Mac, a full disk).
    var macProblem: CheckResult? {
        [CheckID.mac, .disk].compactMap { result($0) }.first { !$0.passes }
    }

    var macState: StepState { macProblem == nil ? .done : .attention }

    var xcodeInstallState: StepState {
        guard let r = result(.xcode) else { return started && !checkedOnce ? .working : .todo }
        switch r.code {
        case "xcode-missing", "xcode-too-old": return xcodeRequested ? .working : .attention
        default: return .done
        }
    }

    var xcodeSetupState: StepState {
        guard let r = result(.xcode) else { return .waiting }
        switch r.code {
        case "xcode-missing", "xcode-too-old": return .waiting
        case nil: return r.passes ? .done : .attention
        default: return autoFix?.code == r.code || (xcodeOpenedForSetup && r.code == "xcode-first-launch") ? .working : .attention
        }
    }

    var visionOSState: StepState {
        guard xcodeReady else { return .waiting }
        guard let r = result(.visionOS) else { return .working }
        if r.passes { return .done }
        return autoFix?.code == r.code || r.fix?.kind == .command ? .working : .attention
    }

    // MARK: Apple Account

    var signInState: StepState {
        guard xcodeReady else { return .waiting }
        guard let r = result(.team) else { return .working }
        if r.code == "apple-account-missing" { return guide.isRunning && guide.job == .signIn ? .working : .attention }
        if r.code == "apple-team-not-loaded" { return guide.isRunning && guide.job == .refresh ? .working : .attention }
        return .done
    }

    var programState: StepState {
        guard let r = result(.team), r.code != "apple-account-missing", r.code != "apple-team-not-loaded" else { return .waiting }
        if !r.passes { return .attention }
        return setup.openChoice == nil ? .done : .attention
    }

    /// The account's name in Xcode, once signed in.
    var accountName: String? {
        setup.team?.name ?? result(.team)?.teams.first?.name
    }

    // MARK: Vision Pro

    var pairState: StepState {
        guard xcodeReady else { return .waiting }
        guard let r = result(.headset) else { return .working }
        return r.code == "headset-not-paired" ? .attention : .done
    }

    var developerModeState: StepState {
        guard pairState == .done else { return .waiting }
        guard let r = result(.developerMode) else { return .working }
        return r.passes ? .done : .attention
    }

    /// The paired headset's name, or Apple's generic one.
    var headsetName: String { setup.headsetName ?? "Apple Vision Pro" }

    // MARK: Text

    func title(of step: Step) -> String {
        switch step {
        case .welcome: return mode == .home ? recipe.title : "Welcome"
        case .xcode: return "Xcode"
        case .steam: return "Steam"
        case .account: return "Apple Account"
        case .headset: return "Vision Pro"
        case .install: return "Install"
        }
    }

    /// A few words on a step in the bar at the top, while something's happening there.
    func caption(of step: Step) -> String? {
        switch step {
        case .xcode:
            if let f = autoFix?.fraction, autoFix?.code == "visionos-missing" { return "Adding visionOS · \(Int(f * 100))%" }
            if xcodeInstallState == .working, result(.xcode) != nil { return "Downloading" }
            return nil
        case .steam:
            if download.running { return "Game files · \(Int(download.fraction * 100))%" }
            return nil
        case .install:
            if installing { return "\(Int(progress.overall * 100))%" }
            return nil
        default:
            return nil
        }
    }
}
