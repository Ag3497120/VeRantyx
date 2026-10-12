import XCTest
@testable import SteamLauncherKit

final class CDNClientTests: XCTestCase {
    private func server(_ host: String, https: String) -> ContentServer {
        var s = ContentServer()
        s.host = host
        s.https = https
        return s
    }

    /// Steam's list for China has servers without HTTPS: they're used over HTTP, after any that have it.
    func testServersWithHTTPSComeFirst() {
        let cdn = CDNClient(servers: [server("plain.example", https: "unavailable"),
                                      server("optional.example", https: "optional"),
                                      server("plain2.example", https: "unavailable"),
                                      server("mandatory.example", https: "mandatory")])
        XCTAssertEqual(cdn.baseURLs.map(\.absoluteString),
                       ["https://optional.example/", "https://mandatory.example/",
                        "http://plain.example/", "http://plain2.example/"])
    }

    func testOnlyHTTPServersKeepTheirOrder() {
        let cdn = CDNClient(servers: [server("a.example", https: "unavailable"), server("b.example", https: "unavailable")])
        XCTAssertEqual(cdn.baseURLs.map(\.absoluteString), ["http://a.example/", "http://b.example/"])
    }
}
