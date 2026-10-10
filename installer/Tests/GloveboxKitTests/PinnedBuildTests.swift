import XCTest
@testable import GloveboxKit

/// The Mac downloads the build package.json pins (steam.manifests), the one we tested.
final class PinnedBuildTests: XCTestCase {
    func testRecipeReadsThePins() throws {
        let package = URL(fileURLWithPath: #filePath).deletingLastPathComponent()  // Tests/GloveboxKitTests
            .appendingPathComponent("../../../packages/alyx/package.json").standardized
        let r = Recipe.alyx.updated(fromPackage: try Data(contentsOf: package))
        XCTAssertEqual(r.pinnedManifests[546_561], 1_436_897_171_613_240_821)
        XCTAssertEqual(r.pinnedManifests[546_564], 9_198_205_754_520_568_813)
        XCTAssertEqual(Set(r.pinnedManifests.keys), Set(r.depots))
    }

    func testNoPinsMeansTheCurrentBuild() {
        let r = Recipe.alyx.updated(fromPackage: Data(#"{"steam": {"app": 546560}}"#.utf8))
        XCTAssertTrue(r.pinnedManifests.isEmpty)
    }

}

/// Which Xcode Glovebox uses when there are several: the Mac's own, not a newer beta beside it.
final class XcodeChoiceTests: XCTestCase {
    let both = [(path: "/Applications/Xcode.app", version: "27.0"), (path: "/Applications/Xcode beta.app", version: "27.1")]

    func testTheMacsOwnXcodeWinsOverANewerBeta() {
        XCTAssertEqual(XcodeInfo.preferred(both, selected: "/Applications/Xcode.app/Contents/Developer", minMajor: 27)?.path,
                       "/Applications/Xcode.app")
    }

    func testWithoutASelectionTheNewestReleaseWins() {
        XCTAssertEqual(XcodeInfo.preferred(both, selected: "/Library/Developer/CommandLineTools", minMajor: 27)?.path,
                       "/Applications/Xcode.app")
    }

    func testABetaWhenItIsTheOnlyNewEnoughOne() {
        let old = [(path: "/Applications/Xcode.app", version: "26.4"), (path: "/Applications/Xcode-beta.app", version: "27.1")]
        XCTAssertEqual(XcodeInfo.preferred(old, selected: "/Applications/Xcode.app/Contents/Developer", minMajor: 27)?.path,
                       "/Applications/Xcode-beta.app")
    }

    func testAnXcodeInTheTrashOrRenamedIsNotOneToUse() {
        XCTAssertTrue(XcodeInfo.isUsableApp("/Applications/Xcode.app"))
        XCTAssertFalse(XcodeInfo.isUsableApp("/Users/username/.Trash/Xcode.app"))
        XCTAssertFalse(XcodeInfo.isUsableApp("/Volumes/Games/.Trashes/501/Xcode.app"))
        XCTAssertFalse(XcodeInfo.isUsableApp("/private/var/tmp/xcodes.noindex/Xcode_27.app.held"))
    }
}

/// Signed out of Xcode, its preferences still cache the team; the account list is what says who's signed in.
final class XcodeAccountTests: XCTestCase {
    func prefs(accounts: [Any]) -> Data {
        let p: [String: Any] = [
            "IDEProvisioningTeamByIdentifier": ["5F0E-ACCOUNT": [["teamID": "TEAM123456", "teamName": "Jane", "isFreeProvisioningTeam": false]]],
            "DVTDeveloperAccountManagerAppleIDLists": ["IDE.Identifiers.Prod": accounts]]
        return try! PropertyListSerialization.data(fromPropertyList: p, format: .xml, options: 0)
    }

    func testSignedOutHasNoTeams() { XCTAssertTrue(XcodeInfo.teams(fromPreferences: prefs(accounts: [])).isEmpty) }
    func testSignedInHasTheTeam() { XCTAssertEqual(XcodeInfo.teams(fromPreferences: prefs(accounts: ["jane"])).map(\.id), ["TEAM123456"]) }
    func testEntriesWithNoNameHaveNoTeams() {
        XCTAssertTrue(XcodeInfo.teams(fromPreferences: prefs(accounts: [""])).isEmpty)
        XCTAssertTrue(XcodeInfo.teams(fromPreferences: prefs(accounts: [["identifier": "", "username": ""]])).isEmpty)
    }
    func testAccountRecordHasTheTeam() {
        XCTAssertEqual(XcodeInfo.teams(fromPreferences: prefs(accounts: [["identifier": "", "username": "jane@example.com"]])).map(\.id), ["TEAM123456"])
    }
}
