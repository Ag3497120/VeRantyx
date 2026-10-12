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

    /// A minute of play per perf.txt line (Klepton's heartbeat), once each; a half-written line waits, and a line with
    /// any other number of fields is not sent.
    func testPerfLogLines() throws {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("gbr-perf-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let log = dir.appendingPathComponent("perf.txt")
        try Data("60 45.0 90.0 0 1210 6790\n1200 29.8 60.0 2 980 7020\n1791246140 1260 29.8 60.0 2 980 7020 3\nbad line\n12".utf8).write(to: log)
        let none = dir.appendingPathComponent("missing")
        func next(_ st: ReportState) throws -> (Report, ReportState) {
            try ReportBuilder.next(recording: none, slowLog: none, perfLog: log, state: st,
                                   app: .init(glovebox: "t", kit: "t"), device: .init(model: "t", os: "t"))
        }
        let (r1, s1) = try next(ReportState())
        XCTAssertEqual(r1.perf.count, 2)
        XCTAssertEqual(r1.perf[0].fps, 45.0); XCTAssertEqual(r1.perf[0].hz, 90.0); XCTAssertEqual(r1.perf[0].avail, 1210)
        XCTAssertEqual(r1.perf[1].s, 1200); XCTAssertEqual(r1.perf[1].heat, 2); XCTAssertEqual(r1.perf[1].foot, 7020)
        XCTAssertFalse(r1.isEmpty)
        let (r2, _) = try next(s1)
        XCTAssertTrue(r2.perf.isEmpty && r2.isEmpty, "the half-written line waits")
        let h = try FileHandle(forWritingTo: log); try h.seekToEnd(); try h.write(contentsOf: Data("60 44.9 90.0 1 1100 6900\n".utf8)); try h.close()
        let (r3, _) = try next(s1)
        XCTAssertEqual(r3.perf.map(\.s), [1260])
        // Only how the game ran goes up: these six, nothing else.
        let sent = try JSONSerialization.jsonObject(with: try JSONEncoder().encode(r3.perf)) as? [[String: Any]]
        XCTAssertEqual(sent?.first.map { Set($0.keys) }, ["s", "fps", "hz", "heat", "avail", "foot"])
    }

    func testOldStateFile() throws {
        let old = #"{"install":"a","seq":3,"sent":[],"slowOffset":10}"#
        let st = try JSONDecoder().decode(ReportState.self, from: Data(old.utf8))
        XCTAssertEqual(st.seq, 3); XCTAssertEqual(st.prepOffset, 0); XCTAssertEqual(st.perfOffset, 0)
    }
}
