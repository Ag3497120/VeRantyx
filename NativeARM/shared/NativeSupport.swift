// Shared by the app, the session helper and offline tests. No game code is loaded here.
import Foundation
import CryptoKit
import Darwin

enum GameBackend: String, CaseIterable, Codable, Identifiable {
    case vxrt, nativeARM64
    var id: String { rawValue }
}

struct NativeFailure: LocalizedError {
    let text: String
    init(_ text: String) { self.text = text }
    var errorDescription: String? { text }
}

struct NativeLayout {
    let drive: URL
    let runtime: URL
    var base: URL { drive.appendingPathComponent("native_alyx") }
    var downloads: URL { base.appendingPathComponent("downloads") }
    var state: URL { base.appendingPathComponent("state") }
    var manifest: URL { base.appendingPathComponent("installation.json") }
    var host: URL { runtime.appendingPathComponent("NativeARM/alyx_host") }
    var helper: URL { runtime.appendingPathComponent("NativeARM/native-session") }
    var moltenVK: URL { runtime.appendingPathComponent("NativeARM/MoltenVK") }
    var packages: URL { runtime.appendingPathComponent("NativeARM/dependencies.json") }
    var encoder: URL { runtime.appendingPathComponent("build/HardwareEncoder") }
    var bridge: URL { runtime.appendingPathComponent("ALVRBridge/target/release/libverantyx_alvr_bridge.dylib") }

    func installation() throws -> NativeInstallation {
        let i = try JSONDecoder().decode(NativeInstallation.self, from: Data(contentsOf: manifest))
        guard i.schema == 1, i.version.range(of: "^[A-Za-z0-9-]+$", options: .regularExpression) != nil,
              i.depots["546561"] != nil, i.depots["546564"] != nil else { throw NativeFailure("Invalid native installation record") }
        let folder = base.appendingPathComponent("versions/\(i.version)")
        try NativeFiles.requireInside(folder, base)
        try NativeFiles.validateGame(folder.appendingPathComponent("game-root"))
        try NativeFiles.validateGuest(folder.appendingPathComponent("lib"))
        guard i.supportSHA256 == NativeFiles.sha256(try Data(contentsOf: packages)) else {
            throw NativeFailure("Native support files changed. Prepare the native installation again in the app.")
        }
        return i
    }

    func versionRoot(_ i: NativeInstallation) -> URL { base.appendingPathComponent("versions/\(i.version)") }
    var bundled: Bool { [host, helper, encoder, bridge, moltenVK.appendingPathComponent("libMoltenVK.dylib"), packages].allSatisfy { FileManager.default.fileExists(atPath: $0.path) } }
}

struct NativeInstallation: Codable {
    var schema = 1
    var version: String
    var depots: [String: String]
    var supportSHA256: String
    var installedAt = Date()
}

struct NativePackage: Codable {
    var name: String
    var version: String
    var url: URL
    var sha256: String
    var bytes: Int64
}

/// A private working copy for Steam's own validation/update. Never hard-links game files.
struct NativeContentCache {
    let root: URL
    var receipt: URL { root.deletingLastPathComponent().appendingPathComponent(root.lastPathComponent + ".seed.json") }
    private struct Seed: Codable { var schema = 1; var source: String?; var files: Int }
    var seeded: Bool {
        guard let data = try? Data(contentsOf: receipt), let seed = try? JSONDecoder().decode(Seed.self, from: data) else { return false }
        return seed.schema == 1 && seed.source != nil && FileManager.default.fileExists(atPath: root.path)
    }
    /// Missing files only: retrying an interrupted seed must preserve Steam's already updated data.
    func seed(from source: URL?, cloneOnly: Bool) throws {
        let completed = seeded
        let fm = FileManager.default
        if let source { try NativeFiles.separate(root, from: [source]); try NativeFiles.separate(receipt, from: [source]) }
        try NativeFiles.ownedDirectory(root)
        if completed { return }
        var files = 0
        let skipped = Set(["steamapps/downloading", "steamapps/temp", "steamapps/workshop"])
        func copy(_ file: URL, _ relative: String, depth: Int = 0) throws {
            try Task.checkCancellation()
            guard depth < 128 else { throw NativeFailure("Too many nested links in native content source") }
            if skipped.contains(relative) { return }
            let target = relative.isEmpty ? root : root.appendingPathComponent(relative)
            if !relative.isEmpty { try NativeFiles.requireInside(target, root) }
            let values = try file.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey, .isRegularFileKey])
            if values.isSymbolicLink == true {
                try NativeFiles.requireInside(file, source!)
                try copy(file.resolvingSymlinksInPath(), relative, depth: depth + 1)
            } else if values.isDirectory == true {
                try NativeFiles.ownedDirectory(target)
                for entry in try fm.contentsOfDirectory(at: file, includingPropertiesForKeys: nil) {
                    try copy(entry, relative.isEmpty ? entry.lastPathComponent : relative + "/" + entry.lastPathComponent, depth: depth + 1)
                }
            } else if values.isRegularFile == true {
                var st = stat()
                if lstat(target.path, &st) == 0 {
                    guard st.st_mode & S_IFMT == S_IFREG else { throw NativeFailure("Native working file must be a regular file: \(target.path)") }
                    return
                }
                guard errno == ENOENT else { throw NativeFailure("Cannot inspect native working file: \(target.path)") }
                if clonefile(file.path, target.path, 0) != 0 {
                    if cloneOnly { throw NativeFailure("Cannot create an independent APFS clone of the installed game (\(String(cString: strerror(errno)))). Keep native data on the same APFS volume as the Windows installation.") }
                    try fm.copyItem(at: file, to: target)
                }
                files += 1
            } else { throw NativeFailure("Unsupported file in native content source: \(file.path)") }
        }
        if let source { try copy(source, "") }
        try Task.checkCancellation()
        try JSONEncoder().encode(Seed(source: source?.path, files: files)).write(to: receipt, options: .atomic)
    }
}

enum NativeFiles {
    static let systemLibraries = "libstdc++.so.6 libgcc_s.so.1 libogg.so.0 libglib-2.0.so.0 libgobject-2.0.so.0 libgio-2.0.so.0 libgmodule-2.0.so.0 libgthread-2.0.so.0 libz.so.1 libuuid.so.1 libssl.so.3 libcrypto.so.3 libpng16.so.16 libfribidi.so.0 libexpat.so.1 libbz2.so.1.0 libbrotlidec.so.1 libbrotlicommon.so.1 libzstd.so.1 libatomic.so.1 libpcre2-8.so.0 libffi.so.8 libmount.so.1 libblkid.so.1 libselinux.so.1".split(separator: " ").map(String.init)
    static let armDirectories = ["game/bin/linuxsteamrtarm64", "game/hlvr/bin/linuxsteamrtarm64"]
    static func sha256(_ data: Data) -> String { SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined() }
    static func hash(_ url: URL) throws -> String {
        let f = try FileHandle(forReadingFrom: url); defer { try? f.close() }
        var hash = SHA256()
        while let data = try f.read(upToCount: 1 << 20), !data.isEmpty { hash.update(data: data) }
        return hash.finalize().map { String(format: "%02x", $0) }.joined()
    }
    static func requireInside(_ child: URL, _ root: URL) throws {
        let r = root.resolvingSymlinksInPath().standardizedFileURL.path + "/"
        guard child.resolvingSymlinksInPath().standardizedFileURL.path.hasPrefix(r) else { throw NativeFailure("Path escapes the native installation: \(child.path)") }
    }
    static func fresh(_ output: URL, sources: [URL]) throws {
        var st = stat()
        guard lstat(output.path, &st) != 0, errno == ENOENT else { throw NativeFailure("Output already exists: \(output.path)") }
        try separate(output, from: sources)
        try FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)
    }
    static func separate(_ output: URL, from sources: [URL]) throws {
        for source in sources {
            let s = source.resolvingSymlinksInPath().standardizedFileURL.path
            let o = output.resolvingSymlinksInPath().standardizedFileURL.path
            guard o != s, !o.hasPrefix(s + "/") else { throw NativeFailure("Output must be outside its source") }
        }
    }
    static func ownedDirectory(_ directory: URL) throws {
        var st = stat()
        if lstat(directory.path, &st) == 0, st.st_mode & S_IFMT != S_IFDIR { throw NativeFailure("Native directory must not be a symlink or file: \(directory.path)") }
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    }
    static func relative(from directory: URL, to destination: URL) -> String {
        let from = directory.standardizedFileURL.pathComponents, to = destination.standardizedFileURL.pathComponents
        let common = zip(from, to).prefix { $0 == $1 }.count
        return (Array(repeating: "..", count: from.count - common) + to.dropFirst(common)).joined(separator: "/")
    }
    /// Clone regular files on APFS; never follow a source symlink outside the downloaded tree.
    static func merge(_ source: URL, into target: URL, sourceRoot: URL? = nil) throws {
        try Task.checkCancellation()
        let fm = FileManager.default, root = sourceRoot ?? source
        let v = try source.resourceValues(forKeys: [.isSymbolicLinkKey, .isDirectoryKey, .isRegularFileKey])
        if v.isSymbolicLink == true {
            try requireInside(source, root)
            try merge(source.resolvingSymlinksInPath(), into: target, sourceRoot: root)
        } else if v.isDirectory == true {
            if fm.fileExists(atPath: target.path) { try requireInside(target, target.deletingLastPathComponent()) }
            try fm.createDirectory(at: target, withIntermediateDirectories: true)
            for entry in try fm.contentsOfDirectory(at: source, includingPropertiesForKeys: nil) {
                try merge(entry, into: target.appendingPathComponent(entry.lastPathComponent), sourceRoot: root)
            }
        } else if v.isRegularFile == true {
            if fm.fileExists(atPath: target.path) { try fm.removeItem(at: target) }
            if clonefile(source.path, target.path, 0) != 0 { try fm.copyItem(at: source, to: target) }
        } else { throw NativeFailure("Unsupported file in download: \(source.path)") }
    }
    static func validateGame(_ root: URL) throws {
        guard FileManager.default.fileExists(atPath: root.appendingPathComponent("game/hlvr/pak01_dir.vpk").path) else {
            throw NativeFailure("Alyx content depot 546561 is incomplete")
        }
        for (dir, name) in zip(armDirectories, ["libengine2.so", "libclient.so"]) {
            guard try ELF.soname(root.appendingPathComponent(dir + "/" + name)) != nil else { throw NativeFailure("Missing ARM64 library: \(name)") }
        }
    }
    static func validateGuest(_ lib: URL) throws {
        for name in ["libengine2.so", "libclient.so"] + systemLibraries {
            guard FileManager.default.fileExists(atPath: lib.appendingPathComponent(name).path) else { throw NativeFailure("Missing native library: \(name)") }
        }
    }
    static func prepare(content: URL, depot: URL, support: URL, output: URL, state: URL) throws {
        let mutable = ["game/hlvr/cfg", "game/hlvr/save", "game/hlvr/SAVE", "game/hlvr/shadercache", "cache", "logs"]
        try separate(state, from: [content, depot, support])
        try ownedDirectory(state)
        for relative in mutable { try requireInside(state.appendingPathComponent(relative), state) }
        try fresh(output, sources: [content, depot, support, state])
        let fm = FileManager.default, game = output.appendingPathComponent("game-root"), lib = output.appendingPathComponent("lib")
        try fm.createDirectory(at: game, withIntermediateDirectories: true)
        try merge(content, into: game)
        if content.standardizedFileURL != depot.standardizedFileURL { try merge(depot, into: game) }
        try validateGame(game)
        try fm.createDirectory(at: lib, withIntermediateDirectories: true)
        var names = Set<String>()
        for source in armDirectories.map({ game.appendingPathComponent($0) }) + [support] {
            for file in try fm.contentsOfDirectory(at: source, includingPropertiesForKeys: nil).sorted(by: { $0.path < $1.path }) where file.lastPathComponent.contains(".so") {
                let name = file.lastPathComponent
                if name.hasPrefix("libvulkan_freedreno") || name.hasPrefix("libmimalloc.so") { continue }
                guard let soname = try ELF.soname(file) else { continue }
                let base = String(name[..<name.range(of: ".so")!.upperBound])
                for alias in [name, soname, base] where names.insert(alias).inserted {
                    let relative = file.path.replacingOccurrences(of: output.path + "/", with: "")
                    let dest = lib.appendingPathComponent(alias)
                    if file.path.hasPrefix(output.path + "/") {
                        try fm.createSymbolicLink(atPath: dest.path, withDestinationPath: "../" + relative)
                    } else { try fm.copyItem(at: file.resolvingSymlinksInPath(), to: dest) }
                }
            }
        }
        try validateGuest(lib)
        // The game's mutable directories live across version swaps, in our own overlay.
        for rel in mutable {
            let from = game.appendingPathComponent(rel), to = state.appendingPathComponent(rel)
            try fm.createDirectory(at: to.deletingLastPathComponent(), withIntermediateDirectories: true)
            if !fm.fileExists(atPath: to.path) {
                if fm.fileExists(atPath: from.path) { try merge(from, into: to) }
                else { try fm.createDirectory(at: to, withIntermediateDirectories: true) }
            }
            if fm.fileExists(atPath: from.path) { try fm.removeItem(at: from) }
            try fm.createSymbolicLink(atPath: from.path, withDestinationPath: relative(from: from.deletingLastPathComponent(), to: to))
        }
    }
}

enum ELF {
    static func soname(_ file: URL) throws -> String? {
        let data = try Data(contentsOf: file, options: .mappedIfSafe)
        guard data.starts(with: [0x7f, 0x45, 0x4c, 0x46]) else { return nil }
        func number(_ at: Int, _ size: Int) throws -> UInt64 {
            guard at >= 0, at <= data.count - size else { throw NativeFailure("Truncated ELF: \(file.lastPathComponent)") }
            return (0..<size).reduce(UInt64(0)) { $0 | UInt64(data[at + $1]) << ($1 * 8) }
        }
        guard data.count >= 64, data[4] == 2, data[5] == 1, try number(18, 2) == 183 else { throw NativeFailure("Not an ARM64 ELF64 library: \(file.lastPathComponent)") }
        let ph = try number(32, 8), stride = try number(54, 2), count = try number(56, 2)
        guard stride >= 56, ph <= UInt64(data.count), count <= UInt64(data.count) / stride,
              ph + stride * count <= UInt64(data.count) else { throw NativeFailure("Invalid ELF program headers") }
        var loads: [(UInt64, UInt64, UInt64)] = [], tags: [UInt64: UInt64] = [:]
        for i in 0..<count {
            let p = Int(ph + stride * i), kind = try number(p, 4), offset = try number(p + 8, 8)
            let address = try number(p + 16, 8), size = try number(p + 32, 8)
            guard offset <= UInt64(data.count), size <= UInt64(data.count) - offset else { throw NativeFailure("Truncated ELF segment") }
            if kind == 1 { loads.append((address, offset, size)) }
            if kind == 2, size >= 16 {
                for at in Swift.stride(from: Int(offset), through: Int(offset + size) - 16, by: 16) {
                    let tag = try number(at, 8); if tag == 0 { break }
                    tags[tag] = try number(at + 8, 8)
                }
            }
        }
        var name = file.lastPathComponent
        if let strings = tags[5], let index = tags[14] {
            let (address, overflow) = strings.addingReportingOverflow(index)
            guard !overflow, let (base, offset, size) = loads.first(where: { address >= $0.0 && address - $0.0 < $0.2 }) else { throw NativeFailure("ELF SONAME outside load segment") }
            let start = Int(offset + address - base), end = Int(offset + size)
            guard let zero = data[start..<end].firstIndex(of: 0), let s = String(data: data[start..<zero], encoding: .utf8) else { throw NativeFailure("Invalid ELF SONAME") }
            name = s
        }
        guard !name.isEmpty, ![".", ".."].contains(name), !name.contains("/"), !name.contains("\\") else { throw NativeFailure("Unsafe ELF SONAME") }
        return name
    }
}

/// KeyValues parser for Valve's app_info_print output and appmanifest files.
enum ValveKV {
    indirect enum Value { case text(String), object([String: Value])
        subscript(_ key: String) -> Value? { if case .object(let d) = self { return d[key] }; return nil }
        var text: String? { if case .text(let s) = self { return s }; return nil }
    }
    static func parse(_ text: String) throws -> Value {
        let chars = Array(text), n = chars.count
        var at = 0
        func token() throws -> String? {
            while at < n {
                if chars[at].isWhitespace { at += 1; continue }
                if chars[at] == "/", at + 1 < n, chars[at + 1] == "/" { while at < n && chars[at] != "\n" { at += 1 }; continue }
                break
            }
            guard at < n else { return nil }
            let c = chars[at]; at += 1
            if c == "{" || c == "}" { return String(c) }
            var out = ""
            if c == "\"" {
                while at < n {
                    let ch = chars[at]; at += 1
                    if ch == "\"" { return out }
                    if ch == "\\", at < n { out.append(chars[at]); at += 1 } else { out.append(ch) }
                }
                throw NativeFailure("Unterminated Steam metadata string")
            }
            out.append(c)
            while at < n, !chars[at].isWhitespace, chars[at] != "{", chars[at] != "}" { out.append(chars[at]); at += 1 }
            return out
        }
        func object(_ nested: Bool, depth: Int = 0) throws -> Value {
            guard depth < 64 else { throw NativeFailure("Steam metadata too deeply nested") }
            var dict: [String: Value] = [:]
            while let key = try token() {
                if key == "}" { guard nested else { throw NativeFailure("Unexpected Steam metadata brace") }; return .object(dict) }
                guard key != "{", let value = try token(), value != "}" else { throw NativeFailure("Incomplete Steam metadata") }
                dict[key] = value == "{" ? try object(true, depth: depth + 1) : .text(value)
            }
            guard !nested else { throw NativeFailure("Incomplete Steam metadata object") }
            return .object(dict)
        }
        return try object(false)
    }
    static func depotManifests(_ output: String) throws -> [String: String] {
        guard let start = output.range(of: "\"546560\""), let last = output.range(of: "}", options: .backwards) else { throw NativeFailure("Steam did not return Alyx metadata") }
        let root = try parse(String(output[start.lowerBound..<last.upperBound]))
        var depots: [String: String] = [:]
        for id in ["546561", "546564"] {
            guard let gid = root["546560"]?["depots"]?[id]?["manifests"]?["public"]?["gid"]?.text,
                  !gid.isEmpty, gid.allSatisfy(\.isNumber) else { throw NativeFailure("Steam did not return the public manifest for depot \(id). Check ownership and Steam Guard.") }
            depots[id] = gid
        }
        return depots
    }
    static func installedAlyxDepots(_ manifest: String) throws -> [String: String] {
        let app = try parse(manifest)["AppState"]
        guard app?["appid"]?.text == "546560", app?["StateFlags"]?.text == "4" else { throw NativeFailure("Steam has not completed the native installation") }
        var result: [String: String] = [:]
        for id in ["546561", "546564"] {
            guard let gid = app?["InstalledDepots"]?[id]?["manifest"]?.text, !gid.isEmpty, gid.allSatisfy(\.isNumber) else {
                throw NativeFailure("Steam did not install native depot \(id)")
            }
            result[id] = gid
        }
        return result
    }
    static func downloadedDepot(_ output: String, client: URL, id: String) throws -> URL {
        let expression = try NSRegularExpression(pattern: "Depot download complete\\s*:\\s*\"([^\"]+)\"", options: .caseInsensitive)
        var candidates: [URL] = []
        if let match = expression.matches(in: output, range: NSRange(output.startIndex..., in: output)).last,
           let r = Range(match.range(at: 1), in: output) { candidates.append(URL(fileURLWithPath: String(output[r]))) }
        candidates += ["steamapps/content/app_546560/depot_", "Steam.AppBundle/Steam/Contents/MacOS/steamapps/content/app_546560/depot_"].map { client.appendingPathComponent($0 + id) }
        for directory in candidates where FileManager.default.fileExists(atPath: directory.path) && directory.lastPathComponent == "depot_" + id {
            try NativeFiles.requireInside(directory, client)
            return directory
        }
        throw NativeFailure("SteamCMD did not provide depot \(id) inside the selected native download folder")
    }
}

struct NativeViewSnapshot: Codable {
    var sequence: UInt32
    var timestamp: Double
    var ipd: Float
    var tangents: [Float]
}

enum NativeIPC {
    static let block = 88 + 4096 * 4096 * 8
    static let hands = 64 + 2 * block
    static let size = hands + 312 + 1024
    static func snapshot(_ file: URL) -> NativeViewSnapshot? {
        let fd = open(file.path, O_RDONLY | O_NOFOLLOW); guard fd >= 0 else { return nil }; defer { close(fd) }
        var st = stat(); guard fstat(fd, &st) == 0, st.st_size >= size else { return nil }
        guard let map = mmap(nil, size, PROT_READ, MAP_SHARED, fd, 0), map != MAP_FAILED else { return nil }; defer { munmap(map, size) }
        func value<T>(_ at: Int, _: T.Type) -> T { map.loadUnaligned(fromByteOffset: at, as: T.self) }
        let first = value(20, UInt32.self); guard first > 0, first & 1 == 0 else { return nil }
        let data = Data(bytes: map.advanced(by: hands), count: 328)
        OSMemoryBarrier()
        guard first == value(20, UInt32.self) else { return nil }
        func part<T>(_ at: Int, _: T.Type) -> T { data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: at, as: T.self) } }
        let timestamp = part(244, Double.self), ipd = part(324, Float.self)
        let tangents = (0..<8).map { part(252 + 4 * $0, Float.self) }
        guard timestamp.isFinite, timestamp > 0, data[286] != 0, part(320, UInt32.self) == 0x44504956,
              ipd.isFinite, (0.04...0.09).contains(ipd), tangents.allSatisfy(\.isFinite),
              (0..<16).allSatisfy({ part($0 * 4, Float.self).isFinite }),
              [0, 2, 4, 6].allSatisfy({ tangents[$0] < 0 && tangents[$0 + 1] > 0 }) else { return nil }
        return NativeViewSnapshot(sequence: first, timestamp: timestamp, ipd: ipd, tangents: tangents)
    }
}

struct NativeSessionConfig: Codable {
    var host: URL
    var encoder: URL
    var game: URL
    var lib: URL
    var state: URL
    var logs: URL
    var alvrLog: URL
    var environment: [String: String]
    var parentPID: Int32
    var waitSeconds: Double = 300
}

struct NativeFrameRate {
    private var previous: (time: Double, pairs: Double)?
    mutating func read(_ line: String) -> Double? {
        func number(_ name: String) -> Double? {
            guard let range = line.range(of: name + "=") else { return nil }
            return Double(line[range.upperBound...].prefix { "0123456789.-".contains($0) })
        }
        guard let time = number("t"), let pairs = number("pairs"), time.isFinite, pairs.isFinite, time >= 0, pairs >= 0 else { return nil }
        guard let old = previous else { previous = (time, pairs); return nil }
        if time == old.time { return nil }
        previous = (time, pairs)
        guard time > old.time, pairs >= old.pairs else { return nil }
        return (pairs - old.pairs) / (time - old.time)
    }
}
