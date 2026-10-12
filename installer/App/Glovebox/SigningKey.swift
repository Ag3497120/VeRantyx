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
    /// again. Its exit code and what codesign printed (why it failed, for the diagnostics); the signature is
    /// stopped if the task is cancelled.
    static func signOnce(with identity: String) async -> (status: Int32, output: String) {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("glovebox-sign-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: dir) }
        let file = dir.appendingPathComponent("signing-check")
        do {
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            try Data("Glovebox\n".utf8).write(to: file)
        } catch { return (1, "couldn't write the scratch file: \(error.localizedDescription)") }
        let p = Process()
        let out = Pipe()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/codesign")
        p.arguments = ["--force", "--sign", identity, "--timestamp=none", file.path]
        p.standardOutput = out
        p.standardError = out
        p.standardInput = FileHandle.nullDevice
        return await withTaskCancellationHandler {
            await withCheckedContinuation { (c: CheckedContinuation<(status: Int32, output: String), Never>) in
                DispatchQueue.global().async {
                    do { try p.run() } catch { c.resume(returning: (127, "codesign didn't start: \(error.localizedDescription)")); return }
                    Children.add(p)
                    let text = String(decoding: out.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
                    p.waitUntilExit()
                    Children.remove(p)
                    c.resume(returning: (p.terminationStatus, text.trimmingCharacters(in: .whitespacesAndNewlines)))
                }
            }
        } onCancel: {
            if p.isRunning { p.terminate() }
        }
    }

    /// The identity's certificate as the diagnostics need it: its name, when it runs out, and whether macOS
    /// trusts it for code signing (codesign can't use one it doesn't, and then fails without asking). Reads
    /// certificates only, so nothing asks.
    static func describe(identity sha1: String) -> String {
        var found: CFTypeRef?
        guard SecItemCopyMatching([kSecClass as String: kSecClassIdentity, kSecMatchLimit as String: kSecMatchLimitAll,
                                   kSecReturnRef as String: true] as CFDictionary, &found) == errSecSuccess,
              let all = found as? [Any] else { return "certificate \(sha1): not found" }
        var matches = 0, line = "certificate \(sha1): not found"
        for item in all {
            var cert: SecCertificate?
            guard SecIdentityCopyCertificate(item as! SecIdentity, &cert) == errSecSuccess, let cert,
                  let c = Certificate(cert), c.sha1 == sha1 else { continue }
            matches += 1
            var trust: SecTrust?, error: CFError?
            let policy = SecPolicyCreateWithProperties(kSecPolicyAppleCodeSigning, nil)
            let trusted: String
            if let policy, SecTrustCreateWithCertificates(cert, policy, &trust) == errSecSuccess, let trust {
                SecTrustSetNetworkFetchAllowed(trust, false)
                trusted = SecTrustEvaluateWithError(trust, &error)
                    ? "yes" : "no (\(error.map { CFErrorCopyDescription($0) as String } ?? "unknown"))"
            } else {
                trusted = "couldn't check"
            }
            line = "certificate \(c.name), until \(c.until), trusted for code signing: \(trusted)"
        }
        // codesign can refuse an identity that's in two keychains at once ("ambiguous").
        return matches > 1 ? line + "; in \(matches) keychains" : line
    }

    /// Apple's developer intermediate that issued the identity's certificate, when this Mac doesn't have it: its
    /// generation ("G3"). codesign then can't build the chain and fails before macOS asks anything (AppleIntermediate,
    /// glovebox-vr/glovebox#4). Reads certificates only, offline.
    static func missingIssuer(identity sha1: String) -> String? {
        guard let cert = certificate(sha1), let issuer = Certificate.issuer(of: cert),
              let generation = AppleIntermediate.generation(issuerCommonName: issuer.cn, unit: issuer.ou) else { return nil }
        return chain([cert]).certificates.count < 2 ? generation : nil
    }

    /// Puts Apple's intermediate on the Mac: downloads it from Apple, checks with macOS's own trust evaluation
    /// (offline) that it signed the identity's certificate and leads to Apple's root, and adds it to the login
    /// keychain by path, since a Mac with no default keychain can't add it by double-click (-25294). A Mac with no
    /// default keychain gets the login keychain back as its default, as macOS sets it up. Asks nothing: adding a
    /// certificate isn't adding a key. What happened, for the signing log.
    static func addIssuer(_ generation: String, identity sha1: String) async -> (ok: Bool, note: String) {
        guard let leaf = certificate(sha1), let url = AppleIntermediate.url(generation: generation) else {
            return (false, "Apple's \(generation) intermediate: certificate \(sha1) not found")
        }
        let data: Data
        do {
            var request = URLRequest(url: url)
            request.timeoutInterval = 30
            let (body, response) = try await URLSession.shared.data(for: request)
            guard (response as? HTTPURLResponse)?.statusCode == 200 else {
                return (false, "Apple's \(generation) intermediate: \(url.absoluteString) answered \((response as? HTTPURLResponse)?.statusCode ?? 0)")
            }
            data = body
        } catch {
            return (false, "Apple's \(generation) intermediate: couldn't download \(url.absoluteString): \(error.localizedDescription)")
        }
        guard let intermediate = SecCertificateCreateWithData(nil, data as CFData) else {
            return (false, "Apple's \(generation) intermediate: \(url.absoluteString) isn't a certificate")
        }
        let checked = chain([leaf, intermediate])
        guard checked.trusted, checked.certificates.count >= 3,
              SecCertificateCopyData(checked.certificates[1]) as Data == data,
              checked.certificates.last.flatMap({ SecCertificateCopySubjectSummary($0) as String? }) == "Apple Root CA" else {
            return (false, "Apple's \(generation) intermediate from \(url.absoluteString) doesn't lead from certificate \(sha1) to Apple Root CA")
        }
        let login = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Keychains/login.keychain-db").path
        var notes: [String] = []
        let (current, _) = await MacTools.run("/usr/bin/security", ["default-keychain"], developerDir: nil, timeout: 20)
        if current != 0 {
            let (set, said) = await MacTools.run("/usr/bin/security", ["default-keychain", "-s", login], developerDir: nil, timeout: 20)
            notes.append("no default keychain; set it to the login keychain: exit \(set) \(said.trimmingCharacters(in: .whitespacesAndNewlines))")
        }
        let file = FileManager.default.temporaryDirectory.appendingPathComponent("glovebox-AppleWWDRCA\(generation)-\(UUID().uuidString).cer")
        defer { try? FileManager.default.removeItem(at: file) }
        do { try data.write(to: file) } catch {
            return (false, (notes + ["Apple's \(generation) intermediate: couldn't write \(file.path)"]).joined(separator: "; "))
        }
        let (code, said) = await MacTools.run("/usr/bin/security", ["add-certificates", "-k", login, file.path], developerDir: nil, timeout: 30)
        notes.append("added Apple's \(generation) intermediate to the login keychain: exit \(code) \(said.trimmingCharacters(in: .whitespacesAndNewlines))")
        let ok = missingIssuer(identity: sha1) == nil
        if !ok { notes.append("certificate \(sha1) still has no chain") }
        return (ok, notes.joined(separator: "; "))
    }

    /// The identity's certificate, by its SHA-1.
    private static func certificate(_ sha1: String) -> SecCertificate? {
        var found: CFTypeRef?
        guard SecItemCopyMatching([kSecClass as String: kSecClassIdentity, kSecMatchLimit as String: kSecMatchLimitAll,
                                   kSecReturnRef as String: true] as CFDictionary, &found) == errSecSuccess,
              let all = found as? [Any] else { return nil }
        for item in all {
            var cert: SecCertificate?
            if SecIdentityCopyCertificate(item as! SecIdentity, &cert) == errSecSuccess, let cert, Certificate(cert)?.sha1 == sha1 {
                return cert
            }
        }
        return nil
    }

    /// The chain macOS builds from these certificates and what it has (keychains and system roots, no network), and
    /// whether it trusts it as plain X.509.
    private static func chain(_ certs: [SecCertificate]) -> (certificates: [SecCertificate], trusted: Bool) {
        var trust: SecTrust?
        guard SecTrustCreateWithCertificates(certs as CFArray, SecPolicyCreateBasicX509(), &trust) == errSecSuccess, let trust
        else { return ([], false) }
        SecTrustSetNetworkFetchAllowed(trust, false)
        let trusted = SecTrustEvaluateWithError(trust, nil)
        return ((SecTrustCopyCertificateChain(trust) as? [SecCertificate]) ?? [], trusted)
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

        /// Who issued it: the issuer's common name and organizational unit.
        static func issuer(of cert: SecCertificate) -> (cn: String?, ou: String?)? {
            let key = kSecOIDX509V1IssuerName as String
            guard let values = SecCertificateCopyValues(cert, [kSecOIDX509V1IssuerName] as CFArray, nil) as? [String: [String: Any]],
                  let issuer = values[key]?[kSecPropertyKeyValue as String] as? [[String: Any]] else { return nil }
            func field(_ oid: CFString) -> String? {
                issuer.first { ($0[kSecPropertyKeyLabel as String] as? String) == oid as String }?[kSecPropertyKeyValue as String] as? String
            }
            return (field(kSecOIDCommonName), field(kSecOIDOrganizationalUnitName))
        }
    }
}
