import XCTest
@testable import GloveboxKit

/// Whether codesign may use a signing key without macOS asking, from the key's access (research/signing-probe).
final class SigningTests: XCTestCase {
    func testAKeyXcodeOrXcodebuildAddedSignsWithoutAsking() {
        // Xcode's own routine: the adding process, codesign and productbuild, or every app; partition "apple:".
        XCTAssertEqual(SigningKeyAccess.of(signApps: ["/Applications/Xcode.app/Contents/Developer/usr/bin/xcodebuild",
                                                      "/usr/bin/codesign", "/usr/bin/productbuild"], partitions: ["apple:"]), .ready)
        XCTAssertEqual(SigningKeyAccess.of(signApps: nil, partitions: ["apple:"]), .ready)
        // After Always Allow: codesign's "apple:" is added to the list.
        XCTAssertEqual(SigningKeyAccess.of(signApps: nil, partitions: ["teamid:59GAB85EFG", "apple:"]), .ready)
        // No partition list at all (a keychain made with `security create-keychain`).
        XCTAssertEqual(SigningKeyAccess.of(signApps: ["/usr/bin/codesign"], partitions: nil), .ready)
    }

    func testAKeyAddedByAnythingElseAsksOnce() {
        // Imported with `security import -T /usr/bin/codesign`, or with -A: codesign still asks.
        XCTAssertEqual(SigningKeyAccess.of(signApps: ["/usr/bin/codesign"], partitions: ["apple-tool:"]), .willAsk)
        XCTAssertEqual(SigningKeyAccess.of(signApps: nil, partitions: ["apple-tool:"]), .willAsk)
        // Added by the App Store's Xcode app.
        XCTAssertEqual(SigningKeyAccess.of(signApps: nil, partitions: ["teamid:59GAB85EFG"]), .willAsk)
        XCTAssertEqual(SigningKeyAccess.of(signApps: ["/usr/bin/codesign"], partitions: ["cdhash:1cb994299f5f1bd90ae042f3c684fa29a4f4112b"]), .willAsk)
        // codesign not listed: asks whatever the partition.
        XCTAssertEqual(SigningKeyAccess.of(signApps: ["/Applications/Xcode.app"], partitions: ["apple:"]), .willAsk)
        XCTAssertEqual(SigningKeyAccess.of(signApps: [], partitions: ["apple:"]), .willAsk)
    }

    func testThePartitionListIsAHexPlist() throws {
        let plist = try PropertyListSerialization.data(fromPropertyList: ["Partitions": ["teamid:59GAB85EFG", "apple:"]],
                                                       format: .xml, options: 0)
        let hex = plist.map { String(format: "%02X", $0) }.joined()
        XCTAssertEqual(SigningKeyAccess.partitions(fromDescription: hex), ["teamid:59GAB85EFG", "apple:"])
        XCTAssertNil(SigningKeyAccess.partitions(fromDescription: "not hex"))
        XCTAssertNil(SigningKeyAccess.partitions(fromDescription: "414243"))
    }
}
