import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif
import SteamLauncherKit

// A command-line driver for SteamLauncherKit, used to test the Steam protocol end to end
// without the visionOS app (on a Mac, or on Linux through tools/ws_bridge.py).
//
//   steamctl info <appid>                          public product info (anonymous)
//   steamctl download-anon <appid> <depot> <dir>   download a depot anonymous accounts may fetch
//   steamctl qr                                    QR sign-in; prints the challenge URL
//   steamctl signin [account]                      password + Approve in the Steam app; saves the session
//   steamctl fetch --app <id> --depot <id> [--manifest <gid>] [--include <prefix>]... --out <dir> [--json]
//                                                  one depot (or part of it), pinned to a build if asked
//   steamctl owns <appid>                          sign-in via saved token, check ownership
//   steamctl cloud <appid> <dir> [--uploads]       sync Steam Cloud files (download-only without --uploads)
//   steamctl cloud-dryrun <assets copy> [--listing <json>] [--uploads]
//                                                  the launcher's save sync on a copy, offline

let args = Array(CommandLine.arguments.dropFirst())
let tokenFile = URL(fileURLWithPath: NSHomeDirectory()).appendingPathComponent(".steamctl-token.json")

final class Ticker: @unchecked Sendable {
    private var last = Date.distantPast
    private let lock = NSLock()
    func due() -> Bool {
        lock.lock(); defer { lock.unlock() }
        guard Date().timeIntervalSince(last) > 1 else { return false }
        last = Date()
        return true
    }
}

func fmt(_ bytes: UInt64) -> String { ByteCountFormatter.string(fromByteCount: Int64(bytes), countStyle: .file) }

func connected() async throws -> CMClient {
    let cm = CMClient()
    await cm.setLogger { print("  [cm] \($0)") }
    try await cm.connect()
    return cm
}

func signedIn() async throws -> (CMClient, SteamCredentials) {
    guard let data = try? Data(contentsOf: tokenFile),
          let creds = try? JSONDecoder().decode(SteamCredentials.self, from: data) else {
        throw SteamError.unsupported("No saved sign-in. Run `steamctl qr` first.")
    }
    let cm = try await connected()
    _ = try await cm.logon(accountName: creds.accountName, refreshToken: creds.refreshToken, machineName: "steamctl")
    return (cm, creds)
}

func run() async throws {
    switch args.first {
    case "info":
        let appID = UInt32(args[1])!
        let cm = try await connected()
        _ = try await cm.logonAnonymous()
        let info = try await SteamApps(cm: cm).appInfo(appID)
        print("\(info.name) (app \(info.appID)), build \(info.buildID), change \(info.changeNumber)\(info.missingToken ? ", public info only" : "")")
        for d in info.depots {
            print("  depot \(d.id)  \(d.name.isEmpty ? "-" : d.name)  \(fmt(d.maxSize))  os=\(d.oslist) arch=\(d.osarch) manifest=\(d.publicManifest.map(String.init) ?? "-")")
        }
        await cm.logOff()

    case "appinfo-raw":
        let cm = try await connected()
        _ = try await cm.logonAnonymous()
        print(try await SteamApps(cm: cm).appInfoKV(UInt32(args[1])!))
        await cm.logOff()

    case "download-anon":
        let appID = UInt32(args[1])!, depotID = UInt32(args[2])!
        let dir = URL(fileURLWithPath: args[3])
        let cm = try await connected()
        let logon = try await cm.logonAnonymous()
        let apps = SteamApps(cm: cm)
        let info = try await apps.appInfo(appID)
        guard let depot = info.depots.first(where: { $0.id == depotID }), let gid = depot.publicManifest else {
            throw SteamError.badData("depot \(depotID) has no public manifest")
        }
        let key = try await apps.depotKey(depotID: depotID, appID: appID)
        print("depot key: ok (\(key.count) bytes)")
        let code = try await apps.manifestRequestCode(appID: appID, depotID: depotID, manifestID: gid)
        print("manifest \(gid), request code \(code)")
        let servers = try await apps.contentServers(cellID: logon.cellID)
        print("content servers: \(servers.prefix(4).map { $0.vhost.isEmpty ? $0.host : $0.vhost })")
        let cdn = CDNClient(servers: servers)
        let manifest = try await cdn.manifest(depotID: depotID, manifestID: gid, requestCode: code, depotKey: key)
        print("manifest: \(manifest.files.count) files, \(fmt(manifest.totalSize)), created \(manifest.creationTime)")
        for f in manifest.files.prefix(8) { print("  \(f.name)  \(fmt(f.size))  chunks=\(f.chunks.count)") }
        let dl = DepotDownloader(cdn: cdn, depotID: depotID, key: key, manifest: manifest, root: dir)
        let t0 = Date()
        let tick = Ticker()
        try await dl.run { p in
            if tick.due() || p.doneBytes == p.totalBytes {
                print("  \(String(format: "%5.1f", p.fraction * 100))%  \(fmt(p.doneBytes)) / \(fmt(p.totalBytes))  \(fmt(UInt64(p.bytesPerSecond)))/s  \(p.doneFiles)/\(p.totalFiles) files")
            }
        }
        print(String(format: "downloaded in %.1f s", Date().timeIntervalSince(t0)))
        let bad = try dl.verify()
        print(bad.isEmpty ? "verify: every file matches its SHA-1" : "verify: \(bad.count) bad files: \(bad.prefix(5))")
        await cm.logOff()

    case "manifest-anon":
        // Manifest sizes are readable without owning the app; names stay encrypted.
        let appID = UInt32(args[1])!, depotID = UInt32(args[2])!
        let cm = try await connected()
        let logon = try await cm.logonAnonymous()
        let apps = SteamApps(cm: cm)
        let info = try await apps.appInfo(appID)
        guard let gid = info.depots.first(where: { $0.id == depotID })?.publicManifest else { throw SteamError.badData("no manifest") }
        let code = try await apps.manifestRequestCode(appID: appID, depotID: depotID, manifestID: gid)
        let cdn = CDNClient(servers: try await apps.contentServers(cellID: logon.cellID))
        let m = try await cdn.manifest(depotID: depotID, manifestID: gid, requestCode: code, depotKey: nil)
        let chunks = m.files.reduce(0) { $0 + $1.chunks.count }
        print("depot \(depotID) manifest \(gid): \(m.files.count) files, \(fmt(m.totalSize)) on disk, \(fmt(m.compressedSize)) to download, \(chunks) chunks, names encrypted: \(m.filenamesEncrypted)")
        if args.contains("--names") {
            // Decrypts the file names where anonymous accounts get the depot key (free depots).
            var named = m
            try named.decryptFilenames(key: try await apps.depotKey(depotID: depotID, appID: appID))
            let longest = named.files.max { $0.name.count < $1.name.count }
            print("names decrypted; longest: \(longest?.name ?? "")")
            for f in named.files.filter(\.isSymlink).prefix(5) { print("  link \(f.name) -> \(f.linkTarget ?? "?")") }
        }
        await cm.logOff()

    case "launcher-test":
        // The launcher's install path end to end on a free app: plan, install, resume, verify.
        let dir = URL(fileURLWithPath: args[1])
        let profile = GameProfile(appID: 1007, title: "Steamworks SDK Redist", depots: [1006],
                                  localPath: { $0.hasPrefix("linux64/") ? nil : "bin/" + $0 },
                                  policy: { $0.name.hasPrefix("libsteamwebrtc") ? .placeholder : .download },
                                  cloudRoots: [:], storeURL: URL(string: "https://store.steampowered.com")!,
                                  heroImageURL: URL(string: "https://store.steampowered.com")!, logoURL: URL(string: "https://store.steampowered.com")!)
        let launcher = SteamLauncher(profile: profile, installRoot: dir)
        try await launcher.logOnAnonymously()
        let plan = try await launcher.plan()
        print("plan: build \(plan.buildID), \(fmt(plan.totalBytes)) to download, free space \(launcher.freeSpace.map(fmt) ?? "?"), needs update: \(launcher.needsUpdate(plan))")
        let tick = Ticker()
        let startInstall = {
            Task {
                try await launcher.install(plan) { p in
                    if tick.due() { print("  \(Int(p.fraction * 100))%  \(fmt(p.doneBytes))  \(p.currentFile)") }
                }
            }
        }
        if args.count > 2, args[2] == "--staged" {
            // Plays the system's background downloader: the back half of the chunks is fetched
            // as plain GETs into the staging folder, exactly as the background session stores them.
            let remaining = launcher.remainingChunks(plan)
            let back = remaining.suffix(remaining.count / 2)
            guard let base = CDNClient(servers: plan.servers).baseURLs.first else { throw SteamError.noServers }
            try FileManager.default.createDirectory(at: launcher.staged.folder, withIntermediateDirectories: true)
            var stagedBytes = 0
            for c in back {
                let (data, resp) = try await URLSession.shared.data(from: URL(string: c.path, relativeTo: base)!.absoluteURL)
                guard (resp as? HTTPURLResponse)?.statusCode == 200 else { throw SteamError.badData("CDN \(c.path)") }
                try data.write(to: launcher.staged.url(c.name))
                stagedBytes += data.count
            }
            print("staged \(back.count) of \(remaining.count) chunks (\(fmt(UInt64(stagedBytes))) as sent by the CDN)")
            print("still to fetch after staging: \(launcher.remainingChunks(plan).count) chunks")
            let t0 = Date()
            try await startInstall().value
            print(String(format: "install with staged chunks took %.1f s", Date().timeIntervalSince(t0)))
            print("staging folder left behind: \(FileManager.default.fileExists(atPath: launcher.staged.folder.path))")
        } else if args.count > 2, args[2] == "--interrupt" {
            let task = startInstall()
            try await Task.sleep(nanoseconds: 400_000_000)
            task.cancel()
            print("interrupted: \((try? await task.value) == nil ? "stopped" : "finished anyway")")
            try await launcher.install(plan) { p in if tick.due() { print("  resume \(Int(p.fraction * 100))%") } }
        } else {
            try await startInstall().value
        }
        print("installed; needs update now: \(launcher.needsUpdate(plan))")
        await launcher.signOut()

    case "signin":
        try await signIn(args)

    case "fetch":
        await fetch(args)

    case "qr":
        let cm = try await connected()
        let session = try await SteamAuth.beginQR(cm: cm, deviceName: "steamctl")
        print("Scan with the Steam app: \(session.challengeURL ?? "?")")
        if args.count > 1, args[1] == "--no-wait" { await cm.logOff(); return }
        let creds = try await session.waitForApproval { print("New QR: \($0)") }
        try JSONEncoder().encode(creds).write(to: tokenFile)
        print("Signed in as \(creds.accountName) (\(creds.steamID ?? 0)); token saved, expires \(creds.expires.map { "\($0)" } ?? "?")")
        await cm.logOff()

    case "owns":
        let appID = UInt32(args[1])!
        let (cm, creds) = try await signedIn()
        let apps = SteamApps(cm: cm)
        let licenses = try await cm.licenses()
        let own = try await apps.ownership(of: appID, licenses: licenses)
        print("\(creds.accountName): \(licenses.count) licenses; owns \(appID): \(own.owned) (package \(own.packageID.map(String.init) ?? "-"))")
        await cm.logOff()

    case "cloud":
        // Two-way sync of an app's saves into <dir>, laid out like the depot (e.g. <dir>/game/hlvr/save).
        let appID = UInt32(args[1])!
        let dir = URL(fileURLWithPath: args[2])
        let (cm, _) = try await signedIn()
        let info = try await SteamApps(cm: cm).appInfo(appID)
        print("cloud rules: \(info.cloudRules)")
        var sync = CloudSync(remote: SteamCloud(cm: cm, appID: appID, buildID: info.buildID), rules: info.cloudRules,
                             rootFolder: { _ in dir }, stateURL: dir.appendingPathComponent(".cloud-state.json"),
                             backupRoot: dir.appendingPathComponent(".cloud-backups"))
        sync.uploads = args.contains("--uploads")
        let r = try await sync.run()
        print("downloaded \(r.downloaded), uploaded \(r.uploaded), would upload \(r.wouldUpload), backed up \(r.backedUp), "
              + "relocated \(r.relocated), unchanged \(r.unchanged)")
        await cm.logOff()

    case "cloud-dryrun":
        try await cloudDryRun()

    default:
        print("usage: steamctl info|download-anon|qr|owns|cloud|cloud-dryrun …")
    }
}

// MARK: Offline dry run of the launcher's save sync

/// Steam Cloud as the launcher last saw it, with no network: the file list comes from a listing or
/// from the launcher's own sync history, and file contents from identical copies already in the
/// folder (a cloud file the launcher downloaded before is on disk under some spelling).
final class OfflineCloud: CloudRemote, @unchecked Sendable {
    let files: [CloudFile]
    let contentBySHA: [String: Data]
    var missing: [String] = []
    var uploaded: [String] = []
    init(files: [CloudFile], contentBySHA: [String: Data]) { self.files = files; self.contentBySHA = contentBySHA }
    func list() async throws -> [CloudFile] { files }
    func download(_ file: CloudFile) async throws -> Data {
        if let d = contentBySHA[file.sha1Hex.lowercased()] { return d }
        missing.append(file.filename)
        return Data("dry run: the cloud's \(file.filename) isn't on the headset\n".utf8)
    }
    func upload(_ files: [(name: String, data: Data, time: Date)]) async throws { uploaded += files.map(\.name) }
}

func hex(_ d: Data) -> String { d.map { String(format: "%02x", $0) }.joined() }

/// Every file under `dir` (backups excluded), relative path → SHA-1.
func tree(_ dir: URL) -> [(String, String)] {
    let e = FileManager.default.enumerator(atPath: dir.path)
    var out: [(String, String)] = []
    while let sub = e?.nextObject() as? String {
        if sub.hasPrefix(".cloud-backups") { continue }
        var isDir: ObjCBool = false
        let u = dir.appendingPathComponent(sub)
        guard FileManager.default.fileExists(atPath: u.path, isDirectory: &isDir), !isDir.boolValue,
              let d = try? Data(contentsOf: u) else { continue }
        out.append((sub, hex(Codec.sha1(d))))
    }
    return out.sorted { $0.0 < $1.0 }
}

/// Runs the launcher's save sync (Alyx's Auto-Cloud rule, the app's paths and history file) on a COPY
/// of the headset's Documents/alyx/assets, offline, and prints what it did. Only the copy changes.
func cloudDryRun() async throws {
    guard args.count >= 2 else {
        print("usage: steamctl cloud-dryrun <copy of Documents/alyx/assets> [--listing <json>] [--uploads]")
        return
    }
    let root = URL(fileURLWithPath: args[1])
    let fm = FileManager.default
    // The headset's disk tells SAVE from save; a disk that doesn't would merge them and fake the result.
    let probe = root.appendingPathComponent(".case-probe")
    try fm.createDirectory(at: probe, withIntermediateDirectories: true)
    let caseBlind = fm.fileExists(atPath: root.appendingPathComponent(".CASE-PROBE").path)
    try? fm.removeItem(at: probe)
    if caseBlind {
        print("error: \(root.path) is on a disk that ignores case, so SAVE/ and save/ can't both exist there.")
        print("Copy the folder onto a case-sensitive volume first, e.g.:")
        print("  hdiutil create -size 2g -fs 'Case-sensitive APFS' -volname alyxdry dry.dmg && hdiutil attach dry.dmg")
        exit(2)
    }
    let stateURL = root.appendingPathComponent(".launcher-cloud.json")
    let history = (try? JSONDecoder().decode([String: String].self, from: Data(contentsOf: stateURL))) ?? [:]

    var files: [CloudFile] = []
    if let i = args.firstIndex(of: "--listing"), i + 1 < args.count {
        // [{"filename": "%GameInstall%game/hlvr/SAVE/S0/autosave.sav", "sha1": "…", "timestamp": 1590000000}]
        let raw = try JSONSerialization.jsonObject(with: Data(contentsOf: URL(fileURLWithPath: args[i + 1]))) as? [[String: Any]] ?? []
        for o in raw {
            var f = CloudFile()
            f.filename = o["filename"] as? String ?? ""
            f.sha1Hex = (o["sha1"] as? String ?? o["sha"] as? String ?? "").lowercased()
            f.timestamp = UInt64(o["timestamp"] as? Int ?? 0)
            f.size = UInt32(o["size"] as? Int ?? 0)
            files.append(f)
        }
        print("cloud: \(files.count) files from \(args[i + 1])")
    } else {
        for (k, sha) in history.sorted(by: { $0.key < $1.key }) {
            var f = CloudFile()
            f.filename = k
            f.sha1Hex = sha
            files.append(f)
        }
        print("cloud: \(files.count) files from the launcher's sync history (the cloud as of the last sync; names lowercased)")
    }

    var content: [String: Data] = [:]
    for (sub, sha) in tree(root) where !sub.hasPrefix(".") {
        content[sha] = try Data(contentsOf: root.appendingPathComponent(sub))
    }
    print("\nbefore:")
    for (sub, sha) in tree(root) where !sub.hasPrefix(".") { print("  \(sub)  \(sha.prefix(10))") }

    let cloud = OfflineCloud(files: files, contentBySHA: content)
    var sync = CloudSync(remote: cloud,
                         rules: [CloudSaveRule(root: "gameinstall", path: "game/hlvr/save", pattern: "*", recursive: true)],
                         rootFolder: { _ in root }, localPath: GameProfile.halfLifeAlyx.localPath,
                         stateURL: stateURL, backupRoot: root.appendingPathComponent(".cloud-backups"))
    sync.uploads = args.contains("--uploads")
    sync.neverUpload = GameProfile.halfLifeAlyx.cloudNeverUpload
    let r = try await sync.run()

    print("\nuploads: \(sync.uploads ? "on" : "off (download-only)")")
    print("relocated:   \(r.relocated.isEmpty ? "-" : r.relocated.joined(separator: ", "))")
    print("downloaded:  \(r.downloaded.count)"); r.downloaded.forEach { print("  \($0)") }
    print("uploaded:    \(r.uploaded.count)"); r.uploaded.forEach { print("  \($0)") }
    print("would upload: \(r.wouldUpload.count)"); r.wouldUpload.forEach { print("  \($0)") }
    print("backed up:   \(r.backedUp.count)"); r.backedUp.forEach { print("  .cloud-backups/\($0)") }
    print("unchanged:   \(r.unchanged)")
    if !cloud.missing.isEmpty {
        print("not on the headset (a real sync downloads these; the copy got a placeholder):")
        cloud.missing.forEach { print("  \($0)") }
    }
    print("\nafter:")
    for (sub, sha) in tree(root) where !sub.hasPrefix(".") { print("  \(sub)  \(sha.prefix(10))") }
}

let done = DispatchSemaphore(value: 0)
Task {
    do { try await run() } catch { print("error: \(error)"); exit(1) }
    done.signal()
}
done.wait()
