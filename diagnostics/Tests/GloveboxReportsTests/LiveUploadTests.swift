import XCTest
@testable import GloveboxReports

/// Against the live endpoint, only when asked (it stores a report in the bucket, marked glovebox "uploader-test"):
///   GLOVEBOX_LIVE_UPLOAD=1 GLOVEBOX_TEST_FOZ=<a headset's recorded.foz> swift test --filter LiveUploadTests
/// The app's own path: the Mac's yes from the build's env lines, a first pass sends the recording and the whole hitch
/// lines, a second sends nothing, a finished line goes up alone, and after a newer no on the headset nothing does.
final class LiveUploadTests: XCTestCase {
    func testUploadsOnlyWhatIsNew() async throws {
        let env = ProcessInfo.processInfo.environment
        guard env["GLOVEBOX_LIVE_UPLOAD"] == "1", let foz = env["GLOVEBOX_TEST_FOZ"] else {
            throw XCTSkip("set GLOVEBOX_LIVE_UPLOAD=1 and GLOVEBOX_TEST_FOZ")
        }
        let docs = FileManager.default.temporaryDirectory.appendingPathComponent("gbr-live-\(UUID().uuidString)")
        let klfz = docs.appendingPathComponent("klfz")
        try FileManager.default.createDirectory(at: klfz, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: docs) }
        try FileManager.default.copyItem(at: URL(fileURLWithPath: foz), to: klfz.appendingPathComponent("recorded.foz"))
        let slow = klfz.appendingPathComponent("slow.txt")
        try Data("0123456789abcdef 250.5 12.0\n89abcdef01234567 80 30.5\nfedcba9876543210".utf8).write(to: slow)
        func append(_ s: String) throws {
            let h = try FileHandle(forWritingTo: slow); defer { try? h.close() }
            try h.seekToEnd(); try h.write(contentsOf: Data(s.utf8))
        }

        let now = Int(Date().timeIntervalSince1970)
        let u = ReportUploader(documents: docs, info: .init(glovebox: "uploader-test", kit: "dev"),
                               macAnswer: ConsentAnswer.fromBuild(env: "GLOVEBOX_SHARE_STUTTERS=1\nGLOVEBOX_SHARE_STUTTERS_AT=\(now)\n"))
        XCTAssertEqual(u.consent, .yes)
        let first = await u.uploadIfAllowed()
        XCTAssertTrue(first)
        let s1 = ReportState.load(u.stateURL)
        print("[live] install \(s1.install): \(s1.seq) batches, \(s1.sent.count) entries, hitch log at \(s1.slowOffset)")
        let second = await u.uploadIfAllowed()
        XCTAssertFalse(second, "nothing new: nothing sent")

        try append(" 40 61.0\n")  // the half-written line is finished
        let third = await u.uploadIfAllowed()
        XCTAssertTrue(third)
        let s3 = ReportState.load(u.stateURL)
        XCTAssertEqual(s3.seq, s1.seq + 1)

        try u.setConsent(.no, at: Date(timeIntervalSince1970: Double(now + 1)))
        try append("aaaaaaaaaaaaaaaa 99 70.0\n")
        XCTAssertEqual(u.consent, .no)
        let fourth = await u.uploadIfAllowed()
        XCTAssertFalse(fourth)
        XCTAssertEqual(ReportState.load(u.stateURL).seq, s3.seq, "a no stops uploads")
        print("[live] \(s3.seq) batches accepted by the endpoint; none after the no")
    }
}
