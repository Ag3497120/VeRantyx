// Reporting a problem: a look at the existing reports first, then GitHub's form filled in as far as the app can
// (the title included: left as the template's placeholder, every report looked the same), with the logs saved as a
// zip file on the Desktop to drag into it (GitHub's forms cannot take files from a link).
import AppKit
import Foundation

@MainActor
enum Report {
    static let issues = URL(string: "https://github.com/Ag3497120/VeRantyx/issues")!

    /// Asks whether to look at the existing reports or write a new one; for a new one saves the logs and opens the form.
    static func start(_ model: AppModel) {
        let alert = NSAlert()
        alert.messageText = T("Before you report", "報告の前に")
        alert.informativeText = T("Someone may have reported the same problem already: a look at the existing reports first can save you the time. For a new report, the app saves your logs as a zip file on the Desktop, with your Mac user name and Steam ID taken out; drag it into the form's Logs field.",
                                  "同じ問題がすでに報告されているかもしれません。先に既存の報告を見ると手間が省けます。新しく報告する時は、アプリがログを zip ファイルにしてデスクトップに保存します（Mac のユーザー名と Steam ID は取り除きます）。フォームの「Logs」欄にドラッグしてください。")
        alert.addButton(withTitle: T("Write a new report", "新しく報告する"))
        alert.addButton(withTitle: T("Look at existing reports", "既存の報告を見る"))
        alert.addButton(withTitle: T("Cancel", "キャンセル"))
        switch alert.runModal() {
        case .alertFirstButtonReturn:
            Task {
                let zip = await saveLogs()
                if let zip { NSWorkspace.shared.activateFileViewerSelecting([zip]) }
                NSWorkspace.shared.open(model.reportURL(logs: zip?.lastPathComponent))
            }
        case .alertSecondButtonReturn:
            NSWorkspace.shared.open(issues)
        default:
            break
        }
    }

    /// The logs (this start's and the previous one's, ALVR's session log) with personal details taken out, zipped on
    /// the Desktop as VeRantyx-logs-<date>.zip. nil when there is nothing to save.
    static func saveLogs() async -> URL? {
        let fm = FileManager.default
        var sources = ((try? fm.contentsOfDirectory(at: Paths.logs, includingPropertiesForKeys: nil)) ?? [])
            .filter { $0.pathExtension == "log" }
        let native = Paths.logs.appendingPathComponent("native")
        let sessions = ((try? fm.contentsOfDirectory(at: native, includingPropertiesForKeys: [.contentModificationDateKey])) ?? [])
            .filter { (try? $0.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true }
            .sorted { ((try? $0.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast)
                > ((try? $1.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast) }.prefix(5)
        for session in sessions {
            sources += ((try? fm.contentsOfDirectory(at: session, includingPropertiesForKeys: nil)) ?? [])
                .filter { ["log", "json"].contains($0.pathExtension) }
        }
        sources += ((try? fm.contentsOfDirectory(at: FPSCapture.directory, includingPropertiesForKeys: nil)) ?? [])
            .filter { $0.pathExtension == "json" }
        for alvr in [Paths.alvrLog, Paths.alvrLog.deletingPathExtension().appendingPathExtension("prev.txt")] where fm.fileExists(atPath: alvr.path) {
            sources.append(alvr)
        }
        guard !sources.isEmpty else { return nil }
        let stamp = { () -> String in let f = DateFormatter(); f.dateFormat = "yyyyMMdd-HHmm"; return f.string(from: Date()) }()
        let name = "VeRantyx-logs-\(stamp)"
        let folder = fm.temporaryDirectory.appendingPathComponent(name)
        try? fm.removeItem(at: folder)
        try? fm.createDirectory(at: folder, withIntermediateDirectories: true)
        for source in sources {
            guard let data = fm.contents(atPath: source.path) else { continue }
            let text = String(decoding: data, as: UTF8.self)
            let parent = source.deletingLastPathComponent()
            let prefix = parent.deletingLastPathComponent() == native ? "native-" + parent.lastPathComponent + "-" : parent.lastPathComponent == "ALVR" ? "alvr-" : ""
            let target = folder.appendingPathComponent(prefix + source.lastPathComponent)
            try? redact(text).write(to: target, atomically: true, encoding: .utf8)
        }
        let desktop = fm.homeDirectoryForCurrentUser.appendingPathComponent("Desktop")
        let zip = desktop.appendingPathComponent(name + ".zip")
        try? fm.removeItem(at: zip)
        let r = await run("/usr/bin/ditto", ["-c", "-k", "--norsrc", "--noextattr", "--keepParent", folder.path, zip.path])
        try? fm.removeItem(at: folder)
        return r.status == 0 ? zip : nil
    }

    /// Takes out what identifies the player: the Mac user name (in paths, also as Windows paths under Z:) and Steam IDs.
    nonisolated static func redact(_ text: String) -> String {
        var t = text
        let user = NSUserName()
        if !user.isEmpty {   // after "Users" and any slashes or backslashes (Windows paths, also escaped in JSON)
            t = t.replacingOccurrences(of: "(Users[/\\\\]+)" + NSRegularExpression.escapedPattern(for: user) + "(?![A-Za-z0-9_])",
                                       with: "$1<user>", options: .regularExpression)
        }
        for (pattern, replacement) in [("7656119[0-9]{10}", "<steam id>"), ("\\[U:1:[0-9]+\\]", "[U:1:<steam id>]")] {
            t = t.replacingOccurrences(of: pattern, with: replacement, options: .regularExpression)
        }
        return t
    }
}
