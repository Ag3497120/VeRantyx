import XCTest
@testable import SteamLauncherKit

/// A CM that takes everything and never answers.
final class SilentTransport: CMTransport {
    var sent: [Data] = []
    func connect(url: URL) async throws {}
    func send(_ data: Data) async throws { sent.append(data) }
    func receive() async throws -> Data {
        try await Task.sleep(nanoseconds: 120_000_000_000)
        throw TransportError.closed
    }
    func close() {}
}

final class CMRequestTests: XCTestCase {
    /// The launcher gives saves a few seconds at Play; a CM that doesn't answer mustn't hold the game for 30 s.
    func testCancellingStopsTheWaitForAReply() async throws {
        let cm = CMClient(makeTransport: { SilentTransport() })
        try await cm.open(endpoint: "test")
        let call = Task { try await cm.call("Cloud.EnumerateUserFiles#1", CloudEnumerateUserFilesRequest(appID: 546560, startIndex: 0)) }
        try await Task.sleep(nanoseconds: 200_000_000)
        let t0 = Date()
        call.cancel()
        do { _ = try await call.value; XCTFail("no reply ever comes") } catch is CancellationError {}
        XCTAssertLessThan(Date().timeIntervalSince(t0), 2)
        await cm.disconnect()
    }
}
