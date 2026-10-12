import Foundation

/// What "Share Diagnostics" (the headset app's account menu) puts in the zip a player attaches to a GitHub issue.
/// It is an allow-list of files in Documents, and nothing else goes in: the Steam sign-in (.steamctl-token.json), the
/// cloud sync's records, the saves and the game stay where they are. Each file is copied with web-address queries and
/// sign-in tokens blanked, and long files keep their start and their end.
public enum DiagnosticsBundle {
    public struct Item: Sendable {
        /// Relative to Documents.
        public let path: String
        /// What it answers, for about.txt: a sentence.
        public let about: String
    }

    public static let items: [Item] = [
        Item(path: "klepton-boot.log",
             about: "Glovebox's log of the last game: each controller button the first time it reaches us, the game's "
                  + "controller actions, and after 5 s without a frame, what every thread was doing."),
        Item(path: "klepton-boot.prev.log", about: "The same log from the game before that, kept when the game starts again."),
        Item(path: "klepton-crash.log", about: "A crash report, when the game crashed."),
        Item(path: "alyx/assets/hlvr/console.log", about: "The game's own console."),
        Item(path: "alyx/assets/hlvr/save/personal.cfg", about: "The game's options: weapon hand, movement, comfort."),
        Item(path: "klfz/slow.txt", about: "Each slow graphics setup in the game and how long it took."),
        Item(path: "klfz/prewarm.log", about: "Each Preparing graphics before Play: how long, how many, how many slow."),
        Item(path: "klfz/perf.txt", about: "How the game ran, once a minute: frame rate, heat, memory."),
        Item(path: "klfz/device-info.json", about: "How the game set up the graphics device."),
    ]

    /// Copies each listed file that exists from `documents` into `folder`, under its own name. Returns one line per
    /// item, saying how big it is and when it was last written, or that this headset doesn't have it.
    public static func collect(from documents: URL, into folder: URL, cap: Int = 10 << 20) throws -> [String] {
        let fm = FileManager.default
        try fm.createDirectory(at: folder, withIntermediateDirectories: true)
        let when = ISO8601DateFormatter()
        var lines: [String] = []
        for item in items {
            let src = documents.appendingPathComponent(item.path)
            let name = src.lastPathComponent
            guard let data = try? Data(contentsOf: src) else {
                lines.append("\(name): not on this headset. \(item.about)")
                continue
            }
            let written = (try? fm.attributesOfItem(atPath: src.path)[.modificationDate] as? Date).map(when.string) ?? "?"
            try scrub(capped(data, cap: cap)).write(to: folder.appendingPathComponent(name))
            lines.append("\(name): \(data.count) bytes, written \(written). \(item.about)")
        }
        return lines
    }

    /// A file over `cap`: its first fifth and its last four fifths of `cap`, with a line saying what was left out.
    /// The start says how the game was set up, the end where it stopped.
    static func capped(_ data: Data, cap: Int) -> Data {
        guard data.count > cap else { return data }
        let head = cap / 5, tail = cap - head
        var out = Data(data.prefix(head))
        out.append(Data("\n[... \(data.count - head - tail) bytes left out by Share Diagnostics ...]\n".utf8))
        out.append(data.suffix(tail))
        return out
    }

    private static let urlQuery = try! NSRegularExpression(pattern: #"(https?://[^\s"'<>?]+)\?[^\s"'<>]+"#)
    private static let token = try! NSRegularExpression(pattern: #"eyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}(\.[A-Za-z0-9_-]+)?"#)

    /// Blanks what could sign someone in: a web address's query (signed download links carry their key there) and
    /// JSON web tokens (Steam's sign-in tokens are those). Bytes that aren't text stay as they are.
    public static func scrub(_ data: Data) -> Data {
        guard let text = String(data: data, encoding: .utf8) else { return data }
        return Data(scrub(text).utf8)
    }

    static func scrub(_ text: String) -> String {
        let all = NSRange(text.startIndex..., in: text)
        let s = urlQuery.stringByReplacingMatches(in: text, range: all, withTemplate: "$1?(removed)")
        return token.stringByReplacingMatches(in: s, range: NSRange(s.startIndex..., in: s), withTemplate: "(token removed)")
    }
}
