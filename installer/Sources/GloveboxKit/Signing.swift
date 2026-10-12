import Foundation

/// Whether macOS lets codesign use a team's signing key without asking: read from the key's access, which
/// takes no password and shows nothing.
///
/// Two things on a key decide it. Its access lists the apps that may sign with it (Xcode's own key routine
/// always lists codesign, or every app). And its partition list, which the login keychain gives a key from
/// the code signature of the process that adds it. codesign is let in only when "apple:" is on that list,
/// which Apple's own system-signed code gives: xcodebuild, swift-frontend, Xcode downloaded from Apple's
/// developer site. The App Store's Xcode app is signed like any App Store app, so a key it adds gets
/// "teamid:59GAB85EFG"; Apple's `security` tool gives "apple-tool:", and other apps "teamid:…" or
/// "cdhash:…". Then codesign asks for the Mac's password, once per signature until Always Allow.
/// (Measured on macOS 27 / Xcode 27, research/signing-probe.)
public enum SigningKeyAccess: String, Equatable, Sendable {
    /// The team has no signing identity on this Mac yet. The build's xcodebuild makes one, which
    /// codesign may use.
    case noKey
    /// codesign may use the key. Nothing will ask.
    case ready
    /// macOS asks once before codesign may use the key.
    case willAsk
    /// Couldn't tell.
    case unknown

    /// - signApps: the apps the key's signing entry lists, nil for any app.
    /// - partitions: the key's partition list, nil when it has none.
    public static func of(signApps: [String]?, partitions: [String]?) -> SigningKeyAccess {
        let listed = signApps.map { $0.contains("/usr/bin/codesign") } ?? true
        let partition = partitions.map { $0.contains("apple:") } ?? true
        return listed && partition ? .ready : .willAsk
    }

    /// The partition list from its access entry's description: a property list, written out in hex.
    public static func partitions(fromDescription hex: String) -> [String]? {
        var bytes = Data(capacity: hex.count / 2)
        var i = hex.startIndex
        while i < hex.endIndex {
            guard let j = hex.index(i, offsetBy: 2, limitedBy: hex.endIndex), let b = UInt8(hex[i..<j], radix: 16) else { return nil }
            bytes.append(b)
            i = j
        }
        guard let plist = try? PropertyListSerialization.propertyList(from: bytes, format: nil) as? [String: Any] else { return nil }
        return plist["Partitions"] as? [String]
    }
}

/// Apple's developer intermediate certificate ("Apple Worldwide Developer Relations Certification Authority", one per
/// generation, G2 to G6), which Xcode normally puts on the Mac. Without the one that issued the team's certificate,
/// codesign can't build the chain and fails before macOS ever asks for the password ("unable to build chain to
/// self-signed root", errSecInternalComponent), and `security find-identity -v` lists no valid identity
/// (glovebox-vr/glovebox#4). Apple publishes each at apple.com/certificateauthority.
public enum AppleIntermediate {
    public static let issuerName = "Apple Worldwide Developer Relations Certification Authority"

    /// The generation ("G3") from a certificate's issuer, when Apple's developer intermediate issued it.
    public static func generation(issuerCommonName cn: String?, unit ou: String?) -> String? {
        guard cn == issuerName, let ou, isGeneration(ou) else { return nil }
        return ou
    }

    /// Where Apple publishes that generation.
    public static func url(generation g: String) -> URL? {
        isGeneration(g) ? URL(string: "https://www.apple.com/certificateauthority/AppleWWDRCA\(g).cer") : nil
    }

    private static func isGeneration(_ s: String) -> Bool {
        s.range(of: #"^G[0-9]{1,2}$"#, options: .regularExpression) != nil
    }
}
