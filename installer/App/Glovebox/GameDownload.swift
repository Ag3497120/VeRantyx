import Foundation
import SteamLauncherKit
import GloveboxKit

/// The Mac's share of the game, from Steam: the files the engine converts (program files and
/// shader files) into `<work>/linux`, where the engine looks for them. The rest of the game
/// downloads on the headset. Run again, it fetches only what's missing or changed.
struct GameDownload {
    var recipe: Recipe
    /// `<work>/linux`: depot paths start with `game/`, so the files land in `<work>/linux/game`.
    var root: URL

    func run(_ credentials: SteamCredentials, progress: @escaping @Sendable (Double, Double?) -> Void) async throws {
        let prefixes = recipe.macDownload
        let profile = GameProfile(
            appID: recipe.steamAppID, title: recipe.title, depots: recipe.depots,
            localPath: { path in prefixes.contains(where: { path.hasPrefix($0) }) ? path : nil },
            policy: { _ in .download }, cloudRoots: [:],
            storeURL: recipe.storeURL, heroImageURL: recipe.storeURL, logoURL: recipe.storeURL,
            pinnedManifests: recipe.pinnedManifests)
        let launcher = SteamLauncher(profile: profile, installRoot: root)
        do {
            try await launcher.connect()
            do {
                try await launcher.logOn(credentials)
            } catch SteamError.result(let r, _) where r == .invalidPassword || r == .accessDenied || r == .expired {
                throw EngineError(code: "steam-session-expired", stage: .download, message: "Steam turned the saved session down (\(r)).")
            }
            let plan: SteamLauncher.InstallPlan
            do {
                plan = try await launcher.plan()
            } catch SteamError.result(let r, let what) where r == .accessDenied {
                throw EngineError(code: "steam-not-owned", stage: .download, message: "\(what): \(r)")
            }
            try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
            // Room for the files, plus a little for the verify-and-rename at the end.
            let need = plan.totalBytes + 200_000_000
            if let free = launcher.freeSpace, free < need {
                throw EngineError(code: "disk-low", stage: .download, message: "Needs \(need) bytes, \(free) free.",
                                  data: .object(["need": .number(Double(need)), "free": .number(Double(free))]))
            }
            let started = Date()
            try await launcher.install(plan) { p in
                // Time left only once the speed has settled; the first seconds are mostly connecting.
                progress(p.fraction, Date().timeIntervalSince(started) > 5 ? p.secondsLeft : nil)
            }
            await launcher.signOut()
        } catch let e as EngineError {
            throw e
        } catch is CancellationError {
            throw CancellationError()
        } catch {
            throw EngineError(code: "download-failed", stage: .download, message: "\(error)")
        }
    }

    /// For a stand-in engine (development, CI screenshots): no Steam, just the folders the fake
    /// engine looks for, and a few seconds of progress.
    func simulate(progress: @escaping @Sendable (Double, Double?) -> Void) async throws {
        for i in 1...20 {
            try await Task.sleep(nanoseconds: 120_000_000)
            progress(Double(i) / 20, Double(20 - i) * 6)
        }
        for d in ["game/bin/linuxsteamrtarm64", "game/hlvr/bin/linuxsteamrtarm64"] {
            try FileManager.default.createDirectory(at: root.appendingPathComponent(d), withIntermediateDirectories: true)
        }
    }
}
