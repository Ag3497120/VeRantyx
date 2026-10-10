import CryptoKit
import Foundation
import GloveboxKit
import Security

/// The team's signing key, read the way `security find-identity` does: certificates and the key's access
/// list only, never the key itself or a password, so macOS shows nothing. See SigningKeyAccess.
enum SigningKey {
    /// Whether codesign may use the identity a build signs with: the team's newest valid "Apple Development"
    /// one, as the engine picks it. `identity`: its certificate's SHA-1.
    static func check(team: String) -> (access: SigningKeyAccess, identity: String?) {
        var found: CFTypeRef?
        let status = SecItemCopyMatching([kSecClass as String: kSecClassIdentity, kSecMatchLimit as String: kSecMatchLimitAll,
                                          kSecReturnRef as String: true] as CFDictionary, &found)
        if status == errSecItemNotFound { return (.noKey, nil) }
        guard status == errSecSuccess, let all = found as? [Any] else { return (.unknown, nil) }
        var newest: (made: Date, identity: SecIdentity, sha1: String)?
        for item in all {
            let identity = item as! SecIdentity
            var cert: SecCertificate?
            guard SecIdentityCopyCertificate(identity, &cert) == errSecSuccess, let cert, let c = Certificate(cert),
                  c.team == team, c.name.hasPrefix("Apple Development"), c.until > Date() else { continue }
            if newest.map({ c.made > $0.made }) ?? true { newest = (c.made, identity, c.sha1) }
        }
        guard let newest else { return (.noKey, nil) }
        var key: SecKey?, access: SecAccess?
        guard SecIdentityCopyPrivateKey(newest.identity, &key) == errSecSuccess, let key,
              SecKeychainItemCopyAccess(unsafeBitCast(key, to: SecKeychainItem.self), &access) == errSecSuccess, let access
        else { return (.unknown, newest.sha1) }
        return (SigningKeyAccess.of(signApps: signApps(access), partitions: partitions(access)), newest.sha1)
    }

    /// One signature with the identity, alone. This is what makes macOS ask, once, when it's going to; the
    /// build's ~94 signatures at once would each ask. It signs a scratch file that isn't a program: a program
    /// gets a signature per architecture, and Allow (not Always Allow) lets in just one, so the next would ask
    /// again. Its exit code; the signature is stopped if the task is cancelled.
    static func signOnce(with identity: String) async -> Int32 {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("glovebox-sign-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: dir) }
        let file = dir.appendingPathComponent("signing-check")
        do {
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            try Data("Glovebox\n".utf8).write(to: file)
        } catch { return 1 }
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/codesign")
        p.arguments = ["--force", "--sign", identity, "--timestamp=none", file.path]
        p.standardOutput = FileHandle.nullDevice
        p.standardError = FileHandle.nullDevice
        p.standardInput = FileHandle.nullDevice
        return await withTaskCancellationHandler {
            await withCheckedContinuation { c in
                DispatchQueue.global().async {
                    do { try p.run() } catch { c.resume(returning: 127); return }
                    Children.add(p)
                    p.waitUntilExit()
                    Children.remove(p)
                    c.resume(returning: p.terminationStatus)
                }
            }
        } onCancel: {
            if p.isRunning { p.terminate() }
        }
    }

    /// The apps the entries that allow signing list; nil when one lets any app sign.
    private static func signApps(_ access: SecAccess) -> [String]? {
        var list: CFArray?
        guard SecAccessCopyACLList(access, &list) == errSecSuccess, let acls = (list as NSArray?) as? [Any] else { return [] }
        let signing: Set<String> = [kSecACLAuthorizationSign as String, kSecACLAuthorizationAny as String]
        var paths: [String] = []
        for item in acls {
            let acl = item as! SecACL
            let tags = ((SecACLCopyAuthorizations(acl) as NSArray) as? [String]) ?? []
            guard !signing.isDisjoint(with: tags) else { continue }
            var apps: CFArray?, desc: CFString?, prompt = SecKeychainPromptSelector()
            guard SecACLCopyContents(acl, &apps, &desc, &prompt) == errSecSuccess else { continue }
            guard let list = (apps as NSArray?) as? [Any] else { return nil }
            for app in list {
                var data: CFData?
                SecTrustedApplicationCopyData(app as! SecTrustedApplication, &data)
                if let data { paths.append(String(decoding: (data as Data).prefix { $0 != 0 }, as: UTF8.self)) }
            }
        }
        return paths
    }

    /// The partition list; nil when the key has none (a keychain made with `security create-keychain`).
    private static func partitions(_ access: SecAccess) -> [String]? {
        guard let acls = (SecAccessCopyMatchingACLList(access, kSecACLAuthorizationPartitionID) as NSArray?) as? [Any],
              let first = acls.first else { return nil }
        var apps: CFArray?, desc: CFString?, prompt = SecKeychainPromptSelector()
        guard SecACLCopyContents(first as! SecACL, &apps, &desc, &prompt) == errSecSuccess, let desc else { return [] }
        return SigningKeyAccess.partitions(fromDescription: desc as String) ?? []
    }

    /// What a signing certificate says about itself: its name, its team (the subject's OU), when it's valid,
    /// and its SHA-1, which names it to codesign.
    private struct Certificate {
        let name: String, team: String, made: Date, until: Date, sha1: String

        init?(_ cert: SecCertificate) {
            sha1 = Insecure.SHA1.hash(data: SecCertificateCopyData(cert) as Data).map { String(format: "%02X", $0) }.joined()
            let keys = [kSecOIDX509V1SubjectName, kSecOIDX509V1ValidityNotBefore, kSecOIDX509V1ValidityNotAfter] as CFArray
            guard let values = SecCertificateCopyValues(cert, keys, nil) as? [String: [String: Any]],
                  let subject = values[kSecOIDX509V1SubjectName as String]?[kSecPropertyKeyValue as String] as? [[String: Any]],
                  let from = values[kSecOIDX509V1ValidityNotBefore as String]?[kSecPropertyKeyValue as String] as? NSNumber,
                  let to = values[kSecOIDX509V1ValidityNotAfter as String]?[kSecPropertyKeyValue as String] as? NSNumber
            else { return nil }
            func field(_ oid: CFString) -> String? {
                subject.first { ($0[kSecPropertyKeyLabel as String] as? String) == oid as String }?[kSecPropertyKeyValue as String] as? String
            }
            name = field(kSecOIDCommonName) ?? ""
            team = field(kSecOIDOrganizationalUnitName) ?? ""
            made = Date(timeIntervalSinceReferenceDate: from.doubleValue)
            until = Date(timeIntervalSinceReferenceDate: to.doubleValue)
        }
    }
}
