import Foundation
import Security
import SteamLauncherKit

/// The Steam refresh token lives in the Keychain, never in files or UserDefaults.
/// It is revocable from the Steam app or store site ("Deauthorize all devices").
enum Keychain {
    private static let service = "SteamLauncher.credentials"

    private static func query(_ account: String) -> [String: Any] {
        [kSecClass as String: kSecClassGenericPassword,
         kSecAttrService as String: service,
         kSecAttrAccount as String: account]
    }

    static func save(_ creds: SteamCredentials) {
        guard let data = try? JSONEncoder().encode(creds) else { return }
        SecItemDelete(query("current") as CFDictionary)
        var q = query("current")
        q[kSecValueData as String] = data
        q[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        SecItemAdd(q as CFDictionary, nil)
        if let g = creds.guardData { setGuardData(g, for: creds.accountName) }
    }

    static func load() -> SteamCredentials? {
        var q = query("current")
        q[kSecReturnData as String] = true
        var out: AnyObject?
        guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess, let data = out as? Data else { return nil }
        return try? JSONDecoder().decode(SteamCredentials.self, from: data)
    }

    static func clear() { SecItemDelete(query("current") as CFDictionary) }

    /// Steam Guard "remember this device" data, so the next password sign-in skips the code.
    static func guardData(for account: String) -> String? {
        var q = query("guard." + account.lowercased())
        q[kSecReturnData as String] = true
        var out: AnyObject?
        guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess, let d = out as? Data else { return nil }
        return String(data: d, encoding: .utf8)
    }

    private static func setGuardData(_ g: String, for account: String) {
        let key = "guard." + account.lowercased()
        SecItemDelete(query(key) as CFDictionary)
        var q = query(key)
        q[kSecValueData as String] = Data(g.utf8)
        SecItemAdd(q as CFDictionary, nil)
    }
}
