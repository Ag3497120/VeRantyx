import SwiftUI
import GameController
import OSLog
import os
import SteamLauncherKit
import GloveboxReports

/// "Share Diagnostics" in the account menu. When something goes wrong, the player sends us what the headset knows
/// about it: the files DiagnosticsBundle lists (the last two games' logs, the game's console and options, the
/// stutter and graphics records), what this app logged since it opened, and about.txt (this build, this headset, its
/// controllers), zipped and handed to the system's share sheet (Mail, AirDrop, Messages, Files) to attach to a
/// GitHub issue. Glovebox sends nothing itself, and the Steam sign-in and the saves never go in.
enum Diagnostics {
    /// `controllers` is `controllerLines()`, read on the main actor; the rest runs off it.
    static func makeZip(controllers: [String]) throws -> URL {
        let fm = FileManager.default
        let docs = StutterSharing.consentFile.deletingLastPathComponent().deletingLastPathComponent()
        let stamp = DateFormatter()
        stamp.locale = Locale(identifier: "en_US_POSIX")
        stamp.dateFormat = "yyyy-MM-dd HHmm"
        let name = "Glovebox Diagnostics \(stamp.string(from: Date()))"
        let work = fm.temporaryDirectory.appendingPathComponent("diagnostics", isDirectory: true)
        try? fm.removeItem(at: work)
        let folder = work.appendingPathComponent(name, isDirectory: true)
        let files = try DiagnosticsBundle.collect(from: docs, into: folder)
        try DiagnosticsBundle.scrub(Data(appLog().utf8)).write(to: folder.appendingPathComponent("app.log"))
        try Data(about(files: files, controllers: controllers).utf8).write(to: folder.appendingPathComponent("about.txt"))

        // A folder read "for uploading" comes back as a zip of it: the system's own zipper, no library.
        var coordinated: NSError?
        var result: Result<URL, Error> = .failure(CocoaError(.fileWriteUnknown))
        NSFileCoordinator().coordinate(readingItemAt: folder, options: .forUploading, error: &coordinated) { zipped in
            let zip = work.appendingPathComponent("\(name).zip")
            result = Result { try fm.copyItem(at: zipped, to: zip); return zip }
        }
        if let coordinated { throw coordinated }
        return try result.get()
    }

    /// The build, the headset and the controllers: what every report needs first and no log says on its own.
    static func about(files: [String], controllers: [String]) -> String {
        let info = GloveboxBuildInfo.load()
        let app = Bundle.main.infoDictionary ?? [:]
        let memory = ProcessInfo.processInfo.physicalMemory >> 30
        let left = os_proc_available_memory() >> 20
        let disk = try? URL(fileURLWithPath: NSHomeDirectory()).resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
        let free = disk?.volumeAvailableCapacityForImportantUsage.map { "\($0 >> 30) GB" } ?? "?"
        let version = app["CFBundleShortVersionString"] as? String ?? "?"
        let build = app["CFBundleVersion"] as? String ?? "?"
        let heat = ProcessInfo.processInfo.thermalState.rawValue
        let reports = StutterSharing.choice.map { $0 ? "on" : "off" } ?? "not answered"
        var lines: [String] = []
        lines.append("Glovebox diagnostics, \(ISO8601DateFormatter().string(from: Date()))")
        lines.append("")
        lines.append("Glovebox \(info.glovebox ?? "?") (kit \(info.kit ?? "?")), app \(version) (\(build))")
        lines.append("Headset: \(model), \(ProcessInfo.processInfo.operatingSystemVersionString)")
        lines.append("Memory: \(memory) GB, \(left) MB left for Glovebox now; heat \(heat) of 3; \(free) free")
        lines.append("Apple team: \(signing)")
        lines.append("Stutter reports: \(reports)")
        lines.append("")
        lines.append(contentsOf: controllers)
        lines += ["", "Files:"] + files.map { "- " + $0 }
        lines += ["- app.log: What this app logged since it was opened.", "- about.txt: This file."]
        return lines.joined(separator: "\n") + "\n"
    }

    /// The controllers connected now, each with the element names its hardware publishes: a button read under any
    /// other name is dead without a sign.
    @MainActor static func controllerLines() -> [String] {
        var lines = ["Controllers connected now: \(GCController.controllers().count)"]
        for c in GCController.controllers() {
            var line = "- \(c.vendorName ?? "?") (\(c.productCategory))"
            if let b = c.battery { line += String(format: ", battery %.0f%%", b.batteryLevel * 100) }
            lines.append(line)
            let p = c.physicalInputProfile
            lines.append("  buttons: " + p.buttons.keys.sorted().joined(separator: ", "))
            lines.append("  sticks: " + p.dpads.keys.sorted().joined(separator: ", "))
            lines.append("  axes: " + p.axes.keys.sorted().joined(separator: ", "))
        }
        return lines
    }

    /// "Apple Vision Pro (M2, RealityDevice14,1)", or the simulator's.
    static var model: String {
        var u = utsname()
        uname(&u)
        var id = withUnsafeBytes(of: &u.machine) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
        if let sim = ProcessInfo.processInfo.environment["SIMULATOR_MODEL_IDENTIFIER"] { id = "simulator, \(sim)" }
        let chip = ["RealityDevice14,1": "M2", "RealityDevice17,1": "M5"][id]
        return chip.map { "Apple Vision Pro (\($0), \(id))" } ?? id
    }

    /// Free or paid, from how long the app's profile lasts (a free team's lasts 7 days), and whether it carries the
    /// bigger memory allowance. Never the team's name.
    static var signing: String {
        guard let url = Bundle.main.url(forResource: "embedded", withExtension: "mobileprovision"),
              let data = try? Data(contentsOf: url),
              let start = data.range(of: Data("<?xml".utf8)), let end = data.range(of: Data("</plist>".utf8)),
              let plist = try? PropertyListSerialization.propertyList(from: data[start.lowerBound..<end.upperBound], format: nil)
                as? [String: Any]
        else { return "no profile (simulator or unsigned)" }
        let made = plist["CreationDate"] as? Date, ends = plist["ExpirationDate"] as? Date
        let days = made.flatMap { m in ends.map { $0.timeIntervalSince(m) / 86400 } } ?? 0
        let ents = plist["Entitlements"] as? [String: Any] ?? [:]
        let memory = ents["com.apple.developer.kernel.increased-memory-limit"] as? Bool == true ? "yes" : "no"
        return (days > 0 && days <= 8 ? "free" : "paid")
            + ", profile ends \(ends.map { ISO8601DateFormatter().string(from: $0) } ?? "?"), increased memory limit \(memory)"
    }

    /// This run's own log lines (ours start with a [tag]), newest last. Before the game starts they go nowhere else.
    static func appLog() -> String {
        guard let store = try? OSLogStore(scope: .currentProcessIdentifier),
              let entries = try? store.getEntries(at: store.position(date: Date().addingTimeInterval(-6 * 3600)))
        else { return "(this app's log could not be read)\n" }
        let time = ISO8601DateFormatter()
        var lines: [String] = []
        for case let e as OSLogEntryLog in entries where e.composedMessage.hasPrefix("[") {
            lines.append("\(time.string(from: e.date)) \(e.composedMessage)")
        }
        return lines.suffix(5000).joined(separator: "\n") + "\n"
    }
}

/// The account menu's "Share Diagnostics": one line on what it is, and the system's share button once the zip is made.
struct DiagnosticsSheet: View {
    @Environment(\.dismiss) private var dismiss
    @State private var zip: URL?
    @State private var failure: String?

    var body: some View {
        VStack(spacing: 28) {
            VStack(spacing: 12) {
                Image(systemName: "stethoscope")
                    .font(.system(size: 40, weight: .medium))
                    .foregroundStyle(Brand.orange)
                    .accessibilityHidden(true)
                Text("Share Diagnostics").font(.largeTitle.weight(.bold))
                Text("Logs from your last game, to attach to a GitHub issue. Your Steam sign-in and saves aren't included.")
                    .font(.title3)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                    .fixedSize(horizontal: false, vertical: true)
            }
            VStack(spacing: 12) {
                if let zip {
                    ShareLink(item: zip) {
                        Label("Share", systemImage: "square.and.arrow.up").frame(maxWidth: .infinity)
                    }
                    .buttonStyle(PillButtonStyle())
                } else if let failure {
                    Label(failure, systemImage: "exclamationmark.circle.fill")
                        .foregroundStyle(.orange)
                        .fixedSize(horizontal: false, vertical: true)
                } else {
                    // The share button's size, so nothing moves when it arrives.
                    Label("Share", systemImage: "square.and.arrow.up").frame(maxWidth: .infinity)
                        .hidden()
                        .padding(.vertical, 14)
                        .overlay { ProgressView() }
                }
                Button { dismiss() } label: { Text("Done").frame(maxWidth: .infinity) }
                    .buttonStyle(PillButtonStyle(prominent: false))
            }
        }
        .padding(40)
        .frame(width: 560)
        .presentationSizing(.fitted)
        .task {
            let controllers = Diagnostics.controllerLines()
            do { zip = try await Task.detached(priority: .userInitiated) { try Diagnostics.makeZip(controllers: controllers) }.value }
            catch { failure = "Couldn't gather the logs: \(error.localizedDescription)" }
        }
    }
}
