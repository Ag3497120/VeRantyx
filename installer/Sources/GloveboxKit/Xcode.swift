import Foundation

/// What the app reads from Xcode and its builds directly, the same way the engine does.
public enum XcodeInfo {
    /// The teams in Xcode's account settings (`defaults export com.apple.dt.Xcode -`), without
    /// loading the account. Paid teams are the ones that aren't free provisioning teams.
    public static func teams(fromPreferences data: Data) -> [Team] {
        guard let prefs = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any],
              let byAccount = prefs["IDEProvisioningTeamByIdentifier"] as? [String: Any] else { return [] }
        // Signing out of Xcode empties its account list but leaves the teams cached: no account, no team.
        // An entry with no name in it (Xcode 16 can leave those) is no account either.
        if let lists = prefs["DVTDeveloperAccountManagerAppleIDLists"] as? [String: Any],
           !lists.values.contains(where: { ($0 as? [Any])?.contains(where: isAccount) ?? false }) {
            return []
        }
        var out: [Team] = []
        for case let list as [[String: Any]] in byAccount.values {
            for t in list {
                guard let id = t["teamID"] as? String, !out.contains(where: { $0.id == id }) else { continue }
                out.append(Team(id: id, name: t["teamName"] as? String ?? id, paid: !(t["isFreeProvisioningTeam"] as? Bool ?? true)))
            }
        }
        return out.sorted { ($0.paid ? 0 : 1, $0.name) < ($1.paid ? 0 : 1, $1.name) }
    }

    /// An entry in Xcode's account list that names someone: a non-empty identifier, or a record with
    /// one in it. A form this doesn't know counts as an account, as every entry did before.
    static func isAccount(_ entry: Any) -> Bool {
        switch entry {
        case let s as String: return !s.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
        case let d as [String: Any]: return d.values.contains(where: isAccount)
        case let a as [Any]: return a.contains(where: isAccount)
        case is NSNumber, is Bool, is Int, is Double, is Date: return false
        default: return true
        }
    }

    /// When a built app's profile (`embedded.mobileprovision`) runs out. The profile is a signed
    /// plist; the plist sits in it as plain text.
    public static func profileExpiry(_ data: Data) -> Date? {
        guard let start = data.range(of: Data("<?xml".utf8)),
              let end = data.range(of: Data("</plist>".utf8), in: start.lowerBound..<data.endIndex),
              let plist = try? PropertyListSerialization.propertyList(from: data[start.lowerBound..<end.upperBound], format: nil)
                as? [String: Any] else { return nil }
        return plist["ExpirationDate"] as? Date
    }

    /// The Xcode to use: the newest one installed, from `CFBundleShortVersionString`s.
    public static func newest(_ candidates: [(path: String, version: String)]) -> (path: String, version: String)? {
        candidates.max { $0.version.compare($1.version, options: .numeric) == .orderedAscending }
    }

    public static func major(_ version: String) -> Int { Int(version.split(separator: ".").first ?? "") ?? 0 }

    /// The Xcode Glovebox uses: the one the Mac already uses (`xcode-select -p`, passed as `selected`) when it's new
    /// enough, else the newest release that is, else the newest beta that is, else the newest. A second, newer Xcode
    /// (a beta) is left alone: its first launch is its own business, not the player's today.
    public static func preferred(_ candidates: [(path: String, version: String)], selected: String?,
                                 minMajor: Int) -> (path: String, version: String)? {
        if let s = selected, let c = candidates.first(where: { s == $0.path || s.hasPrefix($0.path + "/") }),
           major(c.version) >= minMajor {
            return c
        }
        let usable = candidates.filter { major($0.version) >= minMajor }
        return newest(usable.filter { !isBeta($0.path) }) ?? newest(usable) ?? newest(candidates)
    }

    /// Launch Services can still list an Xcode that's no longer one to use: one in the Trash, or one
    /// renamed so it isn't an app any more.
    public static func isUsableApp(_ path: String) -> Bool {
        (path as NSString).pathExtension == "app" && !path.contains("/.Trash/") && !path.contains("/.Trashes/")
    }

    public static func isBeta(_ path: String) -> Bool {
        (path as NSString).lastPathComponent.lowercased().contains("beta")
    }
}
