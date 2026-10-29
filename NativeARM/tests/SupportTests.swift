import Foundation
import Darwin

@main enum SupportTests {
    static let fm = FileManager.default
    static var checks = 0
    static func check(_ condition: @autoclosure () throws -> Bool, _ name: String) throws {
        guard try condition() else { throw NativeFailure("FAIL: " + name) }
        checks += 1
    }
    static func rejects(_ name: String, _ action: () throws -> Void) throws {
        do { try action() } catch { checks += 1; return }
        throw NativeFailure("FAIL: accepted " + name)
    }
    static func write(_ data: Data, _ path: URL) throws {
        try fm.createDirectory(at: path.deletingLastPathComponent(), withIntermediateDirectories: true)
        try data.write(to: path)
    }
    static func elf(_ name: String, machine: UInt16 = 183) -> Data {
        var data = Data(repeating: 0, count: 512)
        func put<T>(_ value: T, _ offset: Int) { var v = value; withUnsafeBytes(of: &v) { data.replaceSubrange(offset..<offset + $0.count, with: $0) } }
        data.replaceSubrange(0..<6, with: [0x7f, 0x45, 0x4c, 0x46, 2, 1])
        put(machine, 18); put(UInt64(64), 32); put(UInt16(56), 54); put(UInt16(2), 56)
        put(UInt32(1), 64); put(UInt64(0), 72); put(UInt64(0x1000), 80); put(UInt64(512), 96)
        put(UInt32(2), 120); put(UInt64(176), 128); put(UInt64(48), 152)
        put(UInt64(5), 176); put(UInt64(0x1100), 184); put(UInt64(14), 192); put(UInt64(0), 200)
        data.replaceSubrange(256..<256 + name.utf8.count + 1, with: Array(name.utf8) + [0])
        return data
    }
    static func ipc(_ file: URL, sequence: UInt32 = 2, valid: Bool = true) throws {
        let fd = open(file.path, O_CREAT | O_RDWR, 0o600); guard fd >= 0 else { throw NativeFailure("test map") }; defer { close(fd) }
        ftruncate(fd, off_t(NativeIPC.size))
        func put<T>(_ value: T, _ at: Int) { var v = value; _ = withUnsafeBytes(of: &v) { pwrite(fd, $0.baseAddress, $0.count, off_t(at)) } }
        for i in 0..<16 { put(Float(i % 5 == 0 ? 1 : 0), NativeIPC.hands + i * 4) }
        put(Double(1), NativeIPC.hands + 244)
        for i in 0..<8 { put(Float(i % 2 == 0 ? -1 : 1), NativeIPC.hands + 252 + 4 * i) }
        put(UInt8(valid ? 1 : 0), NativeIPC.hands + 286)
        put(UInt32(0x44504956), NativeIPC.hands + 320); put(Float(0.064), NativeIPC.hands + 324)
        put(sequence, 20)
    }
    static func main() throws {
        let root = URL(fileURLWithPath: CommandLine.arguments[1]).appendingPathComponent("swift-" + UUID().uuidString)
        try fm.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: root) }
        let library = root.appendingPathComponent("lib.so.2")
        try write(elf("libguest.so.7"), library)
        try check(ELF.soname(library) == "libguest.so.7", "ELF SONAME")
        try write(elf("../escape"), library); try rejects("SONAME traversal") { _ = try ELF.soname(library) }
        try write(elf("lib.so", machine: 62), library); try rejects("x86 ELF") { _ = try ELF.soname(library) }
        try write(Data([0x7f, 0x45, 0x4c, 0x46]), library); try rejects("truncated ELF") { _ = try ELF.soname(library) }
        try write(Data(), library); try check(ELF.soname(library) == nil, "empty depot placeholders")
        let source = root.appendingPathComponent("source"), depot = root.appendingPathComponent("depot"), support = root.appendingPathComponent("support")
        try write(Data("assets".utf8), source.appendingPathComponent("game/hlvr/pak01_dir.vpk"))
        try write(Data("original".utf8), source.appendingPathComponent("game/hlvr/cfg/settings.cfg"))
        for (path, name) in zip(NativeFiles.armDirectories, ["libengine2.so", "libclient.so"]) { try write(elf(name), depot.appendingPathComponent(path + "/" + name)) }
        for name in NativeFiles.systemLibraries { try write(elf(name), support.appendingPathComponent(name)) }
        let output = root.appendingPathComponent("version"), state = root.appendingPathComponent("state")
        try NativeFiles.prepare(content: source, depot: depot, support: support, output: output, state: state)
        try NativeFiles.validateGame(output.appendingPathComponent("game-root")); try NativeFiles.validateGuest(output.appendingPathComponent("lib"))
        try check(ELF.soname(output.appendingPathComponent("lib/libengine2.so")) == "libengine2.so", "guest symlinks survive preparation")
        try write(Data("player".utf8), output.appendingPathComponent("game-root/game/hlvr/cfg/settings.cfg"))
        try check(String(contentsOf: source.appendingPathComponent("game/hlvr/cfg/settings.cfg")) == "original", "source remains unchanged")
        try rejects("existing output") { try NativeFiles.prepare(content: source, depot: depot, support: support, output: output, state: state) }
        try rejects("output inside source") { try NativeFiles.fresh(source.appendingPathComponent("child"), sources: [source]) }
        try rejects("state inside source") { try NativeFiles.prepare(content: source, depot: depot, support: support, output: root.appendingPathComponent("invalid-state"), state: source.appendingPathComponent("state")) }
        let badState = root.appendingPathComponent("bad-state")
        try fm.createDirectory(at: badState.appendingPathComponent("game/hlvr"), withIntermediateDirectories: true)
        try fm.createSymbolicLink(at: badState.appendingPathComponent("game/hlvr/cfg"), withDestinationURL: source.appendingPathComponent("game/hlvr/cfg"))
        try rejects("state link into source") { try NativeFiles.prepare(content: source, depot: depot, support: support, output: root.appendingPathComponent("invalid-state-link"), state: badState) }
        let next = root.appendingPathComponent("next")
        try NativeFiles.prepare(content: source, depot: depot, support: support, output: next, state: state)
        try check(String(contentsOf: next.appendingPathComponent("game-root/game/hlvr/cfg/settings.cfg")) == "player", "settings persist across versions")
        let moved = root.appendingPathComponent("renamed version")
        try fm.moveItem(at: next, to: moved)
        try check(ELF.soname(moved.appendingPathComponent("lib/libclient.so")) == "libclient.so", "relative guest links are relocatable")
        try check(String(contentsOf: moved.appendingPathComponent("game-root/game/hlvr/cfg/settings.cfg"), encoding: .utf8) == "player", "relative overlay links are relocatable")
        let escape = source.appendingPathComponent("escape")
        try fm.createSymbolicLink(at: escape, withDestinationURL: support)
        try rejects("source symlink escape") { try NativeFiles.merge(source, into: root.appendingPathComponent("bad")) }
        try fm.removeItem(at: escape)
        let work = NativeContentCache(root: root.appendingPathComponent("working-copy"))
        try write(Data("transient".utf8), source.appendingPathComponent("steamapps/downloading/546560/partial"))
        try work.seed(from: source, cloneOnly: true)
        try check(work.seeded, "native working copy receipt")
        let asset = work.root.appendingPathComponent("game/hlvr/pak01_dir.vpk")
        let handle = try FileHandle(forWritingTo: asset)
        try handle.write(contentsOf: Data("UPDATE".utf8)); try handle.close()
        try check(String(contentsOf: source.appendingPathComponent("game/hlvr/pak01_dir.vpk"), encoding: .utf8) == "assets", "in-place Steam update cannot modify Windows source")
        try check(!fm.fileExists(atPath: work.root.appendingPathComponent("steamapps/downloading").path), "Steam transient downloads not seeded")
        try work.seed(from: source, cloneOnly: true)
        try check(String(contentsOf: asset, encoding: .utf8) == "UPDATE", "resume preserves Steam-updated files")
        // Simulate a seed interrupted before its completion marker was written.
        try fm.removeItem(at: work.receipt)
        try write(Data("new source file".utf8), source.appendingPathComponent("game/extra.vpk"))
        try work.seed(from: source, cloneOnly: true)
        try check(String(contentsOf: asset, encoding: .utf8) == "UPDATE", "partial seed never overwrites updated data")
        try check(fm.fileExists(atPath: work.root.appendingPathComponent("game/extra.vpk").path), "partial seed fills missing files")
        let initiallyEmpty = NativeContentCache(root: root.appendingPathComponent("initially-empty"))
        try initiallyEmpty.seed(from: nil, cloneOnly: true)
        try check(!initiallyEmpty.seeded, "empty cache does not suppress later local reuse")
        try initiallyEmpty.seed(from: source, cloneOnly: true)
        try check(initiallyEmpty.seeded, "Windows content can seed a previously empty cache")
        let sourceLink = root.appendingPathComponent("linked-work")
        try fm.createSymbolicLink(at: sourceLink, withDestinationURL: source)
        try rejects("working cache links into Windows source") { try NativeContentCache(root: sourceLink).seed(from: source, cloneOnly: true) }
        let installed = "\"AppState\" { \"appid\" \"546560\" \"StateFlags\" \"4\" \"InstalledDepots\" { \"546561\" { \"manifest\" \"11\" } \"546564\" { \"manifest\" \"22\" } } }"
        try check(ValveKV.installedAlyxDepots(installed) == ["546561": "11", "546564": "22"], "verified Linux install depots")
        try rejects("unfinished Steam install") { _ = try ValveKV.installedAlyxDepots(installed.replacingOccurrences(of: "\"StateFlags\" \"4\"", with: "\"StateFlags\" \"6\"")) }
        try rejects("Windows-only manifest presented as native") { _ = try ValveKV.installedAlyxDepots(installed.replacingOccurrences(of: "546564", with: "546562")) }
        let frame = root.appendingPathComponent("frame")
        try ipc(frame)
        try check(NativeIPC.snapshot(frame)?.ipd == Float(0.064), "valid ALVR metadata")
        try ipc(frame, sequence: 3); try check(NativeIPC.snapshot(frame) == nil, "odd sequence rejected")
        try ipc(frame, valid: false); try check(NativeIPC.snapshot(frame) == nil, "untracked pose rejected")
        try check(NativeIPC.snapshot(root.appendingPathComponent("missing")) == nil, "absent map rejected")
        let metadata = "Steam log\n\"546560\" { \"depots\" { \"546561\" { \"manifests\" { \"public\" { \"gid\" \"1234\" } } } \"546564\" { \"manifests\" { \"public\" { \"gid\" \"5678\" } } } } }\nSteam> quit"
        try check(ValveKV.depotManifests(metadata) == ["546561": "1234", "546564": "5678"], "Steam depot manifests")
        try rejects("missing ARM64 manifest") { _ = try ValveKV.depotManifests("\"546560\" {}") }
        try rejects("truncated VDF") { _ = try ValveKV.parse("\"AppState\" {") }
        let client = root.appendingPathComponent("client"), cache = client.appendingPathComponent("steamapps/content/app_546560/depot_546564")
        try fm.createDirectory(at: cache, withIntermediateDirectories: true)
        try check(ValveKV.downloadedDepot("Depot download complete : \"\(cache.path)\"", client: client, id: "546564").path == cache.path, "Steam paths with spaces")
        let outside = root.appendingPathComponent("outside/depot_546564")
        try fm.createDirectory(at: outside, withIntermediateDirectories: true)
        try rejects("Steam download escapes selected drive") { _ = try ValveKV.downloadedDepot("Depot download complete : \"\(outside.path)\"", client: client, id: "546564") }
        var rate = NativeFrameRate()
        try check(rate.read("frame sample: t=10 pairs=100") == nil, "first FPS sample")
        try check(rate.read("frame sample: t=15 pairs=250") == 30, "stereo FPS from frame differences")
        try check(rate.read("frame sample: t=15 pairs=250") == nil, "duplicate FPS sample")
        try check(rate.read("frame sample: t=20 pairs=1") == nil, "frame counter reset")
        try check(NativeFiles.sha256(Data("abc".utf8)) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256")
        print("Native Swift support: \(checks) checks passed")
    }
}
