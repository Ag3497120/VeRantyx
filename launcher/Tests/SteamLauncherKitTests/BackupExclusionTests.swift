import XCTest
@testable import SteamLauncherKit

final class BackupExclusionTests: XCTestCase {
    var docs: URL!
    let keeping = ["alyx/assets/hlvr/save", "alyx/assets/.launcher-cloud.json", "alyx/assets/.cloud-backups"]

    override func setUp() {
        docs = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        for f in ["alyx/assets/hlvr/pak01_000.vpk", "alyx/assets/hlvr/pak01_dir.vpk", "alyx/assets/hlvr/save/s5/autosave.sav",
                  "alyx/assets/core/pak01_dir.vpk", "alyx/assets/.launcher-staged/546561-ab", "alyx/assets/.launcher-install.json",
                  "alyx/assets/.launcher-cloud.json", "alyx/assets/.cloud-backups/s0/quick.sav",
                  "klfz/recorded.foz", "klepton-boot.log"] {
            let u = docs.appendingPathComponent(f)
            try! FileManager.default.createDirectory(at: u.deletingLastPathComponent(), withIntermediateDirectories: true)
            try! Data("x".utf8).write(to: u)
        }
    }
    override func tearDown() { try? FileManager.default.removeItem(at: docs) }

    private func relative(_ urls: [URL]) -> Set<String> {
        let base = docs.resolvingSymlinksInPath().path + "/"
        return Set(urls.map { String($0.resolvingSymlinksInPath().path.dropFirst(base.count)) })
    }

    /// The game, the download's staging, the shader cache and the logs go; the saves and the sync's records stay.
    func testMarksEverythingButTheSaves() {
        let out = BackupExclusion.apply(under: docs, keeping: keeping)
        XCTAssertEqual(relative(out), ["alyx/assets/hlvr/pak01_000.vpk", "alyx/assets/hlvr/pak01_dir.vpk", "alyx/assets/core",
                                       "alyx/assets/.launcher-staged", "alyx/assets/.launcher-install.json", "klfz", "klepton-boot.log"])
        // Every launch again: the same answer.
        XCTAssertEqual(relative(BackupExclusion.apply(under: docs, keeping: keeping)), relative(out))
    }

    /// The game's own folder names win over the case written here (the container is case-sensitive).
    func testKeepsAnyCase() {
        let out = BackupExclusion.apply(under: docs, keeping: ["ALYX/Assets/HLVR/SAVE"])
        XCTAssertFalse(relative(out).contains { $0.hasPrefix("alyx/assets/hlvr/save") })
        XCTAssertTrue(relative(out).contains("alyx/assets/hlvr/pak01_000.vpk"))
    }

    #if canImport(Darwin)
    /// The mark is really there: on what goes, not on the saves or the folders on the way to them.
    func testTheMark() throws {
        BackupExclusion.apply(under: docs, keeping: keeping)
        for f in ["alyx/assets/hlvr/pak01_000.vpk", "alyx/assets/core", "alyx/assets/.launcher-staged", "klfz", "klepton-boot.log"] {
            XCTAssertEqual(BackupExclusion.isExcluded(docs.appendingPathComponent(f)), true, f)
        }
        for f in ["alyx", "alyx/assets", "alyx/assets/hlvr", "alyx/assets/hlvr/save", "alyx/assets/hlvr/save/s5/autosave.sav",
                  "alyx/assets/.launcher-cloud.json", "alyx/assets/.cloud-backups"] {
            XCTAssertEqual(BackupExclusion.isExcluded(docs.appendingPathComponent(f)), false, f)
        }
    }
    #endif
}
