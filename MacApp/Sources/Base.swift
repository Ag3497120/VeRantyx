// The app's ground: its words in several languages, where things are, and the small facts about this Mac it checks.
import AppKit
import Darwin
import Foundation

// MARK: - Words

/// The languages the app speaks. English and Japanese are written at each call site (`T(en, ja)`); the others are
/// tables in Contents/Resources/Localizations/<code>.json, from the exact English text to its translation (MacApp/Localizations).
enum Language {
    /// Each language's code and its own name, in the order the picker shows them.
    static let all: [(code: String, name: String)] = [
        ("en", "English"), ("ja", "日本語"), ("ko", "한국어"), ("zh-Hans", "简体中文"), ("zh-Hant", "繁體中文"),
        ("es", "Español"), ("fr", "Français"), ("de", "Deutsch"), ("pt-BR", "Português (Brasil)"), ("ru", "Русский"),
        ("it", "Italiano"),
    ]

    /// The language chosen in Help ("language" in the app's settings; `-language <code>` on the command line), or else
    /// the first of the Mac's preferred languages the app has; English when it has none of them.
    static let current: String = {
        if let chosen = UserDefaults.standard.string(forKey: "language"), let code = match(chosen) { return code }
        for preferred in Locale.preferredLanguages { if let code = match(preferred) { return code } }
        return "en"
    }()

    /// The app's language for a language identifier ("zh-TW" → zh-Hant, "pt-PT" → pt-BR, "es-419" → es), if it has one.
    static func match(_ identifier: String) -> String? {
        if all.contains(where: { $0.code == identifier }) { return identifier }
        let language = Locale.Language(identifier: identifier)
        guard let code = language.languageCode?.identifier else { return nil }
        if code == "zh" {
            let script = Locale.Language(identifier: language.maximalIdentifier).script?.identifier
            return script == "Hant" ? "zh-Hant" : "zh-Hans"
        }
        if code == "pt" { return "pt-BR" }
        return all.contains(where: { $0.code == code }) ? code : nil
    }

    /// The table of the current language (empty for English and Japanese, or when its file is missing).
    static let table: [String: String] = {
        guard current != "en" && current != "ja",
              let url = Bundle.main.resourceURL?.appendingPathComponent("Localizations/\(current).json"),
              let data = try? Data(contentsOf: url),
              let table = try? JSONDecoder().decode([String: String].self, from: data) else { return [:] }
        return table
    }()

    /// The choice in the settings when the app started ("": automatic): the picker says a restart is needed when it differs.
    static let chosenAtStart = UserDefaults.standard.string(forKey: "language") ?? ""

    /// Starts the app again (a new instance, then this one quits), to speak the language chosen.
    @MainActor static func relaunch() {
        let configuration = NSWorkspace.OpenConfiguration()
        configuration.createsNewApplicationInstance = true
        NSWorkspace.shared.openApplication(at: Bundle.main.bundleURL, configuration: configuration) { _, error in
            if error == nil { DispatchQueue.main.async { NSApp.terminate(nil) } }
        }
    }

    /// Chooses a language for the next start ("" or nil: automatic). AppleLanguages too, so that the menus and the
    /// panels macOS draws speak the same language.
    static func choose(_ code: String?) {
        if let code, !code.isEmpty {
            UserDefaults.standard.set(code, forKey: "language")
            UserDefaults.standard.set([code], forKey: "AppleLanguages")
        } else {
            UserDefaults.standard.removeObject(forKey: "language")
            UserDefaults.standard.removeObject(forKey: "AppleLanguages")
        }
    }
}

let japanese = Language.current == "ja"

/// The text in the app's language: the Japanese given, the table's translation of the English, or the English.
func T(_ en: String, _ ja: String) -> String { japanese ? ja : Language.table[en] ?? en }

/// The same for a text with values in it: `%@` in the English and the Japanese (`%1$@`, `%2$@` when a translation
/// puts them in another order), filled with `values`.
func TF(_ en: String, _ ja: String, _ values: String...) -> String { String(format: T(en, ja), arguments: values) }

/// An error with a message for the player.
struct Failure: LocalizedError {
    var text: String
    init(_ text: String) { self.text = text }
    var errorDescription: String? { text }
}

// MARK: - Places

/// The same places the command-line scripts use (scripts/vxrt_paths.sh), so the app and the scripts share one setup.
enum Paths {
    static let home = FileManager.default.homeDirectoryForCurrentUser
    static let defaultDrive = home.appendingPathComponent("Verantyx_VR_Drive")
    /// Where SteamVR, the games and the Windows runtime files go (another disk can be chosen: they are large).
    static var drive: URL {
        get { UserDefaults.standard.string(forKey: "drive").map { URL(fileURLWithPath: $0) } ?? defaultDrive }
        set { UserDefaults.standard.set(newValue.path, forKey: "drive") }
    }
    static var steamLibrary: URL { drive.appendingPathComponent("vxrt_steam") }
    static var redist: URL {                         // (VXRT_REDIST: elsewhere, as for tools/vxrt_redist.py)
        ProcessInfo.processInfo.environment["VXRT_REDIST"].map { URL(fileURLWithPath: $0) } ?? drive.appendingPathComponent("vxrt_redist")
    }
    static let support = home.appendingPathComponent("Library/Application Support/VeRantyx")
    static let steamcmd = support.appendingPathComponent("steamcmd")
    static let alvrLog = support.appendingPathComponent("ALVR/session_log.txt")
    static let logs = home.appendingPathComponent("Library/Logs/VeRantyx")
    /// The runtime the app carries: the launch scripts, VXRuntime, the encoder, the SteamVR driver, ALVR's streaming
    /// core, laid out as in the repository (so the scripts find them where they look). VERANTYX_REPO points it at a
    /// checkout instead (development).
    static var runtime: URL {
        if let repo = ProcessInfo.processInfo.environment["VERANTYX_REPO"] { return URL(fileURLWithPath: repo) }
        return Bundle.main.resourceURL!.appendingPathComponent("runtime")
    }
    static let steamClient = home.appendingPathComponent("Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steamclient.dylib")
    static let repoURL = URL(string: "https://github.com/Ag3497120/VeRantyx")!
}

// MARK: - Running programs

struct Output { var status: Int32; var text: String }

/// Runs a program to its end, off the main thread, and returns its exit status and output (stdout and stderr).
func run(_ path: String, _ args: [String], env: [String: String]? = nil) async -> Output {
    await withCheckedContinuation { done in
        DispatchQueue.global().async {
            let p = Process()
            p.executableURL = URL(fileURLWithPath: path)
            p.arguments = args
            if let env { p.environment = env }
            let pipe = Pipe()
            p.standardOutput = pipe
            p.standardError = pipe
            p.standardInput = FileHandle.nullDevice
            do { try p.run() } catch { done.resume(returning: Output(status: -1, text: error.localizedDescription)); return }
            let data = pipe.fileHandleForReading.readDataToEndOfFile()
            p.waitUntilExit()
            done.resume(returning: Output(status: p.terminationStatus, text: String(decoding: data, as: UTF8.self)))
        }
    }
}

/// Command lines of the running processes that match a pattern (pgrep -f, as the scripts use).
func pgrep(_ pattern: String) async -> [String] {
    await run("/usr/bin/pgrep", ["-fl", pattern]).text.split(separator: "\n").map(String.init)
}

/// Whether something on this Mac listens on a TCP port (SteamVR's web server: 27062).
func listening(_ port: UInt16) -> Bool {
    let fd = socket(AF_INET, SOCK_STREAM, 0)
    guard fd >= 0 else { return false }
    defer { close(fd) }
    var addr = sockaddr_in()
    addr.sin_family = sa_family_t(AF_INET)
    addr.sin_port = port.bigEndian
    addr.sin_addr.s_addr = inet_addr("127.0.0.1")
    return withUnsafePointer(to: &addr) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } } == 0
}

// MARK: - This Mac

enum Mac {
    static var appleSilicon: Bool {
        var v: Int32 = 0
        var size = MemoryLayout<Int32>.size
        return sysctlbyname("hw.optional.arm64", &v, &size, nil, 0) == 0 && v == 1
    }
    static var chip: String {
        var size = 0
        sysctlbyname("machdep.cpu.brand_string", nil, &size, nil, 0)
        var buf = [CChar](repeating: 0, count: max(size, 1))
        sysctlbyname("machdep.cpu.brand_string", &buf, &size, nil, 0)
        return String(cString: buf)
    }
    static var memoryGB: Int { Int(ProcessInfo.processInfo.physicalMemory >> 30) }
    static var model: String {
        var size = 0
        sysctlbyname("hw.model", nil, &size, nil, 0)
        var buf = [CChar](repeating: 0, count: max(size, 1))
        sysctlbyname("hw.model", &buf, &size, nil, 0)
        return String(cString: buf)
    }
    static var macOS: String { ProcessInfo.processInfo.operatingSystemVersionString }
    /// x86-64 programs run (Rosetta 2 installed): VXRuntime and steamcmd are x86-64.
    static func rosetta() async -> Bool { await run("/usr/bin/arch", ["-x86_64", "/usr/bin/true"]).status == 0 }
    /// Installs Rosetta 2 with macOS's own installer; macOS asks for an administrator's password.
    static func installRosetta() async -> Output {
        await run("/usr/bin/osascript", ["-e", "do shell script \"/usr/sbin/softwareupdate --install-rosetta --agree-to-license\" with administrator privileges"])
    }
    /// Space left on the disk of a folder (or of the nearest folder above it that exists), in bytes.
    static func freeSpace(_ url: URL) -> Int64? {
        var u = url
        while !FileManager.default.fileExists(atPath: u.path) && u.path != "/" { u.deleteLastPathComponent() }
        return (try? u.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]))?.volumeAvailableCapacityForImportantUsage
    }
    static func volumeID(_ url: URL) -> NSObject? {
        var u = url
        while !FileManager.default.fileExists(atPath: u.path) && u.path != "/" { u.deleteLastPathComponent() }
        return (try? u.resourceValues(forKeys: [.volumeIdentifierKey]))?.volumeIdentifier as? NSObject
    }
}

enum Steam {
    static var appURL: URL? { NSWorkspace.shared.urlForApplication(withBundleIdentifier: "com.valvesoftware.steam") }
    /// Steam for Mac has been installed and has run once (it puts its client here when it updates itself).
    static var installed: Bool { appURL != nil && FileManager.default.fileExists(atPath: Paths.steamClient.path) }
    static func running() async -> Bool { !(await pgrep("Steam.AppBundle/Steam/Contents/MacOS/steam_osx")).isEmpty }
    static func open() {
        if let url = appURL { NSWorkspace.shared.openApplication(at: url, configuration: NSWorkspace.OpenConfiguration()) }
        else { NSWorkspace.shared.open(URL(string: "https://store.steampowered.com/about/")!) }
    }
}

func gigabytes(_ bytes: Int64) -> String {
    String(format: bytes >= 100 << 30 ? "%.0f GB" : "%.1f GB", Double(bytes) / Double(1 << 30))
}
