import XCTest
@testable import SteamLauncherKit

final class GloveboxReleaseTests: XCTestCase {
    /// GitHub's answer for the latest release, cut down to what's read.
    private func answer(_ tag: String, draft: Bool = false, prerelease: Bool = false) -> Data {
        Data(#"{"tag_name": "\#(tag)", "name": "Glovebox", "draft": \#(draft), "prerelease": \#(prerelease), "assets": []}"#.utf8)
    }

    func testReadsTheLatestVersion() {
        XCTAssertEqual(GloveboxRelease.version(fromLatestRelease: answer("v0.1.2")), "0.1.2")
        XCTAssertEqual(GloveboxRelease.version(fromLatestRelease: answer("0.2.0")), "0.2.0")
        XCTAssertNil(GloveboxRelease.version(fromLatestRelease: answer("v0.1.3", draft: true)))
        XCTAssertNil(GloveboxRelease.version(fromLatestRelease: answer("v0.1.3-beta.1", prerelease: true)))
        XCTAssertNil(GloveboxRelease.version(fromLatestRelease: answer("nightly")))
        XCTAssertNil(GloveboxRelease.version(fromLatestRelease: Data(#"{"message": "API rate limit exceeded"}"#.utf8)))
    }

    func testOnlyANewerReleaseCounts() {
        XCTAssertEqual(GloveboxRelease.newer("0.1.2", than: "0.1.1"), "0.1.2")
        XCTAssertEqual(GloveboxRelease.newer("0.1.10", than: "0.1.9"), "0.1.10")
        XCTAssertEqual(GloveboxRelease.newer("0.1.0", than: "0.1.0-beta.4"), "0.1.0")
        XCTAssertNil(GloveboxRelease.newer("0.1.1", than: "0.1.1"))
        XCTAssertNil(GloveboxRelease.newer("0.1.1", than: "0.1.2"))
        XCTAssertNil(GloveboxRelease.newer("0.1.0-beta.4", than: "0.1.0"))
        // Builds without a release version, and no answer yet.
        XCTAssertNil(GloveboxRelease.newer("0.1.2", than: "dev"))
        XCTAssertNil(GloveboxRelease.newer("0.1.2", than: nil))
        XCTAssertNil(GloveboxRelease.newer(nil, than: "0.1.1"))
    }
}
