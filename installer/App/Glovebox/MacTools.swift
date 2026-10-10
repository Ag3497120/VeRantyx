import AppKit
import GloveboxKit
import SystemConfiguration

/// What the app checks and runs on the Mac itself. The engine runs on Xcode's Python, so until
/// Xcode works there's no engine to ask: the app checks the Mac, the disk and Xcode on its own.
enum MacTools {
    struct Preflight {
        var checks: [CheckResult]
        /// The Xcode to use (`DEVELOPER_DIR`), so nothing depends on `xcode-select`.
        var developerDir: String?
        var engineCanRun: Bool
        var xcodeMajor: Int?
    }

    static let minXcode = 26
    /// The Xcodes whose license and first launch are through, so they aren't asked again.
    private static var setUp: Set<String> = []
    static let minFreeBytes: Int64 = 15_000_000_000
    static let appStoreXcode = "macappstore://apps.apple.com/app/xcode/id497799835"

    static func preflight() async -> Preflight {
        var checks: [CheckResult] = []
        let os = ProcessInfo.processInfo.operatingSystemVersion
        if appleSilicon {
            checks.append(CheckResult(id: .mac, status: .ok, value: "macOS \(os.majorVersion).\(os.minorVersion)"))
        } else {
            checks.append(CheckResult(id: .mac, status: .fail, code: "mac-unsupported"))
            return Preflight(checks: checks, developerDir: nil, engineCanRun: false)
        }
        let free = (try? URL(fileURLWithPath: NSHomeDirectory())
            .resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]).volumeAvailableCapacityForImportantUsage) ?? nil
        if let free, free < minFreeBytes {
            checks.append(CheckResult(id: .disk, status: .fail, code: "disk-low",
                                      detail: "\(free / 1_000_000_000) GB free, \(minFreeBytes / 1_000_000_000) GB needed",
                                      fix: Fix(kind: .openURL, url: "x-apple.systempreferences:com.apple.settings.Storage")))
        } else {
            checks.append(CheckResult(id: .disk, status: .ok, value: free.map { "\($0 / 1_000_000_000) GB free" }))
        }

        let found = NSWorkspace.shared.urlsForApplications(withBundleIdentifier: "com.apple.dt.Xcode")
            .filter { XcodeInfo.isUsableApp($0.path) }.compactMap { url in
            (Bundle(url: url)?.infoDictionary?["CFBundleShortVersionString"] as? String).map { (path: url.path, version: $0) }
        }
        // While the App Store downloads Xcode, its app can already be in Applications without its
        // tools, and before Launch Services knows it.
        let installed = found.filter { FileManager.default.isExecutableFile(atPath: $0.path + "/Contents/Developer/usr/bin/xcodebuild") }
        let arriving = !found.isEmpty || ((try? FileManager.default.contentsOfDirectory(atPath: "/Applications")) ?? []).contains {
            $0.hasPrefix("Xcode") && $0.hasSuffix(".app") && FileManager.default.fileExists(atPath: "/Applications/" + $0)
        }
        guard let xcode = XcodeInfo.preferred(installed, selected: selectedDeveloperDir(), minMajor: minXcode) else {
            checks.append(CheckResult(id: .xcode, status: .fail, code: "xcode-missing", value: arriving ? "downloading" : nil,
                                      fix: Fix(kind: .openURL, label: "Get Xcode", url: appStoreXcode)))
            return Preflight(checks: checks, developerDir: nil, engineCanRun: false)
        }
        guard XcodeInfo.major(xcode.version) >= minXcode else {
            checks.append(CheckResult(id: .xcode, status: .fail, code: "xcode-too-old", detail: "Xcode \(xcode.version)",
                                      fix: Fix(kind: .openURL, label: "Update Xcode", url: appStoreXcode)))
            return Preflight(checks: checks, developerDir: nil, engineCanRun: false, xcodeMajor: XcodeInfo.major(xcode.version))
        }
        let dev = xcode.path + "/Contents/Developer"
        // A new Xcode builds nothing until its license is accepted and its first-launch packages are
        // in (Python can run before that). Xcode does both itself when it opens, behind Apple's own
        // prompt naming Xcode; Glovebox never asks for the Mac's password. Once through, they stay.
        // Nothing goes through xcrun before they pass: that pops macOS's "Install Command Line
        // Developer Tools" box.
        if !setUp.contains(dev) {
            let xcodebuild = dev + "/usr/bin/xcodebuild"
            let (license, _) = await run(xcodebuild, ["-license", "check"], developerDir: dev)
            var first: Int32 = 0
            if license == 0 { first = await run(xcodebuild, ["-checkFirstLaunchStatus"], developerDir: dev).0 }
            var (code, out): (Int32, String) = (0, "")
            if license == 0, first == 0 { (code, out) = await run("/usr/bin/xcrun", ["python3", "-c", "print(1)"], developerDir: dev) }
            guard license == 0, first == 0, code == 0 else {
                checks.append(CheckResult(id: .xcode, status: .fail, code: "xcode-first-launch",
                                          value: license != 0 ? "license" : "components",
                                          detail: code != 0 ? out.split(separator: "\n").first.map(String.init) : nil,
                                          fix: Fix(kind: .openApp, label: "Open Xcode", url: "xcode://")))
                return Preflight(checks: checks, developerDir: dev, engineCanRun: false, xcodeMajor: XcodeInfo.major(xcode.version))
            }
            setUp.insert(dev)
        }
        checks.append(CheckResult(id: .xcode, status: .ok, value: "Xcode \(xcode.version)"))
        return Preflight(checks: checks, developerDir: dev, engineCanRun: true, xcodeMajor: XcodeInfo.major(xcode.version))
    }

    /// The teams in Xcode's account settings, read the way the engine reads them.
    static func teams() async -> [Team] {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/defaults")
        p.arguments = ["export", "com.apple.dt.Xcode", "-"]
        let (_, data) = await output(p)
        return XcodeInfo.teams(fromPreferences: data)
    }

    static var appleSilicon: Bool {
        var v: Int32 = 0
        var size = MemoryLayout<Int32>.size
        return sysctlbyname("hw.optional.arm64", &v, &size, nil, 0) == 0 && v == 1
    }

    /// Runs a tool and returns its exit code and everything it printed; stopped after `timeout` seconds, if given.
    static func run(_ tool: String, _ args: [String], developerDir: String?, timeout: Double? = nil) async -> (Int32, String) {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: tool)
        p.arguments = args
        var env = ProcessInfo.processInfo.environment
        if let d = developerDir { env["DEVELOPER_DIR"] = d }
        p.environment = env
        let (code, data) = await output(p, mergeErrors: true, timeout: timeout)
        return (code, String(decoding: data, as: UTF8.self))
    }

    private static func output(_ p: Process, mergeErrors: Bool = false, timeout: Double? = nil) async -> (Int32, Data) {
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = mergeErrors ? pipe : FileHandle.nullDevice
        p.standardInput = FileHandle.nullDevice
        return await withCheckedContinuation { c in
            DispatchQueue.global().async {
                do { try p.run() } catch { c.resume(returning: (127, Data())); return }
                Children.add(p)
                if let timeout { DispatchQueue.global().asyncAfter(deadline: .now() + timeout) { if p.isRunning { p.terminate() } } }
                let data = pipe.fileHandleForReading.readDataToEndOfFile()
                p.waitUntilExit()
                Children.remove(p)
                c.resume(returning: (p.terminationStatus, data))
            }
        }
    }

    /// Runs a fix's command (`zsh -c`), passing each line it prints to `line`, e.g.
    /// `xcodebuild -downloadPlatform visionOS` and its percentages.
    static func stream(_ command: String, developerDir: String?, environment: [String: String] = [:],
                       line: @escaping @Sendable (String) -> Void) async -> Int32 {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/zsh")
        p.arguments = ["-c", command]
        var env = ProcessInfo.processInfo.environment
        if let d = developerDir { env["DEVELOPER_DIR"] = d }
        env.merge(environment) { $1 }
        p.environment = env
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = pipe
        p.standardInput = FileHandle.nullDevice
        return await withCheckedContinuation { c in
            DispatchQueue.global().async {
                do { try p.run() } catch { c.resume(returning: 127); return }
                Children.add(p)
                var buffer = Data()
                while true {
                    let chunk = pipe.fileHandleForReading.availableData
                    if chunk.isEmpty { break }
                    buffer.append(chunk)
                    // xcodebuild redraws its progress with \r, so split on both.
                    while let i = buffer.firstIndex(where: { $0 == 0x0A || $0 == 0x0D }) {
                        let l = String(decoding: buffer[buffer.startIndex..<i], as: UTF8.self)
                        buffer.removeSubrange(buffer.startIndex...i)
                        if !l.isEmpty { line(l) }
                    }
                }
                p.waitUntilExit()
                Children.remove(p)
                c.resume(returning: p.terminationStatus)
            }
        }
    }

    /// The first percentage in a line ("… 42.5% (3.4 GB of 8.2 GB)"), as a fraction.
    static func percent(in line: String) -> Double? {
        guard let r = line.range(of: #"(\d{1,3}(\.\d+)?)%"#, options: .regularExpression) else { return nil }
        return Double(line[r].dropLast()).map { min(1, $0 / 100) }
    }

    static func openXcode() {
        guard let url = xcodeURL else { return }
        NSWorkspace.shared.openApplication(at: url, configuration: NSWorkspace.OpenConfiguration())
    }

    /// The Xcode Glovebox uses (XcodeInfo.preferred): the Mac's own, unless it's missing or too old.
    static var xcodeURL: URL? {
        let all = NSWorkspace.shared.urlsForApplications(withBundleIdentifier: "com.apple.dt.Xcode")
            .filter { XcodeInfo.isUsableApp($0.path) }.compactMap { url in
            (Bundle(url: url)?.infoDictionary?["CFBundleShortVersionString"] as? String).map { (path: url.path, version: $0) }
        }
        return XcodeInfo.preferred(all, selected: selectedDeveloperDir(), minMajor: minXcode).map { URL(fileURLWithPath: $0.path) }
    }

    /// `xcode-select -p`: the developer folder the Mac uses (nil when it can't say).
    static func selectedDeveloperDir() -> String? {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/xcode-select")
        p.arguments = ["-p"]
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = FileHandle.nullDevice
        guard (try? p.run()) != nil else { return nil }
        p.waitUntilExit()
        let s = String(decoding: pipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
            .trimmingCharacters(in: .whitespacesAndNewlines)
        return p.terminationStatus == 0 && !s.isEmpty ? s : nil
    }

    /// Xcode's own icon, once it's installed.
    static var xcodeIcon: NSImage? { xcodeURL.map { NSWorkspace.shared.icon(forFile: $0.path) } }

    /// Xcode's first launch offers about 30 GB of platforms. The game needs only visionOS, which
    /// Glovebox adds itself, so Xcode 26 skips the picker and Xcode 27, whose first launch always
    /// shows it, has only visionOS switched on. Settings › Components still offers every platform.
    static func trimXcodeFirstLaunch() {
        guard let xcode = UserDefaults(suiteName: "com.apple.dt.Xcode") else { return }
        if xcode.object(forKey: "IDEFirstLaunchSuppressPlatformChooser") == nil {
            xcode.set(true, forKey: "IDEFirstLaunchSuppressPlatformChooser")
        }
        for (platform, on) in ["iphoneos": false, "watchos": false, "appletvos": false, "xros": true]
        where xcode.object(forKey: "IDEPlatformsFirstLaunchSelected-\(platform)") == nil {
            xcode.set(on, forKey: "IDEPlatformsFirstLaunchSelected-\(platform)")
        }
    }

    struct Headset: Equatable {
        var name: String
        var paired: Bool
        /// devicectl's identifier for it, for `--device`.
        var id: String? = nil
        var developerMode = false
    }

    /// The Vision Pro devicectl sees (paired or just nearby), the way the engine reads it: the
    /// devices it already knows, as the engine lists them (no `--timeout` wait for more).
    static func nearbyHeadset(developerDir: String?) async -> Headset? {
        await headsets(developerDir: developerDir).first
    }

    /// Every Vision Pro devicectl knows, simulators left out.
    static func headsets(developerDir: String?) async -> [Headset] {
        let path = FileManager.default.temporaryDirectory.appendingPathComponent("glovebox-devices-\(UUID().uuidString).json")
        defer { try? FileManager.default.removeItem(at: path) }
        let (code, _) = await run("/usr/bin/xcrun", ["devicectl", "list", "devices", "--json-output", path.path],
                                  developerDir: developerDir)
        guard code == 0, let data = try? Data(contentsOf: path),
              let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let devices = (j["result"] as? [String: Any])?["devices"] as? [[String: Any]] else { return [] }
        return devices.compactMap { d -> Headset? in
            let hp = d["hardwareProperties"] as? [String: Any] ?? [:]
            let dp = d["deviceProperties"] as? [String: Any] ?? [:]
            let cp = d["connectionProperties"] as? [String: Any] ?? [:]
            guard hp["platform"] as? String == "visionOS", cp["transportType"] as? String != "sameMachine" else { return nil }
            return Headset(name: dp["name"] as? String ?? "Apple Vision Pro", paired: cp["pairingState"] as? String == "paired",
                           id: d["identifier"] as? String, developerMode: dp["developerModeStatus"] as? String == "enabled")
        }
    }

    /// Whether an installed app's data container on the headset the engine installs to (the first paired one with
    /// Developer Mode on) holds a file: devicectl copies it, the way the engine's `collect` copies its logs. Nothing
    /// starts on the headset. Nil: no answer (no headset in reach, or devicectl said something else).
    static func headsetHasFile(app bundleID: String, path: String, developerDir: String?) async -> Bool? {
        let all = await headsets(developerDir: developerDir).filter(\.paired)
        guard let device = (all.first { $0.developerMode } ?? all.first)?.id else { return nil }
        // devicectl writes to a file path (a folder fails).
        let copy = FileManager.default.temporaryDirectory.appendingPathComponent("glovebox-headset-\(UUID().uuidString).txt")
        defer { try? FileManager.default.removeItem(at: copy) }
        let (code, out) = await run("/usr/bin/xcrun", ["devicectl", "device", "copy", "from", "--device", device,
                                                      "--domain-type", "appDataContainer", "--domain-identifier", bundleID,
                                                      "--source", path, "--destination", copy.path],
                                    developerDir: developerDir, timeout: 40)
        if code == 0 { return true }
        return out.contains("Failed to retrieve the file node") ? false : nil
    }

    /// This Mac's name, as the headset lists it under Remote Devices.
    static var macName: String {
        Demo.current != nil ? "Jane’s Mac Studio" : computerName ?? "this Mac"
    }

    /// The name in System Settings › General › About, read on this Mac. Not Host.current() or
    /// ProcessInfo.hostName: those look the Mac up on the local network, and macOS then asks to let
    /// Glovebox find devices there.
    static var computerName: String? { SCDynamicStoreCopyComputerName(nil, nil) as String? }
}
