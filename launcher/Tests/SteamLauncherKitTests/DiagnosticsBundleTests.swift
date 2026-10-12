import XCTest
@testable import SteamLauncherKit

final class DiagnosticsBundleTests: XCTestCase {
    var docs: URL!
    var out: URL!

    override func setUp() {
        let base = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        docs = base.appendingPathComponent("Documents")
        out = base.appendingPathComponent("zip")
        let files = [
            "klepton-boot.log": "[cp] button element pressed for the first time: \"Button Menu [L]\"\n",
            "alyx/assets/hlvr/console.log": "paused the game\n",
            "klfz/slow.txt": "p 0123456789abcdef 412 ms\n",
            // never shared
            ".steamctl-token.json": "{\"refresh\": \"eyJhbGciOiJFZERTQSJ9.eyJzdWIiOiI3NjU2MTE5OCJ9.c2lnbmF0dXJl\"}",
            "alyx/assets/.launcher-cloud.json": "{}",
            "alyx/assets/hlvr/save/s5/quick.sav": "save",
            "klfz/recorded.foz": "pipelines",
        ]
        for (path, text) in files {
            let u = docs.appendingPathComponent(path)
            try! FileManager.default.createDirectory(at: u.deletingLastPathComponent(), withIntermediateDirectories: true)
            try! Data(text.utf8).write(to: u)
        }
    }
    override func tearDown() { try? FileManager.default.removeItem(at: docs.deletingLastPathComponent()) }

    /// Only the listed files go, under their own names; the sign-in, cloud records, saves and recording stay.
    func testCopiesOnlyTheListedFiles() throws {
        let lines = try DiagnosticsBundle.collect(from: docs, into: out)
        let names = try FileManager.default.contentsOfDirectory(atPath: out.path).sorted()
        XCTAssertEqual(names, ["console.log", "klepton-boot.log", "slow.txt"])
        XCTAssertEqual(try String(contentsOf: out.appendingPathComponent("console.log"), encoding: .utf8), "paused the game\n")
        XCTAssertEqual(lines.count, DiagnosticsBundle.items.count)
        XCTAssertTrue(lines[0].hasPrefix("klepton-boot.log: 66 bytes, written 20"), lines[0])
        XCTAssertTrue(lines.contains { $0.hasPrefix("klepton-crash.log: not on this headset") })
    }

    /// Nothing in the list can reach a private file.
    func testListNamesNoPrivateFile() {
        for item in DiagnosticsBundle.items {
            XCTAssertFalse(item.path.contains(".steamctl") || item.path.contains(".launcher-") || item.path.contains(".cloud")
                           || item.path.hasSuffix(".sav") || item.path.contains(".."), item.path)
        }
    }

    func testBlanksLinkQueriesAndTokens() {
        let text = "[launcher] cloud sync failed: GET https://cdn.example.com/ugc/7/quick.sav?sig=SECRET&exp=1 timed out\n"
                 + "refresh eyJhbGciOiJFZERTQSJ9.eyJzdWIiOiI3NjU2MTE5OCJ9.c2lnbmF0dXJl end\n"
                 + "plain https://glovebox.vision/beta stays\n"
        XCTAssertEqual(DiagnosticsBundle.scrub(text),
                       "[launcher] cloud sync failed: GET https://cdn.example.com/ugc/7/quick.sav?(removed) timed out\n"
                     + "refresh (token removed) end\n"
                     + "plain https://glovebox.vision/beta stays\n")
    }

    /// A long log keeps its start and its end, and says what's missing between them.
    func testLongFilesKeepStartAndEnd() {
        let data = Data((0..<1000).map { UInt8($0 % 251) })
        let out = DiagnosticsBundle.capped(data, cap: 100)
        XCTAssertEqual(out.prefix(20), data.prefix(20))
        XCTAssertEqual(out.suffix(80), data.suffix(80))
        XCTAssertTrue(String(decoding: out, as: UTF8.self).contains("[... 900 bytes left out by Share Diagnostics ...]"))
        XCTAssertEqual(DiagnosticsBundle.capped(data, cap: 1000), data)
    }
}
