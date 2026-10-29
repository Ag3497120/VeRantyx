import Foundation

@main enum AppModelTests {
    @MainActor static func main() async throws {
        let fm = FileManager.default
        let root = URL(fileURLWithPath: CommandLine.arguments[1]).appendingPathComponent("app-model-" + UUID().uuidString)
        try fm.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: root) }
        let logURL = root.appendingPathComponent("alvr-log.txt")
        let connectedLog = "21:00:01 [INFO] VeRantyx: [\"Pico 4 Ultra\"] connected; controllers ready\n"
        let now = Date(timeIntervalSince1970: 2000), start = now.addingTimeInterval(-10)
        try Data(connectedLog.utf8).write(to: logURL)
        try fm.setAttributes([.modificationDate: start.addingTimeInterval(-20)], ofItemAtPath: logURL.path)
        _ = try logURL.resourceValues(forKeys: [.contentModificationDateKey]) // prime URL's stale metadata cache
        try fm.setAttributes([.modificationDate: now], ofItemAtPath: logURL.path)
        guard Play.modificationDate(logURL) == now,
              Play.alvrConnected(Play.tail(logURL, 6000), modifiedAt: Play.modificationDate(logURL), since: start, now: now) else {
            throw NativeFailure("Growing ALVR log remained stale: headset spinner regression")
        }
        let disconnectedLog = connectedLog + "21:00:02 [INFO] Client disconnected\n21:00:03 [INFO] VeRantyx: vibration #1\n"
        guard !Play.alvrConnected(disconnectedLog, modifiedAt: now, since: start, now: now),
              Play.alvrConnected(disconnectedLog + connectedLog, modifiedAt: now, since: start, now: now),
              !Play.alvrConnected(connectedLog, modifiedAt: now.addingTimeInterval(-13), since: start, now: now),
              !Play.alvrConnected(connectedLog, modifiedAt: start.addingTimeInterval(-1), since: start, now: now) else {
            throw NativeFailure("ALVR disconnect/reconnect or log freshness regression")
        }
        setenv("VERANTYX_VR_DRIVE", root.appendingPathComponent("drive").path, 1)
        setenv("VERANTYX_REPO", root.appendingPathComponent("runtime").path, 1)
        unsetenv("VERANTYX_BACKEND")
        let model = AppModel(savingPreferences: false, refreshAutomatically: false)
        model.headset = .quest; model.routes = [:]; model.chooseBackend(.vxrt)
        model.freeBytes = 64 << 30
        let layout = model.nativeLayout
        for file in [layout.host, layout.helper, layout.encoder, layout.bridge, layout.moltenVK.appendingPathComponent("libMoltenVK.dylib"), layout.packages] {
            try fm.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
            try Data().write(to: file)
        }
        let alyx = Title.named("546560")!, lab = Title.named("450390")!
        var game = Game(id: alyx.id, name: alyx.name, folder: layout.base, exe: nil, title: alyx, antiCheat: [], bytes: 0)
        model.games = [game]; model.steamInstalled = true; model.steamRunning = true
        model.installedBackends[alyx.id] = [.vxrt, .nativeARM64]
        model.chooseBackend(.nativeARM64)
        guard !model.windowsRuntimeReady, model.ready, model.setupSteps.done == model.setupSteps.of,
              model.canStart(game, flat: false), !model.canStart(game, flat: true), model.playSetupReady else { throw NativeFailure("Native readiness still requires Windows setup") }
        model.setRoute(.visionApp, forID: game.id)
        guard !model.canStart(game, flat: false), model.nativeReason(for: alyx) != nil, !model.ready else { throw NativeFailure("Native route must require ALVR") }
        model.setRoute(.alvr, forID: game.id)
        model.installedBackends[alyx.id] = [.vxrt]
        guard !model.isInstalled(game.id), !model.canStart(game, flat: false), !model.setupGamesDone else { throw NativeFailure("Windows install counted as native") }
        let nativePlan = model.installationPlan(for: [alyx.id])
        guard nativePlan == [GameInstallRequest(id: alyx.id, backend: .nativeARM64)] else { throw NativeFailure("Native setup requested Windows dependencies") }
        let mixedPlan = model.installationPlan(for: [alyx.id, lab.id])
        guard mixedPlan.count == 3, mixedPlan.first?.id == Title.steamVR,
              mixedPlan.contains(GameInstallRequest(id: lab.id, backend: .vxrt)), mixedPlan.contains(GameInstallRequest(id: alyx.id, backend: .nativeARM64)) else { throw NativeFailure("Mixed setup plan lost a variant or SteamVR") }
        model.chooseBackend(.vxrt)
        guard model.isInstalled(game.id), !model.canStart(game, flat: false) else { throw NativeFailure("Windows readiness changed") }
        model.rosetta = true; model.steamVR = true; model.redistCount = 1
        guard model.canStart(game, flat: false), model.ready else { throw NativeFailure("Windows launch regression") }
        model.installedBackends[alyx.id] = [.nativeARM64]
        guard !model.isInstalled(game.id) else { throw NativeFailure("Native install counted as Windows") }
        model.chooseBackend(.nativeARM64)
        model.quality = .sharp
        guard model.quality(for: game) == .sharp, model.quality(forID: lab.id) == .sharp else { throw NativeFailure("Shared quality not applied to both runtimes") }
        model.quality = .standard
        guard model.quality(for: game) == .standard else { throw NativeFailure("Old native quality overrides shared quality") }
        model.rosetta = false; model.steamVR = false; model.redistCount = 0
        guard model.ready, model.backend(forID: lab.id) == .vxrt else { throw NativeFailure("Windows fallback blocks native setup") }
        model.showInstallation(lab)
        guard model.page == .setup, model.setupFocusID == lab.id, model.setupNeedsWindows,
              model.play == nil, model.job == nil else { throw NativeFailure("Installation navigation started work") }
        var future = alyx; future.id = "999999"; future.name = "Synthetic native title"
        guard future.backend(preferred: .nativeARM64) == .nativeARM64, lab.backend(preferred: .nativeARM64) == .vxrt else { throw NativeFailure("Runtime resolver is tied to Alyx ID") }
        let before = model.installedBackends
        model.chooseBackend(.vxrt); model.chooseBackend(.nativeARM64)
        guard model.installedBackends == before else { throw NativeFailure("Mode switch changed installations") }
        model.getAndPlay(alyx, flat: false, reinstall: true, startAfterInstall: false)
        guard model.flow?.backend == .nativeARM64, model.flow?.steamcmd == [alyx.id], model.flow?.stage == .confirm,
              model.flow?.startAfterInstall == false else { throw NativeFailure("Native repair must prepare without autoplay") }
        model.chooseBackend(.vxrt)
        guard model.preferredBackend == .nativeARM64 else { throw NativeFailure("Runtime changed during an install flow") }
        model.cancelFlow()
        game.antiCheat = ["test"]
        guard !model.canStart(game, flat: false) else { throw NativeFailure("Anti-cheat refusal regression") }
        let migrated = LaunchPreferences.load(["gameBackends": [alyx.id: "nativeARM64"], "nativeAlyxQuality": "sharp", "quality": "standard"])
        guard migrated.backend == .nativeARM64, migrated.quality == .sharp else { throw NativeFailure("Legacy native settings migration") }
        let current = LaunchPreferences.load(["preferredBackend": "vxrt", "quality": "3k", "gameBackends": [alyx.id: "nativeARM64"], "nativeAlyxQuality": "sharp"])
        guard current.backend == .vxrt, current.quality == .k3 else { throw NativeFailure("Legacy keys overrode migrated settings") }
        guard LaunchPreferences.load([:]).backend == .vxrt else { throw NativeFailure("Fresh install default changed") }
        let linuxArgs = SteamCmd.linuxCommand(script: root.appendingPathComponent("steamcmd.sh"), account: "owner", installAt: root.appendingPathComponent("native content"), commands: ["+app_update", "546560", "-beta", "public", "validate"])
        guard let installIndex = linuxArgs.firstIndex(of: "+force_install_dir"), let loginIndex = linuxArgs.firstIndex(of: "+login"),
              installIndex < loginIndex, linuxArgs.contains("linux"), linuxArgs.suffix(6) == ["+app_update", "546560", "-beta", "public", "validate", "+quit"] else { throw NativeFailure("Linux content repair command or force_install_dir ordering") }
        model.installedBackends = [:]; model.steamVR = false; model.account = "synthetic-owner"
        model.wanted = [alyx.id]; model.flowLog = []
        setenv("VERANTYX_DRY_RUN", "1", 1)
        await model.download()
        guard model.flowLog.contains(where: { $0.contains("dry run: nativeARM64") }), model.play == nil,
              !model.console.running, !model.setupDownloading else { throw NativeFailure("Native setup did not use native installer or attempted autoplay") }
        model.chooseBackend(.vxrt); model.flowLog = []
        await model.download()
        guard model.flowLog.contains(where: { $0.contains("would download 250820") }), model.flowLog.contains(where: { $0.contains("would download 546560") }),
              !model.flowLog.contains(where: { $0.contains("nativeARM64") }), model.play == nil else { throw NativeFailure("Windows setup did not use Windows downloads") }
        unsetenv("VERANTYX_DRY_RUN")
        guard FPSCapture.median([30, 10, .nan, 20, 40]) == 25, FPSCapture.median([]) == nil else { throw NativeFailure("FPS comparison statistics") }
        let console = Console()
        let metadata = "\"546560\" { \"depots\" { \"546561\" { \"manifests\" { \"public\" { \"gid\" \"1\" } } } \"546564\" { \"manifests\" { \"public\" { \"gid\" \"2\" } } } } }"
        try console.start(["/usr/bin/printf", "%s\n", metadata] + Array(repeating: "console output Привет", count: 1200) + ["last line"], environment: ProcessInfo.processInfo.environment)
        guard await console.finished() == 0, console.transcript.contains("last line"), console.lines.count <= 601,
              try ValveKV.depotManifests(console.transcript)["546564"] == "2" else { throw NativeFailure("PTY metadata capture or final output drain") }
        try console.start(["/usr/bin/printf", "second run"], environment: ProcessInfo.processInfo.environment)
        guard await console.finished() == 0, console.transcript == "second run" else { throw NativeFailure("PTY state not cleared") }
        let archiveRoot = root.appendingPathComponent("package")
        let library = archiveRoot.appendingPathComponent("usr/lib/aarch64-linux-gnu/libtest.so.1")
        try fm.createDirectory(at: library.deletingLastPathComponent(), withIntermediateDirectories: true)
        try Data("synthetic library".utf8).write(to: library)
        let tar = root.appendingPathComponent("data.tar.gz"), deb = root.appendingPathComponent("test.deb")
        guard await run("/usr/bin/tar", ["-czf", tar.path, "-C", archiveRoot.path, "usr"]).status == 0,
              await run("/usr/bin/ar", ["-r", deb.path, tar.path]).status == 0 else { throw NativeFailure("Synthetic support archive") }
        let destination = root.appendingPathComponent("unpacked")
        try await NativeArchive.extractDeb(deb, into: destination)
        guard try Data(contentsOf: destination.appendingPathComponent("usr/lib/aarch64-linux-gnu/libtest.so.1")) == Data("synthetic library".utf8) else { throw NativeFailure("Support archive extraction") }
        // Full installer regression with a fake CLI, old Windows manifest and changed
        // public depots. All payloads are synthetic; URLSession and real Steam are unused.
        let common = Paths.steamLibrary.appendingPathComponent("steamapps/common/Half-Life Alyx")
        let windowsACF = Paths.steamLibrary.appendingPathComponent("steamapps/appmanifest_546560.acf")
        try fm.createDirectory(at: common.appendingPathComponent("game/hlvr"), withIntermediateDirectories: true)
        let oldAssets = Data("old Windows assets".utf8)
        try oldAssets.write(to: common.appendingPathComponent("game/hlvr/pak01_dir.vpk"))
        let win64 = common.appendingPathComponent("game/hlvr/bin/win64")
        try fm.createDirectory(at: win64, withIntermediateDirectories: true)
        try fm.createSymbolicLink(atPath: win64.appendingPathComponent("win64").path, withDestinationPath: ".")
        let outside = root.appendingPathComponent("outside-install")
        try fm.createDirectory(at: outside, withIntermediateDirectories: true)
        try Data("external data".utf8).write(to: outside.appendingPathComponent("private-file"))
        try fm.createSymbolicLink(at: common.appendingPathComponent("game/hlvr/external"), withDestinationURL: outside)
        let oldManifest = "\"AppState\" { \"appid\" \"546560\" \"name\" \"Half-Life: Alyx\" \"installdir\" \"Half-Life Alyx\" \"StateFlags\" \"4\" \"InstalledDepots\" { \"546561\" { \"manifest\" \"old\" } } }"
        try oldManifest.write(to: windowsACF, atomically: true, encoding: .utf8)
        let client = layout.downloads.appendingPathComponent("steamcmd")
        try fm.createDirectory(at: client, withIntermediateDirectories: true)
        let newManifest = "\"AppState\" { \"appid\" \"546560\" \"StateFlags\" \"4\" \"InstalledDepots\" { \"546561\" { \"manifest\" \"1\" } \"546564\" { \"manifest\" \"2\" } } }"
        try newManifest.write(to: client.appendingPathComponent("linux.acf"), atomically: true, encoding: .utf8)
        try metadata.write(to: client.appendingPathComponent("public.vdf"), atomically: true, encoding: .utf8)
        let target = layout.downloads.appendingPathComponent("alyx-content")
        try target.path.write(to: client.appendingPathComponent("expected-target"), atomically: true, encoding: .utf8)
        var armELF = Data(repeating: 0, count: 64)
        armELF.replaceSubrange(0..<6, with: [0x7f, 0x45, 0x4c, 0x46, 2, 1]); armELF[18] = 183; armELF[54] = 56
        try armELF.write(to: client.appendingPathComponent("synthetic.so"))
        let fake = #"""
        #!/bin/bash
        set -eu
        here="$(cd "$(dirname "$0")" && pwd)"
        case " $* " in
          *" +app_info_print "*) cat "$here/public.vdf" ;;
          *" +app_update "*)
            if [[ -f "$here/reject-update" ]]; then echo 'ERROR! synthetic update failure'; exit 0; fi
            previous=''; target=''
            for value in "$@"; do
              if [[ "$previous" == +force_install_dir ]]; then target="$value"; fi
              previous="$value"
            done
            [[ "$target" == "$(cat "$here/expected-target")" ]]
            mkdir -p "$target/game/bin/linuxsteamrtarm64" "$target/game/hlvr/bin/linuxsteamrtarm64" "$target/steamapps"
            cp "$here/synthetic.so" "$target/game/bin/linuxsteamrtarm64/libengine2.so"
            cp "$here/synthetic.so" "$target/game/hlvr/bin/linuxsteamrtarm64/libclient.so"
            printf 'updated native assets' > "$target/game/hlvr/pak01_dir.vpk"
            cp "$here/linux.acf" "$target/steamapps/appmanifest_546560.acf"
            echo "Success! App '546560' fully installed."
            ;;
          *) exit 3 ;;
        esac
        """#
        let script = client.appendingPathComponent("steamcmd.sh")
        try fake.write(to: script, atomically: true, encoding: .utf8)
        try fm.setAttributes([.posixPermissions: 0o700], ofItemAtPath: script.path)
        let packages = Data("[]".utf8)
        try packages.write(to: layout.packages)
        let support = layout.downloads.appendingPathComponent("support-" + NativeFiles.sha256(packages).prefix(16)).appendingPathComponent("usr/lib/aarch64-linux-gnu")
        try fm.createDirectory(at: support, withIntermediateDirectories: true)
        for name in NativeFiles.systemLibraries { try armELF.write(to: support.appendingPathComponent(name)) }
        model.chooseBackend(.nativeARM64)
        guard await model.downloadNative(), model.play == nil else { throw NativeFailure("Old Windows content was not incrementally prepared") }
        guard try Data(contentsOf: common.appendingPathComponent("game/hlvr/pak01_dir.vpk")) == oldAssets,
              try String(contentsOf: windowsACF, encoding: .utf8) == oldManifest,
              model.isInstalled(alyx.id, backend: .vxrt), model.isInstalled(alyx.id, backend: .nativeARM64) else { throw NativeFailure("Native content update changed Windows data or installation state") }
        let prepared = layout.versionRoot(try layout.installation()).appendingPathComponent("game-root")
        for relative in ["game/hlvr/bin/win64/win64", "game/hlvr/external"] {
            guard !fm.fileExists(atPath: target.appendingPathComponent(relative).path),
                  !fm.fileExists(atPath: prepared.appendingPathComponent(relative).path) else { throw NativeFailure("Windows symbolic links reached the native installation") }
        }
        guard try fm.destinationOfSymbolicLink(atPath: win64.appendingPathComponent("win64").path) == ".",
              try fm.destinationOfSymbolicLink(atPath: common.appendingPathComponent("game/hlvr/external").path) == outside.path,
              try String(contentsOf: outside.appendingPathComponent("private-file"), encoding: .utf8) == "external data" else { throw NativeFailure("Native preparation changed a source link or its external target") }
        let completed = try Data(contentsOf: layout.manifest)
        try Data().write(to: client.appendingPathComponent("reject-update"))
        guard !(await model.downloadNative()), try Data(contentsOf: layout.manifest) == completed,
              try Data(contentsOf: common.appendingPathComponent("game/hlvr/pak01_dir.vpk")) == oldAssets else { throw NativeFailure("Failed retry published an incomplete installation") }
        print("App model: global runtime, migration, mixed installs, shared quality, readiness, navigation and Windows regression checks passed")
        print("App integration: PTY metadata / Unicode / output drain, synthetic Debian archive and FPS statistics passed")
        print("Native installer: old-manifest reuse, symbolic-link omission, fake Linux validation, original preservation, no autoplay and failed-retry publication checks passed")
    }
}
