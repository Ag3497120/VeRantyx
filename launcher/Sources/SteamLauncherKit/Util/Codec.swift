import Foundation
import CSteamSupport

/// Swift wrappers for the C helpers: Steam's AES scheme, VZip (LZMA), VZstd, zlib, hashes.
public enum Codec {
    public static func sha1(_ d: Data) -> Data {
        var out = [UInt8](repeating: 0, count: 20)
        d.withUnsafeBytes { steam_sha1($0.bindMemory(to: UInt8.self).baseAddress, d.count, &out) }
        return Data(out)
    }

    public static func crc32(_ d: Data) -> UInt32 {
        d.withUnsafeBytes { steam_crc32($0.bindMemory(to: UInt8.self).baseAddress, d.count) }
    }

    public static func adler32Seed0(_ d: Data) -> UInt32 {
        d.withUnsafeBytes { steam_adler32_seed0($0.bindMemory(to: UInt8.self).baseAddress, d.count) }
    }

    /// Steam's symmetric decrypt: ECB-encrypted IV followed by AES-256-CBC/PKCS#7.
    public static func steamDecrypt(_ d: Data, key: Data) throws -> Data {
        guard key.count == 32 else { throw SteamError.badData("depot key must be 32 bytes") }
        var out = Data(count: d.count)
        let n = key.withUnsafeBytes { k in
            d.withUnsafeBytes { i in
                out.withUnsafeMutableBytes { o in
                    steam_aes_decrypt(k.bindMemory(to: UInt8.self).baseAddress, i.bindMemory(to: UInt8.self).baseAddress, d.count,
                                      o.bindMemory(to: UInt8.self).baseAddress)
                }
            }
        }
        guard n >= 0 else { throw SteamError.badData("decryption failed (wrong depot key?)") }
        return out.prefix(n)
    }

    public static func steamEncrypt(_ d: Data, key: Data, iv: Data? = nil) throws -> Data {
        let iv = iv ?? Data((0..<16).map { _ in UInt8.random(in: 0...255) })
        var out = Data(count: d.count + 48)
        let n = key.withUnsafeBytes { k in
            iv.withUnsafeBytes { v in
                d.withUnsafeBytes { i in
                    out.withUnsafeMutableBytes { o in
                        steam_aes_encrypt(k.bindMemory(to: UInt8.self).baseAddress, v.bindMemory(to: UInt8.self).baseAddress,
                                          i.bindMemory(to: UInt8.self).baseAddress, d.count, o.bindMemory(to: UInt8.self).baseAddress)
                    }
                }
            }
        }
        guard n >= 0 else { throw SteamError.badData("encryption failed") }
        return out.prefix(n)
    }

    public static func inflate(_ d: Data, windowBits: Int32, expected: Int) throws -> Data {
        var out = Data(count: expected)
        let n = d.withUnsafeBytes { i in
            out.withUnsafeMutableBytes { o in
                steam_inflate(windowBits, i.bindMemory(to: UInt8.self).baseAddress, d.count, o.bindMemory(to: UInt8.self).baseAddress, expected)
            }
        }
        guard n >= 0 else { throw SteamError.badData("inflate failed") }
        return out.prefix(n)
    }

    public static func gunzip(_ d: Data, expected: Int) throws -> Data { try inflate(d, windowBits: 31, expected: expected) }

    private static func le32(_ d: Data, _ at: Int) -> UInt32 {
        let i = d.startIndex + at
        return UInt32(d[i]) | UInt32(d[i + 1]) << 8 | UInt32(d[i + 2]) << 16 | UInt32(d[i + 3]) << 24
    }

    /// Decompresses a decrypted depot chunk: VZip (LZMA), VZstd, or a one-file zip.
    public static func decompressChunk(_ d: Data, expected: Int) throws -> Data {
        guard d.count >= 16 else { throw SteamError.badData("chunk too short") }
        let b = Array(d.prefix(4))
        if b == [0x56, 0x53, 0x5A, 0x61] {  // "VSZa"
            let payload = d.subdata(in: (d.startIndex + 8)..<(d.endIndex - 15))
            let size = Int(le32(d, d.count - 11))
            var out = Data(count: size)
            let n = payload.withUnsafeBytes { i in
                out.withUnsafeMutableBytes { o in
                    steam_zstd_decode(i.bindMemory(to: UInt8.self).baseAddress, payload.count, o.bindMemory(to: UInt8.self).baseAddress, size)
                }
            }
            guard n == size else { throw SteamError.badData("zstd chunk failed") }
            return out
        }
        if b[0] == 0x56, b[1] == 0x5A, b[2] == 0x61 {  // "VZa"
            let prop = d[d.startIndex + 7]
            let dict = le32(d, 8)
            let size = Int(le32(d, d.count - 6))
            let payload = d.subdata(in: (d.startIndex + 12)..<(d.endIndex - 10))
            var out = Data(count: size)
            let n = payload.withUnsafeBytes { i in
                out.withUnsafeMutableBytes { o in
                    steam_lzma_decode(prop, dict, i.bindMemory(to: UInt8.self).baseAddress, payload.count, o.bindMemory(to: UInt8.self).baseAddress, size)
                }
            }
            guard n == size else { throw SteamError.badData("lzma chunk failed") }
            return out
        }
        if b == [0x50, 0x4B, 0x03, 0x04] {
            return try unzipSingle(d)
        }
        throw SteamError.badData("unknown chunk compression \(b.map { String(format: "%02x", $0) }.joined())")
    }

    /// Extracts the only file of a zip archive (manifests and old-style chunks).
    public static func unzipSingle(_ d: Data) throws -> Data {
        // Use the central directory: local headers may defer sizes to a data descriptor.
        var eocd = -1
        var i = d.count - 22
        while i >= max(0, d.count - 22 - 65535) {
            if le32(d, i) == 0x0605_4B50 { eocd = i; break }
            i -= 1
        }
        guard eocd >= 0 else { throw SteamError.badData("zip without end record") }
        let cdOffset = Int(le32(d, eocd + 16))
        guard le32(d, cdOffset) == 0x0201_4B50 else { throw SteamError.badData("zip central directory") }
        func le16(_ at: Int) -> Int { Int(d[d.startIndex + at]) | Int(d[d.startIndex + at + 1]) << 8 }
        let method = le16(cdOffset + 10)
        let compSize = Int(le32(d, cdOffset + 20))
        let size = Int(le32(d, cdOffset + 24))
        let local = Int(le32(d, cdOffset + 42))
        let dataStart = local + 30 + le16(local + 26) + le16(local + 28)
        let comp = d.subdata(in: (d.startIndex + dataStart)..<(d.startIndex + dataStart + compSize))
        switch method {
        case 0: return comp
        case 8: return try inflate(comp, windowBits: -15, expected: size)
        default: throw SteamError.badData("zip method \(method)")
        }
    }
}

public extension Data {
    var hex: String { map { String(format: "%02x", $0) }.joined() }
    init?(hex: String) {
        var out = Data(capacity: hex.count / 2)
        var it = hex.makeIterator()
        while let a = it.next() {
            guard let b = it.next(), let v = UInt8(String([a, b]), radix: 16) else { return nil }
            out.append(v)
        }
        self = out
    }
}
