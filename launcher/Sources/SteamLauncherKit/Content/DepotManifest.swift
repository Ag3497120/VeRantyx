import Foundation

/// A depot manifest: the file list of one depot build, with each file split into chunks
/// that are content-addressed by SHA-1 on Steam's CDN.
public struct DepotManifest: Sendable {
    public struct Chunk: Sendable, Hashable {
        public let sha: Data          // chunk ID on the CDN
        public let checksum: UInt32   // Adler-32 (seed 0) of the uncompressed bytes
        public let offset: UInt64
        public let size: UInt32
        public let compressedSize: UInt32
    }

    public struct File: Sendable {
        public var name: String
        public let size: UInt64
        public let flags: UInt32
        public let sha: Data
        public let chunks: [Chunk]
        public var linkTarget: String?

        public var isDirectory: Bool { flags & 0x40 != 0 }
        public var isExecutable: Bool { flags & 0x20 != 0 }
        public var isSymlink: Bool { flags & 0x200 != 0 }
    }

    public let depotID: UInt32
    public let manifestID: UInt64
    public let creationTime: Date
    public private(set) var filenamesEncrypted: Bool
    public let totalSize: UInt64
    public let compressedSize: UInt64
    public private(set) var files: [File]

    static let payloadMagic: UInt32 = 0x71F6_17D0
    static let metadataMagic: UInt32 = 0x1F48_12BE
    static let signatureMagic: UInt32 = 0x1B81_B817
    static let endMagic: UInt32 = 0x32C4_15AB

    /// A manifest built in memory (tests).
    init(depotID: UInt32, manifestID: UInt64, files: [File], filenamesEncrypted: Bool = false) {
        self.depotID = depotID
        self.manifestID = manifestID
        self.creationTime = Date()
        self.filenamesEncrypted = filenamesEncrypted
        self.totalSize = files.reduce(0) { $0 + $1.size }
        self.compressedSize = 0
        self.files = files
    }

    /// Parses the raw manifest (after unzipping the CDN response).
    public init(binary d: Data) throws {
        var i = 0
        func u32() throws -> UInt32 {
            guard i + 4 <= d.count else { throw SteamError.badData("manifest truncated") }
            let s = d.startIndex + i
            i += 4
            return UInt32(d[s]) | UInt32(d[s + 1]) << 8 | UInt32(d[s + 2]) << 16 | UInt32(d[s + 3]) << 24
        }
        var payload = Data(), metadata = Data()
        while i < d.count {
            let magic = try u32()
            if magic == Self.endMagic { break }
            let len = Int(try u32())
            guard i + len <= d.count else { throw SteamError.badData("manifest section") }
            let section = d.subdata(in: (d.startIndex + i)..<(d.startIndex + i + len))
            i += len
            switch magic {
            case Self.payloadMagic: payload = section
            case Self.metadataMagic: metadata = section
            case Self.signatureMagic: break
            default: throw SteamError.badData("manifest magic \(String(magic, radix: 16))")
            }
        }

        var depot: UInt32 = 0, gid: UInt64 = 0, created: UInt32 = 0, encrypted = false, total: UInt64 = 0, comp: UInt64 = 0
        try ProtoReader.fields(metadata) { f in
            switch f.number {
            case 1: depot = f.uint32
            case 2: gid = f.uint64
            case 3: created = f.uint32
            case 4: encrypted = f.bool
            case 5: total = f.uint64
            case 6: comp = f.uint64
            default: break
            }
        }
        var files: [File] = []
        try ProtoReader.fields(payload) { f in
            guard f.number == 1 else { return }
            var name = "", size: UInt64 = 0, flags: UInt32 = 0, sha = Data(), chunks: [Chunk] = [], link: String?
            try ProtoReader.fields(f.bytes) { g in
                switch g.number {
                case 1: name = g.string
                case 2: size = g.uint64
                case 3: flags = g.uint32
                case 5: sha = g.bytes
                case 7: link = g.string
                case 6:
                    var csha = Data(), crc: UInt32 = 0, off: UInt64 = 0, orig: UInt32 = 0, cmp: UInt32 = 0
                    try ProtoReader.fields(g.bytes) { h in
                        switch h.number {
                        case 1: csha = h.bytes
                        case 2: crc = h.uint32
                        case 3: off = h.uint64
                        case 4: orig = h.uint32
                        case 5: cmp = h.uint32
                        default: break
                        }
                    }
                    chunks.append(Chunk(sha: csha, checksum: crc, offset: off, size: orig, compressedSize: cmp))
                default: break
                }
            }
            files.append(File(name: name, size: size, flags: flags, sha: sha, chunks: chunks.sorted { $0.offset < $1.offset },
                              linkTarget: (link?.isEmpty ?? true) ? nil : link))
        }
        self.depotID = depot
        self.manifestID = gid
        self.creationTime = Date(timeIntervalSince1970: TimeInterval(created))
        self.filenamesEncrypted = encrypted
        self.totalSize = total
        self.compressedSize = comp
        self.files = files
    }

    /// File names (and symlink targets) are AES-encrypted with the depot key and base64'd the
    /// MIME way: a line break every 72 characters, so any path longer than about 40 characters
    /// spans several lines. Foundation's decoder rejects line breaks, so they are removed first.
    public mutating func decryptFilenames(key: Data) throws {
        guard filenamesEncrypted else { return }
        func dec(_ s: String) throws -> String {
            guard let raw = Data(base64Encoded: String(s.filter { !$0.isWhitespace })) else {
                throw SteamError.badData("filename base64")
            }
            var plain = try Codec.steamDecrypt(raw, key: key)
            while plain.last == 0 { plain.removeLast() }
            return String(decoding: plain, as: UTF8.self).replacingOccurrences(of: "\\", with: "/")
        }
        for k in files.indices {
            files[k].name = try dec(files[k].name)
            if let l = files[k].linkTarget { files[k].linkTarget = try dec(l) }
        }
        files.sort { $0.name.lowercased() < $1.name.lowercased() }
        filenamesEncrypted = false
    }
}
