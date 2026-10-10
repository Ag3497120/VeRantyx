import XCTest
@testable import GloveboxKit

/// The engine's logs: the checks every few seconds must not push out the last install's.
final class LogTests: XCTestCase {
    func testChecksDontPushOutTheInstallsLogs() throws {
        let fm = FileManager.default
        let folder = fm.temporaryDirectory.appendingPathComponent("glovebox-logs-\(UUID().uuidString)")
        try fm.createDirectory(at: folder, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: folder) }
        // An install an hour ago (12 builds before it), then 40 checks since.
        let now = Date()
        func write(_ name: String, minutesAgo: Double) throws {
            let url = folder.appendingPathComponent(name)
            try Data("x".utf8).write(to: url)
            try fm.setAttributes([.modificationDate: now.addingTimeInterval(-minutesAgo * 60)], ofItemAtPath: url.path)
        }
        for i in 0..<12 { try write("build-202610072\(String(format: "%05d", i)).log", minutesAgo: 120 - Double(i)) }
        try write("install-20261007-225512.log", minutesAgo: 60)
        for i in 0..<40 { try write("doctor-20261007-23\(String(format: "%04d", i)).log", minutesAgo: 40 - Double(i)) }
        try write("notes.txt", minutesAgo: 500)

        Engine.pruneLogs(in: folder)

        let left = try fm.contentsOfDirectory(atPath: folder.path)
        XCTAssertEqual(left.filter { $0.hasPrefix("doctor-") }.count, 30)
        XCTAssertEqual(left.filter { $0.hasPrefix("build-") }.count, 10)
        XCTAssertTrue(left.contains("install-20261007-225512.log"))
        // The newest of each kind stay.
        XCTAssertTrue(left.contains("doctor-20261007-230039.log"))
        XCTAssertFalse(left.contains("doctor-20261007-230000.log"))
        XCTAssertTrue(left.contains("build-20261007200011.log"))
        XCTAssertFalse(left.contains("build-20261007200000.log"))
        XCTAssertTrue(left.contains("notes.txt"))
    }

    /// A tester's diagnostics showed the checks' log under "Last log lines" for a build's error: the checks run every
    /// few seconds, so theirs was the newest.
    func testDiagnosticsShowTheFailedRunsLog() throws {
        let fm = FileManager.default
        let folder = fm.temporaryDirectory.appendingPathComponent("glovebox-logs-\(UUID().uuidString)")
        try fm.createDirectory(at: folder, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: folder) }
        let build = folder.appendingPathComponent("build-20261008-024000.log")
        let doctor = folder.appendingPathComponent("doctor-20261008-024100.log")
        try Data("step app\nerror: Unable to process request\nexit 1\n".utf8).write(to: build)
        try Data("check team ok\n".utf8).write(to: doctor)
        try fm.setAttributes([.modificationDate: Date().addingTimeInterval(-60)], ofItemAtPath: build.path)

        let failed = EngineError(code: "apple-agreement-pending", stage: .build, log: build.path)
        XCTAssertEqual(DiagnosticsReport.logTail(for: failed, in: folder).first, "(build-20261008-024000.log)")
        XCTAssertTrue(DiagnosticsReport.logTail(for: failed, in: folder).contains("error: Unable to process request"))
        // No run named (or its log pruned): the newest log, as before.
        XCTAssertEqual(DiagnosticsReport.logTail(for: nil, in: folder).first, "(doctor-20261008-024100.log)")
        let gone = EngineError(code: "internal", log: folder.appendingPathComponent("build-old.log").path)
        XCTAssertEqual(DiagnosticsReport.logTail(for: gone, in: folder).first, "(doctor-20261008-024100.log)")
    }
}
