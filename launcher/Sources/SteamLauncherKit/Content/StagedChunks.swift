import Foundation

/// One chunk to fetch from the CDN, named the way it is kept on disk until it is unpacked.
public struct ChunkRequest: Codable, Sendable, Hashable {
    /// "<depot>-<chunk id>": the file name in the staging folder.
    public let name: String
    /// The chunk's path on any CDN server.
    public let path: String
    /// Bytes on the wire (the chunk as stored on the CDN), for the system's download scheduler.
    public let bytes: UInt32
}

/// Chunks fetched by the system's background downloader while the launcher was away: the CDN's
/// bytes as sent (still encrypted and compressed), one file per chunk, until the next install run
/// unpacks them into the game files. Unpacking needs the depot key, which never leaves memory.
public struct StagedChunks: Sendable {
    public let folder: URL

    public init(folder: URL) { self.folder = folder }

    /// The staging folder for an install, on the same volume as the game so moves are free.
    public init(installRoot: URL) {
        folder = installRoot.appendingPathComponent(".launcher-staged", isDirectory: true)
    }

    public static func name(depotID: UInt32, _ c: DepotManifest.Chunk) -> String { "\(depotID)-\(c.sha.hex)" }

    public func url(_ name: String) -> URL { folder.appendingPathComponent(name) }

    /// Every staged chunk, read in one directory listing.
    public func names() -> Set<String> {
        Set((try? FileManager.default.contentsOfDirectory(atPath: folder.path)) ?? [])
    }

    public func clear() { try? FileManager.default.removeItem(at: folder) }
}
