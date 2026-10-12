import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif

/// Steam's content delivery network (SteamPipe): plain GETs for manifests and chunks, over HTTPS where the server
/// has it. Some have none (Steam's list for China, say): those are fetched over HTTP, which both apps allow
/// (NSAppTransportSecurity), as Steam's own client does. A chunk is encrypted with the depot key and checked
/// after decrypting, so a changed one fails its check instead of landing on disk.
public final class CDNClient: @unchecked Sendable {
    public let servers: [ContentServer]
    private let session: URLSession
    private let lock = NSLock()
    private var penalty: [String: Int] = [:]

    public init(servers: [ContentServer]) {
        self.servers = servers
        let cfg = URLSessionConfiguration.default
        cfg.httpMaximumConnectionsPerHost = 16
        cfg.timeoutIntervalForRequest = 30
        cfg.requestCachePolicy = .reloadIgnoringLocalCacheData
        cfg.urlCache = nil
        self.session = URLSession(configuration: cfg)
    }

    private func host(_ s: ContentServer) -> String { s.vhost.isEmpty ? s.host : s.vhost }

    private static func scheme(_ s: ContentServer) -> String { s.https == "unavailable" ? "http" : "https" }

    /// Servers in order of preference: ones that recently failed to the back, and HTTPS ones ahead of those
    /// without it.
    private func ranked() -> [ContentServer] {
        lock.lock(); defer { lock.unlock() }
        func key(_ e: (offset: Int, element: ContentServer)) -> (Int, Int, Int) {
            (penalty[host(e.element)] ?? 0, Self.scheme(e.element) == "https" ? 0 : 1, e.offset)
        }
        return servers.enumerated().sorted { key($0) < key($1) }.map(\.element)
    }

    private func mark(_ s: ContentServer, ok: Bool) {
        lock.lock(); defer { lock.unlock() }
        let h = host(s)
        penalty[h] = ok ? max(0, (penalty[h] ?? 0) - 1) : (penalty[h] ?? 0) + 2
    }

    private func get(_ path: String, attempts: Int = 5) async throws -> Data {
        var lastError: Error = SteamError.noServers
        let list = ranked()
        guard !list.isEmpty else { throw SteamError.noServers }
        for n in 0..<attempts {
            let s = list[n % list.count]
            guard let url = URL(string: "\(Self.scheme(s))://\(host(s))/\(path)") else { continue }
            do {
                let (data, resp) = try await session.data(from: url)
                let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
                guard code == 200 else {
                    mark(s, ok: false)
                    lastError = SteamError.badData("CDN \(host(s)) answered \(code) for \(path)")
                    // 401/403/404 are not server trouble: another server gives the same answer.
                    if code == 401 || code == 403 || code == 404, n >= 1 { break }
                    continue
                }
                mark(s, ok: true)
                return data
            } catch {
                mark(s, ok: false)
                lastError = error
            }
        }
        throw lastError
    }

    public func manifest(depotID: UInt32, manifestID: UInt64, requestCode: UInt64, depotKey: Data?) async throws -> DepotManifest {
        let path = requestCode > 0
            ? "depot/\(depotID)/manifest/\(manifestID)/5/\(requestCode)"
            : "depot/\(depotID)/manifest/\(manifestID)/5"
        let zipped = try await get(path)
        var m = try DepotManifest(binary: Codec.unzipSingle(zipped))
        if let depotKey { try m.decryptFilenames(key: depotKey) }
        return m
    }

    /// Downloads, decrypts, decompresses and verifies one chunk.
    public func chunk(depotID: UInt32, _ c: DepotManifest.Chunk, key: Data) async throws -> Data {
        var lastError: Error = SteamError.noServers
        for _ in 0..<3 {
            do {
                return try Self.decode(await get(Self.chunkPath(depotID: depotID, c)), c, key: key)
            } catch {
                lastError = error
            }
        }
        throw lastError
    }

    /// Where a chunk lives on any CDN server.
    public static func chunkPath(depotID: UInt32, _ c: DepotManifest.Chunk) -> String {
        "depot/\(depotID)/chunk/\(c.sha.hex)"
    }

    /// Turns a chunk exactly as the CDN sent it into the file bytes: decrypt, decompress, verify.
    public static func decode(_ raw: Data, _ c: DepotManifest.Chunk, key: Data) throws -> Data {
        let plain = try Codec.decompressChunk(Codec.steamDecrypt(raw, key: key), expected: Int(c.size))
        guard plain.count == Int(c.size), Codec.adler32Seed0(plain) == c.checksum else {
            throw SteamError.badData("chunk \(c.sha.hex) failed its checksum")
        }
        return plain
    }

    /// Server base URLs, best first, for downloads made outside this client (the system's
    /// background downloader).
    public var baseURLs: [URL] {
        ranked().compactMap { s in URL(string: "\(Self.scheme(s))://\(host(s))/") }
    }
}
