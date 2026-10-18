// The Microsoft runtime DLLs Windows games and SteamVR load (msvcp140, vcruntime140, msvcp100, d3dcompiler_43, ...),
// taken from the redistributable installers the user's own Steam installs carry (_CommonRedist/vcredist/<year>/
// VC_redist.x64.exe, _CommonRedist/DirectX/*.cab) and the copies apps ship beside their executables, into one folder
// VXRuntime loads them from (~/Verantyx_VR_Drive/vxrt_redist). Nothing is downloaded and nothing leaves the Mac.
// This is tools/vxrt_redist.py's "collect" for a Mac without Python: the same choice of files, the same output.
//
// An installer is opened without running it: a WiX bundle (VC_redist) is a Windows executable with cabinets inside,
// and cabinets hold the files (bsdtar, which comes with macOS, reads cabinets). Only 64-bit (x86-64) DLLs whose names
// are on the list are kept; when several copies of one DLL turn up, the highest file version wins.
import CryptoKit
import Darwin
import Foundation

enum Redist {
    static let wanted = ["msvcp1*.dll", "msvcp140_*.dll", "msvcr1*.dll", "vcruntime140*.dll", "concrt140.dll", "vcomp1*.dll",
                         "vccorlib1*.dll", "vcamp1*.dll", "mfc1*.dll", "mfcm1*.dll", "atl1*.dll",
                         "d3dcompiler_4*.dll", "d3dx9_*.dll", "d3dx10*.dll", "d3dx11_*.dll", "d3dcsx_4*.dll",
                         "xinput1_*.dll", "xinput9_1_0.dll", "x3daudio1_*.dll", "xapofx1_*.dll", "xaudio2_*.dll", "xactengine*.dll"]

    static func isWanted(_ name: String) -> Bool { wanted.contains { fnmatch($0, name.lowercased(), 0) == 0 } }

    struct PE { var machine: UInt16; var version: [Int]?; var originalName: String? }

    /// Machine, file version (VS_FIXEDFILEINFO) and original file name (the version resource's) of a PE file.
    static func peInfo(_ url: URL) -> PE? {
        guard let b = try? Data(contentsOf: url, options: .alwaysMapped), b.count >= 0x40, b[0] == 0x4D, b[1] == 0x5A else { return nil }
        func u16(_ o: Int) -> Int { o + 2 <= b.count ? Int(b[b.startIndex + o]) | Int(b[b.startIndex + o + 1]) << 8 : 0 }
        func u32(_ o: Int) -> Int { u16(o) | u16(o + 2) << 16 }
        let pe = u32(0x3C)
        guard pe + 6 <= b.count, b[pe] == 0x50, b[pe + 1] == 0x45, b[pe + 2] == 0, b[pe + 3] == 0 else { return nil }
        let machine = UInt16(u16(pe + 4))
        var version: [Int]? = nil
        if let r = b.range(of: Data([0xBD, 0x04, 0xEF, 0xFE])) {
            let i = r.lowerBound - b.startIndex
            let ms = u32(i + 8), ls = u32(i + 12)
            version = [ms >> 16, ms & 0xFFFF, ls >> 16, ls & 0xFFFF]
        }
        var name: String? = nil
        let key = "OriginalFilename".data(using: .utf16LittleEndian)!
        if let r = b.range(of: key) {
            var k = r.upperBound - b.startIndex
            while k < b.count - 1 && b[k] == 0 && b[k + 1] == 0 { k += 2 }      // (padding to the value)
            var end = k
            while end < b.count - 1 && !(b[end] == 0 && b[end + 1] == 0) { end += 2 }
            if end <= b.count, let s = String(data: b[(b.startIndex + k)..<(b.startIndex + end)], encoding: .utf16LittleEndian) {
                let t = s.trimmingCharacters(in: .whitespaces)
                name = t.isEmpty ? nil : t
            }
        }
        return PE(machine: machine, version: version, originalName: name)
    }

    /// (offset, size) of the cabinets inside a file (a WiX bundle carries its payloads that way).
    static func cabinets(in data: Data) -> [(Int, Int)] {
        var out: [(Int, Int)] = []
        let magic = Data([0x4D, 0x53, 0x43, 0x46, 0, 0, 0, 0])
        var from = data.startIndex
        while let r = data.range(of: magic, in: from..<data.endIndex) {
            let i = r.lowerBound - data.startIndex
            if i + 12 <= data.count {
                let size = Int(data[r.lowerBound + 8]) | Int(data[r.lowerBound + 9]) << 8 | Int(data[r.lowerBound + 10]) << 16 | Int(data[r.lowerBound + 11]) << 24
                if size > 0 && size <= data.count - i { out.append((i, size)) }
            }
            from = r.lowerBound + 4
        }
        return out
    }

    /// The regular files under a folder (symbolic links to folders are not followed, as os.walk does not).
    static func files(under root: URL) -> [URL] {
        guard let walk = FileManager.default.enumerator(at: root, includingPropertiesForKeys: [.isRegularFileKey]) else { return [] }
        var out: [URL] = []
        for case let u as URL in walk where (try? u.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true { out.append(u) }
        return out
    }

    /// Every file inside an installer or cabinet, nested cabinets opened too.
    static func unpack(_ url: URL, into work: URL, depth: Int = 0) async -> [URL] {
        var files: [URL] = []
        try? FileManager.default.createDirectory(at: work, withIntermediateDirectories: true)
        guard let data = try? Data(contentsOf: url, options: .alwaysMapped), data.count >= 4 else { return [] }
        let head = data.prefix(4)
        let cabs: [(Int, Int)] = head == Data([0x4D, 0x53, 0x43, 0x46]) ? [(0, data.count)] : head.prefix(2) == Data([0x4D, 0x5A]) ? cabinets(in: data) : []
        for (n, (off, size)) in cabs.enumerated() {
            let piece = work.appendingPathComponent("d\(depth)_\(n).cab")
            try? data.subdata(in: (data.startIndex + off)..<(data.startIndex + off + size)).write(to: piece)
            let into = work.appendingPathComponent("d\(depth)_\(n)")
            try? FileManager.default.createDirectory(at: into, withIntermediateDirectories: true)
            guard await run("/usr/bin/bsdtar", ["-xf", piece.path, "-C", into.path]).status == 0 else { continue }
            for f in Redist.files(under: into) {
                let sig = (try? FileHandle(forReadingFrom: f).read(upToCount: 4)) ?? Data()
                if sig == Data([0x4D, 0x53, 0x43, 0x46]) && depth < 3 {
                    files += await unpack(f, into: work.appendingPathComponent("n\(depth)_\(n)_\(files.count)"), depth: depth + 1)
                } else {
                    files.append(f)
                }
            }
        }
        return files
    }

    /// The DLL's own name: its version resource's original file name when the file's name holds it, else the file
    /// name without an installer's decoration (msvcp140.dll_amd64, F_CENTRAL_msvcp120_x64 -> msvcp1x0.dll).
    static func dllName(_ url: URL, _ info: PE) -> String {
        let base = url.lastPathComponent.lowercased()
        if let orig = info.originalName?.lowercased(), orig.hasSuffix(".dll"), base.contains(String(orig.dropLast(4))) { return orig }
        let re = try! NSRegularExpression(pattern: "^(?:f_central_)?(.+?)(?:\\.dll)?(?:_(?:system_)?(?:amd64|x64|arm64))?$")
        var name = base
        if let m = re.firstMatch(in: base, range: NSRange(base.startIndex..., in: base)), let r = Range(m.range(at: 1), in: base) { name = String(base[r]) }
        return name.hasSuffix(".dll") ? name : name + ".dll"
    }

    struct Found { var name: String; var version: [Int]?; var file: URL; var source: String }

    static func relative(_ url: URL) -> String {
        let drive = Paths.drive.standardizedFileURL.path + "/"
        let p = url.standardizedFileURL.path
        return p.hasPrefix(drive) ? String(p.dropFirst(drive.count)) : p
    }

    /// Every wanted x86-64 DLL under the roots: from installers in _CommonRedist folders, and loose ones.
    static func candidates(_ roots: [URL], work: URL, note: @escaping @Sendable (String) -> Void) async -> [Found] {
        var found: [Found] = []
        let installer = try! NSRegularExpression(pattern: "vc_?redist.*x64|x64\\.cab$|_x64\\.cab$")
        for root in roots {
            for p in Redist.files(under: root) {
                let low = p.lastPathComponent.lowercased()
                let dir = p.deletingLastPathComponent().path.lowercased()
                if dir.contains("_commonredist") && (low.hasSuffix(".exe") || low.hasSuffix(".cab")) &&
                    (installer.firstMatch(in: low, range: NSRange(low.startIndex..., in: low)) != nil || low.hasSuffix("_x64.cab")) {
                    note(p.lastPathComponent)
                    let tag = "\(found.count)_" + low.map { $0.isLetter || $0.isNumber ? $0 : "_" }.map(String.init).joined()
                    for inner in await unpack(p, into: work.appendingPathComponent(tag)) {
                        guard let info = peInfo(inner), info.machine == 0x8664 else { continue }
                        let name = dllName(inner, info)
                        guard isWanted(name) else { continue }
                        let keep = work.appendingPathComponent("keep/\(found.count)_\(inner.lastPathComponent)")
                        try? FileManager.default.createDirectory(at: keep.deletingLastPathComponent(), withIntermediateDirectories: true)
                        try? FileManager.default.copyItem(at: inner, to: keep)
                        found.append(Found(name: name, version: info.version, file: keep, source: relative(p) + " (installer)"))
                    }
                } else if low.hasSuffix(".dll") && isWanted(low) {
                    if let info = peInfo(p), info.machine == 0x8664 {
                        found.append(Found(name: low, version: info.version, file: p, source: relative(p)))
                    }
                }
            }
        }
        return found
    }

    static func newer(_ a: [Int]?, than b: [Int]?) -> Bool {
        let x = a ?? [0], y = b ?? [0]
        for i in 0..<max(x.count, y.count) {
            let p = i < x.count ? x[i] : -1, q = i < y.count ? y[i] : -1
            if p != q { return p > q }
        }
        return false
    }

    /// Collects the DLLs into vxrt_redist with its manifest.json; returns how many.
    static func collect(note: @escaping @Sendable (String) -> Void = { _ in }) async throws -> Int {
        let roots = [Paths.drive.appendingPathComponent("vxrt_steam"), Paths.drive.appendingPathComponent("vxrt_library")]
        let work = FileManager.default.temporaryDirectory.appendingPathComponent("vxrt_redist_\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: work) }
        let found = await candidates(roots, work: work, note: note)
        var best: [String: Found] = [:]
        for f in found where best[f.name] == nil || newer(f.version, than: best[f.name]!.version) { best[f.name] = f }
        let out = Paths.redist
        try FileManager.default.createDirectory(at: out, withIntermediateDirectories: true)
        var dlls: [String: Any] = [:]
        for (name, f) in best.sorted(by: { $0.key < $1.key }) {
            let dest = out.appendingPathComponent(name)
            let tmp = out.appendingPathComponent(name + ".tmp")
            try? FileManager.default.removeItem(at: tmp)
            try FileManager.default.copyItem(at: f.file, to: tmp)
            guard Darwin.rename(tmp.path, dest.path) == 0 else { throw Failure("\(dest.path): \(String(cString: strerror(errno)))") }
            let digest = SHA256.hash(data: try Data(contentsOf: dest)).map { String(format: "%02x", $0) }.joined()
            dlls[name] = ["version": f.version.map { $0.map(String.init).joined(separator: ".") } ?? NSNull(), "sha256": digest, "source": f.source] as [String: Any]
        }
        let manifest: [String: Any] = ["roots": roots.map { relative($0) }, "dlls": dlls]
        try JSONSerialization.data(withJSONObject: manifest, options: [.prettyPrinted, .sortedKeys]).write(to: out.appendingPathComponent("manifest.json"))
        return best.count
    }

    /// How many DLLs the folder holds (0: not collected yet).
    static var count: Int {
        guard let d = FileManager.default.contents(atPath: Paths.redist.appendingPathComponent("manifest.json").path),
              let j = try? JSONSerialization.jsonObject(with: d) as? [String: Any], let dlls = j["dlls"] as? [String: Any] else { return 0 }
        return dlls.count
    }
}
