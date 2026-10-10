import XCTest
@testable import SteamLauncherKit

/// The headset downloads the same game build as the Mac: GameProfile.halfLifeAlyx's pins are package.json's.
final class PinnedBuildTests: XCTestCase {
    func testHeadsetPinsMatchThePackage() throws {
        let package = URL(fileURLWithPath: #filePath).deletingLastPathComponent()  // Tests/SteamLauncherKitTests
            .appendingPathComponent("../../../packages/alyx/package.json").standardized
        let j = try XCTUnwrap(JSONSerialization.jsonObject(with: Data(contentsOf: package)) as? [String: Any])
        let steam = try XCTUnwrap(j["steam"] as? [String: Any])
        let pins = try XCTUnwrap(steam["manifests"] as? [String: String])
        let fromPackage = Dictionary(uniqueKeysWithValues: pins.map { (UInt32($0.key)!, UInt64($0.value)!) })
        XCTAssertEqual(GameProfile.halfLifeAlyx.pinnedManifests, fromPackage)
        XCTAssertEqual(Set(fromPackage.keys), Set(GameProfile.halfLifeAlyx.depots))
    }
}
