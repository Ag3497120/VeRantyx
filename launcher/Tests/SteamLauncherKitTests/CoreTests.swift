import XCTest
@testable import SteamLauncherKit

final class CoreTests: XCTestCase {
    func testProtoRoundTrip() throws {
        var h = CMsgProtoBufHeader()
        h.steamID = 76561197960287930
        h.clientSessionID = -5
        h.jobIDSource = 42
        h.targetJobName = "Authentication.BeginAuthSessionViaQR#1"
        let back = try CMsgProtoBufHeader(proto: h.encoded())
        XCTAssertEqual(back.steamID, h.steamID)
        XCTAssertEqual(back.clientSessionID, -5)
        XCTAssertEqual(back.jobIDSource, 42)
        XCTAssertEqual(back.targetJobName, h.targetJobName)
    }

    func testSteamAESRoundTrip() throws {
        let key = Data((0..<32).map { UInt8($0) })
        let msg = Data("game/hlvr/maps/a1_intro_world.vpk".utf8)
        let enc = try Codec.steamEncrypt(msg, key: key)
        XCTAssertEqual(try Codec.steamDecrypt(enc, key: key), msg)
    }

    /// Steam sends encrypted names as base64 wrapped every 72 characters, so long paths span
    /// several lines (the "filename base64" error on Alyx). Symlink targets are encrypted the same way.
    func testDecryptsWrappedFilenames() throws {
        let key = Data((0..<32).map { UInt8($0 * 7 &+ 3) })
        func wrapped(_ name: String) throws -> String {
            let b64 = try Codec.steamEncrypt(Data(name.utf8), key: key).base64EncodedString()
            var lines: [Substring] = []
            var rest = Substring(b64)
            while !rest.isEmpty { lines.append(rest.prefix(72)); rest = rest.dropFirst(72) }
            return lines.joined(separator: "\n") + "\n"
        }
        let long = "game\\hlvr\\maps\\a1_intro_world_2\\resourcecompiler_staging\\a1_intro_world_2.vpk"
        let target = "../libexec/steam-runtime-tools-0/aarch64-linux-gnu-srt-launcher-service"
        let encName = try wrapped(long)
        XCTAssertTrue(encName.dropLast().contains("\n"), "the test name must span several lines")
        var m = DepotManifest(depotID: 546561, manifestID: 1, files: [
            .init(name: encName, size: 1, flags: 0, sha: Data(), chunks: []),
            .init(name: try wrapped("bin/srt-logger"), size: 0, flags: 0x200, sha: Data(), chunks: [], linkTarget: try wrapped(target)),
        ], filenamesEncrypted: true)
        try m.decryptFilenames(key: key)
        XCTAssertEqual(m.files.map(\.name), ["bin/srt-logger", long.replacingOccurrences(of: "\\", with: "/")])
        XCTAssertEqual(m.files[0].linkTarget, target)
    }

    /// A VZip (Valve's LZMA wrapper) chunk made with Python's lzma raw LZMA1 encoder.
    func testVZipChunk() throws {
        let vz = Data(base64Encoded: "Vlph3gRyjV0AAAEAACQYSYZ5PgrXgTq7Cp1HdUdheYtk5LdaUbidBI8D855j8unFiqCWeG+FAfD3rkXOjbw6W8f//wNiAADeBHKNyAUAAHp2")!
        let out = try Codec.decompressChunk(vz, expected: 1480)
        XCTAssertEqual(out, Data(String(repeating: "Half-Life: Alyx on Apple Vision Pro. ", count: 40).utf8))
    }

    func testTextKeyValues() throws {
        let text = """
        "appinfo"
        {
            "common" { "name" "Half-Life: Alyx" }
            "depots"
            {
                "546561" { "manifests" { "public" { "gid" "1436897171613240821" } } }
                "branches" { "public" { "buildid" "25487405" } }
            }
        }
        """
        let kv = try KV.parseText(Data(text.utf8))
        XCTAssertEqual(kv[path: "appinfo", "common", "name"]?.string, "Half-Life: Alyx")
        XCTAssertEqual(kv[path: "appinfo", "depots", "546561", "manifests", "public", "gid"]?.uint64, 1436897171613240821)
    }

    func testBinaryKeyValues() throws {
        var d = Data([0x00]) + Data("546560".utf8) + Data([0])
        d += Data([0x00]) + Data("appids".utf8) + Data([0])
        d += Data([0x02]) + Data("0".utf8) + Data([0]) + Data([0x80, 0x55, 0x08, 0x00])
        d += Data([0x08, 0x08, 0x08])
        let kv = try KV.parseBinary(d)
        XCTAssertEqual(kv[path: "546560", "appids", "0"]?.int, 546_176)
    }

    func testJWTSubject() {
        // header.payload.signature with payload {"sub":"76561197960287930","exp":1900000000}
        let payload = Data(#"{"sub":"76561197960287930","exp":1900000000}"#.utf8).base64EncodedString()
            .replacingOccurrences(of: "=", with: "")
        let token = "eyJhbGciOiJFZERTQSJ9.\(payload).sig"
        XCTAssertEqual(SteamID.fromJWT(token), 76561197960287930)
        XCTAssertEqual(SteamID.expiry(ofJWT: token), Date(timeIntervalSince1970: 1_900_000_000))
    }

    func testCloudRootSplit() {
        let s = CloudSync.split("%GameInstall%hlvr/SAVE/quick.sav")
        XCTAssertEqual(s.root, "GameInstall")
        XCTAssertEqual(s.path, "hlvr/SAVE/quick.sav")
        XCTAssertNil(CloudSync.split("remote.vdf").root)
    }

    func testRSAPublicKeyDER() {
        let der = RSA.derRSAPublicKey(modulus: Data([0xC0, 0x01]), exponent: Data([0x01, 0x00, 0x01]))
        XCTAssertEqual(der.hex, "300a0203" + "00c001" + "0203010001")
    }

    /// Something far longer than a Steam password in the field gets its own message, not Security's
    /// "RSAencrypt wrong input size". A 2048-bit key carries up to 245 bytes.
    /// The forms' live check: bytes, not characters, since accents and emoji take more than one.
    func testPasswordFieldLimit() {
        XCTAssertFalse(SteamAuth.passwordTooLong(String(repeating: "a", count: 245)))
        XCTAssertTrue(SteamAuth.passwordTooLong(String(repeating: "a", count: 246)))
        XCTAssertFalse(SteamAuth.passwordTooLong(String(repeating: "é", count: 122)))
        XCTAssertTrue(SteamAuth.passwordTooLong(String(repeating: "é", count: 123)))
    }

    func testPasswordLongerThanTheKeyCarries() {
        let key = String(repeating: "c3", count: 256)
        XCTAssertThrowsError(try RSA.encryptPKCS1(Data(repeating: 0x61, count: 246), modulusHex: key, exponentHex: "010001")) {
            XCTAssertEqual($0 as? SteamError, .passwordTooLong)
        }
        XCTAssertThrowsError(try RSA.encryptPKCS1(Data("pw".utf8), modulusHex: "c001", exponentHex: "010001")) {
            XCTAssertEqual($0 as? SteamError, .badData("RSA key"))
        }
    }

    #if canImport(Security)
    /// A key Steam handed out (GetPasswordRSAPublicKey, Oct 2026): 245 bytes encrypt, as does an empty password.
    func testPasswordEncryptsUpToTheLimit() throws {
        let steam = "be3f1e99ae673537d6dfaa7b421c42a420db090d20bdc86efce1c36a5579c0b008d19481ff4d74db622787c764584bbacb7e9311d67669ad8e9e0c6977340d66a128c787aeff89047e4a554e3a4bae093fa2d74b94d7680c583bfb7a75bdcd4b129ed4269b7fabf7840fefef6e84d3fd718b9e65e36bf2474bfc197a582f1627cf61ee40dccda4ca79f52ed0254d96648779eadeeb7223592d2519bff2fffea5b3f480aead447211dfd99f8a35f51a72eded4cead3e99d03ff872a628fa38037455df401d0fd6e6770653c73da169e810c7fc21a3b2c8883000c59b32888e72619a80ec928f32ac7ac78d0b691056b02d1f7a1e518290e3e0b677c3224fdb11f"
        XCTAssertEqual(try RSA.encryptPKCS1(Data(repeating: 0x61, count: 245), modulusHex: steam, exponentHex: "010001").count, 256)
        XCTAssertEqual(try RSA.encryptPKCS1(Data("pässwörd🎮".utf8), modulusHex: steam, exponentHex: "010001").count, 256)
    }
    #endif
}
