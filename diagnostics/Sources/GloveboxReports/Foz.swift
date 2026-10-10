import Foundation
import Compression

/// A Fossilize stream archive (.foz): a 16-byte header, then entries of a 40-character hex name, a payload header
/// (stored size, format 1 raw / 2 deflate, CRC, size after decompression) and the payload. A torn tail (the app was
/// killed mid-write) is skipped, as Fossilize does.
public struct FozEntry {
    public let name: String
    public let payload: Data
    /// Characters 22-24 of the name: 04 shader module, 06 graphics pipeline, 07 compute pipeline, ...
    public var tag: Substring { name.dropFirst(22).prefix(2) }
}

public enum FozError: Error { case notFoz, badEntry(String) }

public func readFoz(_ data: Data) throws -> [FozEntry] {
    guard data.count >= 16, data.prefix(12) == Data([0x81]) + "FOSSILIZEDB".data(using: .ascii)! else { throw FozError.notFoz }
    var out: [FozEntry] = []
    var off = 16
    while off + 56 <= data.count {
        let base = data.startIndex + off
        guard let name = String(data: data[base ..< base + 40], encoding: .ascii) else { throw FozError.badEntry("name") }
        let stored = Int(data.u32(at: base + 40)), format = data.u32(at: base + 44), size = Int(data.u32(at: base + 52))
        guard off + 56 + stored <= data.count else { break }
        let raw = data[base + 56 ..< base + 56 + stored]
        let payload = format == 2 ? try inflateZlib(Data(raw), expected: size) : Data(raw)
        out.append(FozEntry(name: name, payload: payload))
        off += 56 + stored
    }
    return out
}

/// Fossilize compresses with miniz (zlib framing): skip the 2-byte header, raw-inflate, ignore the Adler trailer.
func inflateZlib(_ data: Data, expected: Int) throws -> Data {
    guard data.count > 6 else { throw FozError.badEntry("deflate") }
    let body = data.dropFirst(2)
    var out = Data(count: expected)
    let n = out.withUnsafeMutableBytes { dst in
        body.withUnsafeBytes { src in
            compression_decode_buffer(dst.bindMemory(to: UInt8.self).baseAddress!, expected,
                                      src.bindMemory(to: UInt8.self).baseAddress!, body.count, nil, COMPRESSION_ZLIB)
        }
    }
    guard n == expected else { throw FozError.badEntry("inflate \(n) of \(expected)") }
    return out
}

/// Raw deflate (what the report endpoint expects).
func deflateRaw(_ data: Data) -> Data {
    var out = Data(count: max(64, data.count + data.count / 10 + 64))
    let n = out.withUnsafeMutableBytes { dst in
        data.withUnsafeBytes { src in
            compression_encode_buffer(dst.bindMemory(to: UInt8.self).baseAddress!, dst.count,
                                      src.bindMemory(to: UInt8.self).baseAddress!, data.count, nil, COMPRESSION_ZLIB)
        }
    }
    return out.prefix(n)
}

extension Data {
    func u32(at i: Index) -> UInt32 {
        UInt32(self[i]) | UInt32(self[i + 1]) << 8 | UInt32(self[i + 2]) << 16 | UInt32(self[i + 3]) << 24
    }
}

/// A shader module payload: its JSON header, a NUL, then the SPIR-V as LEB128 varints, one per 32-bit word.
/// Returns the JSON header and the SPIR-V bytes.
public func splitModule(_ payload: Data) throws -> (json: String, code: Data) {
    guard let nul = payload.firstIndex(of: 0), let json = String(data: payload[payload.startIndex ..< nul], encoding: .utf8)
    else { throw FozError.badEntry("module") }
    var code = Data()
    var v: UInt32 = 0, shift: UInt32 = 0
    for b in payload[(nul + 1)...] {
        v |= UInt32(b & 0x7f) << shift
        shift += 7
        if b & 0x80 == 0 {
            withUnsafeBytes(of: v.littleEndian) { code.append(contentsOf: $0) }
            v = 0; shift = 0
        }
    }
    return (json, code)
}
