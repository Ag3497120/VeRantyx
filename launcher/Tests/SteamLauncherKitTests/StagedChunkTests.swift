import XCTest
@testable import SteamLauncherKit

/// Chunks the system's background downloader leaves behind are unpacked into the game files.
final class StagedChunkTests: XCTestCase {
    // The VZip fixture from CoreTests and its 1480 plain bytes.
    let vzip = Data(base64Encoded: "Vlph3gRyjV0AAAEAACQYSYZ5PgrXgTq7Cp1HdUdheYtk5LdaUbidBI8D855j8unFiqCWeG+FAfD3rkXOjbw6W8f//wNiAADeBHKNyAUAAHp2")!
    let plain = Data(String(repeating: "Half-Life: Alyx on Apple Vision Pro. ", count: 40).utf8)
    let key = Data((0..<32).map { UInt8(255 - $0) })

    private func fixture() throws -> (root: URL, dl: DepotDownloader, raw: Data, shared: String, other: String) {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("staged-\(UUID().uuidString)")
        let raw = try Codec.steamEncrypt(vzip, key: key)
        let c0 = DepotManifest.Chunk(sha: Codec.sha1(plain), checksum: Codec.adler32Seed0(plain), offset: 0,
                                     size: UInt32(plain.count), compressedSize: UInt32(raw.count))
        let c1 = DepotManifest.Chunk(sha: c0.sha, checksum: c0.checksum, offset: UInt64(plain.count),
                                     size: c0.size, compressedSize: c0.compressedSize)
        let missing = DepotManifest.Chunk(sha: Data(repeating: 0xAB, count: 20), checksum: 1, offset: 0,
                                          size: 100, compressedSize: 90)
        let files = [
            // Two copies of the same chunk in one file: fetched once, written twice.
            DepotManifest.File(name: "game/a.bin", size: UInt64(plain.count * 2), flags: 0, sha: Data(), chunks: [c0, c1]),
            DepotManifest.File(name: "game/b.bin", size: 100, flags: 0, sha: Data(), chunks: [missing]),
        ]
        let manifest = DepotManifest(depotID: 7, manifestID: 99, files: files)
        // No CDN servers: any attempt to fetch over the network fails the test.
        let dl = DepotDownloader(cdn: CDNClient(servers: []), depotID: 7, key: key, manifest: manifest, root: root)
        return (root, dl, raw, StagedChunks.name(depotID: 7, c0), StagedChunks.name(depotID: 7, missing))
    }

    func testRemainingIsOnePerChunkAndSkipsStaged() throws {
        let f = try fixture()
        defer { try? FileManager.default.removeItem(at: f.root) }
        XCTAssertEqual(f.dl.remaining().map(\.name), [f.shared, f.other])
        XCTAssertEqual(f.dl.remaining().first?.path, "depot/7/chunk/\(Codec.sha1(plain).hex)")

        let staged = StagedChunks(installRoot: f.root)
        try FileManager.default.createDirectory(at: staged.folder, withIntermediateDirectories: true)
        try f.raw.write(to: staged.url(f.shared))
        XCTAssertEqual(f.dl.remaining(staged: staged).map(\.name), [f.other])
    }

    func testStagedChunksAreUnpackedWithoutTheNetwork() async throws {
        let f = try fixture()
        defer { try? FileManager.default.removeItem(at: f.root) }
        let staged = StagedChunks(installRoot: f.root)
        try FileManager.default.createDirectory(at: staged.folder, withIntermediateDirectories: true)
        try f.raw.write(to: staged.url(f.shared))

        let last = Box<DownloadProgress>()
        // b.bin's chunk is still with the background downloader, so this run leaves it alone.
        try await f.dl.run(staged: staged, skipping: [f.other]) { last.value = $0 }

        let a = try Data(contentsOf: f.root.appendingPathComponent("game/a.bin"))
        XCTAssertEqual(a, plain + plain)
        XCTAssertFalse(FileManager.default.fileExists(atPath: staged.url(f.shared).path), "unpacked chunks are removed")
        XCTAssertEqual(last.value?.doneBytes, UInt64(plain.count * 2))
        XCTAssertEqual(last.value?.doneFiles, 1)
        // The journal has both copies, so only b.bin's chunk is left.
        XCTAssertEqual(f.dl.remaining(staged: staged).map(\.name), [f.other])
    }

    func testBadStagedChunkIsFetchedAgain() async throws {
        let f = try fixture()
        defer { try? FileManager.default.removeItem(at: f.root) }
        let staged = StagedChunks(installRoot: f.root)
        try FileManager.default.createDirectory(at: staged.folder, withIntermediateDirectories: true)
        try Data("<html>error page</html>".utf8).write(to: staged.url(f.shared))

        do {
            try await f.dl.run(staged: staged, skipping: [f.other]) { _ in }
            XCTFail("a bad staged chunk should be fetched from the CDN, which this test doesn't have")
        } catch SteamError.noServers {
        }
        XCTAssertFalse(FileManager.default.fileExists(atPath: staged.url(f.shared).path))
        XCTAssertEqual(f.dl.remaining(staged: staged).map(\.name), [f.shared, f.other])
    }
}

private final class Box<T>: @unchecked Sendable {
    private let lock = NSLock()
    private var _value: T?
    var value: T? {
        get { lock.lock(); defer { lock.unlock() }; return _value }
        set { lock.lock(); _value = newValue; lock.unlock() }
    }
}
