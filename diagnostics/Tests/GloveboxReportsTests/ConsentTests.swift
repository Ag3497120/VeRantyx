import XCTest
@testable import GloveboxReports

final class ConsentTests: XCTestCase {
    var docs: URL!

    override func setUpWithError() throws {
        docs = FileManager.default.temporaryDirectory.appendingPathComponent("gbr-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: docs, withIntermediateDirectories: true)
    }
    override func tearDownWithError() throws { try? FileManager.default.removeItem(at: docs) }

    func testBuildEnv() {
        let env = "KL_VK_FOZ_RECORD=auto\nGLOVEBOX_SHARE_STUTTERS=1\nGLOVEBOX_SHARE_STUTTERS_AT=1791000000\n"
        XCTAssertEqual(ConsentAnswer.fromBuild(env: env), ConsentAnswer(answer: .yes, at: 1791000000))
        XCTAssertEqual(ConsentAnswer.fromBuild(env: "GLOVEBOX_SHARE_STUTTERS=0\n"), ConsentAnswer(answer: .no, at: 0))
        XCTAssertNil(ConsentAnswer.fromBuild(env: "KL_VK_FOZ_RECORD=auto\n"))
        XCTAssertNil(ConsentAnswer.fromBuild(env: "GLOVEBOX_SHARE_STUTTERS=maybe\n"))
    }

    func testNotAnsweredAsks() {
        XCTAssertEqual(ReportUploader(documents: docs, info: .init(), macAnswer: nil).consent, .ask)
    }

    func testMacAnswerAloneApplies() {
        let u = ReportUploader(documents: docs, info: .init(), macAnswer: .init(answer: .yes, at: 100))
        XCTAssertEqual(u.consent, .yes)
    }

    func testNewestWins() throws {
        let u = ReportUploader(documents: docs, info: .init(), macAnswer: .init(answer: .yes, at: 100))
        try u.setConsent(.no, at: Date(timeIntervalSince1970: 200))
        XCTAssertEqual(u.consent, .no)
        // a rebuild after a change on the Mac carries a newer time
        let rebuilt = ReportUploader(documents: docs, info: .init(), macAnswer: .init(answer: .yes, at: 300))
        XCTAssertEqual(rebuilt.consent, .yes)
        // an older headset answer loses to the build's
        try rebuilt.setConsent(.no, at: Date(timeIntervalSince1970: 250))
        XCTAssertEqual(rebuilt.consent, .yes)
    }

    func testHeadsetFileAndForget() throws {
        let u = ReportUploader(documents: docs, info: .init(), macAnswer: .init(answer: .no, at: 100))
        try u.setConsent(.yes, at: Date(timeIntervalSince1970: 150))
        let d = try Data(contentsOf: docs.appendingPathComponent("klfz/report-consent.json"))
        let j = try JSONSerialization.jsonObject(with: d) as? [String: Any]
        XCTAssertEqual(j?["answer"] as? Bool, true)
        XCTAssertEqual(j?["at"] as? Double, 150)
        XCTAssertEqual(u.consent, .yes)
        try u.setConsent(.ask)
        XCTAssertEqual(u.consent, .no)
    }

    func testReadsLauncherFile() throws {
        let u = ReportUploader(documents: docs, info: .init(), macAnswer: .init(answer: .yes, at: 100))
        try FileManager.default.createDirectory(at: docs.appendingPathComponent("klfz"), withIntermediateDirectories: true)
        try Data(#"{"answer":false,"at":100.5}"#.utf8).write(to: docs.appendingPathComponent("klfz/report-consent.json"))
        XCTAssertEqual(u.consent, .no)
        // same second as the Mac's answer: the Mac's stands, as in the launcher
        try Data(#"{"answer":false,"at":100}"#.utf8).write(to: docs.appendingPathComponent("klfz/report-consent.json"))
        XCTAssertEqual(u.consent, .yes)
    }

    func testNoUploadWithoutYes() async {
        let u = ReportUploader(documents: docs, info: .init(endpoint: "http://127.0.0.1:9/never"), macAnswer: nil)
        let sent = await u.uploadIfAllowed()
        XCTAssertFalse(sent)
    }
}
