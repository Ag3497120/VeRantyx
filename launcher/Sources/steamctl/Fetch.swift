import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif
import SteamLauncherKit

// The two commands the Glovebox engine uses (installer/CLI-CONTRACT.md):
//
//   steamctl signin [account]       password, then Approve in the Steam app; saves the session
//   steamctl fetch --app <id> --depot <id> [--manifest <gid>] [--include <prefix>]... --out <dir> [--json]
//
// `fetch` takes the session from the first stdin line, {"steam":{"account":…,"refreshToken":…}}, when
// stdin isn't a terminal (that's how the engine passes it on), and from `signin`'s file otherwise.
// With --json, stdout carries only contract events: progress while it downloads, one error if it
// fails. Exit code 0 means every file is there and verified; running it again resumes.

/// One line of the engine contract on stdout.
func emit(_ fields: [String: Any]) {
    var f = fields
    f["v"] = 1
    guard let data = try? JSONSerialization.data(withJSONObject: f, options: [.sortedKeys]) else { return }
    FileHandle.standardOutput.write(data + Data("\n".utf8))
}

func stderrLine(_ s: String) { FileHandle.standardError.write(Data((s + "\n").utf8)) }

struct FetchFailure: Error {
    let code: String
    var message: String
    var data: [String: Any] = [:]
}

/// The session handed over on stdin, or the one `signin` saved.
func sessionForFetch() throws -> SteamCredentials {
    if isatty(0) == 0, let line = readLine(strippingNewline: true), !line.isEmpty {
        guard let obj = (try? JSONSerialization.jsonObject(with: Data(line.utf8))) as? [String: Any],
              let steam = obj["steam"] as? [String: Any],
              let account = steam["account"] as? String, let token = steam["refreshToken"] as? String, !token.isEmpty else {
            throw FetchFailure(code: "steam-session-expired", message: "stdin had no Steam session line")
        }
        return SteamCredentials(accountName: account, refreshToken: token, guardData: steam["guardData"] as? String)
    }
    guard let data = try? Data(contentsOf: tokenFile),
          let creds = try? JSONDecoder().decode(SteamCredentials.self, from: data) else {
        throw FetchFailure(code: "steam-session-expired", message: "No Steam session. Run `steamctl signin` first.")
    }
    return creds
}

func fetch(_ args: [String]) async {
    let json = args.contains("--json")
    func value(_ flag: String) -> String? { args.firstIndex(of: flag).flatMap { $0 + 1 < args.count ? args[$0 + 1] : nil } }
    let includes = args.indices.filter { args[$0] == "--include" && $0 + 1 < args.count }.map { args[$0 + 1] }
    guard let appID = value("--app").flatMap(UInt32.init), let depot = value("--depot").flatMap(UInt32.init),
          let out = value("--out") else {
        stderrLine("usage: steamctl fetch --app <id> --depot <id> [--manifest <gid>] [--include <prefix>]... --out <dir> [--json]")
        exit(2)
    }
    let pinned = value("--manifest").flatMap(UInt64.init)

    func fail(_ f: FetchFailure) -> Never {
        if json {
            var e: [String: Any] = ["event": "error", "stage": "download", "code": f.code, "message": f.message]
            if !f.data.isEmpty { e["data"] = f.data }
            emit(e)
        } else {
            stderrLine("error (\(f.code)): \(f.message)")
        }
        exit(1)
    }

    let profile = GameProfile(
        appID: appID, title: "app \(appID)", depots: [depot],
        localPath: { path in includes.isEmpty || includes.contains(where: { path.hasPrefix($0) }) ? path : nil },
        policy: { _ in .download },
        cloudRoots: [:],
        storeURL: URL(string: "https://store.steampowered.com/app/\(appID)/")!,
        heroImageURL: URL(string: "https://store.steampowered.com")!, logoURL: URL(string: "https://store.steampowered.com")!,
        pinnedManifests: pinned.map { [depot: $0] } ?? [:])
    let root = URL(fileURLWithPath: out)
    let launcher = SteamLauncher(profile: profile, installRoot: root)
    launcher.log = { stderrLine("[steam] \($0)") }

    do {
        if args.contains("--anonymous") {
            // CI only: free depots anyone may fetch, to test this command end to end without an account.
            try await launcher.connect()
            try await launcher.logOnAnonymously()
        } else {
            let creds: SteamCredentials
            do { creds = try sessionForFetch() } catch let f as FetchFailure { fail(f) }
            try await launcher.connect()
            do {
                try await launcher.logOn(creds)
            } catch SteamError.result(let r, _) where r == .invalidPassword || r == .accessDenied || r == .expired {
                fail(FetchFailure(code: "steam-session-expired", message: "Steam turned the saved session down (\(r))."))
            }
            guard try await launcher.checkOwnership().owned else {
                fail(FetchFailure(code: "steam-not-owned", message: "This account doesn't own app \(appID)."))
            }
        }
        let plan: SteamLauncher.InstallPlan
        do {
            plan = try await launcher.plan()
        } catch SteamError.result(let r, let what) where r == .accessDenied {
            fail(FetchFailure(code: "steam-not-owned", message: "\(what): \(r)"))
        }
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        // Room for the whole depot slice, plus a little for the verify-and-rename at the end.
        if let free = launcher.freeSpace, free < plan.totalBytes + 200_000_000 {
            fail(FetchFailure(code: "disk-low", message: "Needs \(fmt(plan.totalBytes)), \(fmt(free)) free.",
                              data: ["need": plan.totalBytes + 200_000_000, "free": free]))
        }
        let manifestID = plan.depots.first?.manifest.manifestID ?? 0
        stderrLine("depot \(depot) manifest \(manifestID)\(pinned == nil ? " (current)" : " (pinned)"): \(fmt(plan.totalBytes)) to fetch")

        let started = Date()
        let throttle = Throttle(interval: 0.25)
        let human = Ticker()
        try await launcher.install(plan) { p in
            if json {
                guard throttle.due() else { return }
                var e: [String: Any] = ["event": "progress", "stage": "download", "fraction": p.fraction,
                                        "done": p.doneBytes, "total": p.totalBytes, "unit": "bytes"]
                // Only once the speed has settled; the first seconds are mostly connection setup.
                if Date().timeIntervalSince(started) > 5, let left = p.secondsLeft { e["secondsLeft"] = Int(left.rounded()) }
                emit(e)
            } else if human.due() {
                print("  \(Int(p.fraction * 100))%  \(fmt(p.doneBytes)) / \(fmt(p.totalBytes))  \(fmt(UInt64(p.bytesPerSecond)))/s")
            }
        }
        if json {
            emit(["event": "progress", "stage": "download", "fraction": 1.0, "done": plan.totalBytes, "total": plan.totalBytes, "unit": "bytes"])
        } else {
            print("fetched depot \(depot) manifest \(manifestID) into \(root.path)")
        }
        await launcher.signOut()
    } catch let f as FetchFailure {
        fail(f)
    } catch {
        fail(FetchFailure(code: "download-failed", message: "\(error)"))
    }
}

/// `steamctl signin`: the Terminal version of the launcher's sign-in. The password goes only to
/// Steam (RSA-encrypted with Steam's key); the saved file holds the refresh token, never the password.
func signIn(_ args: [String]) async throws {
    var account = args.dropFirst().first ?? ""
    if account.isEmpty {
        print("Steam account name: ", terminator: "")
        account = readLine()?.trimmingCharacters(in: .whitespaces) ?? ""
    }
    guard !account.isEmpty, let pw = getpass("Password: "), case let password = String(cString: pw), !password.isEmpty else {
        stderrLine("Nothing entered.")
        exit(2)
    }
    let saved = (try? Data(contentsOf: tokenFile)).flatMap { try? JSONDecoder().decode(SteamCredentials.self, from: $0) }
    let cm = CMClient()
    try await cm.connect()
    let device = "Glovebox on \(SteamAuth.deviceName())"
    let session = try await SteamAuth.beginCredentials(cm: cm, accountName: account, password: password,
                                                       guardData: saved?.accountName == account ? saved?.guardData : nil,
                                                       deviceName: device)
    switch session.prompt {
    case .approveInApp:
        print("Approve the sign-in in the Steam app on your phone.")
    case .deviceCode, .emailCode:
        print(session.prompt == .deviceCode ? "Code from the Steam app: " : "Code Steam emailed you: ", terminator: "")
        try await session.submit(code: readLine()?.trimmingCharacters(in: .whitespaces) ?? "")
    case .qr:
        break
    }
    var creds = try await session.waitForApproval()
    if creds.accountName.isEmpty { creds.accountName = account }
    if creds.guardData == nil { creds.guardData = saved?.accountName == account ? saved?.guardData : nil }
    try JSONEncoder().encode(creds).write(to: tokenFile, options: .atomic)
    try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: tokenFile.path)
    print("Signed in as \(creds.accountName). Session saved to \(tokenFile.path).")
    await cm.logOff()
}

final class Throttle: @unchecked Sendable {
    private var last = Date.distantPast
    private let interval: TimeInterval
    private let lock = NSLock()
    init(interval: TimeInterval) { self.interval = interval }
    func due() -> Bool {
        lock.lock(); defer { lock.unlock() }
        guard Date().timeIntervalSince(last) >= interval else { return false }
        last = Date()
        return true
    }
}
