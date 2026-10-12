import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif

/// Whether a newer Glovebox is out than the one the headset app was built from. Glovebox's releases are on GitHub
/// (glovebox-vr/glovebox): the Mac app updates itself from there and then builds the headset app again. The latest
/// one comes from GitHub's API, not from the release's appcast.xml, so headsets don't add to that file's download
/// count (the count of Mac apps checking for updates).
public enum GloveboxRelease {
    public static let latestURL = URL(string: "https://api.github.com/repos/glovebox-vr/glovebox/releases/latest")!

    /// The latest release's version ("v0.1.2" is 0.1.2), from GitHub's answer for `latestURL`.
    public static func version(fromLatestRelease data: Data) -> String? {
        struct Release: Decodable {
            var tag_name: String
            var draft: Bool?
            var prerelease: Bool?
        }
        guard let r = try? JSONDecoder().decode(Release.self, from: data), r.draft != true, r.prerelease != true
        else { return nil }
        let v = r.tag_name.hasPrefix("v") ? String(r.tag_name.dropFirst()) : r.tag_name
        return isRelease(v) ? v : nil
    }

    /// A release's version starts with a number ("0.1.2", "0.1.0-beta.4"); a build from a checkout ("dev") has none.
    public static func isRelease(_ v: String) -> Bool { v.first?.isNumber == true }

    /// Whether release `a` came before `b`, as the Mac app decides (HeadsetStatus.isOlder): numbers compared as
    /// numbers, and a pre-release before its release (0.1.0-beta.4 before 0.1.0).
    public static func isOlder(_ a: String, than b: String) -> Bool {
        func split(_ v: String) -> (String, String?) {
            guard let i = v.firstIndex(of: "-") else { return (v, nil) }
            return (String(v[..<i]), String(v[v.index(after: i)...]))
        }
        let (ar, ap) = split(a), (br, bp) = split(b)
        switch ar.compare(br, options: .numeric) {
        case .orderedAscending: return true
        case .orderedDescending: return false
        case .orderedSame:
            guard let ap else { return false }
            guard let bp else { return true }
            return ap.compare(bp, options: .numeric) == .orderedAscending
        }
    }

    /// `latest` when it's newer than `installed`, else nil.
    public static func newer(_ latest: String?, than installed: String?) -> String? {
        guard let latest, let installed, isRelease(latest), isRelease(installed), isOlder(installed, than: latest)
        else { return nil }
        return latest
    }

    /// Asks GitHub for the latest release's version. Nil when offline, turned away (GitHub answers 60 times an hour
    /// per address without an account) or anything else unexpected: the caller keeps what it knew.
    public static func fetchLatest() async -> String? {
        var req = URLRequest(url: latestURL, cachePolicy: .reloadIgnoringLocalCacheData, timeoutInterval: 15)
        req.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        req.setValue("Glovebox", forHTTPHeaderField: "User-Agent")
        guard let (data, resp) = try? await URLSession.shared.data(for: req),
              (resp as? HTTPURLResponse)?.statusCode == 200 else { return nil }
        return version(fromLatestRelease: data)
    }
}
