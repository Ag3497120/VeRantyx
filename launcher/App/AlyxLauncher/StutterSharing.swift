import Foundation

/// May the headset send stutter reports? Asked once: in the Mac installer, whose answer is built
/// into the app (`GLOVEBOX_SHARE_STUTTERS=1|0` and `GLOVEBOX_SHARE_STUTTERS_AT=<unix time>` in the
/// bundled alyx-demo/alyx-env.txt), else here while the game downloads. An answer given here goes
/// to Documents/klfz/report-consent.json (`{"answer": true, "at": <unix time>}`), where the
/// uploader reads it too. The newest answer wins, so a change on the Mac lands with the next
/// update, and the account menu's switch changes it any time.
enum StutterSharing {
    struct Answer: Codable, Equatable {
        var answer: Bool
        var at: Double
    }

    /// The answer, or nil while nobody has asked.
    static var choice: Bool? {
        [fromMac, fromHeadset].compactMap { $0 }.max { $0.at < $1.at }?.answer
    }

    static func set(_ on: Bool) {
        let url = consentFile
        try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try? JSONEncoder().encode(Answer(answer: on, at: Date().timeIntervalSince1970)).write(to: url, options: .atomic)
    }

    static var consentFile: URL {
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first
            ?? URL(fileURLWithPath: NSHomeDirectory()).appendingPathComponent("Documents")
        return docs.appendingPathComponent("klfz/report-consent.json")
    }

    static var fromHeadset: Answer? {
        (try? Data(contentsOf: consentFile)).flatMap { try? JSONDecoder().decode(Answer.self, from: $0) }
    }

    /// The Mac installer's answer, from the bundled defaults.
    static let fromMac: Answer? = {
        guard let url = Bundle.main.url(forResource: "alyx-env", withExtension: "txt", subdirectory: "alyx-demo"),
              let text = try? String(contentsOf: url, encoding: .utf8) else { return nil }
        var values: [String: String] = [:]
        for line in text.split(separator: "\n") {
            let kv = line.split(separator: "=", maxSplits: 1).map { $0.trimmingCharacters(in: .whitespaces) }
            if kv.count == 2, kv[0].hasPrefix("GLOVEBOX_SHARE_STUTTERS") { values[kv[0]] = kv[1] }
        }
        guard let a = values["GLOVEBOX_SHARE_STUTTERS"], a == "1" || a == "0" else { return nil }
        return Answer(answer: a == "1", at: values["GLOVEBOX_SHARE_STUTTERS_AT"].flatMap(Double.init) ?? 0)
    }()
}
