import Foundation
import Security
import SteamLauncherKit
import GloveboxKit

/// Steam sign-in and the ownership check. First choice: a QR code the Steam app on the phone scans,
/// then Approve, so no password is typed. Else account and password, then Approve in the Steam app,
/// the way the headset launcher signs in. A password goes only to Steam; the Keychain keeps the
/// refresh token, which the engine gets on stdin for the download.
@MainActor
final class SteamAccount: ObservableObject {
    enum Step: Equatable {
        case form
        case working
        case approve(canEnterCode: Bool)
        case code(email: String?)
        case checkingOwnership
        case owned
        case notOwned
    }

    @Published var step: Step = .form { didSet { if step != oldValue { onChange?() } } }
    @Published var error: String? { didSet { if error != oldValue { onChange?() } } }
    @Published var accountName = ""
    enum Method { case qr, password }
    @Published var method: Method = .qr
    /// The QR code's link while it waits for a scan. Steam changes it every so often.
    @Published var qrURL: String?

    let recipe: Recipe
    private var launcher: SteamLauncher
    private var session: SteamAuthSession?
    private var task: Task<Void, Never>?
    private var qrTask: Task<Void, Never>?
    /// The demo shows a made-up code and never talks to Steam.
    var demo = false
    /// Called whenever the sign-in moves along (signed in and owns the game: `step == .owned`).
    var onChange: (@MainActor () -> Void)?

    init(recipe: Recipe) {
        self.recipe = recipe
        launcher = Self.makeLauncher(recipe)
        accountName = SteamKeychain.load()?.accountName ?? ""
    }

    private static func makeLauncher(_ recipe: Recipe) -> SteamLauncher {
        let profile = GameProfile(appID: recipe.steamAppID, title: recipe.title, depots: [], localPath: { _ in nil },
                                  policy: { _ in .skip }, cloudRoots: [:], storeURL: recipe.storeURL,
                                  heroImageURL: recipe.storeURL, logoURL: recipe.storeURL)
        return SteamLauncher(profile: profile, installRoot: FileManager.default.temporaryDirectory)
    }

    /// What the Steam app's Approve screen calls this sign-in.
    static var deviceName: String { "Glovebox on \(MacTools.computerName ?? "Mac")" }

    var hasSession: Bool { SteamKeychain.load() != nil }

    // MARK: QR code

    /// Shows a QR code and waits for the Steam app to scan and approve it. A code that runs out
    /// before anyone scans it is replaced with a fresh one. The Steam page calls it while it shows.
    func startQR() {
        guard !demo, method == .qr, step == .form, qrTask == nil else { return }
        qrTask = Task { await runQR() }
    }

    private func runQR() async {
        while !Task.isCancelled {
            do {
                let s = try await launcher.beginQRSignIn(deviceName: Self.deviceName)
                session = s
                qrURL = s.challengeURL
                let creds = try await s.waitForApproval(
                    onNewChallenge: { url in Task { @MainActor [weak self] in self?.qrURL = url } },
                    onRemoteInteraction: { Task { @MainActor [weak self] in self?.step = .approve(canEnterCode: false) } })
                qrTask = nil
                qrURL = nil
                error = nil
                await finish(creds)
                return
            } catch SteamError.result(let r, _) where r == .expired || r == .fileNotFound {
                // Nobody scanned or approved it in time: a fresh code.
                if Task.isCancelled { return }
                if step != .form { step = .form }
            } catch {
                if Task.isCancelled { return }
                qrTask = nil
                qrURL = nil
                step = .form
                self.error = Self.friendly(error)
                return
            }
        }
    }

    /// Stops waiting for a scan (the page went away, or the player chose the password).
    func stopQR() {
        guard qrTask != nil else { return }
        qrTask?.cancel()
        qrTask = nil
        qrURL = nil
        session = nil
        if case .approve = step { step = .form }
    }

    func usePassword() {
        stopQR()
        error = nil
        method = .password
    }

    func useQR() {
        error = nil
        method = .qr
    }

    // MARK: Account and password

    func signIn(account: String, password: String) {
        stopQR()
        error = nil
        step = .working
        task?.cancel()
        task = Task {
            do {
                let s = try await launcher.beginPasswordSignIn(account: account, password: password,
                                                               guardData: SteamKeychain.guardData(for: account),
                                                               deviceName: Self.deviceName)
                session = s
                accountName = account
                switch s.prompt {
                case .approveInApp(let canEnterCode): step = .approve(canEnterCode: canEnterCode)
                case .deviceCode: step = .code(email: nil)
                case .emailCode(let hint): step = .code(email: hint)
                case .qr: break
                }
                var creds = try await s.waitForApproval()
                if creds.accountName.isEmpty { creds.accountName = account }
                await finish(creds)
            } catch {
                if Task.isCancelled { return }
                step = .form
                self.error = Self.friendly(error)
            }
        }
    }

    func submitCode(_ code: String) {
        guard let s = session else { return }
        Task {
            do { try await s.submit(code: code) } catch { self.error = Self.friendly(error) }
        }
    }

    func useCode() { step = .code(email: nil) }

    func cancel() {
        task?.cancel()
        stopQR()
        session = nil
        step = .form
    }

    /// Checks a saved session still works and still owns the game. False sends the player to sign in.
    func verifySaved() async -> Bool {
        guard let creds = SteamKeychain.load() else { return false }
        await finish(creds)
        return step == .owned
    }

    func signOut() {
        task?.cancel()
        stopQR()
        method = .qr
        SteamKeychain.clear()
        Task { [launcher] in await launcher.signOut() }
        launcher = Self.makeLauncher(recipe)
        accountName = ""
        step = .form
    }

    /// The engine said the session expired: forget it and ask again.
    func sessionExpired() {
        SteamKeychain.clear()
        step = .form
        error = "Sign in again"
    }

    private func finish(_ creds: SteamCredentials) async {
        // A saved session checked while the page shows a code: the code isn't needed any more.
        stopQR()
        step = .checkingOwnership
        do {
            try await launcher.logOn(creds)
            SteamKeychain.save(creds)
            accountName = creds.accountName
            let own = try await launcher.checkOwnership()
            step = own.owned ? .owned : .notOwned
        } catch SteamError.result(let r, _) where r == .invalidPassword || r == .accessDenied || r == .expired {
            SteamKeychain.clear()
            step = .form
            error = "Sign in again"
        } catch {
            step = .form
            self.error = Self.friendly(error)
        }
    }

    static func friendly(_ error: Error) -> String {
        if let e = error as? SteamError, case .result(let r, _) = e {
            if r == .invalidPassword { return "Wrong account name or password" }
            if r == .expired { return "The request expired. Try again." }
            if r == .rateLimitExceeded { return "Too many tries. Wait a few minutes." }
        }
        if (error as? URLError) != nil || (error as? SteamError) == .disconnected || (error as? SteamError) == .notConnected {
            return "Can't reach Steam. Check your internet connection."
        }
        return "Couldn't sign in. Try again."
    }
}

/// The Steam session in the keychain: refresh token and Steam Guard data, never the password.
///
/// - Only release builds (release.sh sets `GloveboxKeychainService`) use "Glovebox Steam session". Every other build,
///   Debug and unsigned test builds included, uses "Glovebox Steam session (dev)", and demos use none, so a test
///   build never reads a player's real session, nor brings up the keychain's access dialog for it.
/// - The data protection keychain (TN3137) when the app is signed with a keychain access group, which a Developer ID
///   app gets from its provisioning profile (release.sh): it has no per-app access dialogs, whatever signs an update.
///   Otherwise the login keychain.
/// - An earlier version's item in the login keychain moves over once, read with dialogs turned off: if it would need
///   one, it stays where it is and the player signs in to Steam again instead.
enum SteamKeychain {
    static var service = (Bundle.main.object(forInfoDictionaryKey: "GloveboxKeychainService") as? String)
        .flatMap { $0.isEmpty ? nil : $0 } ?? "Glovebox Steam session (dev)"
    private static var enabled: Bool { Demo.current == nil }
    /// What deleting the login-keychain copy returned after a move (the self-test reports it).
    static var lastMigrationDelete: OSStatus?
    static let dataProtection: Bool = {
        guard let task = SecTaskCreateFromSelf(nil) else { return false }
        return SecTaskCopyValueForEntitlement(task, "keychain-access-groups" as CFString, nil) != nil
    }()

    fileprivate static func query(_ account: String, legacy: Bool = false) -> [String: Any] {
        var q: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service,
                                kSecAttrAccount as String: account]
        // Explicit both ways: an app with a keychain access group, asking without the key, can land on its data
        // protection item (a delete meant for the login keychain removed the new copy instead).
        if dataProtection { q[kSecUseDataProtectionKeychain as String] = !legacy }
        return q
    }

    fileprivate static func read(_ account: String) -> Data? {
        guard enabled else { return nil }
        var q = query(account)
        q[kSecReturnData as String] = true
        var out: CFTypeRef?
        if SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess, let data = out as? Data { return data }
        guard dataProtection else { return nil }
        // An earlier version's login-keychain item: moved over if it reads without a dialog, else left alone.
        var lq = query(account, legacy: true)
        lq[kSecReturnData as String] = true
        SecKeychainSetUserInteractionAllowed(false)  // through the delete too: never a dialog
        defer { SecKeychainSetUserInteractionAllowed(true) }
        guard SecItemCopyMatching(lq as CFDictionary, &out) == errSecSuccess, let data = out as? Data else { return nil }
        write(account, data)
        lastMigrationDelete = SecItemDelete(query(account, legacy: true) as CFDictionary)
        return data
    }

    fileprivate static func write(_ account: String, _ data: Data, legacy: Bool = false) {
        guard enabled else { return }
        SecItemDelete(query(account, legacy: legacy) as CFDictionary)
        var q = query(account, legacy: legacy)
        q[kSecValueData as String] = data
        q[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        SecItemAdd(q as CFDictionary, nil)
    }

    static func load() -> SteamCredentials? {
        read("current").flatMap { try? JSONDecoder().decode(SteamCredentials.self, from: $0) }
    }

    static func save(_ creds: SteamCredentials) {
        guard let data = try? JSONEncoder().encode(creds) else { return }
        write("current", data)
        if let g = creds.guardData { write("guard.\(creds.accountName)", Data(g.utf8)) }
    }

    /// Steam Guard's "this device is trusted" data, kept after sign-out so the next sign-in is quicker.
    static func guardData(for account: String) -> String? {
        read("guard.\(account)").flatMap { String(data: $0, encoding: .utf8) }
    }

    static func clear() {
        guard enabled else { return }
        SecItemDelete(query("current") as CFDictionary)
        // A login-keychain copy left behind by a move (deleting it can need a dialog, so it's only tried silently)
        if dataProtection {
            SecKeychainSetUserInteractionAllowed(false)
            SecItemDelete(query("current", legacy: true) as CFDictionary)
            SecKeychainSetUserInteractionAllowed(true)
        }
    }
}

/// `-keychain-selftest write|read|legacy-write|clean`: the keychain path a release build takes, on a throwaway item
/// ("Glovebox keychain self-test"), never the Steam session. Prints one line and quits before anything else starts.
/// release checks: does the data protection keychain work with no dialog, does an update read what the last build
/// wrote, does a login-keychain item move over silently.
enum KeychainSelfTest {
    static func runIfAsked() {
        let args = CommandLine.arguments
        guard let i = args.firstIndex(of: "-keychain-selftest"), i + 1 < args.count else { return }
        SteamKeychain.service = "Glovebox keychain self-test"
        let build = Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "?"
        let line: String
        switch args[i + 1] {
        case "write": SteamKeychain.selfTest(write: "written by build \(build)"); line = "wrote"
        case "legacy-write": SteamKeychain.selfTest(write: "written to the login keychain by build \(build)", legacy: true)
            line = "wrote to the login keychain"
        case "read":
            line = (SteamKeychain.selfTestRead().map { "read: \($0)" } ?? "read: nothing")
                + (SteamKeychain.lastMigrationDelete.map { "; moved from the login keychain (delete \($0))" } ?? "")
        case "clean": SteamKeychain.selfTestClean(); line = "cleaned"
        default: line = "unknown self-test"
        }
        print("keychain self-test (build \(build), data protection keychain: \(SteamKeychain.dataProtection)): \(line)")
        exit(0)
    }
}

extension SteamKeychain {
    static func selfTest(write value: String, legacy: Bool = false) { write("selftest", Data(value.utf8), legacy: legacy) }
    static func selfTestRead() -> String? { read("selftest").flatMap { String(data: $0, encoding: .utf8) } }
    static func selfTestClean() {
        SecItemDelete(query("selftest") as CFDictionary)
        SecItemDelete(query("selftest", legacy: true) as CFDictionary)
    }
}

