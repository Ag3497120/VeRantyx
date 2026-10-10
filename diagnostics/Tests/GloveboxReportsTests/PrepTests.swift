import XCTest
@testable import GloveboxReports

/// "Preparing graphics" before Play rides in the report: whole prewarm.log lines, once each.
final class PrepTests: XCTestCase {
    func testPrewarmLogLines() throws {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("gbr-prep-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let log = dir.appendingPathComponent("prewarm.log")
        try Data("1791244461 182.3 3083 1675 0\n1791244999 0.0 0 0 -2\n1791245".utf8).write(to: log)
        let none = dir.appendingPathComponent("missing")
        func next(_ st: ReportState) throws -> (Report, ReportState) {
            try ReportBuilder.next(recording: none, slowLog: none, prewarmLog: log, state: st,
                                   app: .init(glovebox: "t", kit: "t"), device: .init(model: "t", os: "t"))
        }
        let (r1, s1) = try next(ReportState())
        XCTAssertEqual(r1.prep.count, 2)
        XCTAssertEqual(r1.prep[0].n, 3083); XCTAssertEqual(r1.prep[0].slow, 1675); XCTAssertEqual(r1.prep[0].s, 182.3)
        XCTAssertEqual(r1.prep[1].rc, -2)
        XCTAssertFalse(r1.isEmpty)
        let (r2, _) = try next(s1)
        XCTAssertTrue(r2.prep.isEmpty, "the half-written line waits")
        let h = try FileHandle(forWritingTo: log); try h.seekToEnd(); try h.write(contentsOf: Data("600 1.4 3083 0 0\n".utf8)); try h.close()
        let (r3, _) = try next(s1)
        XCTAssertEqual(r3.prep.map(\.s), [1.4])
    }

    func testOldStateFile() throws {
        let old = #"{"install":"a","seq":3,"sent":[],"slowOffset":10}"#
        let st = try JSONDecoder().decode(ReportState.self, from: Data(old.utf8))
        XCTAssertEqual(st.seq, 3); XCTAssertEqual(st.prepOffset, 0)
    }
}
