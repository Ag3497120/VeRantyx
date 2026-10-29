// Native Alyx uses the same Steam/Guard console as Windows downloads, and its own installation record.
import Foundation
import Darwin

extension GameBackend {
    var name: String {
        self == .vxrt ? T("Windows (VXRuntime)", "Windows（VXRuntime）") : T("Native ARM64", "ネイティブ ARM64")
    }
}

/// Game-specific adapters are declared by Title; shared setup and cards do not identify games by app ID.
enum NativeGameSupport {
    case alyx
    var appID: String { switch self { case .alyx: return "546560" } }
    var summary: String {
        switch self {
        case .alyx: return T("Native Alyx via ALVR (Pico / Quest). Headset playtest pending.", "ALVR 経由のネイティブ Alyx（Pico・Quest）。ヘッドセットでのプレイ確認は未実施。")
        }
    }
    func layout(drive: URL, runtime: URL) -> NativeLayout {
        switch self { case .alyx: return NativeLayout(drive: drive, runtime: runtime) }
    }
    func cachedContentAvailable(drive: URL, runtime: URL) -> Bool {
        switch self {
        case .alyx: return FileManager.default.fileExists(atPath: layout(drive: drive, runtime: runtime).downloads.appendingPathComponent("alyx-content/game/hlvr/pak01_dir.vpk").path)
        }
    }
    @MainActor func download(in model: AppModel) async -> Bool {
        switch self { case .alyx: return await model.downloadNative() }
    }
}

extension AppModel {
    var nativeLayout: NativeLayout { NativeLayout(drive: drive, runtime: Paths.runtime) }
    var nativeCanClone: Bool { (try? drive.resourceValues(forKeys: [.volumeSupportsFileCloningKey]).volumeSupportsFileCloning) == true }

    @discardableResult func downloadNative() async -> Bool {
        let layout = nativeLayout, fm = FileManager.default
        if Play.dryRun {
            flowNote("dry run: nativeARM64; APFS seed from existing Windows content; SteamCMD Linux app_update 546560 -beta public validate (depots 546561 + 546564); hashed ARM64 support packages; prepare into \(layout.base.path); no SteamVR or Windows DLLs")
            return false
        }
        guard layout.bundled else { error = T("Rebuild VeRantyx with its native components included.", "ネイティブ部品を含めて VeRantyx を再ビルドしてください。"); return false }
        nativeDownloadCancelled = false
        showConsole = true
        var incomplete: URL?
        defer { if let incomplete { try? fm.removeItem(at: incomplete) } }
        do {
            for directory in [layout.base, layout.downloads, layout.state, layout.base.appendingPathComponent("versions")] {
                try NativeFiles.ownedDirectory(directory)
            }
            // A private SteamCMD keeps depot downloads on the selected drive, away from Windows manifests.
            let client = layout.downloads.appendingPathComponent("steamcmd")
            let script = client.appendingPathComponent("steamcmd.sh")
            if !fm.isExecutableFile(atPath: script.path) {
                job = Job(title: T("Getting steamcmd from Valve…", "Valve から steamcmd を取得中…"))
                try await SteamCmd.install(at: client)
            }
            func checkCancellation() throws { if nativeDownloadCancelled || Task.isCancelled { throw CancellationError() } }
            func steam(_ commands: [String], installAt: URL? = nil) async throws -> String {
                try checkCancellation()
                var env = ProcessInfo.processInfo.environment
                env["TERM"] = "dumb"; env["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"
                try console.start(SteamCmd.linuxCommand(script: script, account: account, installAt: installAt, commands: commands), environment: env)
                let progress = Task { @MainActor in
                    while console.running { job?.progress = console.progress; job?.detail = console.stage; try? await Task.sleep(nanoseconds: 300_000_000) }
                }
                let status = await console.finished(); progress.cancel()
                try checkCancellation()
                let output = console.transcript
                guard status == 0, output.range(of: "ERROR!", options: .caseInsensitive) == nil, !output.contains("Depot download failed") else {
                    throw NativeFailure("SteamCMD did not finish (\(status)). " + console.lines.suffix(8).joined(separator: "\n"))
                }
                return output
            }
            job = Job(title: T("Checking native Alyx in your Steam account…", "Steam アカウントのネイティブ Alyx を確認中…"))
            var depots = try ValveKV.depotManifests(await steam(["+app_info_print", "546560"]))
            let cache = NativeContentCache(root: layout.downloads.appendingPathComponent("alyx-content"))
            let windows = Library.installed()["546560"]
            let source = windows?.folder
            let alreadyHasContent = fm.fileExists(atPath: cache.root.appendingPathComponent("game/hlvr/pak01_dir.vpk").path)
            let sourceHasContent = source.map { fm.fileExists(atPath: $0.appendingPathComponent("game/hlvr/pak01_dir.vpk").path) } == true
            // A manifest mismatch does not imply that all content changed. Steam validates
            // and updates an independent working copy, preserving the Windows installation.
            let fullCopies: Int64 = nativeCanClone ? (sourceHasContent || alreadyHasContent ? 0 : 1) : 2
            let need = Title.named("546560")!.bytes * fullCopies + (2 << 30)
            if let free = Mac.freeSpace(drive), free < need + (5 << 30) {
                throw NativeFailure(TF("About %@ of additional space is needed for native preparation; %@ is free. APFS can reuse installed game data. Steam may require more space for changed files.",
                    "ネイティブ版の準備に約 %@ の追加容量が必要ですが、空きは %@ です。APFS なら既存のゲームデータを再利用できます。変更ファイル用に Steam が追加容量を求める場合があります。", gigabytes(need + (5 << 30)), gigabytes(free)))
            }
            if !cache.seeded, sourceHasContent {
                job = Job(title: T("Reusing installed Alyx files…", "インストール済み Alyx のファイルを再利用中…"))
                let cloning = nativeCanClone
                let seed = Task.detached { try cache.seed(from: source, cloneOnly: cloning) }
                nativePreparation = seed
                try await seed.value
                nativePreparation = nil
                try checkCancellation()
                // Keep Valve's existing install metadata in the private copy when the
                // original library keeps it outside the game directory.
                if let windows {
                    let manifest = cache.root.appendingPathComponent("steamapps/appmanifest_546560.acf")
                    if !fm.fileExists(atPath: manifest.path) {
                        try fm.createDirectory(at: manifest.deletingLastPathComponent(), withIntermediateDirectories: true)
                        try fm.copyItem(at: windows.app.manifest, to: manifest)
                    }
                }
                flowNote("Seeded an independent native working copy from installed Windows files; Steam will validate it")
            } else { try cache.seed(from: nil, cloneOnly: nativeCanClone) }
            job = Job(title: T("Checking existing files and downloading native updates…", "既存ファイルを確認してネイティブ版の更新をダウンロード中…"))
            _ = try await steam(["+app_update", "546560", "-beta", "public", "validate"], installAt: cache.root)
            try checkCancellation()
            let installedText = try String(contentsOf: cache.root.appendingPathComponent("steamapps/appmanifest_546560.acf"), encoding: .utf8)
            let actual = try ValveKV.installedAlyxDepots(installedText)
            if actual != depots {
                depots = try ValveKV.depotManifests(await steam(["+app_info_print", "546560"]))
                guard actual == depots else { throw NativeFailure("Steam's public depots changed during preparation. Retry to validate the updated files; the working copy is kept.") }
            }
            try NativeFiles.validateGame(cache.root)
            let content = cache.root, binary = cache.root
            let lockData = try Data(contentsOf: layout.packages)
            let packages = try JSONDecoder().decode([NativePackage].self, from: lockData)
            let support = layout.downloads.appendingPathComponent("support-" + NativeFiles.sha256(lockData).prefix(16))
            try fm.createDirectory(at: support, withIntermediateDirectories: true)
            for (index, package) in packages.enumerated() {
                try checkCancellation()
                job = Job(title: T("Preparing native support libraries…", "ネイティブ用ライブラリを準備中…"), detail: package.name, progress: Double(index) / Double(packages.count))
                let deb = support.appendingPathComponent(package.name + ".deb")
                if (try? NativeFiles.hash(deb)) != package.sha256 {
                    let (temporary, response) = try await URLSession.shared.download(from: package.url)
                    defer { try? fm.removeItem(at: temporary) }
                    try checkCancellation()
                    guard (response as? HTTPURLResponse)?.statusCode == 200, try NativeFiles.hash(temporary) == package.sha256 else { throw NativeFailure("Download checksum failed: \(package.name)") }
                    if fm.fileExists(atPath: deb.path) { try fm.removeItem(at: deb) }
                    try fm.moveItem(at: temporary, to: deb)
                }
                try await NativeArchive.extractDeb(deb, into: support)
            }
            try checkCancellation()
            job = Job(title: T("Preparing native Alyx…", "ネイティブ Alyx を準備中…"))
            let version = UUID().uuidString, stage = layout.base.appendingPathComponent("versions/" + version)
            incomplete = stage
            let install = NativeInstallation(version: version, depots: depots, supportSHA256: NativeFiles.sha256(lockData))
            let assets = content
            let preparation = Task.detached {
                try NativeFiles.prepare(content: assets, depot: binary, support: support.appendingPathComponent("usr/lib/aarch64-linux-gnu"), output: stage, state: layout.state)
            }
            nativePreparation = preparation
            try await preparation.value
            nativePreparation = nil
            try checkCancellation()
            try JSONEncoder().encode(install).write(to: layout.manifest, options: .atomic)
            incomplete = nil
            flowNote("Native ARM64 installation ready; depots \(depots)")
            job = nil; await loadLibrary(); return true
        } catch {
            nativePreparation = nil; job = nil
            self.error = nativeDownloadCancelled || error is CancellationError ? T("Download cancelled. Press Download to resume.", "ダウンロードを中止しました。再開するにはダウンロードを押してください。") : error.localizedDescription
            await loadLibrary(); return false
        }
    }

    func cancelNativeDownload() { nativeDownloadCancelled = true; nativeDownloadTask?.cancel(); nativePreparation?.cancel(); if console.running { console.cancel() } }
}

enum NativeArchive {
    /// Only verified Debian archives. Inspect names before tar, reject links escaping the private extraction tree.
    static func extractDeb(_ deb: URL, into root: URL) async throws {
        let members = await run("/usr/bin/ar", ["-t", deb.path])
        guard members.status == 0, let member = members.text.split(separator: "\n").map(String.init).first(where: { $0.hasPrefix("data.tar.") }),
              ["data.tar.xz", "data.tar.gz", "data.tar.zst"].contains(member) else { throw NativeFailure("Invalid Debian archive") }
        let stage = root.appendingPathComponent("extract-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: stage, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: stage) }
        // ar writes into its current directory; never into the game or the application bundle.
        let p = Process(); p.executableURL = URL(fileURLWithPath: "/usr/bin/ar"); p.arguments = ["-x", deb.path, member]; p.currentDirectoryURL = stage
        p.standardInput = FileHandle.nullDevice; p.standardOutput = FileHandle.nullDevice; p.standardError = FileHandle.nullDevice
        try p.run(); await Task.detached { p.waitUntilExit() }.value
        guard p.terminationStatus == 0 else { throw NativeFailure("Could not unpack Debian archive") }
        let tar = stage.appendingPathComponent(member)
        let list = await run("/usr/bin/tar", ["-tf", tar.path])
        guard list.status == 0, list.text.split(separator: "\n").allSatisfy({ path in !path.hasPrefix("/") && !path.split(separator: "/").contains("..") }) else { throw NativeFailure("Unsafe Debian archive path") }
        let result = await run("/usr/bin/tar", ["-xf", tar.path, "-C", stage.path])
        guard result.status == 0 else { throw NativeFailure(result.text) }
        // Extract each package separately: absolute or parent-relative links cannot affect a later tar invocation.
        let libs = stage.appendingPathComponent("usr/lib/aarch64-linux-gnu"), dest = root.appendingPathComponent("usr/lib/aarch64-linux-gnu")
        try FileManager.default.createDirectory(at: dest, withIntermediateDirectories: true)
        if FileManager.default.fileExists(atPath: libs.path) {
            try await Task.detached {
                for file in try FileManager.default.contentsOfDirectory(at: libs, includingPropertiesForKeys: nil) where file.lastPathComponent.contains(".so") {
                    try NativeFiles.requireInside(file, libs)
                    try NativeFiles.merge(file, into: dest.appendingPathComponent(file.lastPathComponent), sourceRoot: libs)
                }
            }.value
        }
        try preserveNotices(stage: stage, root: root, package: deb.deletingPathExtension().lastPathComponent)
    }
    private static func preserveNotices(stage: URL, root: URL, package: String) throws {
        let copyright = stage.appendingPathComponent("usr/share/doc")
        if let iterator = FileManager.default.enumerator(at: copyright, includingPropertiesForKeys: [.isRegularFileKey]) {
            let notices = root.appendingPathComponent("licenses/" + package)
            try FileManager.default.createDirectory(at: notices, withIntermediateDirectories: true)
            for case let file as URL in iterator where file.lastPathComponent == "copyright" {
                if (try? file.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true {
                    let target = notices.appendingPathComponent(file.deletingLastPathComponent().lastPathComponent + ".txt")
                    if FileManager.default.fileExists(atPath: target.path) { try FileManager.default.removeItem(at: target) }
                    try FileManager.default.copyItem(at: file, to: target)
                }
            }
        }
    }
}
